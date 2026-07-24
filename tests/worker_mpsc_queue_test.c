#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../doca/object.h"

#define PRODUCERS 4
#define PER_PRODUCER 20000

struct producer_arg {
    dpu_mpsc_comp_queue_t *queue;
    int id;
    atomic_int *finished;
};

static void *
producer_main(void *opaque)
{
    struct producer_arg *arg = opaque;
    for (int i = 0; i < PER_PRODUCER; i++) {
        dpu_comp_entry_t entry = {
            .src_pod_id = arg->id,
            .seq = (uint16_t)(i + 1),
            .length = (uint32_t)i,
        };
        while (mpsc_comp_queue_enqueue(arg->queue, &entry) != 0)
            sched_yield();
    }
    atomic_fetch_add_explicit(arg->finished, 1, memory_order_release);
    return NULL;
}

int
main(void)
{
    /* Upstream ports carry the same raw-port owner that host ring selection
     * uses. LB backend choice does not change that connection owner. */
    struct dpu_conntrack *ct = calloc(1, sizeof(*ct));
    assert(ct != NULL);
    ct->next_uport = DMESH_UPORT_BASE;
    for (uint16_t owner = 0; owner < 4; owner++) {
        uint16_t up = dpu_upstream_create(ct, 3, (uint16_t)(101 + owner),
                                          7, 0, owner, 4);
        assert(up >= DMESH_UPORT_BASE);
        assert(up % 4 == owner);
        assert(ct->upstream[up].backend_pod == 7);
    }
    free(ct);

    dpu_mpsc_comp_queue_t *queue = malloc(sizeof(*queue));
    assert(queue != NULL);
    mpsc_comp_queue_init(queue);

    /* Full queue and wraparound. */
    for (int i = 0; i < DPU_COMP_QUEUE_SIZE; i++) {
        dpu_comp_entry_t entry = { .length = (uint32_t)i };
        assert(mpsc_comp_queue_enqueue(queue, &entry) == 0);
    }
    dpu_comp_entry_t extra = { 0 };
    assert(mpsc_comp_queue_enqueue(queue, &extra) == -1);
    for (int i = 0; i < DPU_COMP_QUEUE_SIZE; i++) {
        dpu_comp_entry_t *entry = mpsc_comp_queue_peek(queue);
        assert(entry != NULL && entry->length == (uint32_t)i);
        mpsc_comp_queue_dequeue(queue);
    }
    assert(mpsc_comp_queue_empty(queue));

    pthread_t threads[PRODUCERS];
    struct producer_arg args[PRODUCERS];
    atomic_int finished;
    atomic_init(&finished, 0);
    for (int p = 0; p < PRODUCERS; p++) {
        args[p] = (struct producer_arg){
            .queue = queue,
            .id = p,
            .finished = &finished,
        };
        assert(pthread_create(&threads[p], NULL, producer_main, &args[p]) == 0);
    }

    uint32_t last[PRODUCERS] = { 0 };
    int consumed = 0;
    while (consumed < PRODUCERS * PER_PRODUCER) {
        dpu_comp_entry_t *entry = mpsc_comp_queue_peek(queue);
        if (!entry) {
            sched_yield();
            continue;
        }
        int producer = entry->src_pod_id;
        assert(producer >= 0 && producer < PRODUCERS);
        assert(entry->seq == (uint16_t)(last[producer] + 1));
        assert(entry->length == last[producer]);
        last[producer]++;
        consumed++;
        mpsc_comp_queue_dequeue(queue);
    }

    for (int p = 0; p < PRODUCERS; p++) {
        pthread_join(threads[p], NULL);
        assert(last[p] == PER_PRODUCER);
    }
    assert(atomic_load_explicit(&finished, memory_order_acquire) == PRODUCERS);
    assert(mpsc_comp_queue_empty(queue));
    free(queue);
    puts("worker_mpsc_queue_test: PASS");
    return 0;
}
