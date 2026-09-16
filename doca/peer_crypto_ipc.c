#include "peer_crypto_ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <openssl/crypto.h>

/* ---- codec ------------------------------------------------------------- */

struct cursor { uint8_t *p; size_t left; int bad; };
static void put(struct cursor *c, const void *p, size_t n)
{
    if (c->bad || n > c->left) { c->bad = 1; return; }
    memcpy(c->p, p, n); c->p += n; c->left -= n;
}
static void get(struct cursor *c, void *p, size_t n)
{
    if (c->bad || n > c->left) { c->bad = 1; return; }
    memcpy(p, c->p, n); c->p += n; c->left -= n;
}
static void number(struct cursor *c, uint64_t v, unsigned width)
{
    uint8_t b[8];
    for (unsigned i = 0; i < width; i++) b[width - 1 - i] = (uint8_t)(v >> (8 * i));
    put(c, b, width);
}
static uint64_t take(struct cursor *c, unsigned width)
{
    uint8_t b[8] = {0}; uint64_t v = 0;
    get(c, b, width);
    for (unsigned i = 0; i < width; i++) v = (v << 8) | b[i];
    return v;
}
static void header(struct cursor *c, uint16_t type, uint32_t body_len)
{
    put(c, "DMCR", 4); number(c, PEER_CRYPTO_IPC_VERSION, 2); number(c, type, 2);
    number(c, body_len, 4); number(c, 0, 4);
}
int peer_crypto_ipc_header_decode(const void *buf, size_t len, uint16_t *type, uint32_t *body_len)
{
    if (!type || !body_len) return -1;
    *type = 0; *body_len = 0;
    if (!buf && len) return -1;
    if (len < PEER_CRYPTO_IPC_HEADER_LEN) return 1;
    struct cursor c = {(uint8_t *)buf, PEER_CRYPTO_IPC_HEADER_LEN, 0};
    char magic[4]; get(&c, magic, 4);
    unsigned version = (unsigned)take(&c, 2);
    *type = (uint16_t)take(&c, 2); *body_len = (uint32_t)take(&c, 4);
    uint32_t reserved = (uint32_t)take(&c, 4);
    if (c.bad || memcmp(magic, "DMCR", 4) || version != PEER_CRYPTO_IPC_VERSION || reserved ||
        *type < PEER_CRYPTO_IPC_HELLO || *type > PEER_CRYPTO_IPC_COMPLETION ||
        *body_len > PEER_CRYPTO_IPC_BODY_MAX) { *type = 0; *body_len = 0; return -1; }
    return 0;
}
int peer_crypto_ipc_hello_encode(unsigned major, unsigned minor, void *buf, size_t cap, size_t *len)
{
    if (len) *len = 0;
    if (!buf || !len || major > 0xffff || minor > 0xffff) return -1;
    struct cursor c = {buf, cap, 0};
    header(&c, PEER_CRYPTO_IPC_HELLO, 4); number(&c, major, 2); number(&c, minor, 2);
    if (c.bad) return -1;
    *len = cap - c.left; return 0;
}
int peer_crypto_ipc_hello_decode(const void *body, size_t len, unsigned *major, unsigned *minor)
{
    if (!major || !minor) return -1;
    *major = *minor = 0;
    if (!body || len != 4) return -1;
    struct cursor c = {(uint8_t *)body, len, 0};
    *major = (unsigned)take(&c, 2); *minor = (unsigned)take(&c, 2);
    return c.bad ? -1 : 0;
}
static int request_valid(const struct peer_crypto_request *r)
{
    return r && r->action >= PEER_CRYPTO_INSTALL_RX && r->action <= PEER_CRYPTO_REMOVE_ALL &&
        r->token.manager && r->token.generation && r->operation && r->local_endpoint <= 1 &&
        r->lane_count && r->lane_count <= PEER_ASSOC_LANES_MAX && r->binding.epoch;
}
int peer_crypto_ipc_request_encode(const struct peer_crypto_request *r, void *buf, size_t cap, size_t *len)
{
    if (len) *len = 0;
    if (!buf || !len || !request_valid(r)) return -1;
    struct cursor c = {buf, cap, 0};
    size_t body = 8 + 8 + 4 + 8 + 2 + 2 + 8 + 8 + 16 + 16 + 4 + 4 + 20 + 2 + r->lane_count * (8 + 3 + 3 + 16 + 16);
    header(&c, PEER_CRYPTO_IPC_REQUEST, (uint32_t)body);
    number(&c, r->token.manager, 8); number(&c, r->token.generation, 8); number(&c, r->token.slot, 4);
    number(&c, r->operation, 8); number(&c, (unsigned)r->action, 2); number(&c, r->local_endpoint, 2);
    number(&c, r->binding.epoch, 8); number(&c, r->old_epoch, 8);
    put(&c, r->binding.session, 16); put(&c, r->binding.association, 16);
    number(&c, r->binding.spi[0], 4); number(&c, r->binding.spi[1], 4);
    put(&c, r->material, 20); number(&c, r->lane_count, 2);
    for (unsigned i = 0; i < r->lane_count; i++) {
        const struct peer_sec_lane *l = &r->lanes[i];
        number(&c, l->id, 8); number(&c, l->qpn[0], 3); number(&c, l->qpn[1], 3);
        put(&c, l->gid[0], 16); put(&c, l->gid[1], 16);
    }
    if (c.bad) return -1;
    *len = cap - c.left; return 0;
}
int peer_crypto_ipc_request_decode(const void *body, size_t len, struct peer_crypto_request *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!body) return -1;
    struct peer_crypto_request r = {0}; struct cursor c = {(uint8_t *)body, len, 0};
    r.token.manager = take(&c, 8); r.token.generation = take(&c, 8); r.token.slot = (uint32_t)take(&c, 4);
    r.operation = take(&c, 8); r.action = (enum peer_crypto_action)take(&c, 2);
    r.local_endpoint = (unsigned)take(&c, 2); r.binding.epoch = take(&c, 8); r.old_epoch = take(&c, 8);
    get(&c, r.binding.session, 16); get(&c, r.binding.association, 16);
    r.binding.spi[0] = (uint32_t)take(&c, 4); r.binding.spi[1] = (uint32_t)take(&c, 4);
    get(&c, r.material, 20); r.lane_count = (unsigned)take(&c, 2);
    if (c.bad || r.lane_count > PEER_ASSOC_LANES_MAX) { OPENSSL_cleanse(&r, sizeof(r)); return -1; }
    for (unsigned i = 0; i < r.lane_count; i++) {
        struct peer_sec_lane *l = &r.lanes[i];
        l->id = take(&c, 8); l->qpn[0] = (uint32_t)take(&c, 3); l->qpn[1] = (uint32_t)take(&c, 3);
        get(&c, l->gid[0], 16); get(&c, l->gid[1], 16);
    }
    if (c.bad || c.left || !request_valid(&r)) { OPENSSL_cleanse(&r, sizeof(r)); return -1; }
    *out = r; OPENSSL_cleanse(&r, sizeof(r)); return 0;
}
int peer_crypto_ipc_completion_encode(const struct peer_crypto_completion *d, void *buf, size_t cap, size_t *len)
{
    if (len) *len = 0;
    if (!buf || !len || !d || d->status > 0 || d->status < -2) return -1;
    struct cursor c = {buf, cap, 0};
    header(&c, PEER_CRYPTO_IPC_COMPLETION, 8 + 8 + 4 + 8 + 2 + 2 + 16 + 16 + 8);
    number(&c, d->token.manager, 8); number(&c, d->token.generation, 8); number(&c, d->token.slot, 4);
    number(&c, d->operation, 8); number(&c, (unsigned)d->action, 2); number(&c, (uint16_t)(int16_t)d->status, 2);
    put(&c, d->session, 16); put(&c, d->association, 16); number(&c, d->epoch, 8);
    if (c.bad) return -1;
    *len = cap - c.left; return 0;
}
int peer_crypto_ipc_completion_decode(const void *body, size_t len, struct peer_crypto_completion *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!body) return -1;
    struct cursor c = {(uint8_t *)body, len, 0};
    out->token.manager = take(&c, 8); out->token.generation = take(&c, 8); out->token.slot = (uint32_t)take(&c, 4);
    out->operation = take(&c, 8); out->action = (enum peer_crypto_action)take(&c, 2);
    out->status = (int16_t)take(&c, 2);
    get(&c, out->session, 16); get(&c, out->association, 16); out->epoch = take(&c, 8);
    if (c.bad || c.left || out->status > 0 || out->status < -2 ||
        out->action < PEER_CRYPTO_INSTALL_RX || out->action > PEER_CRYPTO_REMOVE_ALL) {
        memset(out, 0, sizeof(*out)); return -1;
    }
    return 0;
}

