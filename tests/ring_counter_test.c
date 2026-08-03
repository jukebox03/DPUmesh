#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../doca/ring.h"

#define TEST_RING_SIZE 256u
#define TEST_PRODUCERS 8u
#define TEST_ITEMS_PER_PRODUCER 20000u

struct test_state {
    struct dma_ring ring;
    struct dma_ring_ctrl ctrl;
    struct dma_desc descs[TEST_RING_SIZE];
};

static void
test_state_init(struct test_state *s)
{
    s->ring.size = TEST_RING_SIZE;
    s->ring.descs = s->descs;
    s->ring.ctrl = &s->ctrl;
    s->ring.enq_pos = 0;
    s->ctrl.consumer_head = 0;
}

static int
descriptor_ready(const struct dma_ring *ring, uint64_t ticket)
{
    const struct dma_desc *desc = &ring->descs[ticket % ring->size];
    return __atomic_load_n(&desc->publish_seq, __ATOMIC_ACQUIRE) ==
           ticket + 1;
}

static void
test_out_of_order_prefix(void)
{
    struct test_state *s = calloc(1, sizeof(*s));
    assert(s != NULL);
    test_state_init(s);

    dma_ring_publish_desc(&s->ring, 1);
    dma_ring_publish_desc(&s->ring, 2);
    assert(!descriptor_ready(&s->ring, 0));
    assert(descriptor_ready(&s->ring, 1));
    assert(descriptor_ready(&s->ring, 2));

    dma_ring_publish_desc(&s->ring, 0);
    assert(descriptor_ready(&s->ring, 0));
    free(s);
}

/* A producer that gives up on a full ring must leave the published sequence
 * gapless: the DPA consumer steps strictly in ticket order and can never pass a
 * ticket that was claimed and never published. */
static void
test_refused_claim_leaves_no_gap(void)
{
    struct test_state *s = calloc(1, sizeof(*s));
    uint64_t ticket = 0;
    assert(s != NULL);
    test_state_init(s);

    for (uint32_t i = 0; i < s->ring.size; i++) {
        assert(dma_ring_try_claim(&s->ring, &ticket));
        assert(ticket == i);
        dma_ring_publish_desc(&s->ring, ticket);
    }

    /* Full: the claim is refused and the ticket counter does not move. */
    const uint64_t enq_pos_when_full = s->ring.enq_pos;
    assert(!dma_ring_try_claim(&s->ring, &ticket));
    assert(!dma_ring_try_claim(&s->ring, &ticket));
    assert(s->ring.enq_pos == enq_pos_when_full);

    /* One slot released: the next claim resumes at the very next ticket. */
    __atomic_store_n(&s->ring.ctrl->consumer_head, 1, __ATOMIC_RELEASE);
    assert(dma_ring_try_claim(&s->ring, &ticket));
    assert(ticket == enq_pos_when_full);
    dma_ring_publish_desc(&s->ring, ticket);
    assert(descriptor_ready(&s->ring, ticket));
    free(s);
}

struct producer_arg {
    struct test_state *state;
};

static void *
producer_main(void *opaque)
{
    struct producer_arg *arg = opaque;
    struct dma_ring *ring = &arg->state->ring;

    for (uint32_t i = 0; i < TEST_ITEMS_PER_PRODUCER; i++) {
        uint64_t ticket;
        while (!dma_ring_try_claim(ring, &ticket))
            sched_yield();

        uint32_t slot = (uint32_t)(ticket % ring->size);
        ring->descs[slot].size = (uint32_t)(ticket + 1);
        dma_ring_publish_desc(ring, ticket);
    }
    return NULL;
}

