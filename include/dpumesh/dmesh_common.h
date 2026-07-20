#ifndef DMESH_COMMON_H
#define DMESH_COMMON_H

/* DPA capacity. Ring j of pod p maps to EU (p*K + j) % N. */
#define MAX_DPA_EU          8   /* max DPA EU threads: array cap (dpa_threads[]) + the N clamp */
#define MAX_DPA_RINGS       8   /* per-EU forward-ring capacity (rings[] per EU) */
#define DPA_THREADS_DEFAULT 4   /* default N; env DPUMESH_DPA_THREADS overrides */
#define DPUMESH_RINGS_PER_POD_DEFAULT 2   /* default K; env DPUMESH_RINGS_PER_POD */
#define MAX_EU_PER_POD      MAX_DPA_RINGS  /* per-pod ring array (a pod spans <= K <= this EUs) */

/* pods[] capacity; the runtime cap is MAX_DPA_RINGS * N / K. */
#define MAX_PODS   (MAX_DPA_RINGS * MAX_DPA_EU / DPUMESH_RINGS_PER_POD_DEFAULT)  /* 8*8/2 = 32 */

/* Complete nonnegative int8 pod-id space. */
#define POD_ID_SPACE        128

/* Per-pod DPU staging mirrors host TX byte offsets. */
#define DPU_BUFFER_SIZE     (32 * 1024 * 1024)  /* 32MB = 4096 × 8KB */
#define DPUMESH_SLOT_SIZE   8192               /* matches DPUMESH_SLOT_SIZE_DEFAULT */
/* Forward descriptor count; the credit counter occupies the next slot. */
#define DMA_RING_SIZE       4096

/* Endpoint tuples carry separate service-id and pod-id fields. Clients use a
 * blank destination pod for DPU resolution; replies name a concrete pod. */
#define DMESH_POD_BLANK     (-1)   /* dst_pod == -1 -> DPU must resolve dst_service */
#define DMESH_PORT_BLANK     0     /* dst_port == 0 -> service listener / accept queue */
#define DMESH_SVC_NONE      (-1)   /* no service id */

/* DPU upstream ids occupy the upper half of the port space. */
#define DMESH_UPORT_BASE    32768

/* Connection roles. FREE and SERVER_PENDING are internal slot states. */
#define DMESH_ROLE_FREE          0
#define DMESH_ROLE_CLIENT        1
#define DMESH_ROLE_SERVER        2
/* Unaccepted server connection with an active inbox. */
#define DMESH_ROLE_SERVER_PENDING 3

#endif /* DMESH_COMMON_H */
