#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../doca/dpa.h"

/* These declarations are DPU-architecture-gated in dpa.h. The host white-box
 * build discards their callers with section GC, but the compiler still parses
 * the full worker source before link-time collection. */
doca_error_t init_dpa_objects(struct objects *objs);
doca_error_t dmesh_doca_dpa_thread_create(
    struct dmesh_doca_dpa_thread *dpa_thread, int eu_id);
doca_error_t dmesh_doca_dpa_msgq_send_try(
    struct dmesh_doca_dpa_msgq *msgq, void *msg, uint32_t msg_size);
doca_error_t setup_pod_dma(struct objects *objs, struct pod_state *pod);
int progress_setup_pod_dma(struct objects *objs, struct pod_state *pod);

/* White-box coverage for the production registry scan and service RR cursor.
 * Function-section GC discards the hardware worker paths from this source. */
#include "../doca/dpu_worker.c"

#define LB_SERVICE 11
#define LB_THREADS 4
#define LB_PICKS_PER_THREAD 10000

struct lb_arg {
    struct objects *objs;
    atomic_uint *counts;
};

static void *
lb_thread(void *opaque)
{
    struct lb_arg *arg = (struct lb_arg *)opaque;
    for (int i = 0; i < LB_PICKS_PER_THREAD; i++) {
        int32_t pod = dpu_route_l4(arg->objs, LB_SERVICE);
        assert(pod >= 0 && pod < POD_ID_SPACE);
        atomic_fetch_add_explicit(&arg->counts[pod], 1,
                                  memory_order_relaxed);
    }
    return NULL;
}

int
main(void)
{
    struct objects *objs = calloc(1, sizeof(*objs));
    assert(objs != NULL);
    atomic_uint counts[POD_ID_SPACE];
    for (int i = 0; i < POD_ID_SPACE; i++)
        atomic_init(&counts[i], 0);

    const int32_t ids[] = { 7, 9, 12 };
    objs->num_pods = 3;
    for (int i = 0; i < 3; i++) {
        objs->pods[i].pod_id = ids[i];
        objs->pods[i].service_id = LB_SERVICE;
        __atomic_store_n(&objs->pods[i].dma_ready, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&objs->pods[i].registered, 1, __ATOMIC_RELEASE);
    }

    /* Stable registry: exact RR order. */
    for (int i = 0; i < 12; i++)
        assert(dpu_route_l4(objs, LB_SERVICE) == ids[i % 3]);

    /* A backend that remains registered but loses its DMA gate is never chosen. */
    __atomic_store_n(&objs->pods[1].dma_ready, 0, __ATOMIC_RELEASE);
    for (int i = 0; i < 100; i++) {
        int32_t pod = dpu_route_l4(objs, LB_SERVICE);
        assert(pod == ids[0] || pod == ids[2]);
    }
    __atomic_store_n(&objs->pods[1].dma_ready, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&objs->svc_rr[LB_SERVICE], 0, __ATOMIC_RELAXED);

    /* Concurrent connection owners share only the atomic cursor. The backend
     * registry is stable here, so all picks are lossless and evenly distributed. */
    pthread_t tids[LB_THREADS];
    struct lb_arg arg = { .objs = objs, .counts = counts };
    for (int i = 0; i < LB_THREADS; i++)
        assert(pthread_create(&tids[i], NULL, lb_thread, &arg) == 0);
    for (int i = 0; i < LB_THREADS; i++)
        pthread_join(tids[i], NULL);

    unsigned total = 0, min = UINT32_MAX, max = 0;
    for (int i = 0; i < 3; i++) {
        unsigned n = atomic_load_explicit(&counts[ids[i]],
                                          memory_order_relaxed);
        total += n;
        if (n < min) min = n;
        if (n > max) max = n;
    }
    assert(total == LB_THREADS * LB_PICKS_PER_THREAD);
    assert(max - min <= 1);
    assert(dpu_route_l4(objs, 99) == -1);

    free(objs);
    puts("lb_policy_test: PASS");
    return 0;
}
