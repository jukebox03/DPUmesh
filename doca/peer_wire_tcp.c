/* The TCP carrier behind `peer_wire_ops`.
 *
 * Messages are length-prefixed with four big-endian bytes, which is the whole
 * of the framing: TCP gives an ordered byte stream and this restores the
 * message boundaries the layer above was written against. Nothing here is on
 * the fast path — the RDMA carrier is — so it is written for clarity, copying
 * each message once on the way out and once on the way in. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "peer_wire.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

/* One per peer node per worker, plus the inbound connections still proving
 * who they are. Far above the 256-channel table it feeds. */
#define TCP_CONN_MAX  512u
#define TCP_HDR       4u
#define TCP_BUF       (TCP_HDR + PEER_WIRE_MSG_MAX)
#define TCP_EVENTS    64

enum { TCP_CONNECTING = 0, TCP_READY, TCP_DEAD };

struct tcp_conn {
    struct tcp_ctx *ctx;
    int      fd;
    int      state;
    uint8_t  in_use;
    uint32_t armed;                  /* the epoll mask currently registered */
    uint8_t *rx;                     /* TCP_BUF: the stream, reassembling */
    uint32_t rx_len;
    uint8_t *tx;                     /* TCP_BUF: at most one message in flight */
    uint32_t tx_len;
    uint32_t tx_pos;
};

struct tcp_ctx {
    int      epfd;
    int      listen_fd;
    uint16_t port;
    struct tcp_conn conns[TCP_CONN_MAX];
};

/* ---- slots ------------------------------------------------------------- */

static struct tcp_conn *tcp_slot(struct tcp_ctx *ctx)
{
    for (uint32_t i = 0; i < TCP_CONN_MAX; i++) {
        struct tcp_conn *c = &ctx->conns[i];
        if (c->in_use)
            continue;
        uint8_t *buffers = calloc(2, TCP_BUF);
        if (!buffers)
            return NULL;
        memset(c, 0, sizeof(*c));
        c->ctx = ctx;
        c->fd = -1;
        c->rx = buffers;
        c->tx = buffers + TCP_BUF;
        c->in_use = 1;
        return c;
    }
    return NULL;
}

static void tcp_release(struct tcp_conn *c)
{
    if (!c->in_use)
        return;
    if (c->fd >= 0) {
        epoll_ctl(c->ctx->epfd, EPOLL_CTL_DEL, c->fd, NULL);
        close(c->fd);
    }
    free(c->rx);                     /* the single allocation both halves share */
    memset(c, 0, sizeof(*c));
}

/* Only what the connection is waiting for is armed. EPOLLOUT stays off unless
 * a connect is outstanding or a message is stuck, because a writable socket is
 * writable almost always and an event loop woken by that never sleeps. */
static int tcp_arm(struct tcp_conn *c)
{
    if (c->state == TCP_DEAD || c->fd < 0)
        return -1;
    uint32_t want = EPOLLIN | EPOLLRDHUP;
    if (c->state == TCP_CONNECTING || c->tx_pos < c->tx_len)
        want |= EPOLLOUT;
    if (want == c->armed)
        return 0;
    struct epoll_event ev = { .events = want, .data = { .ptr = c } };
    if (epoll_ctl(c->ctx->epfd, EPOLL_CTL_MOD, c->fd, &ev) != 0) {
        c->state = TCP_DEAD;
        return -1;
    }
    c->armed = want;
    return 0;
}

static int tcp_register(struct tcp_conn *c)
{
    uint32_t want = EPOLLIN | EPOLLRDHUP;
    if (c->state == TCP_CONNECTING)
        want |= EPOLLOUT;
    struct epoll_event ev = { .events = want, .data = { .ptr = c } };
    if (epoll_ctl(c->ctx->epfd, EPOLL_CTL_ADD, c->fd, &ev) != 0)
        return -1;
    c->armed = want;
    return 0;
}

/* ---- transfer ---------------------------------------------------------- */

