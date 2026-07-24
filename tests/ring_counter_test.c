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

struct producer_arg {
    struct test_state *state;
};

static void *
producer_main(void *opaque)
{
    struct producer_arg *arg = opaque;
    struct dma_ring *ring = &arg->state->ring;

    for (uint32_t i = 0; i < TEST_ITEMS_PER_PRODUCER; i++) {
        uint64_t ticket =
            __atomic_fetch_add(&ring->enq_pos, 1, __ATOMIC_RELAXED);
        while (ticket -
                   __atomic_load_n(&ring->ctrl->consumer_head,
                                   __ATOMIC_ACQUIRE) >=
               ring->size)
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

int
main(void)
{
    test_out_of_order_prefix();
    test_concurrent_wrap();
    puts("ring_counter_test: PASS");
    return 0;
}
