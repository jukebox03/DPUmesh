#ifndef DMESH_BROKERLINK_H
#define DMESH_BROKERLINK_H

#include <stddef.h>

#include <dpumesh/dmesh_common.h>
#include "dmesh_ipc.h"

/* K forward-ring memfds, L original reverse-ring memfds, TX/RX memfds, and
 * the pod-global broker->workload doorbell eventfd. */
#define DMESH_BROKER_MAX_FDS  (2 * MAX_EU_PER_POD + 3)

int dmesh_broker_connect(const char *path, const char *service,
                         char *error, size_t error_len);
int dmesh_broker_recv_ready(int socket_fd, struct dmesh_ipc_ready *ready,
                            int *fds, size_t fds_cap,
                            char *error, size_t error_len);
int dmesh_broker_send_ready(int socket_fd,
                            const struct dmesh_ipc_ready *ready,
                            const int *fds, size_t fd_count);
int dmesh_broker_recv_hello(int socket_fd, struct dmesh_ipc_hello *hello);
int dmesh_broker_send_error(int socket_fd, int code, const char *message);

#endif /* DMESH_BROKERLINK_H */
