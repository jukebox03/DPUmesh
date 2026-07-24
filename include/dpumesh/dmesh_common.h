#ifndef DMESH_COMMON_H
#define DMESH_COMMON_H

/* DPA capacity. Ring/EU/ARM ownership is defined in dmesh_topology.h. */
#define MAX_DPA_EU          32  /* DPUMESH_DPA_THREADS limit */
#define DPA_THREADS_AUTO_CAP 16 /* automatic selection cap */
#define MAX_DPA_RINGS       8   /* per-EU forward-ring capacity (rings[] per EU) */
#define DPA_THREADS_DEFAULT 8   /* fallback N when the device query is unavailable */
#define DPUMESH_RINGS_PER_POD_DEFAULT 2   /* default K; env DPUMESH_RINGS_PER_POD */
#define MAX_EU_PER_POD      MAX_DPA_RINGS  /* per-pod ring array (a pod spans <= K <= this EUs) */

#if MAX_DPA_EU > 32
#error "MAX_DPA_EU exceeds the uint32_t DPA ADD/DEL ACK masks"
#endif

/* Pod-state table capacity. */
#define MAX_PODS   32

/* Complete nonnegative int8 pod-id space. */
#define POD_ID_SPACE        128

/* Per-pod DPU staging mirrors host TX byte offsets. */
#define DPU_BUFFER_SIZE     (64 * 1024 * 1024)  /* 64MB = 8192 × 8KB */
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
