#define _GNU_SOURCE
#include "peer_pair_transport.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

/* Queue cursors may wrap; unsigned distance remains <= capacity. Connection,
 * operation and grant generations never wrap. No borrowed pointers cross it. */
union message { struct peer_pair_command command; struct peer_pair_event event; };
struct mailbox {
    atomic_uint put, get;
    int fd;
    union message slots[PEER_PAIR_MAILBOX_CAP];
};
struct pair_conn {
    struct peer_pair_transport *rt;
    struct peer_pair_ref ref;
    struct peer_pair_offer offer;
    struct peer_assoc_token association;
    struct peer_verbs_endpoint endpoint;
    struct peer_sec_lane lane;
    struct peer_assoc_grant grant;
    struct dmesh_peer_channel *channel;
    uint32_t channel_incarnation;
    void *wire;
    uint64_t created, operation, epoch;
    uint64_t deadline[3];
    unsigned local;
    int used, incoming, prepared, enabled, active, paused, resume_required;
    int fault, released, blocked, destroyed, quarantine, notify_revoke;
    enum peer_pair_command_type waiting;
};
struct peer_pair_transport {
    struct peer_pair_transport_config cfg;
    struct peer_pair_driver driver;
    struct dmesh_peer_table *table;
    struct pair_conn *conns;
    struct mailbox commands, events;
    uint64_t id, generation, last_now, incoming_operation;
    int epfd;
};

