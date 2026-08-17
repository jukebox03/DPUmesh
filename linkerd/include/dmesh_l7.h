#ifndef DMESH_L7_H
#define DMESH_L7_H

/* DPUmesh and embedded L7 adapter ABI. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMESH_L7_MODE_DECISION 1
#define DMESH_L7_MODE_OPAQUE   2
#define DMESH_L7_MODE_FULL     3

#define DMESH_L7_DECLINE_ERROR          (-1)
#define DMESH_L7_DECLINE_NOT_ATTACHED   (-2)
#define DMESH_L7_DECLINE_MODE           (-3)
#define DMESH_L7_DECLINE_SESSION_LIMIT  (-4)
#define DMESH_L7_DECLINE_UNKNOWN_REPLY  (-5)

/* Addresses are in host byte order. `workload` is NUL-terminated. */
struct dmesh_l7_flow {
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port;
    int32_t  src_pod;
    int32_t  dst_service;
    int32_t  peer_pod;
    uint8_t  mode;
    uint8_t  is_reply;
    /* Max namespace (63) + max Pod DNS subdomain (253) plus JSON framing. */
    char     workload[384];
};

static inline uint64_t dmesh_l7_conn_handle(int32_t pod, uint16_t port)
{
    return ((uint64_t)(uint8_t)pod << 16) | (uint64_t)port;
}

static inline int32_t dmesh_l7_handle_pod(uint64_t conn)
{
    return (int32_t)(int8_t)(conn >> 16);
}

static inline uint16_t dmesh_l7_handle_port(uint64_t conn)
{
    return (uint16_t)conn;
}

#define DMESH_L7_BACKEND_ANY (-1)
#define DMESH_L7_ORIGIN      (-2)

/* L7 adapter entry points. */
int l7_worker_run(int worker_id, void *driver);

#ifndef DMESH_L7_RUNTIME_OWNER
int  l7_worker_attach(int worker_id);
int  l7_worker_step(int worker_id);
void l7_worker_detach(int worker_id);
#endif

int  l7_conn_open(int worker_id, uint64_t conn,
                  const struct dmesh_l7_flow *flow);
int  l7_conn_segment(int worker_id, uint64_t conn,
                     const uint8_t *base, uint32_t pos, uint32_t len);
/* EOF closes one input half; close drops its complete paired session. */
void l7_conn_eof(int worker_id, uint64_t conn);
void l7_conn_close(int worker_id, uint64_t conn);

struct dmesh_l7_verdict {
    int allow;
    int32_t backend_pod;
};

int  l7_resolve(int worker_id, const struct dmesh_l7_flow *flow,
                struct dmesh_l7_verdict *out);
void l7_report(int worker_id, uint64_t conn, uint64_t bytes_in,
               uint64_t bytes_out, uint64_t duration_ns, int reason);

/* DPUmesh data-path entry points. */
int dmesh_l7_backends(int worker_id, int32_t service, int32_t *out, int max);
int dmesh_l7_send(int worker_id, uint64_t conn, int32_t backend_pod,
                  const uint8_t *buf, size_t len);
uint8_t *dmesh_l7_tx_reserve(int worker_id, uint64_t conn, uint32_t *cap);
int dmesh_l7_tx_commit(int worker_id, uint64_t conn, int32_t backend_pod,
                       uint32_t len);
void dmesh_l7_release(int worker_id, uint64_t conn, uint32_t pos, uint32_t len);

/* Persistent runtime backend implemented by DPUmesh. */
int dmesh_l7_driver_notification_fds(void *driver, int *completion_fd,
                                     int *dma_fd, int *wake_fd);
int dmesh_l7_driver_arm(void *driver);
int dmesh_l7_driver_drain(void *driver, int budget);
int dmesh_l7_driver_clear_notifications(void *driver);
int dmesh_l7_driver_maintenance(void *driver);
int dmesh_l7_driver_stopped(void *driver);
void dmesh_l7_driver_ready(void *driver);
void dmesh_l7_driver_failed(void *driver);

#ifdef __cplusplus
}
#endif

#endif /* DMESH_L7_H */
