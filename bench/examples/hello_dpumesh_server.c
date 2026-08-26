/* Minimal native DPUmesh byte-stream echo server. For clarity it admits one
 * connection at a time; apps/echo_dpumesh.c shows the multi-connection form. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dpumesh/dmesh.h>

#define EVENT_BATCH 16
#define PENDING_MAX 4096

static int wait_eq(int fd)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int rc;
    do {
        rc = poll(&pfd, 1, -1);
    } while (rc < 0 && errno == EINTR);
    if (rc <= 0)
        return -1;
    uint64_t count;
    while (read(fd, &count, sizeof(count)) == sizeof(count)) { }
    return 0;
}

static void close_later(dmesh_qp_t **list, int *count, dmesh_qp_t *qp)
{
    for (int i = 0; i < *count; i++)
        if (list[i] == qp)
            return;
    list[(*count)++] = qp;
}

int main(void)
{
    if (!getenv("DPUMESH_SERVICE")) {
        fprintf(stderr, "DPUMESH_SERVICE must name this server's Kubernetes Service\n");
        return 2;
    }
    dmesh_channel_t *channel = dmesh_create_channel();
    dmesh_eq_t *eq = channel ? dmesh_create_eq(channel) : NULL;
    int eq_fd = eq ? dmesh_eq_fd(eq) : -1;
    if (!channel || !eq || eq_fd < 0) {
        perror("DPUmesh setup");
        return 1;
    }

    dmesh_qp_t *active = NULL;
    uint8_t pending[PENDING_MAX];
    size_t pending_len = 0;
    fprintf(stderr, "hello server ready as %s\n", getenv("DPUMESH_SERVICE"));

    for (;;) {
        if (wait_eq(eq_fd) != 0)
            break;
        dmesh_event_t events[EVENT_BATCH];
        int n;
        while ((n = dmesh_poll_eq(eq, events, EVENT_BATCH)) > 0) {
            dmesh_qp_t *close_after_batch[EVENT_BATCH];
            int close_count = 0;
            for (int i = 0; i < n; i++) {
                dmesh_event_t *event = &events[i];
                if (event->type == DMESH_EVENT_CONN_REQ) {
                    if (!active)
                        active = event->qp;
                    else
                        close_later(close_after_batch, &close_count, event->qp);
                } else if (event->type == DMESH_EVENT_RECV) {
                    if (event->qp != active ||
                        event->len > PENDING_MAX - pending_len) {
                        if (event->qp == active)
                            close_later(close_after_batch, &close_count, active);
                    } else {
                        memcpy(pending + pending_len, event->buf, event->len);
                        pending_len += event->len;
                    }
                    dmesh_release_rx_buffer(channel, event);
                } else if (event->qp == active &&
                           (event->type == DMESH_EVENT_RECV_FIN ||
                            event->type == DMESH_EVENT_TX_ERROR)) {
                    close_later(close_after_batch, &close_count, active);
                }
            }

            for (int i = 0; i < close_count; i++) {
                dmesh_qp_t *qp = close_after_batch[i];
                if (qp == active) {
                    active = NULL;
                    pending_len = 0;
                }
                dmesh_destroy_qp(qp);
            }
        }
        if (!active || !pending_len)
            continue;

        void *tx = dmesh_alloc(active, (uint32_t)pending_len);
        if (!tx) {
            if (errno == EAGAIN)
                continue;
            dmesh_destroy_qp(active);
            active = NULL;
            pending_len = 0;
            continue;
        }
        memcpy(tx, pending, pending_len);
        if (dmesh_post_send(active, tx, (uint32_t)pending_len) != 0 ||
            dmesh_flush(active) != 0) {
            dmesh_destroy_qp(active);
            active = NULL;
        }
        pending_len = 0;
    }

    dmesh_destroy_qp(active);
    dmesh_destroy_eq(eq);
    dmesh_destroy_channel(channel);
    return 1;
}