/* Returns 1 when bytes left the socket, 0 when none could, -1 on fault. */
static int tcp_flush(struct tcp_conn *c)
{
    int moved = 0;
    while (c->tx_pos < c->tx_len) {
        ssize_t n = send(c->fd, c->tx + c->tx_pos, c->tx_len - c->tx_pos,
                         MSG_NOSIGNAL);
        if (n > 0) {
            c->tx_pos += (uint32_t)n;
            moved = 1;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        c->state = TCP_DEAD;
        return -1;
    }
    if (c->tx_pos == c->tx_len)
        c->tx_pos = c->tx_len = 0;
    return moved;
}

static int tcp_send_msg(void *wc, const void *buf, size_t len)
{
    struct tcp_conn *c = wc;
    if (!c || !c->in_use || c->state == TCP_DEAD)
        return -1;
    if (len == 0 || len > PEER_WIRE_MSG_MAX)
        return -1;
    if (c->state != TCP_READY)
        return 0;
    if (c->tx_pos < c->tx_len) {
        if (tcp_flush(c) < 0)
            return -1;
        if (c->tx_pos < c->tx_len) {
            if (tcp_arm(c) != 0)
                return -1;
            return 0;                /* the one message in flight still is */
        }
    }
    c->tx[0] = (uint8_t)(len >> 24);
    c->tx[1] = (uint8_t)(len >> 16);
    c->tx[2] = (uint8_t)(len >> 8);
    c->tx[3] = (uint8_t)len;
    memcpy(c->tx + TCP_HDR, buf, len);
    c->tx_len = TCP_HDR + (uint32_t)len;
    c->tx_pos = 0;
    if (tcp_flush(c) < 0)
        return -1;
    if (tcp_arm(c) != 0)
        return -1;
    return 1;
}

static long tcp_recv_msg(void *wc, void *buf, size_t cap)
{
    struct tcp_conn *c = wc;
    if (!c || !c->in_use || c->state == TCP_DEAD)
        return -1;
    if (c->state != TCP_READY)
        return 0;
    for (;;) {
        if (c->rx_len >= TCP_HDR) {
            uint32_t len = ((uint32_t)c->rx[0] << 24) | ((uint32_t)c->rx[1] << 16) |
                           ((uint32_t)c->rx[2] << 8) | (uint32_t)c->rx[3];
            if (len == 0 || len > PEER_WIRE_MSG_MAX) {
                c->state = TCP_DEAD;
                return -1;
            }
            if (c->rx_len >= TCP_HDR + len) {
                if (len > cap) {
                    c->state = TCP_DEAD;
                    return -1;
                }
                memcpy(buf, c->rx + TCP_HDR, len);
                uint32_t used = TCP_HDR + len;
                c->rx_len -= used;
                if (c->rx_len)
                    memmove(c->rx, c->rx + used, c->rx_len);
                return (long)len;
            }
        }
        ssize_t n = recv(c->fd, c->rx + c->rx_len, TCP_BUF - c->rx_len, 0);
        if (n > 0) {
            c->rx_len += (uint32_t)n;
            continue;
        }
        if (n == 0) {
            c->state = TCP_DEAD;     /* a peer that closed mid-session faulted */
            return -1;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        c->state = TCP_DEAD;
        return -1;
    }
}

/* ---- lifecycle --------------------------------------------------------- */

static int tcp_tune(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return 0;
}

static int tcp_connect(void *wctx, uint32_t ip_be, uint16_t port, void **wc)
{
    struct tcp_ctx *ctx = wctx;
    if (!wc)
        return -1;
    *wc = NULL;
    struct tcp_conn *c = tcp_slot(ctx);
    if (!c)
        return -1;
    c->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (c->fd < 0) {
        tcp_release(c);
        return -1;
    }
    tcp_tune(c->fd);
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ip_be;
    addr.sin_port = htons(port);
    c->state = TCP_CONNECTING;
    if (connect(c->fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        c->state = TCP_READY;
    else if (errno != EINPROGRESS && errno != EINTR) {
        tcp_release(c);
        return -1;
    }
    if (tcp_register(c) != 0) {
        tcp_release(c);
        return -1;
    }
    *wc = c;
    return 0;
}

static int tcp_progress(void *wctx, void **accepted, int max, int *n_accepted)
{
    struct tcp_ctx *ctx = wctx;
    struct epoll_event evs[TCP_EVENTS];
    int progressed = 0;
    if (n_accepted)
        *n_accepted = 0;
    int n = epoll_wait(ctx->epfd, evs, TCP_EVENTS, 0);
    for (int i = 0; i < n; i++) {
        if (evs[i].data.ptr == ctx) {
            while (accepted && n_accepted && *n_accepted < max) {
                int fd = accept4(ctx->listen_fd, NULL, NULL,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd < 0)
                    break;
                struct tcp_conn *c = tcp_slot(ctx);
                if (!c) {
                    close(fd);       /* at the pool bound; the peer retries */
                    break;
                }
                c->fd = fd;
                c->state = TCP_READY;
                tcp_tune(fd);
                if (tcp_register(c) != 0) {
                    tcp_release(c);
                    continue;
                }
                accepted[(*n_accepted)++] = c;
                progressed = 1;
            }
            continue;
        }
        struct tcp_conn *c = evs[i].data.ptr;
        if (!c->in_use || c->state == TCP_DEAD)
            continue;
        if (evs[i].events & (EPOLLERR | EPOLLHUP)) {
            c->state = TCP_DEAD;
            progressed = 1;
            continue;
        }
        if (c->state == TCP_CONNECTING && (evs[i].events & EPOLLOUT)) {
            int err = 0;
            socklen_t elen = sizeof(err);
            if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err)
                c->state = TCP_DEAD;
            else
                c->state = TCP_READY;
            progressed = 1;
        }
        if (c->state == TCP_READY && c->tx_pos < c->tx_len && tcp_flush(c) > 0)
            progressed = 1;
        (void)tcp_arm(c);
    }
    return progressed;
}

static int tcp_established(void *wc)
{
    struct tcp_conn *c = wc;
    return c && c->in_use && c->state == TCP_READY;
}

static int tcp_faulted(void *wc)
{
    struct tcp_conn *c = wc;
    return !c || !c->in_use || c->state == TCP_DEAD;
}

static void tcp_close(void *wc)
{
    struct tcp_conn *c = wc;
    if (c)
        tcp_release(c);
}

static int tcp_epfd(void *wctx)
{
    struct tcp_ctx *ctx = wctx;
    return ctx ? ctx->epfd : -1;
}

static void tcp_ctx_free(void *wctx)
{
    struct tcp_ctx *ctx = wctx;
    if (!ctx)
        return;
    for (uint32_t i = 0; i < TCP_CONN_MAX; i++)
        tcp_release(&ctx->conns[i]);
    if (ctx->listen_fd >= 0)
        close(ctx->listen_fd);
    if (ctx->epfd >= 0)
        close(ctx->epfd);
    free(ctx);
}

static const struct peer_wire_ops TCP_OPS = {
    .connect     = tcp_connect,
    .progress    = tcp_progress,
    .send_msg    = tcp_send_msg,
    .recv_msg    = tcp_recv_msg,
    .established = tcp_established,
    .faulted     = tcp_faulted,
    .close       = tcp_close,
    .epfd        = tcp_epfd,
    .ctx_free    = tcp_ctx_free,
};

int peer_wire_tcp_new(uint32_t bind_ip_be, uint16_t port,
                      const struct peer_wire_ops **ops, void **wctx,
                      char *error, size_t error_len)
{
    if (error && error_len)
        error[0] = '\0';
    if (!ops || !wctx)
        return -1;
    *ops = NULL;
    *wctx = NULL;
    struct tcp_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        snprintf(error, error_len, "peer wire: out of memory");
        return -1;
    }
    ctx->epfd = -1;
    ctx->listen_fd = -1;
    ctx->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epfd < 0) {
        snprintf(error, error_len, "peer wire: epoll_create1: %s", strerror(errno));
        goto fail;
    }
    ctx->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (ctx->listen_fd < 0) {
        snprintf(error, error_len, "peer wire: socket: %s", strerror(errno));
        goto fail;
    }
    int one = 1;
    setsockopt(ctx->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = bind_ip_be;
    addr.sin_port = htons(port);
    if (bind(ctx->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(error, error_len, "peer wire: bind port %u: %s", port,
                 strerror(errno));
        goto fail;
    }
    if (listen(ctx->listen_fd, 64) != 0) {
        snprintf(error, error_len, "peer wire: listen: %s", strerror(errno));
        goto fail;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(ctx->listen_fd, (struct sockaddr *)&addr, &alen) == 0)
        ctx->port = ntohs(addr.sin_port);
    struct epoll_event ev = { .events = EPOLLIN, .data = { .ptr = ctx } };
    if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->listen_fd, &ev) != 0) {
        snprintf(error, error_len, "peer wire: epoll_ctl listener: %s",
                 strerror(errno));
        goto fail;
    }
    *ops = &TCP_OPS;
    *wctx = ctx;
    return 0;
fail:
    tcp_ctx_free(ctx);
    return -1;
}

uint16_t peer_wire_tcp_port(void *wctx)
{
    struct tcp_ctx *ctx = wctx;
    return ctx ? ctx->port : 0;
}
