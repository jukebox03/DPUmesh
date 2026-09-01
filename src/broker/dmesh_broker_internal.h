#ifndef DMESH_BROKER_INTERNAL_H
#define DMESH_BROKER_INTERNAL_H

#include <signal.h>

int dmesh_broker_run(int socket_fd, const char *agent_socket,
                     volatile sig_atomic_t *stop_requested);

#endif /* DMESH_BROKER_INTERNAL_H */