/* ---- runtime-side adapter ---------------------------------------------- */

#define INFLIGHT_MAX 256u
#define TX_MAX (8u * PEER_CRYPTO_IPC_FRAME_MAX)
#define RECONNECT_NS 1000000000ull
struct inflight { int used; struct peer_crypto_completion key; };
struct peer_crypto_ipc {
    char path[108];
    int fd, epfd, ready;               /* ready: HELLO received */
    uint64_t next_connect_ns;
    uint8_t tx[TX_MAX]; size_t tx_len;
    uint8_t rx[PEER_CRYPTO_IPC_FRAME_MAX]; size_t rx_len;
    struct inflight inflight[INFLIGHT_MAX];
    struct peer_crypto_completion pending[INFLIGHT_MAX]; unsigned pending_head, pending_count;
};
static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void queue_completion(struct peer_crypto_ipc *c, const struct peer_crypto_completion *d)
{
    if (c->pending_count == INFLIGHT_MAX) return;
    c->pending[(c->pending_head + c->pending_count) % INFLIGHT_MAX] = *d; c->pending_count++;
}
/* Every request outstanding on a lost owner ends as unknown outcome: the
 * hardware state left with the process and nobody can say what it did. */
static void disconnect(struct peer_crypto_ipc *c)
{
    if (c->fd >= 0) { epoll_ctl(c->epfd, EPOLL_CTL_DEL, c->fd, NULL); close(c->fd); }
    c->fd = -1; c->ready = 0; c->rx_len = 0;
    OPENSSL_cleanse(c->tx, sizeof(c->tx)); c->tx_len = 0;
    for (unsigned i = 0; i < INFLIGHT_MAX; i++) {
        if (!c->inflight[i].used) continue;
        struct peer_crypto_completion d = c->inflight[i].key; d.status = -2;
        queue_completion(c, &d); c->inflight[i].used = 0;
    }
    c->next_connect_ns = now_ns() + RECONNECT_NS;
}
static void try_connect(struct peer_crypto_ipc *c)
{
    if (c->fd >= 0 || now_ns() < c->next_connect_ns) return;
    c->next_connect_ns = now_ns() + RECONNECT_NS;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    memcpy(addr.sun_path, c->path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) && errno != EINPROGRESS) { close(fd); return; }
    /* Readable-only: a writable socket would keep the descriptor the manager
     * sleeps on permanently ready. A connect still in progress surfaces as
     * EAGAIN on the first receive and as a hangup if it fails. */
    struct epoll_event e = {.events = EPOLLIN | EPOLLRDHUP};
    if (epoll_ctl(c->epfd, EPOLL_CTL_ADD, fd, &e)) { close(fd); return; }
    c->fd = fd;
}
static int flush_tx(struct peer_crypto_ipc *c)
{
    while (c->tx_len) {
        ssize_t n = send(c->fd, c->tx, c->tx_len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        OPENSSL_cleanse(c->tx, (size_t)n);
        memmove(c->tx, c->tx + n, c->tx_len - (size_t)n); c->tx_len -= (size_t)n;
    }
    return 0;
}
static int consume(struct peer_crypto_ipc *c)
{
    for (;;) {
        uint16_t type; uint32_t body_len;
        int rc = peer_crypto_ipc_header_decode(c->rx, c->rx_len, &type, &body_len);
        if (rc < 0) return -1;
        if (rc > 0 || c->rx_len < PEER_CRYPTO_IPC_HEADER_LEN + body_len) return 0;
        const uint8_t *body = c->rx + PEER_CRYPTO_IPC_HEADER_LEN;
        if (type == PEER_CRYPTO_IPC_HELLO) {
            unsigned major, minor;
            if (peer_crypto_ipc_hello_decode(body, body_len, &major, &minor)) return -1;
            c->ready = 1;
        } else if (type == PEER_CRYPTO_IPC_COMPLETION) {
            struct peer_crypto_completion d;
            if (peer_crypto_ipc_completion_decode(body, body_len, &d)) return -1;
            for (unsigned i = 0; i < INFLIGHT_MAX; i++) {
                struct inflight *f = &c->inflight[i];
                if (f->used && f->key.operation == d.operation && f->key.token.manager == d.token.manager &&
                    f->key.token.generation == d.token.generation && f->key.token.slot == d.token.slot &&
                    f->key.action == d.action) { f->used = 0; queue_completion(c, &d); break; }
            }
        } else return -1;   /* the owner never sends requests */
        size_t frame = PEER_CRYPTO_IPC_HEADER_LEN + body_len;
        memmove(c->rx, c->rx + frame, c->rx_len - frame); c->rx_len -= frame;
    }
}
static void pump(struct peer_crypto_ipc *c)
{
    try_connect(c);
    if (c->fd < 0) return;
    if (flush_tx(c) < 0) { disconnect(c); return; }
    for (;;) {
        ssize_t n = recv(c->fd, c->rx + c->rx_len, sizeof(c->rx) - c->rx_len, 0);
        if (n == 0) { disconnect(c); return; }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            disconnect(c); return;
        }
        c->rx_len += (size_t)n;
        if (consume(c) < 0) { disconnect(c); return; }
        if (c->rx_len == sizeof(c->rx)) { disconnect(c); return; }   /* oversized frame */
    }
}
static int ipc_submit(void *ctx, const struct peer_crypto_request *r)
{
    struct peer_crypto_ipc *c = ctx;
    pump(c);
    if (c->fd < 0 || !c->ready || !request_valid(r)) return -1;
    struct inflight *slot = NULL;
    for (unsigned i = 0; i < INFLIGHT_MAX && !slot; i++) if (!c->inflight[i].used) slot = &c->inflight[i];
    uint8_t frame[PEER_CRYPTO_IPC_FRAME_MAX]; size_t len;
    if (!slot || peer_crypto_ipc_request_encode(r, frame, sizeof(frame), &len)) return slot ? -1 : 0;
    if (sizeof(c->tx) - c->tx_len < len) { OPENSSL_cleanse(frame, sizeof(frame)); return 0; }
    memcpy(c->tx + c->tx_len, frame, len); c->tx_len += len; OPENSSL_cleanse(frame, sizeof(frame));
    slot->used = 1;
    slot->key = (struct peer_crypto_completion){.token = r->token, .operation = r->operation,
        .action = r->action, .epoch = r->binding.epoch};
    memcpy(slot->key.session, r->binding.session, 16); memcpy(slot->key.association, r->binding.association, 16);
    if (flush_tx(c) < 0) disconnect(c);   /* the queued completion (-2) reports it */
    return 1;
}
static int ipc_poll(void *ctx, struct peer_crypto_completion *out)
{
    struct peer_crypto_ipc *c = ctx;
    pump(c);
    if (!c->pending_count) return 0;
    *out = c->pending[c->pending_head]; c->pending_head = (c->pending_head + 1) % INFLIGHT_MAX;
    c->pending_count--; return 1;
}
static int ipc_fd(void *ctx) { return ((struct peer_crypto_ipc *)ctx)->epfd; }
static int ipc_healthy(void *ctx)
{
    struct peer_crypto_ipc *c = ctx;
    pump(c);
    return c->fd >= 0 && c->ready;
}
struct peer_crypto_adapter peer_crypto_ipc_adapter(struct peer_crypto_ipc *c)
{
    return (struct peer_crypto_adapter){.ctx = c, .submit = ipc_submit, .poll = ipc_poll,
                                        .fd = ipc_fd, .healthy = ipc_healthy};
}
int peer_crypto_ipc_new(const char *path, struct peer_crypto_ipc **out, char *error, size_t error_len)
{
    if (error && error_len) error[0] = 0;
    if (!out) return -1;
    *out = NULL;
    if (!path || !*path || strlen(path) >= sizeof(((struct peer_crypto_ipc *)0)->path)) {
        if (error && error_len) snprintf(error, error_len, "crypto socket path missing or too long");
        return -1;
    }
    struct peer_crypto_ipc *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    strcpy(c->path, path); c->fd = -1;
    c->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (c->epfd < 0) { free(c); if (error && error_len) snprintf(error, error_len, "epoll: %s", strerror(errno)); return -1; }
    try_connect(c);
    *out = c; return 0;
}
void peer_crypto_ipc_free(struct peer_crypto_ipc *c)
{
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    close(c->epfd);
    OPENSSL_cleanse(c, sizeof(*c)); free(c);
}
