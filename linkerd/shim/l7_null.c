/* Reference L7 consumer: a terminating proxy with no protocol of its own.
 *
 * It exercises the whole adapter contract — connection lifecycle, staging
 * custody, the egress arena, backend selection and backpressure — so the
 * datapath side can be validated before the real L7 layer links in. Behaviour
 * follows the mode DPUmesh assigned to each connection:
 *
 *   opaque    a byte stream with no boundaries: copy it onward as it arrives,
 *             letting the data plane pick the backend. This is the cost floor.
 *   full      reassemble length-prefixed messages, choose a backend per
 *             message, then re-emit. This is what a terminating proxy costs:
 *             the payload is copied once into the layer and once back out.
 *   decision  no payload arrives; l7_resolve answers once per connection.
 *
 * Plain C with no DOCA dependency: this file is a stand-in for a Rust
 * staticlib exporting the same symbols.
 */

#include "dmesh_l7.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The validator's length-prefixed framing: [magic][seq][payload_len][aux]. */
#define L7_HDR        16u
#define L7_REQ_MAGIC  0x62526571u        /* "bReq" */
#define L7_REP_MAGIC  0x62526570u        /* "bRep" */
#define L7_MSG_MAX    (64u * 1024u)

#define L7_CONN_SLOTS 4096u              /* power of two; open addressing */
#define L7_BACKENDS   32

struct l7_conn {
    uint64_t key;                        /* handle + 1, so 0 means empty */
    struct dmesh_l7_flow flow;
    uint8_t *asm_buf;                    /* partial message, `full` mode only */
    uint32_t asm_len;
    uint32_t rr;                         /* per-connection round robin cursor */
    int32_t  backend;                    /* pod + 1 when pinned for this conn's life */
};

#define L7_DRAIN_MAX  64                 /* connections awaiting a retry */

struct l7_worker {
    struct l7_conn *slots;
    /* Connections holding a reassembled message the arena could not take. The
     * worker loop steps this: without it a message buffered here would wait for
     * the next arriving segment, which may never come. */
    uint64_t drain[L7_DRAIN_MAX];
    int      drain_n;
    uint64_t conns_opened, conns_closed;
    uint64_t bytes_in, bytes_out, messages;
    uint64_t send_retries, send_errors;
    uint64_t resolved;
};

static __thread struct l7_worker tls_worker;

/* DPUMESH_L7_NULL_TRACE=1 logs the connection lifecycle. Read once per thread
 * because the worker loop calls into here on every revolution. */
static int
l7_trace(void)
{
    static __thread int cached = -1;
    if (cached < 0) {
        const char *env = getenv("DPUMESH_L7_NULL_TRACE");
        cached = env && *env && *env != '0';
    }
    return cached;
}

/* Open addressing on the handle. The table is sized well above the connection
 * count a worker holds, so a probe run stays short. A closed slot becomes a
 * tombstone rather than empty: emptying it would cut the probe run of every
 * later key that hashed through it. */
#define L7_SLOT_EMPTY 0u
#define L7_SLOT_DEAD  ((uint64_t)-1)

static struct l7_conn *
l7_conn_slot(uint64_t conn, int create)
{
    if (!tls_worker.slots)
        return NULL;
    uint64_t key = conn + 1;
    uint32_t h = (uint32_t)((conn * 2654435761u) & (L7_CONN_SLOTS - 1u));
    struct l7_conn *reuse = NULL;
    for (uint32_t i = 0; i < L7_CONN_SLOTS; i++) {
        struct l7_conn *s = &tls_worker.slots[(h + i) & (L7_CONN_SLOTS - 1u)];
        if (s->key == key)
            return s;
        if (s->key == L7_SLOT_DEAD) {
            if (!reuse)
                reuse = s;
            continue;
        }
        if (s->key == L7_SLOT_EMPTY) {
            if (!create)
                return NULL;
            if (!reuse)
                reuse = s;
            break;
        }
    }
    if (!create || !reuse)
        return NULL;
    memset(reuse, 0, sizeof(*reuse));
    reuse->key = key;
    return reuse;
}

static void
l7_conn_drop(struct l7_conn *s)
{
    free(s->asm_buf);
    memset(s, 0, sizeof(*s));
    s->key = L7_SLOT_DEAD;
}

int
l7_worker_attach(int worker_id)
{
    memset(&tls_worker, 0, sizeof(tls_worker));
    tls_worker.slots = (struct l7_conn *)calloc(L7_CONN_SLOTS,
                                                sizeof(*tls_worker.slots));
    if (!tls_worker.slots) {
        fprintf(stderr, "[l7_ref] worker %d: out of memory\n", worker_id);
        return -1;
    }
    fprintf(stderr, "[l7_ref] worker %d attached\n", worker_id);
    return 0;
}

