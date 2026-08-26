/* Minimal native DPUmesh client. The hello-dpumesh Service replies
 * with the same byte stream, so this shows the complete application contract:
 * channel -> EQ -> service QP -> zero-copy receive -> ordered teardown. */
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
#define REPLY_MAX   4096

static int wait_eq(int fd)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int rc;
    do {
        rc = poll(&pfd, 1, 5000);
    } while (rc < 0 && errno == EINTR);
    if (rc <= 0)
        return -1;
    uint64_t count;
    while (read(fd, &count, sizeof(count)) == sizeof(count)) { }
    return 0;
}

static int drain(dmesh_channel_t *channel, dmesh_eq_t *eq, dmesh_qp_t *qp,
                 char *reply, size_t *reply_len)
{
    dmesh_event_t events[EVENT_BATCH];
    int n;
    while ((n = dmesh_poll_eq(eq, events, EVENT_BATCH)) > 0) {
        for (int i = 0; i < n; i++) {
            dmesh_event_t *event = &events[i];
            if (event->type == DMESH_EVENT_RECV) {
                if (event->qp != qp || event->len > REPLY_MAX - *reply_len) {
                    dmesh_release_rx_buffer(channel, event);
                    return -1;
                }
                memcpy(reply + *reply_len, event->buf, event->len);
                *reply_len += event->len;
                dmesh_release_rx_buffer(channel, event);
            } else if (event->qp == qp &&
                       (event->type == DMESH_EVENT_RECV_FIN ||
                        event->type == DMESH_EVENT_TX_ERROR)) {
                return -1;
            }
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *service = argc > 1 ? argv[1] : "echo-dpumesh";
    const char *message = argc > 2 ? argv[2] : "hello from DPUmesh\n";
    size_t message_len = strlen(message), reply_len = 0;
    char reply[REPLY_MAX];
    int rc = 1;

    if (getenv("DPUMESH_SERVICE") || message_len == 0 || message_len > REPLY_MAX) {
        fprintf(stderr, "usage: env -u DPUMESH_SERVICE %s [service] [message]\n", argv[0]);
        return 2;
    }

    dmesh_channel_t *channel = dmesh_create_channel();
    dmesh_eq_t *eq = channel ? dmesh_create_eq(channel) : NULL;
    dmesh_qp_t *qp = eq ? dmesh_create_qp(eq, service) : NULL;
    int eq_fd = eq ? dmesh_eq_fd(eq) : -1;
    if (!channel || !eq || !qp || eq_fd < 0) {
        perror("DPUmesh setup");
        goto out;
    }

    void *tx;
    while ((tx = dmesh_alloc(qp, (uint32_t)message_len)) == NULL) {
        if (errno != EAGAIN || wait_eq(eq_fd) != 0 ||
            drain(channel, eq, qp, reply, &reply_len) < 0) {
            perror("dmesh_alloc");
            goto out;
        }
    }
    memcpy(tx, message, message_len);
    if (dmesh_post_send(qp, tx, (uint32_t)message_len) != 0 ||
        dmesh_flush(qp) != 0) {
        perror("DPUmesh send");
        goto out;
    }

    while (reply_len < message_len) {
        if (wait_eq(eq_fd) != 0 || drain(channel, eq, qp, reply, &reply_len) < 0) {
            fprintf(stderr, "DPUmesh reply timed out or failed\n");
            goto out;
        }
    }
    if (reply_len != message_len || memcmp(reply, message, message_len) != 0) {
        fprintf(stderr, "echo reply did not match the request\n");
        goto out;
    }
    fwrite(reply, 1, reply_len, stdout);
    rc = 0;

out:
    dmesh_destroy_qp(qp);
    dmesh_destroy_eq(eq);
    dmesh_destroy_channel(channel);
    return rc;
}