static uint64_t clock_ns(void *unused)
{
    (void)unused;
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void signal_queue(struct mailbox *q)
{
    uint64_t one = 1;
    ssize_t n;
    do { n = write(q->fd, &one, sizeof(one)); } while (n < 0 && errno == EINTR);
    /* EAGAIN means a wake is already pending. Only free closes these fds,
     * after both owners have stopped using the runtime. */
}
static int room(struct mailbox *q)
{
    return atomic_load_explicit(&q->put, memory_order_relaxed) -
           atomic_load_explicit(&q->get, memory_order_acquire) < PEER_PAIR_MAILBOX_CAP;
}
static unsigned available(struct mailbox *q)
{
    return PEER_PAIR_MAILBOX_CAP - (atomic_load_explicit(&q->put, memory_order_relaxed) -
           atomic_load_explicit(&q->get, memory_order_acquire));
}
static int push(struct mailbox *q, const void *p, size_t n)
{
    unsigned put = atomic_load_explicit(&q->put, memory_order_relaxed);
    if (put - atomic_load_explicit(&q->get, memory_order_acquire) == PEER_PAIR_MAILBOX_CAP) return 0;
    memcpy(&q->slots[put % PEER_PAIR_MAILBOX_CAP], p, n);
    atomic_store_explicit(&q->put, put + 1, memory_order_release);
    signal_queue(q);
    return 1;
}
static int queued(struct mailbox *q)
{
    return atomic_load_explicit(&q->put, memory_order_acquire) !=
           atomic_load_explicit(&q->get, memory_order_relaxed);
}
static int pop(struct mailbox *q, void *p, size_t n)
{
    uint64_t wake;
    ssize_t rc;
    do { rc = read(q->fd, &wake, sizeof(wake)); } while (rc < 0 && errno == EINTR);
    unsigned get = atomic_load_explicit(&q->get, memory_order_relaxed);
    if (get == atomic_load_explicit(&q->put, memory_order_acquire)) return 0;
    memcpy(p, &q->slots[get % PEER_PAIR_MAILBOX_CAP], n);
    atomic_store_explicit(&q->get, get + 1, memory_order_release);
    /* A caller may consume just one item then sleep. Preserve fd readability
     * for the rest; a concurrent producer also signals after publication. */
    if (queued(q)) signal_queue(q);
    return 1;
}
static int ref_equal(struct peer_pair_ref a, struct peer_pair_ref b)
{ return a.runtime == b.runtime && a.generation == b.generation && a.slot == b.slot; }
static int token_equal(struct peer_assoc_token a, struct peer_assoc_token b)
{ return a.manager == b.manager && a.generation == b.generation && a.slot == b.slot; }
static int text_valid(const char *s, size_t size)
{ return s[0] && memchr(s, 0, size); }
static int pair_equal(const struct dmesh_peer_pair *a, const struct dmesh_peer_pair *b)
{
    return !strncmp(a->local_uid, b->local_uid, sizeof(a->local_uid)) &&
        !strncmp(a->remote_uid, b->remote_uid, sizeof(a->remote_uid)) &&
        dmesh_peer_registration_equal(&a->local, &b->local) &&
        dmesh_peer_registration_equal(&a->remote, &b->remote) &&
        !memcmp(a->association, b->association, 16) &&
        a->lane_id == b->lane_id && a->lane_generation == b->lane_generation;
}
static int lane_equal(const struct peer_sec_lane *a, const struct peer_sec_lane *b)
{
    uint8_t x[PEER_SEC_LANE_LEN], y[PEER_SEC_LANE_LEN];
    return !peer_sec_lane_encode(a, x) && !peer_sec_lane_encode(b, y) && !memcmp(x, y, sizeof(x));
}
static int leases_valid(const uint64_t deadline[3], uint64_t now)
{ return now < deadline[0] && now < deadline[1] && now < deadline[2]; }
static void fault(struct pair_conn *c)
{
    if (!c->fault) c->notify_revoke = 1;
    c->fault = 1; c->paused = 1;
}
static int identity_valid(struct peer_pair_transport *rt, const struct peer_pair_offer *o)
{
    struct dmesh_peer_table *t = rt->table;
    struct dmesh_peer_registration reg;
    const uint8_t *key = NULL;
    uint32_t ip = 0; uint16_t port = 0;
    if (!t || !t->ops || !t->ops->node_binding || !t->ops->pod_on_node ||
        !t->ops->local_registration || !text_valid(o->node, sizeof(o->node)) ||
        !text_valid(o->pair.local_uid, sizeof(o->pair.local_uid)) ||
        !text_valid(o->pair.remote_uid, sizeof(o->pair.remote_uid))) return 0;
    if (!strcmp(o->node, t->node_name) || !strcmp(o->pair.local_uid, o->pair.remote_uid)) return 0;
    return t->ops->node_binding(t->ops_ctx, o->node, &key, &ip, &port) == 1 && key &&
        !memcmp(key, o->key, 32) && ip == o->ip_be && port == o->port &&
        t->ops->pod_on_node(t->ops_ctx, o->pair.local_uid, t->node_name) == 1 &&
        t->ops->pod_on_node(t->ops_ctx, o->pair.remote_uid, o->node) == 1 &&
        t->ops->local_registration(t->ops_ctx, o->pair.local_uid, &reg) == 1 &&
        dmesh_peer_registration_valid(&reg) && dmesh_peer_registration_equal(&reg, &o->pair.local);
}
static int live(struct pair_conn *c)
{
    struct peer_pair_transport *rt = c->rt;
    uint64_t now = rt->cfg.now_ns(rt->cfg.now_ctx);
    if (!c->used || c->fault || c->released) return 0;
    if (now < rt->last_now || !identity_valid(rt, &c->offer) ||
        (c->prepared && !leases_valid(c->deadline, now)) ||
        (!c->active && now - c->created >= rt->cfg.setup_timeout_ns) ||
        (c->wire && rt->driver.wire->faulted(c->wire))) {
        fault(c); return 0;
    }
    return 1;
}
static struct pair_conn *lookup(struct peer_pair_transport *rt, struct peer_pair_ref ref)
{
    if (ref.runtime != rt->id || ref.slot >= rt->cfg.connections) return NULL;
    struct pair_conn *c = &rt->conns[ref.slot];
    return c->used && ref_equal(c->ref, ref) ? c : NULL;
}
static struct pair_conn *allocate(struct peer_pair_transport *rt, const struct peer_pair_offer *o, int incoming)
{
    if (rt->generation == UINT64_MAX || !identity_valid(rt, o) || !o->incarnation) return NULL;
    struct pair_conn *free_conn = NULL;
    for (unsigned i = 0; i < rt->cfg.connections; i++) {
        struct pair_conn *c = &rt->conns[i];
        if (!c->used) { if (!free_conn) free_conn = c; continue; }
        if (!strcmp(c->offer.node, o->node) && !strcmp(c->offer.pair.local_uid, o->pair.local_uid) &&
            !strcmp(c->offer.pair.remote_uid, o->pair.remote_uid)) return NULL;
    }
    if (!free_conn) return NULL;
    struct pair_conn *c = free_conn;
    memset(c, 0, sizeof(*c)); c->rt = rt; c->used = 1; c->offer = *o;
    c->ref = (struct peer_pair_ref){rt->id, ++rt->generation, (uint32_t)(c - rt->conns)};
    c->created = rt->cfg.now_ns(rt->cfg.now_ctx); c->incoming = incoming;
    return c;
}
static void locate_channel(struct pair_conn *c)
{
    if (c->channel || !c->rt->table) return;
    for (unsigned i = 0; i < DMESH_CHANNEL_MAX; i++) {
        struct dmesh_peer_channel *ch = &c->rt->table->channels[i];
        if (ch->in_use && ch->conn == c) {
            ch->retirement_refs++;
            c->channel = ch; c->channel_incarnation = ch->incarnation; return;
        }
    }
}
static int dma_fenced(struct pair_conn *c)
{
    return !c->channel || c->rt->cfg.dma_fenced(c->rt->cfg.dma_ctx, c->channel,
                                              c->channel_incarnation) == 1;
}
static int drained(struct pair_conn *c)
{
    struct dmesh_peer_channel *ch = c->channel;
    if (ch && ch->conn == c && (ch->staging_bytes || ch->inflight_bytes ||
        ch->ack_staged || ch->tx_len || ch->rx_len)) return 0;
    return dma_fenced(c) && c->wire && c->rt->driver.send_drained(c->wire) == 1;
}
static void reset_channel(struct pair_conn *c)
{
    locate_channel(c);
    if (c->channel && c->channel->conn == c)
        dmesh_peer_reset(c->rt->table, c->channel, "Pod pair transport revoked");
    else c->released = 1;   /* no channel owns it: inbound, or prepared ahead of a stream */
}
static void completed(struct peer_pair_transport *rt, struct pair_conn *c,
                      const struct peer_pair_command *cmd, int status)
{
    struct peer_pair_event e = {.type = PEER_PAIR_COMPLETED, .ref = cmd->ref,
        .operation = cmd->operation, .command = cmd->type, .association = cmd->association, .status = status};
    if (c) { e.ref = c->ref; e.endpoint = c->endpoint; }
    /* Caller reserves one queue position before consuming a command. */
    (void)push(&rt->events, &e, sizeof(e));
}
static int prepared_pair(const struct peer_pair_offer *o, int incoming)
{
    const struct dmesh_peer_pair *p = &o->pair;
    const struct dmesh_peer_stream_open *in = &o->intent;
    return dmesh_peer_registration_valid(&p->remote) && peer_sec_nonzero(p->association, 16) &&
        p->lane_id && p->lane_generation && in->dst_port && !in->reserved &&
        text_valid(in->src_service_key, sizeof(in->src_service_key)) &&
        text_valid(in->src_pod_uid, sizeof(in->src_pod_uid)) &&
        text_valid(in->dst_pod_uid, sizeof(in->dst_pod_uid)) &&
        !strcmp(in->src_pod_uid, incoming ? p->remote_uid : p->local_uid) &&
        !strcmp(in->dst_pod_uid, incoming ? p->local_uid : p->remote_uid) &&
        in->src_generation == (incoming ? p->remote.generation : p->local.generation);
}
static int authorized(struct peer_pair_transport *rt, const struct peer_pair_offer *o)
{
    return rt->table && rt->table->ops && rt->table->ops->pair_authorize &&
        rt->table->ops->pair_authorize(rt->table->ops_ctx, &o->pair, &o->intent) == 1;
}
static int intent_equal(const struct dmesh_peer_stream_open *a, const struct dmesh_peer_stream_open *b)
{
    return !strncmp(a->src_pod_uid, b->src_pod_uid, sizeof(a->src_pod_uid)) &&
        !strncmp(a->dst_pod_uid, b->dst_pod_uid, sizeof(a->dst_pod_uid)) &&
        !strncmp(a->src_service_key, b->src_service_key, sizeof(a->src_service_key)) &&
        a->dst_port == b->dst_port && a->src_generation == b->src_generation &&
        a->source_token == b->source_token && a->reserved == b->reserved;
}
static int enable(struct pair_conn *c, const struct peer_pair_command *cmd, uint64_t now)
{
    uint8_t encoded[PEER_SEC_LANE_LEN];
    unsigned i = cmd->local_endpoint;
    const struct peer_sec_lane *lane = &cmd->lane;
    if (!c->prepared || c->enabled || cmd->hardware_ready != 1 || i > 1 || cmd->epoch != 1 ||
        !cmd->association.manager || !cmd->association.generation || !leases_valid(cmd->deadline, now) ||
        i != (unsigned)(strcmp(c->rt->table->node_name, c->offer.node) > 0) ||
        peer_sec_lane_encode(lane, encoded) || lane->worker[i] != c->rt->cfg.worker ||
        lane->id != c->offer.pair.lane_id || lane->generation != c->offer.pair.lane_generation ||
        lane->qpn[i] != c->endpoint.qpn || lane->psn[i] != c->endpoint.psn ||
        lane->mtu != c->endpoint.mtu || memcmp(lane->gid[i], c->endpoint.gid, 16)) return -1;
    struct peer_verbs_endpoint remote = {.qpn = lane->qpn[1-i], .psn = lane->psn[1-i], .mtu = lane->mtu};
    memcpy(remote.gid, lane->gid[1-i], 16);
    if (c->rt->driver.activate(c->wire, &remote, 1)) return -1;
    c->association = cmd->association; c->local = i; c->lane = *lane; c->epoch = 1; c->enabled = 1;
    /* ENABLE cannot extend an unrenewed authorization from PREPARE. */
    for (unsigned j = 0; j < 3; j++) if (cmd->deadline[j] < c->deadline[j]) c->deadline[j] = cmd->deadline[j];
    return 0;
}
static int grant(struct pair_conn *c, const struct peer_assoc_grant *g, uint64_t now)
{
    if (c->active && (g->revision <= c->grant.revision || g->epoch < c->epoch ||
        (c->paused && c->resume_required))) return 1;
    if (!c->enabled || !g->revision || g->epoch != c->epoch || c->waiting ||
        !token_equal(c->association, g->token) || !pair_equal(&c->offer.pair, &g->pair) ||
        !lane_equal(&c->lane, &g->lane) || !leases_valid(g->deadline, now)) return -1;
    for (unsigned i = 0; i < 3; i++) {
        uint64_t old_gen = c->active ? c->grant.lease_generation[i] : 0;
        if (g->lease_generation[i] < old_gen ||
            (g->deadline[i] > c->deadline[i] && g->lease_generation[i] <= old_gen)) return -1;
    }
    c->grant = *g; memcpy(c->deadline, g->deadline, sizeof(c->deadline));
    c->active = 1; c->paused = 0;
    if (c->incoming && !c->channel) {
        enum dmesh_peer_refusal why;
        struct dmesh_peer_channel *ch = dmesh_peer_accept_pair(c->rt->table, c->offer.node,
            c->offer.incarnation, c, c->offer.key, &why);
        if (!ch) return -1;
        ch->retirement_refs++;
        c->channel = ch; c->channel_incarnation = ch->incarnation;
    }
    return 0;
}
static void execute(struct peer_pair_transport *rt, const struct peer_pair_command *cmd, uint64_t now)
{
    struct pair_conn *c = lookup(rt, cmd->ref);
    int status = -1;
    if (!cmd->operation) { completed(rt, c, cmd, -1); return; }
    if (cmd->type == PEER_PAIR_AUTHORIZE) {
        struct peer_pair_event e = {.type = PEER_PAIR_COMPLETED, .command = cmd->type,
            .operation = cmd->operation, .ref = cmd->ref, .status = -1, .offer = cmd->offer};
        struct dmesh_peer_table *t = rt->table;
        /* Only inbound inquiries: the TLS owner supplies authenticated remote
         * registration/intent; this worker supplies the trusted local snapshot. */
        if (!cmd->ref.runtime && !cmd->ref.generation && !cmd->ref.slot && t && t->ops &&
            t->ops->local_registration && text_valid(e.offer.pair.local_uid, sizeof(e.offer.pair.local_uid)) &&
            t->ops->local_registration(t->ops_ctx, e.offer.pair.local_uid, &e.offer.pair.local) == 1 &&
            identity_valid(rt, &e.offer) && prepared_pair(&e.offer, 1) && authorized(rt, &e.offer))
            e.status = 0;
        if (e.status) memset(&e.offer, 0, sizeof(e.offer));
        (void)push(&rt->events, &e, sizeof(e)); return;
    }
    if (!c && cmd->type == PEER_PAIR_PREPARE && !cmd->ref.runtime && !cmd->ref.generation && !cmd->ref.slot) {
        int incoming = !cmd->outbound;
        if (cmd->operation <= rt->incoming_operation) { completed(rt, NULL, cmd, 1); return; }
        rt->incoming_operation = cmd->operation;
        if (leases_valid(cmd->deadline, now) && prepared_pair(&cmd->offer, incoming) && authorized(rt, &cmd->offer))
            c = allocate(rt, &cmd->offer, incoming);
    }
    if (!c) { completed(rt, NULL, cmd, cmd->ref.runtime ? 1 : -1); return; }
    if (cmd->operation <= c->operation) { completed(rt, c, cmd, 1); return; }
    if (cmd->type >= PEER_PAIR_GRANT && !token_equal(c->association, cmd->association)) {
        completed(rt, c, cmd, 1); return;
    }
    /* A deferred QUIESCE or DESTROY keeps its completion identity. BLOCK is
     * allowed to cancel it; the canceled operation gets a failure completion. */
    if (c->waiting) {
        if (cmd->type != PEER_PAIR_BLOCK) { completed(rt, c, cmd, -1); return; }
        struct peer_pair_command canceled = {.type = c->waiting, .operation = c->operation,
            .ref = c->ref, .association = c->association};
        completed(rt, c, &canceled, -1); c->waiting = 0;
    }
    c->operation = cmd->operation;
    locate_channel(c);
    if (cmd->type < PEER_PAIR_BLOCK && !live(c)) goto done;
    switch (cmd->type) {
    case PEER_PAIR_PREPARE:
        if (c->prepared || !leases_valid(cmd->deadline, now) || !identity_valid(rt, &cmd->offer) ||
            !authorized(rt, &cmd->offer) ||
            !prepared_pair(&cmd->offer, c->incoming) || strcmp(c->offer.node, cmd->offer.node) ||
            strcmp(c->offer.pair.local_uid, cmd->offer.pair.local_uid) ||
            strcmp(c->offer.pair.remote_uid, cmd->offer.pair.remote_uid) ||
            !dmesh_peer_registration_equal(&c->offer.pair.local, &cmd->offer.pair.local) ||
            memcmp(c->offer.key, cmd->offer.key, 32) || c->offer.ip_be != cmd->offer.ip_be ||
            c->offer.port != cmd->offer.port || c->offer.incarnation != cmd->offer.incarnation ||
            !intent_equal(&c->offer.intent, &cmd->offer.intent)) break;
        c->offer = cmd->offer; memcpy(c->deadline, cmd->deadline, sizeof(c->deadline));
        if (rt->driver.prepare(rt->driver.ctx, &c->wire, &c->endpoint)) {
            status = c->wire ? -2 : -1; c->quarantine = !!c->wire; break;
        }
        c->prepared = 1; status = 0; break;
    case PEER_PAIR_ENABLE: status = enable(c, cmd, now); break;
    case PEER_PAIR_GRANT: status = grant(c, &cmd->grant, now); break;
    case PEER_PAIR_QUIESCE:
        if (!c->active || c->paused || !token_equal(c->association, cmd->association)) break;
        c->paused = 1; c->resume_required = 1;
        if (!drained(c)) { c->waiting = cmd->type; return; }
        status = 0; break;
    case PEER_PAIR_RESUME:
        if (!c->active || !c->paused || !token_equal(c->association, cmd->association) ||
            c->epoch == UINT64_MAX || cmd->epoch != c->epoch + 1 || !drained(c)) break;
        c->epoch = cmd->epoch; c->resume_required = 0; status = 0; break;
    case PEER_PAIR_BLOCK:
        fault(c); reset_channel(c); c->blocked = 1; status = 0; break;
    case PEER_PAIR_DESTROY:
        if (!c->blocked || cmd->hardware_ready != 1) break;
        if (c->wire && rt->driver.close(c->wire)) { c->quarantine = 1; status = -2; break; }
        c->wire = NULL; c->quarantine = 0;
        if (!dma_fenced(c)) { c->waiting = cmd->type; return; }
        c->destroyed = 1; status = 0; break;
    case PEER_PAIR_FORGET:
        if (!c->released || !c->destroyed || c->wire || c->quarantine || !dma_fenced(c)) break;
        if (c->channel) c->channel->retirement_refs--;
        completed(rt, c, cmd, 0); memset(c, 0, sizeof(*c)); return;
    default: break;
    }
done:
    if (status < 0) fault(c);
    completed(rt, c, cmd, status);
}
static int connect_pair(void *ctx, const char *node, const uint8_t key[32], uint32_t ip, uint16_t port,
    uint32_t incarnation, const struct dmesh_peer_pair *pair, const struct dmesh_peer_stream_open *intent, void **out)
{
    struct peer_pair_transport *rt = ctx;
    if (!out) return -1;
    *out = NULL;
    if (!rt || !node || !key || !pair || !intent || !room(&rt->events) ||
        strnlen(node, DMESH_K8S_NAME_MAX) == DMESH_K8S_NAME_MAX) return -1;
    struct peer_pair_offer o = {.ip_be = ip, .port = port, .incarnation = incarnation, .pair = *pair, .intent = *intent};
    memcpy(o.node, node, strlen(node) + 1); memcpy(o.key, key, 32);
    /* A lane the node owner prepared ahead of this worker's first stream is
     * adopted rather than duplicated. The new intent is authorized afresh: the
     * lane certifies the Pod pair, not every Service claim made over it. */
    for (unsigned i = 0; i < rt->cfg.connections; i++) {
        struct pair_conn *c = &rt->conns[i];
        if (!c->used || c->incoming || c->channel || c->fault || c->released ||
            strcmp(c->offer.node, o.node) || strcmp(c->offer.pair.local_uid, o.pair.local_uid) ||
            strcmp(c->offer.pair.remote_uid, o.pair.remote_uid)) continue;
        if (memcmp(c->offer.key, o.key, 32) || c->offer.ip_be != o.ip_be || c->offer.port != o.port ||
            !dmesh_peer_registration_equal(&c->offer.pair.local, &o.pair.local) ||
            !authorized(rt, &o) || !live(c)) return -1;
        *out = c; return 0;
    }
    struct pair_conn *c = allocate(rt, &o, 0);
    if (!c) return -1;
    struct peer_pair_event e = {.type = PEER_PAIR_OPEN, .ref = c->ref, .offer = o};
    (void)push(&rt->events, &e, sizeof(e)); *out = c; return 0;
}
static int pair_ready(void *conn, struct dmesh_peer_pair *out)
{
    struct pair_conn *c = conn;
    if (!c || !out || !live(c)) return -1;
    if (!c->active) return 0;
    *out = c->grant.pair; return 1;
}
static int pair_admit(void *conn)
{ struct pair_conn *c = conn; return c && live(c) && c->active && !c->paused; }
static int peer_key(void *conn, uint8_t key[32])
{
    struct pair_conn *c = conn;
    if (!c || !key || !live(c) || !c->active) return -1;
    memcpy(key, c->offer.key, 32); return 0;
}
static long send_frame(void *conn, const void *buf, size_t len)
{
    struct pair_conn *c = conn;
    if (!c || !live(c) || !c->active || !buf || !len || len > PEER_WIRE_MSG_MAX) return -1;
    int n = c->rt->driver.wire->send_msg(c->wire, buf, len);
    if (n < 0) fault(c);
    return n == 1 ? (long)len : n;
}
static long recv_frame(void *conn, void *buf, size_t cap)
{
    struct pair_conn *c = conn;
    if (!c || !live(c) || !c->active) return -1;
    long n = c->rt->driver.wire->recv_msg(c->wire, buf, cap);
    if (n < 0) fault(c);
    return n;
}
static void close_conn(void *conn)
{
    struct pair_conn *c = conn;
    if (!c || !c->used) return;
    locate_channel(c); c->released = 1; fault(c);
    /* QP remains owned until the node owner confirms hardware BLOCK. */
}
static int progress(void *ctx)
{
    struct peer_pair_transport *rt = ctx;
    uint64_t now = rt->cfg.now_ns(rt->cfg.now_ctx);
    int accepted = 0, moved = 0;
    void *unexpected = NULL;
    int n = rt->driver.wire->progress(rt->driver.ctx, &unexpected, 1, &accepted);
    if (n > 0) moved = 1;
    if (accepted && unexpected) rt->driver.wire->close(unexpected);
    for (unsigned i = 0; i < rt->cfg.connections; i++) {
        struct pair_conn *c = &rt->conns[i];
        if (!c->used) continue;
        locate_channel(c);
        if (n < 0 || accepted || now < rt->last_now) fault(c);
        (void)live(c);
        if (c->waiting && room(&rt->events)) {
            int done = c->waiting == PEER_PAIR_QUIESCE ? drained(c) : dma_fenced(c);
            if (done || c->fault) {
                struct peer_pair_command cmd = {.type = c->waiting, .operation = c->operation,
                    .ref = c->ref, .association = c->association};
                /* A DESTROY already expects a revoked channel; that is not
                 * permission to report a host DMA fence before it completes. */
                if (c->waiting != PEER_PAIR_DESTROY || done) {
                    if (cmd.type == PEER_PAIR_DESTROY) c->destroyed = 1;
                    completed(rt, c, &cmd, done ? 0 : -1); c->waiting = 0; moved = 1;
                }
            }
        }
        if (c->fault) reset_channel(c);
        if (c->notify_revoke && room(&rt->events)) {
            struct peer_pair_event e = {.type = PEER_PAIR_REVOKE, .ref = c->ref, .association = c->association};
            (void)push(&rt->events, &e, sizeof(e)); c->notify_revoke = 0; moved = 1;
        }
    }
    if (now >= rt->last_now) rt->last_now = now;
    struct peer_pair_command cmd;
    for (unsigned i = 0; i < PEER_PAIR_MAILBOX_CAP && available(&rt->events) >= 2 &&
        pop(&rt->commands, &cmd, sizeof(cmd)); i++) { execute(rt, &cmd, now); moved = 1; }
    return moved;
}
static int pending(void *ctx)
{
    struct peer_pair_transport *rt = ctx;
    if (queued(&rt->commands)) return 1;
    for (unsigned i = 0; i < rt->cfg.connections; i++) {
        struct pair_conn *c = &rt->conns[i];
        if (c->used && (c->waiting || c->notify_revoke || (!c->fault && !c->active))) return 1;
    }
    return 0;
}
static const struct dmesh_peer_transport OPS = {.peer_key = peer_key, .send = send_frame, .recv = recv_frame,
    .close = close_conn, .connect_pair = connect_pair, .pair_ready = pair_ready, .pair_admit = pair_admit,
    .progress = progress, .pending = pending};
const struct dmesh_peer_transport *peer_pair_transport_ops(void) { return &OPS; }
int peer_pair_transport_epfd(struct peer_pair_transport *rt) { return rt ? rt->epfd : -1; }
int peer_pair_manager_fd(struct peer_pair_transport *rt) { return rt ? rt->events.fd : -1; }
int peer_pair_manager_submit(struct peer_pair_transport *rt, const struct peer_pair_command *cmd)
{ return rt && cmd ? push(&rt->commands, cmd, sizeof(*cmd)) : -1; }
int peer_pair_manager_poll(struct peer_pair_transport *rt, struct peer_pair_event *event)
{
    if (!event) return -1;
    memset(event, 0, sizeof(*event));
    return rt ? pop(&rt->events, event, sizeof(*event)) : -1;
}
void peer_pair_transport_attach(struct peer_pair_transport *rt, struct dmesh_peer_table *t)
{ if (rt) rt->table = t; }
int peer_pair_transport_new_driver(const struct peer_pair_transport_config *cfg,
    const struct peer_pair_driver *d, struct peer_pair_transport **out)
{
    if (!out) return -1;
    *out = NULL;
    if (!cfg || cfg->worker >= PEER_ASSOC_LANES_MAX || !cfg->connections || cfg->connections > DMESH_CHANNEL_MAX ||
        !cfg->setup_timeout_ns || !cfg->dma_fenced || !d || !d->ctx || !d->wire ||
        !d->prepare || !d->activate || !d->send_drained || !d->close || !d->wire->progress ||
        !d->wire->send_msg || !d->wire->recv_msg || !d->wire->faulted || !d->wire->ctx_free ||
        !d->wire->close || !d->wire->epfd) return -1;
    struct peer_pair_transport *rt = calloc(1, sizeof(*rt));
    if (!rt) return -1;
    rt->epfd = rt->commands.fd = rt->events.fd = -1; rt->cfg = *cfg; rt->driver = *d;
    if (!rt->cfg.now_ns) rt->cfg.now_ns = clock_ns;
    ssize_t n;
    do { n = getrandom(&rt->id, sizeof(rt->id), 0); } while (n < 0 && errno == EINTR);
    if (n != sizeof(rt->id) || !rt->id) goto fail;
    rt->conns = calloc(cfg->connections, sizeof(*rt->conns));
    if (!rt->conns) goto fail;
    rt->commands.fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    rt->events.fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    rt->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (rt->commands.fd < 0 || rt->events.fd < 0 || rt->epfd < 0) goto fail;
    struct epoll_event e = {.events = EPOLLIN};
    if (epoll_ctl(rt->epfd, EPOLL_CTL_ADD, rt->commands.fd, &e)) goto fail;
    int carrier = d->wire->epfd(d->ctx);
    if (carrier >= 0 && epoll_ctl(rt->epfd, EPOLL_CTL_ADD, carrier, &e)) goto fail;
    atomic_init(&rt->commands.put, 0); atomic_init(&rt->commands.get, 0);
    atomic_init(&rt->events.put, 0); atomic_init(&rt->events.get, 0);
    rt->last_now = rt->cfg.now_ns(rt->cfg.now_ctx);
    *out = rt; return 0;
fail:
    if (rt->epfd >= 0) close(rt->epfd);
    if (rt->commands.fd >= 0) close(rt->commands.fd);
    if (rt->events.fd >= 0) close(rt->events.fd);
    free(rt->conns); free(rt); return -1;
}
int peer_pair_transport_free(struct peer_pair_transport *rt)
{
    if (!rt) return 0;
    for (unsigned i = 0; i < rt->cfg.connections; i++) if (rt->conns[i].used) return -1;
    if (queued(&rt->commands) || queued(&rt->events)) return -1;
    close(rt->epfd); close(rt->commands.fd); close(rt->events.fd);
    rt->driver.wire->ctx_free(rt->driver.ctx); free(rt->conns); free(rt); return 0;
}