static void
l7_drain_add(uint64_t conn)
{
    for (int i = 0; i < tls_worker.drain_n; i++)
        if (tls_worker.drain[i] == conn)
            return;
    /* When the set is full the connection is left out: the next segment to
     * arrive on it retries the flush. */
    if (tls_worker.drain_n < L7_DRAIN_MAX)
        tls_worker.drain[tls_worker.drain_n++] = conn;
}

static void
l7_drain_remove(uint64_t conn)
{
    for (int i = 0; i < tls_worker.drain_n; i++)
        if (tls_worker.drain[i] == conn) {
            tls_worker.drain[i] = tls_worker.drain[--tls_worker.drain_n];
            return;
        }
}

static int l7_flush_framed(int worker_id, uint64_t conn, struct l7_conn *s);

int
l7_worker_step(int worker_id)
{
    int did = 0;
    /* The flush maintains the set itself, so this walk only decides which entry
     * to visit next: removal swaps the last entry into the current slot, and
     * that entry has not been visited yet. */
    for (int i = 0; i < tls_worker.drain_n; ) {
        uint64_t conn = tls_worker.drain[i];
        struct l7_conn *s = l7_conn_slot(conn, 0);
        if (!s) {
            l7_drain_remove(conn);
            continue;
        }
        int before = tls_worker.drain_n;
        if (l7_flush_framed(worker_id, conn, s) > 0)
            did = 1;
        if (tls_worker.drain_n == before)
            i++;
    }
    return did;
}

int
l7_conn_open(int worker_id, uint64_t conn, const struct dmesh_l7_flow *flow)
{
    struct l7_conn *s = l7_conn_slot(conn, 1);
    if (!s || !flow)
        return -1;
    s->flow = *flow;
    tls_worker.conns_opened++;
    if (l7_trace())
        fprintf(stderr, "[l7_ref] worker %d conn %llu open: pod %d -> service %d "
                "(%u -> %u) mode %u reply %u workload '%s'\n",
                worker_id, (unsigned long long)conn,
                flow->src_pod, flow->dst_service, flow->src_port, flow->dst_port,
                flow->mode, flow->is_reply,
                flow->workload[0] ? flow->workload : "-");
    return 0;
}

/* Copy into egress memory and publish. Returns what was published, 0 to retry. */
static int
l7_emit(int worker_id, uint64_t conn, int32_t backend,
        const uint8_t *src, uint32_t len)
{
    uint32_t cap = 0;
    uint8_t *dst = dmesh_l7_tx_reserve(worker_id, conn, &cap);
    if (!dst || cap == 0) {
        tls_worker.send_retries++;
        return 0;
    }
    uint32_t take = len < cap ? len : cap;
    memcpy(dst, src, take);
    int sent = dmesh_l7_tx_commit(worker_id, conn, backend, take);
    if (sent < 0)
        tls_worker.send_errors++;
    else if (sent == 0)
        tls_worker.send_retries++;
    return sent;
}

/* An opaque stream carries no message boundaries, so a delivery may end
 * anywhere. Writing into one reservation keeps every delivery a single
 * scatter-gather source. */
static int
l7_segment_opaque(int worker_id, uint64_t conn,
                  const uint8_t *base, uint32_t pos, uint32_t len)
{
    int sent = l7_emit(worker_id, conn, DMESH_L7_BACKEND_ANY, base + pos, len);
    if (sent > 0) {
        dmesh_l7_release(worker_id, conn, pos, (uint32_t)sent);
        tls_worker.bytes_in  += (uint32_t)sent;
        tls_worker.bytes_out += (uint32_t)sent;
    }
    return sent;
}

/* DPUMESH_L7_FRAMED_RR selects how often the balancer moves: `message` for a
 * backend per message, anything else for one backend for the connection's life.
 *
 * Only the per-connection choice preserves response order on a protocol that
 * matches replies positionally. Per message is legal for a protocol that
 * correlates responses itself — HTTP/2 stream identifiers — and reorders
 * without one. It also costs: a destination that changes between messages
 * cannot share a delivery with the message before it, so each one becomes its
 * own publication to the host. */
static int
l7_route_per_message(void)
{
    static __thread int cached = -1;
    if (cached < 0) {
        const char *env = getenv("DPUMESH_L7_FRAMED_RR");
        cached = env && env[0] == 'm';
    }
    return cached;
}

/* Message boundary at `off`, or 0 when it is incomplete. -1 when the bytes are
 * not this protocol. */
