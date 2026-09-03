/* The seam between the peer channel and the wire.
 *
 * Reading order: a connection climbs four states, and every rule in this file
 * exists to serve one of them.
 *
 *   CONNECTING  the carrier is still opening. Nothing above it exists yet.
 *   TLS         the mutual handshake runs. Ciphertext the session emits goes
 *               out as wire messages; wire messages come back in. Neither side
 *               has said its name.
 *   PROLOGUE    the session is up and each end holds the other's raw public
 *               key. The initiator writes the prologue; the responder reads it
 *               and learns which node and which incarnation it is talking to.
 *   ESTABLISHED `peer_key` answers, so the channel above leaves AUTHENTICATING
 *               and its frames begin to flow.
 *
 * The one rule that is easy to get wrong: TLS is a sequenced stream, so once
 * plaintext has been written into it the ciphertext is committed and cannot be
 * unwritten. A frame therefore may not enter the session unless the previous
 * frame's ciphertext has already been handed to the carrier — otherwise a
 * would-block would make the channel retry a frame the session had already
 * counted, and the peer would see it twice. `transport_send` checks the
 * retained ciphertext before it touches the session, which bounds what one
 * connection holds to a single frame's worth. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "peer_transport.h"

#include "peer_tls.h"

#include <errno.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The channel table bounds live peers; the surplus is inbound connections
 * still proving who they are. */
#define PEER_CONN_MAX      512u
#define PEER_ACCEPT_BATCH  16

enum { PC_CONNECTING = 0, PC_TLS, PC_PROLOGUE, PC_ESTABLISHED, PC_FAULTED };
enum { CONN_OWNER_RT = 0, CONN_OWNER_CHANNEL };

struct peer_conn {
    struct peer_transport_rt *rt;
    void    *wc;
    struct peer_tls_conn *tls;
    uint8_t  in_use;
    uint8_t  initiator;
    uint8_t  owner;
    uint8_t  state;
    uint8_t  peer_key[32];
    /* Outbound: what to send once the session is up. Inbound: what arrived. */
    uint8_t  prologue[DMESH_PEER_PROLOGUE_MAX];
    uint32_t prologue_len;
    uint32_t prologue_lines;
    uint64_t deadline_ns;
    /* Ciphertext the carrier would not take. Never more than one message,
     * because nothing new enters the session while this is non-empty. */
    uint8_t *out;
    uint32_t out_len;
    uint8_t *in;                     /* one arriving message, being decrypted */
};

struct peer_transport_rt {
    const struct peer_wire_ops *wire;
    void    *wire_ctx;
    struct peer_tls_ctx *tls_ctx;
    struct dmesh_peer_table *table;
    char     node_name[DMESH_K8S_NAME_MAX];
    uint8_t  public_key[32];
    uint64_t timeout_ns;
    uint64_t (*now_ns)(void *ctx);
    void    *now_ctx;
    uint64_t accepted;
    uint64_t refused;
    uint64_t faults;
    /* One past the highest slot in use. A worker asks this runtime to make
     * progress on every pass, and a node talks to a handful of peers, so the
     * pass must cost what those peers cost rather than what the pool could
     * hold. Recomputed as the pass walks, so it falls back as peers go. */
    uint32_t conn_high;
    struct peer_conn conns[PEER_CONN_MAX];
};