static void
test_concurrent_wrap(void)
{
    const uint64_t total =
        (uint64_t)TEST_PRODUCERS * TEST_ITEMS_PER_PRODUCER;
    struct test_state *s = calloc(1, sizeof(*s));
    pthread_t producers[TEST_PRODUCERS];
    struct producer_arg args[TEST_PRODUCERS];
    uint64_t head = 0;

    assert(s != NULL);
    test_state_init(s);
    for (uint32_t p = 0; p < TEST_PRODUCERS; p++) {
        args[p].state = s;
        assert(pthread_create(&producers[p], NULL, producer_main,
                              &args[p]) == 0);
    }

    while (head < total) {
        if (!descriptor_ready(&s->ring, head)) {
            sched_yield();
            continue;
        }
        uint32_t slot = (uint32_t)(head % TEST_RING_SIZE);
        assert(s->descs[slot].size == (uint32_t)(head + 1));
        head++;
        __atomic_store_n(&s->ctrl.consumer_head, head,
                         __ATOMIC_RELEASE);
    }

    for (uint32_t p = 0; p < TEST_PRODUCERS; p++)
        pthread_join(producers[p], NULL);
    assert(s->ctrl.consumer_head == total);
    free(s);
}

static int
reverse_entry_ready(const struct dmesh_rev_ring_entry *entries,
                    uint32_t size, uint64_t ticket)
{
    const struct dmesh_rev_ring_entry *entry = &entries[ticket % size];
    return __atomic_load_n(&entry->publish_seq, __ATOMIC_ACQUIRE) ==
           ticket + 1u;
}

static void
test_reverse_generation_wrap(void)
{
    enum { SIZE = 8 };
    struct dmesh_rev_ring_entry entries[SIZE] = { 0 };

    for (uint64_t ticket = 0; ticket < SIZE; ticket++) {
        entries[ticket].kind = DMESH_REV_ENTRY_TX_ACK;
        entries[ticket].payload.ack.seq = (uint16_t)ticket;
        __atomic_store_n(&entries[ticket].publish_seq, ticket + 1u,
                         __ATOMIC_RELEASE);
        assert(reverse_entry_ready(entries, SIZE, ticket));
    }

    assert(!reverse_entry_ready(entries, SIZE, SIZE));
    entries[0].payload.ack.seq = SIZE;
    __atomic_store_n(&entries[0].publish_seq, SIZE + 1u,
                     __ATOMIC_RELEASE);
    assert(reverse_entry_ready(entries, SIZE, SIZE));
    assert(entries[0].payload.ack.seq == SIZE);
}

static void
test_dpa_producer_completion_reports(void)
{
    uint32_t deferred = 0;
    uint32_t reports = 0;

    for (uint32_t i = 1; i <= 4 * DPA_PRODUCER_REPORT_BATCH; i++) {
        int report = dpa_producer_report_due(&deferred);
        assert(report == (i % DPA_PRODUCER_REPORT_BATCH == 0));
        reports += (uint32_t)report;
        assert(deferred < DPA_PRODUCER_REPORT_BATCH);
    }

    assert(reports == 4);
    assert(deferred == 0);
}

static void
test_dpa_reschedule_quantum(void)
{
    uint32_t completed = 0;

    assert(!dpa_reschedule_due(&completed,
                               DPA_RESCHEDULE_DMA_QUANTUM - 1, 0));
    assert(completed == DPA_RESCHEDULE_DMA_QUANTUM - 1);
    assert(dpa_reschedule_due(&completed, 1, 0));
    assert(completed == 0);

    assert(!dpa_reschedule_due(&completed,
                               DPA_RESCHEDULE_DMA_QUANTUM, 1));
    assert(completed == DPA_RESCHEDULE_DMA_QUANTUM);
    assert(dpa_reschedule_budget(completed, 1) ==
           DPA_PRODUCER_REPORT_BATCH - 1);
    assert(dpa_reschedule_due(&completed,
                              DPA_PRODUCER_REPORT_BATCH - 1, 0));
    assert(completed == 0);
}

int
main(void)
{
    test_out_of_order_prefix();
    test_refused_claim_leaves_no_gap();
    test_concurrent_wrap();
    test_reverse_generation_wrap();
    test_dpa_producer_completion_reports();
    test_dpa_reschedule_quantum();
    puts("ring_counter_test: PASS");
    return 0;
}