static int
l7_msg_len(const struct l7_conn *s, uint32_t off, uint32_t *total)
{
    if (s->asm_len - off < L7_HDR)
        return 0;
    uint32_t magic, payload_len;
    memcpy(&magic,       s->asm_buf + off + 0, 4);
    memcpy(&payload_len, s->asm_buf + off + 8, 4);
    if ((magic != L7_REQ_MAGIC && magic != L7_REP_MAGIC) ||
        payload_len > L7_MSG_MAX - L7_HDR)
        return -1;
    *total = L7_HDR + payload_len;
    return s->asm_len - off >= *total;
}

static int32_t
l7_pick_backend(int worker_id, struct l7_conn *s)
{
    if (s->flow.is_reply)
        return DMESH_L7_BACKEND_ANY;     /* conntrack owns the reply direction */
    if (!l7_route_per_message() && s->backend != 0)
        return s->backend - 1;           /* stored biased so 0 means unset */
    int32_t pods[L7_BACKENDS];
    int n = dmesh_l7_backends(worker_id, s->flow.dst_service, pods, L7_BACKENDS);
    if (n <= 0)
        return DMESH_L7_BACKEND_ANY;
    int32_t pick = pods[s->rr++ % (uint32_t)n];
    if (!l7_route_per_message())
        s->backend = pick + 1;
    return pick;
}

/* Emit every complete message the assembly buffer holds, coalescing a run of
 * consecutive messages that share a backend into one delivery. The buffer is
 * consumed strictly in order and only after a delivery is published, so a
 * failed reservation costs a repeated copy and never a lost message. Returns
 * the bytes emitted, or -1 when the stream is not this protocol. */
static int
l7_flush_framed(int worker_id, uint64_t conn, struct l7_conn *s)
{
    uint32_t done = 0;
    int per_message = l7_route_per_message();

    for (;;) {
        uint32_t total = 0;
        int have = l7_msg_len(s, done, &total);
        if (have < 0)
            return -1;
        if (have == 0)
            break;                       /* message still arriving */

        uint32_t cap = 0;
        uint8_t *dst = dmesh_l7_tx_reserve(worker_id, conn, &cap);
        if (!dst || cap == 0) {
            tls_worker.send_retries++;
            break;
        }
        int32_t backend = l7_pick_backend(worker_id, s);
        if (total > cap) {
            /* Wider than a reservation: send it as its own delivery, which
             * chains as many chunks as it needs. */
            (void)dmesh_l7_tx_commit(worker_id, conn, backend, 0);
            int sent = dmesh_l7_send(worker_id, conn, backend,
                                     s->asm_buf + done, total);
            if (sent < 0)
                return -1;
            if ((uint32_t)sent < total)
                break;
            done += total;
            tls_worker.bytes_out += total;
            tls_worker.messages++;
            continue;
        }
        uint32_t run = 0, msgs = 0;
        while (have > 0 && run + total <= cap) {
            memcpy(dst + run, s->asm_buf + done + run, total);
            run += total;
            msgs++;
            if (per_message)
                break;                   /* every message re-picks a backend */
            have = l7_msg_len(s, done + run, &total);
            if (have < 0)
                return -1;
        }
        int sent = dmesh_l7_tx_commit(worker_id, conn, backend, run);
        if (sent < 0) {
            tls_worker.send_errors++;
            return -1;
        }
        if (sent == 0) {                 /* reservation returned; bytes still buffered */
            tls_worker.send_retries++;
            break;
        }
        done += run;
        tls_worker.bytes_out += run;
        tls_worker.messages += msgs;
    }
    if (done) {
        memmove(s->asm_buf, s->asm_buf + done, s->asm_len - done);
        s->asm_len -= done;
    }
    if (s->asm_len)
        l7_drain_add(conn);
    else
        l7_drain_remove(conn);
    return (int)done;
}

/* A framed stream is reassembled here, which is what makes the proxy
 * terminating: the layer holds bytes the data plane no longer owns, and the
 * bytes it emits are its own. Each complete message is routed independently. */
static int
l7_segment_framed(int worker_id, uint64_t conn, struct l7_conn *s,
                  const uint8_t *base, uint32_t pos, uint32_t len)
{
    if (!s->asm_buf) {
        s->asm_buf = (uint8_t *)malloc(L7_MSG_MAX);
        if (!s->asm_buf)
            return -1;
    }
    uint32_t room = L7_MSG_MAX - s->asm_len;
    uint32_t take = len < room ? len : room;
    if (take == 0)
        return 0;                        /* drain what is buffered first */
    memcpy(s->asm_buf + s->asm_len, base + pos, take);
    s->asm_len += take;
    tls_worker.bytes_in += take;
    /* Custody is returned for what was copied out of staging, not for what was
     * forwarded: the bytes now live in this layer's own buffer. */
    dmesh_l7_release(worker_id, conn, pos, take);

    if (l7_flush_framed(worker_id, conn, s) < 0)
        return -1;
    return (int)take;
}