static uint64_t rt_now(const struct peer_transport_rt *rt)
{
    if (rt->now_ns)
        return rt->now_ns(rt->now_ctx);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- connection slots -------------------------------------------------- */

static struct peer_conn *conn_alloc(struct peer_transport_rt *rt, int initiator)
{
    for (uint32_t i = 0; i < PEER_CONN_MAX; i++) {
        struct peer_conn *c = &rt->conns[i];
        if (c->in_use)
            continue;
        uint8_t *buffers = calloc(2, PEER_WIRE_MSG_MAX);
        if (!buffers)
            return NULL;
        memset(c, 0, sizeof(*c));
        c->rt = rt;
        c->out = buffers;
        c->in = buffers + PEER_WIRE_MSG_MAX;
        c->initiator = initiator ? 1 : 0;
        c->owner = CONN_OWNER_RT;
        c->state = PC_CONNECTING;
        c->deadline_ns = rt_now(rt) + rt->timeout_ns;
        if (peer_tls_conn_new(rt->tls_ctx, initiator, &c->tls) != 0) {
            free(buffers);
            memset(c, 0, sizeof(*c));
            return NULL;
        }
        c->in_use = 1;
        if (i + 1 > rt->conn_high)
            rt->conn_high = i + 1;
        return c;
    }
    return NULL;
}

static void conn_free(struct peer_conn *c)
{
    if (!c || !c->in_use)
        return;
    if (c->wc && c->rt->wire->close)
        c->rt->wire->close(c->wc);
    peer_tls_conn_free(c->tls);
    /* The prologue names nodes and the buffers held plaintext-derived bytes;
     * neither outlives the connection. */
    OPENSSL_cleanse(c->out, PEER_WIRE_MSG_MAX);
    OPENSSL_cleanse(c->in, PEER_WIRE_MSG_MAX);
    free(c->out);                    /* the single allocation both halves share */
    memset(c, 0, sizeof(*c));
}

static void conn_fault(struct peer_conn *c)
{
    if (c->state == PC_FAULTED)
        return;
    c->state = PC_FAULTED;
    c->rt->faults++;
}

/* ---- session pump ------------------------------------------------------ */

/* Hand the carrier whatever ciphertext is retained. 1 when it took it, 0 when
 * it would not, -1 when the connection died. */
static int conn_flush(struct peer_conn *c)
{
    if (!c->out_len)
        return 0;
    int r = c->rt->wire->send_msg(c->wc, c->out, c->out_len);
    if (r < 0)
        return -1;
    if (r == 0)
        return 0;
    c->out_len = 0;
    return 1;
}

/* Move everything the session wants to say into the retained buffer. A session
 * that still has output after the buffer is full has exceeded the bound the
 * carrier's message size was chosen against, which is a fault rather than
 * something to carry over. */
static int conn_drain(struct peer_conn *c)
{
    for (;;) {
        if (c->out_len >= PEER_WIRE_MSG_MAX)
            break;
        long n = peer_tls_out(c->tls, c->out + c->out_len,
                              PEER_WIRE_MSG_MAX - c->out_len);
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        c->out_len += (uint32_t)n;
    }
    return peer_tls_out_pending(c->tls) ? -1 : 0;
}

/* Feed one arriving message into the session. 1 fed, 0 none waiting, -1 dead. */
static int conn_feed(struct peer_conn *c)
{
    long n = c->rt->wire->recv_msg(c->wc, c->in, PEER_WIRE_MSG_MAX);
    if (n < 0)
        return -1;
    if (n == 0)
        return 0;
    if (peer_tls_in(c->tls, c->in, (size_t)n) != 0)
        return -1;
    return 1;
}

/* ---- prologue ---------------------------------------------------------- */

/* `dpumesh-peer-v1\n<initiator>\n<responder>\n<incarnation>\n`, exactly as
 * dmesh_peer_prologue writes it. Parsed rather than compared so the
 * initiator's name and incarnation can be handed to the table; a mismatched
 * responder name faults the connection. */
static int prologue_parse(const uint8_t *buf, uint32_t len,
                          char *initiator, size_t initiator_len,
                          char *responder, size_t responder_len,
                          uint32_t *incarnation)
{
    const char *field[4] = { NULL, NULL, NULL, NULL };
    size_t flen[4] = { 0, 0, 0, 0 };
    uint32_t start = 0;
    int n = 0;
    for (uint32_t i = 0; i < len && n < 4; i++) {
        if (buf[i] != '\n')
            continue;
        field[n] = (const char *)buf + start;
        flen[n] = i - start;
        n++;
        start = i + 1;
    }
    if (n != 4)
        return -1;
    if (flen[0] != strlen("dpumesh-peer-v1") ||
        memcmp(field[0], "dpumesh-peer-v1", flen[0]) != 0)
        return -1;
    if (flen[1] == 0 || flen[1] >= initiator_len ||
        flen[2] == 0 || flen[2] >= responder_len || flen[3] == 0 || flen[3] > 10)
        return -1;
    memcpy(initiator, field[1], flen[1]);
    initiator[flen[1]] = '\0';
    memcpy(responder, field[2], flen[2]);
    responder[flen[2]] = '\0';
    if (memchr(initiator, '\0', flen[1]) || memchr(responder, '\0', flen[2]))
        return -1;
    uint64_t value = 0;
    for (size_t i = 0; i < flen[3]; i++) {
        if (field[3][i] < '0' || field[3][i] > '9')
            return -1;
        value = value * 10 + (uint64_t)(field[3][i] - '0');
    }
    if (value == 0 || value > 0xFFFFFFFFull)
        return -1;
    *incarnation = (uint32_t)value;
    return 0;
}

/* The connection authenticated and named itself. Hand it to the table, which
 * decides whether this node will talk to it and closes it when it will not;
 * conn_free zeroes the slot, so only `in_use` may be read afterwards. */
static void conn_adopt(struct peer_transport_rt *rt, struct peer_conn *c)
{
    char initiator[DMESH_K8S_NAME_MAX];
    char responder[DMESH_K8S_NAME_MAX];
    uint32_t incarnation = 0;
    if (!rt->table ||
        prologue_parse(c->prologue, c->prologue_len, initiator, sizeof(initiator),
                       responder, sizeof(responder), &incarnation) != 0 ||
        strcmp(responder, rt->node_name) != 0) {
        conn_fault(c);
        return;
    }
    /* The channel asks for the key the moment it has the connection, so the
     * state has to be final before the handover. */
    c->state = PC_ESTABLISHED;
    c->owner = CONN_OWNER_CHANNEL;
    enum dmesh_peer_refusal reason = DMESH_PEER_OK;
    struct dmesh_peer_channel *channel =
        dmesh_peer_accept(rt->table, initiator, incarnation, c, c->peer_key,
                          &reason);
    if (channel) {
        rt->accepted++;
        return;
    }
    rt->refused++;
    /* Every refusal but one closes the connection on its way out. */
    if (c->in_use)
        conn_free(c);
}

/* Read the prologue a byte at a time. A bulk read would swallow the frames
 * that follow it into a buffer the channel above never sees. */
static int conn_read_prologue(struct peer_transport_rt *rt, struct peer_conn *c,
                              int *moved)
{
    while (c->prologue_lines < 4) {
        uint8_t byte = 0;
        long n = peer_tls_read(c->tls, &byte, 1);
        if (n < 0) {
            conn_fault(c);
            return -1;
        }
        if (n == 0) {
            int fed = conn_feed(c);
            if (fed < 0) {
                conn_fault(c);
                return -1;
            }
            if (fed == 0)
                return 0;
            *moved = 1;
            continue;
        }
        if (c->prologue_len >= DMESH_PEER_PROLOGUE_MAX) {
            conn_fault(c);
            return -1;
        }
        c->prologue[c->prologue_len++] = byte;
        if (byte == '\n')
            c->prologue_lines++;
        *moved = 1;
    }
    conn_adopt(rt, c);
    return 1;                        /* the slot may be gone or adopted */
}

/* ---- the step ---------------------------------------------------------- */

/* peer_key deliberately has only "ready" and "not ready" answers. Faults
 * found below that seam therefore reset the channel here; otherwise a failed
 * connect or handshake would look pending forever. */
static void conn_reset_owner(struct peer_transport_rt *rt, struct peer_conn *c,
                             const char *why)
{
    if (c->owner != CONN_OWNER_CHANNEL || !rt->table) {
        conn_free(c);
        return;
    }
    for (uint32_t i = 0; i < DMESH_CHANNEL_MAX; i++) {
        struct dmesh_peer_channel *channel = &rt->table->channels[i];
        if (channel->in_use && channel->conn == c) {
            dmesh_peer_transport_failed(rt->table, channel, why);
            return;                  /* the reset closed it */
        }
    }
    conn_free(c);
}

/* Returns whether this connection actually moved. Pending state is not
 * movement: a caller that treats it as such never sleeps. */
static int conn_step(struct peer_transport_rt *rt, struct peer_conn *c)
{
    int moved = 0;

    if (c->state == PC_FAULTED)
        return 0;
    if (c->rt->wire->faulted(c->wc)) {
        conn_fault(c);
        return 1;
    }

    if (c->state == PC_CONNECTING) {
        if (!rt->wire->established(c->wc))
            goto deadline;
        c->state = PC_TLS;
        moved = 1;
    }

    if (c->state == PC_TLS) {
        for (;;) {
            int done = peer_tls_handshake(c->tls);
            if (done < 0 || conn_drain(c) < 0 || conn_flush(c) < 0) {
                conn_fault(c);
                return 1;
            }
            if (done == 1)
                break;
            int fed = conn_feed(c);
            if (fed < 0) {
                conn_fault(c);
                return 1;
            }
            if (fed == 0)
                break;
            moved = 1;
        }
        if (peer_tls_established(c->tls)) {
            if (peer_tls_peer_key(c->tls, c->peer_key) < 0) {
                conn_fault(c);
                return 1;
            }
            c->state = PC_PROLOGUE;
            c->prologue_lines = 0;
            if (!c->initiator)
                c->prologue_len = 0;
            moved = 1;
        }
    }

    if (c->state == PC_PROLOGUE && c->initiator) {
        if (c->out_len && conn_flush(c) < 0) {
            conn_fault(c);
            return 1;
        }
        if (!c->out_len) {
            if (peer_tls_write(c->tls, c->prologue, c->prologue_len) != 0 ||
                conn_drain(c) < 0 || conn_flush(c) < 0) {
                conn_fault(c);
                return 1;
            }
            c->state = PC_ESTABLISHED;
            moved = 1;
        }
    } else if (c->state == PC_PROLOGUE) {
        if (conn_read_prologue(rt, c, &moved) != 0)
            return 1;                /* adopted, refused, or faulted */
    }

    if (c->state == PC_ESTABLISHED && c->out_len) {
        int flushed = conn_flush(c);
        if (flushed < 0) {
            conn_fault(c);
            return 1;
        }
        if (flushed > 0)
            moved = 1;
    }

deadline:
    if (c->state != PC_ESTABLISHED && c->state != PC_FAULTED &&
        rt_now(rt) >= c->deadline_ns) {
        conn_fault(c);
        conn_reset_owner(rt, c, "peer handshake timed out");
        return 1;
    }
    return moved;
}

/* ---- the seam ---------------------------------------------------------- */

static int transport_connect(void *ctx, uint32_t ip_be, uint16_t port,
                             const uint8_t *prologue, size_t prologue_len,
                             void **out)
{
    struct peer_transport_rt *rt = ctx;
    if (!rt || !out)
        return -EINVAL;
    *out = NULL;
    if (!prologue || prologue_len == 0 || prologue_len > DMESH_PEER_PROLOGUE_MAX)
        return -EINVAL;
    struct peer_conn *c = conn_alloc(rt, 1);
    if (!c)
        return -ENOSPC;
    memcpy(c->prologue, prologue, prologue_len);
    c->prologue_len = (uint32_t)prologue_len;
    if (rt->wire->connect(rt->wire_ctx, ip_be, port, &c->wc) != 0) {
        conn_free(c);
        return -ECONNREFUSED;
    }
    /* From here the channel owns it: it is the one that closes it. */
    c->owner = CONN_OWNER_CHANNEL;
    *out = c;
    return 0;
}

static int transport_peer_key(void *cv, uint8_t key[32])
{
    struct peer_conn *c = cv;
    if (!c || !c->in_use || c->state != PC_ESTABLISHED)
        return -1;
    memcpy(key, c->peer_key, 32);
    return 0;
}

static long transport_send(void *cv, const void *buf, size_t len)
{
    struct peer_conn *c = cv;
    if (!c || !c->in_use || c->state == PC_FAULTED)
        return -1;
    if (c->state != PC_ESTABLISHED)
        return -1;
    if (len == 0 || len > PEER_WIRE_MSG_MAX)
        return -1;
    /* Nothing enters the session until what it last produced has left. */
    if (c->out_len) {
        if (conn_flush(c) < 0) {
            conn_fault(c);
            return -1;
        }
        if (c->out_len)
            return 0;
    }
    if (peer_tls_write(c->tls, buf, len) != 0 || conn_drain(c) < 0 ||
        conn_flush(c) < 0) {
        conn_fault(c);
        return -1;
    }
    return (long)len;
}

static long transport_recv(void *cv, void *buf, size_t len)
{
    struct peer_conn *c = cv;
    if (!c || !c->in_use || c->state == PC_FAULTED)
        return -1;
    if (c->state != PC_ESTABLISHED || len == 0)
        return 0;
    for (;;) {
        long n = peer_tls_read(c->tls, buf, len);
        if (n > 0)
            return n;
        if (n < 0) {
            conn_fault(c);
            return -1;
        }
        int fed = conn_feed(c);
        if (fed < 0) {
            conn_fault(c);
            return -1;
        }
        if (fed == 0)
            return 0;
    }
}

static void transport_close(void *cv)
{
    conn_free(cv);
}

static const struct dmesh_peer_transport TRANSPORT_OPS = {
    .connect  = transport_connect,
    .peer_key = transport_peer_key,
    .send     = transport_send,
    .recv     = transport_recv,
    .close    = transport_close,
};

const struct dmesh_peer_transport *dmesh_peer_transport_ops(void)
{
    return &TRANSPORT_OPS;
}

/* ---- runtime ----------------------------------------------------------- */

int dmesh_peer_transport_new(const struct peer_transport_config *config,
                             struct peer_transport_rt **out,
                             char *error, size_t error_len)
{
    if (error && error_len)
        error[0] = '\0';
    if (!out)
        return -1;
    *out = NULL;
    if (!config || !config->node_name || !*config->node_name || !config->seed ||
        !config->wire || !config->wire_ctx) {
        snprintf(error, error_len, "peer transport: incomplete configuration");
        return -1;
    }
    struct peer_transport_rt *rt = calloc(1, sizeof(*rt));
    if (!rt) {
        snprintf(error, error_len, "peer transport: out of memory");
        return -1;
    }
    rt->wire = config->wire;
    rt->wire_ctx = config->wire_ctx;
    rt->now_ns = config->now_ns;
    rt->now_ctx = config->now_ctx;
    rt->timeout_ns = config->handshake_timeout_ns
                         ? config->handshake_timeout_ns
                         : DMESH_PEER_HANDSHAKE_TIMEOUT_NS;
    snprintf(rt->node_name, sizeof(rt->node_name), "%s", config->node_name);
    if (peer_tls_ctx_new(config->seed, rt->node_name, &rt->tls_ctx, error,
                         error_len) != 0) {
        free(rt);
        return -1;
    }
    peer_tls_ctx_public_key(rt->tls_ctx, rt->public_key);
    *out = rt;
    return 0;
}

void dmesh_peer_transport_free(struct peer_transport_rt *rt)
{
    if (!rt)
        return;
    for (uint32_t i = 0; i < PEER_CONN_MAX; i++)
        conn_free(&rt->conns[i]);
    peer_tls_ctx_free(rt->tls_ctx);
    if (rt->wire && rt->wire->ctx_free)
        rt->wire->ctx_free(rt->wire_ctx);
    OPENSSL_cleanse(rt, sizeof(*rt));
    free(rt);
}

void dmesh_peer_transport_attach(struct peer_transport_rt *rt,
                                 struct dmesh_peer_table *table)
{
    if (rt)
        rt->table = table;
}

int dmesh_peer_transport_progress(struct peer_transport_rt *rt)
{
    if (!rt)
        return 0;
    void *accepted[PEER_ACCEPT_BATCH];
    int n = 0;
    int progressed = rt->wire->progress(rt->wire_ctx, accepted,
                                        PEER_ACCEPT_BATCH, &n) ? 1 : 0;
    for (int i = 0; i < n; i++) {
        struct peer_conn *c = conn_alloc(rt, 0);
        if (!c) {
            rt->wire->close(accepted[i]);
            continue;
        }
        c->wc = accepted[i];
        c->state = PC_TLS;            /* the carrier accepted it already */
        progressed = 1;
    }
    uint32_t limit = rt->conn_high;
    uint32_t high = 0;
    for (uint32_t i = 0; i < limit; i++) {
        struct peer_conn *c = &rt->conns[i];
        if (!c->in_use)
            continue;
        if (conn_step(rt, c))
            progressed = 1;
        if (c->in_use && c->state == PC_FAULTED)
            conn_reset_owner(rt, c, "peer transport failed while authenticating");
        if (c->in_use)
            high = i + 1;
    }
    rt->conn_high = high;
    return progressed;
}

int dmesh_peer_transport_pending(struct peer_transport_rt *rt)
{
    if (!rt)
        return 0;
    for (uint32_t i = 0; i < rt->conn_high; i++) {
        const struct peer_conn *c = &rt->conns[i];
        if (!c->in_use)
            continue;
        if (c->out_len)
            return 1;
        if (c->state != PC_ESTABLISHED && c->state != PC_FAULTED)
            return 1;
    }
    return 0;
}

int dmesh_peer_transport_epfd(struct peer_transport_rt *rt)
{
    if (!rt || !rt->wire->epfd)
        return -1;
    return rt->wire->epfd(rt->wire_ctx);
}

void dmesh_peer_transport_public_key(const struct peer_transport_rt *rt,
                                     uint8_t key[32])
{
    if (rt)
        memcpy(key, rt->public_key, 32);
}

void dmesh_peer_transport_stats(const struct peer_transport_rt *rt,
                                struct peer_transport_stats *out)
{
    if (!rt || !out)
        return;
    memset(out, 0, sizeof(*out));
    out->accepted = rt->accepted;
    out->refused = rt->refused;
    out->faults = rt->faults;
    for (uint32_t i = 0; i < rt->conn_high; i++)
        if (rt->conns[i].in_use)
            out->live++;
}