int
l7_conn_segment(int worker_id, uint64_t conn,
                const uint8_t *base, uint32_t pos, uint32_t len)
{
    if (!base || len == 0)
        return 0;
    struct l7_conn *s = l7_conn_slot(conn, 0);
    if (!s)
        return -1;
    if (s->flow.mode == DMESH_L7_MODE_FULL)
        return l7_segment_framed(worker_id, conn, s, base, pos, len);
    return l7_segment_opaque(worker_id, conn, base, pos, len);
}

void
l7_conn_eof(int worker_id, uint64_t conn)
{
    if (l7_trace())
        fprintf(stderr, "[l7_ref] worker %d conn %llu eof\n",
                worker_id, (unsigned long long)conn);
}

void
l7_conn_close(int worker_id, uint64_t conn)
{
    struct l7_conn *s = l7_conn_slot(conn, 0);
    l7_drain_remove(conn);
    if (s)
        l7_conn_drop(s);
    tls_worker.conns_closed++;
    if (l7_trace())
        fprintf(stderr, "[l7_ref] worker %d conn %llu close\n",
                worker_id, (unsigned long long)conn);
}

void
l7_worker_detach(int worker_id)
{
    fprintf(stderr, "[l7_ref] worker %d detached: conns %llu/%llu, msgs %llu, "
            "bytes in/out %llu/%llu, resolved %llu, "
            "retries %llu, errors %llu\n",
            worker_id,
            (unsigned long long)tls_worker.conns_closed,
            (unsigned long long)tls_worker.conns_opened,
            (unsigned long long)tls_worker.messages,
            (unsigned long long)tls_worker.bytes_in,
            (unsigned long long)tls_worker.bytes_out,
            (unsigned long long)tls_worker.resolved,
            (unsigned long long)tls_worker.send_retries,
            (unsigned long long)tls_worker.send_errors);
    free(tls_worker.slots);
    tls_worker.slots = NULL;
}

/* One answer per connection, with no payload behind it: admit, and name the
 * backend the data plane should pin the stream to.
 *
 * The cursor is offset by the flow rather than being a plain per-worker
 * sequence. Each worker holds its own instance and sees only its share of the
 * connections, so independent sequences all start at the same backend and the
 * spread collapses; deriving the offset from the flow keeps workers from
 * agreeing by accident. */
int
l7_resolve(int worker_id, const struct dmesh_l7_flow *flow,
           struct dmesh_l7_verdict *out)
{
    if (!flow || !out)
        return -1;
    out->allow = 1;
    out->backend_pod = DMESH_L7_BACKEND_ANY;
    int32_t pods[L7_BACKENDS];
    int n = dmesh_l7_backends(worker_id, flow->dst_service, pods, L7_BACKENDS);
    if (n > 0) {
        static __thread uint32_t rr;
        /* Avalanche rather than a weighted sum: a sum whose weights share a
         * factor with the backend count collapses to one backend. */
        uint32_t spread = (uint32_t)flow->src_pod * 0x9E3779B1u ^
                          ((uint32_t)flow->src_port * 0x85EBCA6Bu);
        spread ^= spread >> 15;
        spread *= 0xC2B2AE35u;
        spread ^= spread >> 13;
        out->backend_pod = pods[(spread + rr++) % (uint32_t)n];
    }
    tls_worker.resolved++;
    if (l7_trace())
        fprintf(stderr, "[l7_ref] worker %d resolve: pod %d:%u -> service %d = "
                "allow %d backend %d (of %d candidates)\n", worker_id,
                flow->src_pod, flow->src_port, flow->dst_service,
                out->allow, out->backend_pod, n);
    return 0;
}

void
l7_report(int worker_id, uint64_t conn, uint64_t bytes_in,
          uint64_t bytes_out, uint64_t duration_ns, int reason)
{
    tls_worker.bytes_in  += bytes_in;
    tls_worker.bytes_out += bytes_out;
    if (l7_trace())
        fprintf(stderr, "[l7_ref] worker %d conn %llu report: in %llu out %llu "
                "%llu ns reason %d\n", worker_id, (unsigned long long)conn,
                (unsigned long long)bytes_in, (unsigned long long)bytes_out,
                (unsigned long long)duration_ns, reason);
}
