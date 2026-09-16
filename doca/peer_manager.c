#include "peer_manager.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#define NOTICE_CAP 64u
#define SECOND_NS 1000000000ull
#ifdef PEER_MANAGER_TRACE
#include <stdio.h>
#define TRACE(...) do { fprintf(stderr, "peer_manager: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#else
#define TRACE(...) do { } while (0)
#endif

/* A pair is one Pod-pair association from the worker OPEN (or the peer's
 * PROPOSE) that started it until every lane is forgotten. Setup states run the
 * negotiation; OPEN hands the sequence to the registry and only routes what it
 * emits; the two teardown states retire the lanes once nothing else can. */
enum pair_state {
    PAIR_WAIT_SESSION = 1, PAIR_PROPOSED, PAIR_AUTHORIZING, PAIR_PREPARING,
    PAIR_WAIT_PARAMS, PAIR_OPEN, PAIR_FORGET, PAIR_CANCEL
};
struct lane {
    unsigned worker;
    struct peer_pair_ref ref;             /* zero until a worker owns a lane conn */
    uint32_t incarnation;                 /* the conn's own, when a stream opened it */
    struct dmesh_peer_stream_open intent; /* likewise */
    struct peer_verbs_endpoint endpoint;
    uint64_t op;                          /* outstanding worker operation */
    int done, status;                     /* current round */
    int prepared, enabled, quarantined, stuck;
    enum peer_pair_command_type cleanup;  /* next teardown step, 0 none */
};
struct pair {
    int used, proposer;
    enum pair_state state;
    unsigned local, session;
    uint64_t session_id;
    char node[DMESH_K8S_NAME_MAX];
    uint8_t key[32];
    uint32_t ip_be, incarnation;
    uint16_t port;
    struct dmesh_peer_pair pair;
    struct dmesh_peer_stream_open intent;
    uint8_t association[16];
    uint32_t spi[2];
    unsigned lane_count, origin;
    struct peer_sec_lane lanes[PEER_SEC_LANES_MAX];
    struct lane lane[PEER_SEC_LANES_MAX];
    uint64_t deadline[3], generation[3], created;
    struct peer_assoc_token token;
    struct peer_crypto_request crypto;
    int crypto_inflight, crypto_submitted, hw_done, hw_status;
    enum peer_pair_command_type round;
    int regrant, granted, rekeying, send_params;
    uint64_t epoch, policy_op, rekey_at;
    uint32_t rekey_spi[2];
};
struct session {
    int used, initiator;
    uint64_t id;
    char node[DMESH_K8S_NAME_MAX];
    struct peer_control *control;
    struct peer_manager *m;
};
struct worker { struct peer_pair_transport *rt; uint64_t next_op; };
struct notice { int kind; char uid[DMESH_POD_UID_MAX]; uint64_t generation, deadline; };
struct peer_manager {
    struct peer_manager_config cfg;
    uint8_t public_key[32];
    struct peer_tls_ctx *tls;
    struct peer_associations *registry;
    struct session sessions[PEER_MANAGER_SESSIONS_MAX];
    struct worker workers[PEER_ASSOC_LANES_MAX];
    struct pair *pairs;
    unsigned pair_cap;
    int crypto_down;
    uint64_t session_ids, lane_generation, authority_generation, authority_deadline, last_now;
    int epfd, notify_fd;
    pthread_mutex_t notify_mu;
    struct notice notices[NOTICE_CAP];
    unsigned notice_head, notice_count;
    pthread_t thread;
    int running;
    atomic_int stop;
    struct peer_manager_stats stats;
};

static int pair_rekey_start(struct peer_manager *m, struct pair *p, uint64_t now);
static uint64_t clock_ns(void *unused)
{
    (void)unused;
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * SECOND_NS + (uint64_t)t.tv_nsec;
}
static uint64_t after(uint64_t now, uint64_t delta)
{ return delta > UINT64_MAX - now ? UINT64_MAX : now + delta; }
static int token_equal(struct peer_assoc_token a, struct peer_assoc_token b)
{ return a.manager == b.manager && a.generation == b.generation && a.slot == b.slot; }
static int ref_set(struct peer_pair_ref r) { return r.runtime || r.generation || r.slot; }
static void wake(int fd)
{
    uint64_t one = 1; ssize_t n;
    do { n = write(fd, &one, sizeof(one)); } while (n < 0 && errno == EINTR);
}

/* ---- pairs ------------------------------------------------------------- */

static struct pair *pair_find(struct peer_manager *m, const char *node, const char *local_uid,
                              const char *remote_uid)
{
    for (unsigned i = 0; i < m->pair_cap; i++) {
        struct pair *p = &m->pairs[i];
        if (p->used && !strcmp(p->node, node) && !strcmp(p->pair.local_uid, local_uid) &&
            !strcmp(p->pair.remote_uid, remote_uid)) return p;
    }
    return NULL;
}
static struct pair *pair_by_association(struct peer_manager *m, const uint8_t association[16])
{
    for (unsigned i = 0; i < m->pair_cap; i++)
        if (m->pairs[i].used && !memcmp(m->pairs[i].association, association, 16)) return &m->pairs[i];
    return NULL;
}
static struct pair *pair_by_token(struct peer_manager *m, struct peer_assoc_token t)
{
    for (unsigned i = 0; i < m->pair_cap; i++)
        if (m->pairs[i].used && m->pairs[i].state >= PAIR_OPEN && token_equal(m->pairs[i].token, t))
            return &m->pairs[i];
    return NULL;
}
static int spi_taken(struct peer_manager *m, uint32_t spi)
{
    for (unsigned i = 0; i < m->pair_cap; i++) {
        struct pair *p = &m->pairs[i];
        if (p->used && (p->spi[1-p->local] == spi || p->rekey_spi[1-p->local] == spi)) return 1;
    }
    return 0;
}
static uint32_t spi_reserve(struct peer_manager *m)
{
    for (unsigned n = 0; n < 64; n++) {
        uint32_t spi;
        if (RAND_bytes((uint8_t *)&spi, sizeof(spi)) != 1) return 0;
        if (spi >= 256 && !spi_taken(m, spi)) return spi;
    }
    return 0;
}
static struct pair *pair_alloc(struct peer_manager *m, uint64_t now)
{
    for (unsigned i = 0; i < m->pair_cap; i++) {
        struct pair *p = &m->pairs[i];
        if (p->used) continue;
        memset(p, 0, sizeof(*p)); p->used = 1; p->created = now;
        m->stats.pairs++; return p;
    }
    return NULL;
}
static void pair_free(struct peer_manager *m, struct pair *p)
{
    if (p->state >= PAIR_OPEN) (void)peer_association_release(m->registry, p->token);
    peer_crypto_request_cleanse(&p->crypto);
    memset(p, 0, sizeof(*p)); m->stats.pairs--;
}
static struct session *pair_session(struct peer_manager *m, struct pair *p)
{
    struct session *s = &m->sessions[p->session];
    return s->used && s->id == p->session_id ? s : NULL;
}
/* A lane slot counts once the lane count is fixed, or earlier while it holds
 * the conn of a stream that opened before PROPOSE fixed the count. */
static int lane_live(const struct pair *p, unsigned i)
{ return i < p->lane_count || ref_set(p->lane[i].ref); }
static struct lane *pair_lane_for_worker(struct pair *p, unsigned worker)
{
    for (unsigned i = 0; i < PEER_SEC_LANES_MAX; i++)
        if (lane_live(p, i) && p->lane[i].worker == worker) return &p->lane[i];
    return NULL;
}

/* ---- workers ----------------------------------------------------------- */

static uint64_t worker_submit(struct peer_manager *m, unsigned w, struct peer_pair_command *cmd)
{
    struct worker *k = &m->workers[w];
    if (w >= m->cfg.workers || !k->rt || k->next_op == UINT64_MAX) return 0;
    cmd->operation = k->next_op + 1;
    if (peer_pair_manager_submit(k->rt, cmd) != 1) return 0;
    return ++k->next_op;
}
/* The offer a worker sees for lane i: the pair's identity plus the intent and
 * incarnation of the stream that opened this lane, when one did. */
static void pair_offer(struct pair *p, unsigned i, struct peer_pair_offer *o)
{
    memset(o, 0, sizeof(*o));
    strcpy(o->node, p->node); memcpy(o->key, p->key, 32);
    o->ip_be = p->ip_be; o->port = p->port; o->pair = p->pair;
    memcpy(o->pair.association, p->association, 16);
    o->pair.lane_id = p->lanes[i].id; o->pair.lane_generation = p->lanes[i].generation;
    if (p->lane[i].incarnation) { o->incarnation = p->lane[i].incarnation; o->intent = p->lane[i].intent; }
    else { o->incarnation = p->incarnation; o->intent = p->intent; }
}
static int lane_submit(struct peer_manager *m, struct pair *p, unsigned i,
                       enum peer_pair_command_type type, uint64_t now)
{
    struct lane *l = &p->lane[i];
    struct peer_pair_command cmd = {.type = type, .ref = l->ref};
    if (l->enabled) cmd.association = p->token;
    switch (type) {
    case PEER_PAIR_AUTHORIZE:
        pair_offer(p, 0, &cmd.offer); memset(&cmd.offer.pair.local, 0, sizeof(cmd.offer.pair.local));
        memset(&cmd.ref, 0, sizeof(cmd.ref)); break;
    case PEER_PAIR_PREPARE:
        pair_offer(p, i, &cmd.offer); memcpy(cmd.deadline, p->deadline, sizeof(cmd.deadline));
        cmd.outbound = p->proposer && !ref_set(l->ref); break;
    case PEER_PAIR_ENABLE:
        cmd.association = p->token; cmd.local_endpoint = p->local; cmd.hardware_ready = 1;
        cmd.epoch = 1; cmd.lane = p->lanes[i]; memcpy(cmd.deadline, p->deadline, sizeof(cmd.deadline)); break;
    case PEER_PAIR_GRANT:
        if (peer_association_grant(m->registry, p->token, l->worker, now, &cmd.grant) != 1) return -1;
        break;
    case PEER_PAIR_RESUME: cmd.epoch = p->crypto.binding.epoch; break;
    case PEER_PAIR_DESTROY: cmd.hardware_ready = 1; break;
    default: break;
    }
    uint64_t op = worker_submit(m, l->worker, &cmd);
    if (!op) return 0;
    l->op = op; l->done = 0; return 1;
}
static int round_start(struct peer_manager *m, struct pair *p, enum peer_pair_command_type type)
{
    p->round = type;
    for (unsigned i = 0; i < p->lane_count; i++) { p->lane[i].done = !ref_set(p->lane[i].ref); p->lane[i].status = 0; }
    (void)m; return 0;
}
/* Submit what the round still owes; 1 when every lane has answered. */
static int round_progress(struct peer_manager *m, struct pair *p, uint64_t now, int *status)
{
    int complete = 1, worst = 0;
    for (unsigned i = 0; i < p->lane_count; i++) {
        struct lane *l = &p->lane[i];
        if (l->done) { if (l->status < worst) worst = l->status; continue; }
        complete = 0;
        if (!l->op) {
            int rc = lane_submit(m, p, i, p->round, now);
            if (rc < 0 && p->round != PEER_PAIR_GRANT) { l->done = 1; l->status = -1; }
        }
    }
    if (complete && status) *status = worst;
    return complete;
}

/* ---- sessions ---------------------------------------------------------- */

static int message_cb(void *ctx, const struct peer_sec_header *h, const uint8_t *body);
static int resolve_cb(void *ctx, const uint8_t key[32], char node[PEER_SEC_NAME_MAX])
{
    struct peer_manager *m = ((struct session *)ctx)->m;
    char name[DMESH_K8S_NAME_MAX];
    if (m->cfg.ops->node_by_key(m->cfg.ops_ctx, key, name) != 1 || !name[0] ||
        strlen(name) >= PEER_SEC_NAME_MAX) return 0;
    strcpy(node, name); return 1;
}
static struct session *session_find(struct peer_manager *m, const char *node, int initiator)
{
    for (unsigned i = 0; i < PEER_MANAGER_SESSIONS_MAX; i++) {
        struct session *s = &m->sessions[i];
        if (s->used && s->initiator == initiator && !strcmp(s->node, node)) return s;
    }
    return NULL;
}
static struct session *session_slot(struct peer_manager *m)
{
    for (unsigned i = 0; i < PEER_MANAGER_SESSIONS_MAX; i++)
        if (!m->sessions[i].used) return &m->sessions[i];
    return NULL;
}
static void session_config(struct peer_manager *m, struct session *s, struct peer_control_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->local.cluster, m->cfg.cluster); strcpy(cfg->local.node, m->cfg.node);
    memcpy(cfg->local.public_key, m->public_key, 32); memcpy(cfg->local.boot, m->cfg.boot, 16);
    cfg->local.nonce[0] = 1; cfg->local.workers = (uint16_t)m->cfg.workers;
    cfg->authority_deadline_ns = m->authority_deadline;
    cfg->setup_timeout_ns = m->cfg.setup_timeout_ns; cfg->control_lease_ns = m->cfg.control_lease_ns;
    cfg->message = message_cb; cfg->message_ctx = s;
    cfg->resolve = resolve_cb; cfg->resolve_ctx = s;
}
static struct session *session_open(struct peer_manager *m, const char *node, const uint8_t *key,
                                    void *wire, int initiator, uint64_t now)
{
    struct session *s = session_slot(m);
    if (!s || m->session_ids == UINT64_MAX || m->authority_deadline <= now) return NULL;
    memset(s, 0, sizeof(*s)); s->m = m; s->initiator = initiator;
    struct peer_control_config cfg; session_config(m, s, &cfg);
    if (node) { strcpy(cfg.remote_node, node); memcpy(cfg.remote_key, key, 32); strcpy(s->node, node); }
    if (peer_control_new(&cfg, m->tls, initiator, m->cfg.wire, wire, now, &s->control)) return NULL;
    s->used = 1; s->id = ++m->session_ids; m->stats.sessions++; return s;
}
static struct session *session_dial(struct peer_manager *m, const char *node, uint64_t now)
{
    const uint8_t *key = NULL; uint32_t ip = 0; uint16_t port = 0; void *wire = NULL;
    if (m->cfg.ops->node_binding(m->cfg.ops_ctx, node, &key, &ip, &port) != 1 || !key ||
        m->cfg.wire->connect(m->cfg.wire_ctx, ip, port, &wire) || !wire) return NULL;
    struct session *s = session_open(m, node, key, wire, 1, now);
    if (!s) m->cfg.wire->close(wire);
    return s;
}
static void session_close(struct peer_manager *m, struct session *s)
{
    peer_control_free(s->control); memset(s, 0, sizeof(*s)); m->stats.sessions--;
}
static int session_referenced(struct peer_manager *m, struct session *s)
{
    for (unsigned i = 0; i < m->pair_cap; i++)
        if (m->pairs[i].used && m->pairs[i].session == (unsigned)(s - m->sessions) &&
            m->pairs[i].session_id == s->id) return 1;
    return 0;
}
static int session_send(struct peer_manager *m, struct pair *p, uint16_t type, uint64_t epoch,
                        const void *body, size_t len, uint64_t now)
{
    struct session *s = pair_session(m, p);
    if (!s) return -1;
    return peer_control_send(s->control, type, p->association, epoch, body, len, now);
}
static void session_error(struct peer_manager *m, struct pair *p, uint16_t code, uint64_t now)
{
    uint8_t body[PEER_SEC_ERROR_LEN];
    if (!peer_sec_error_encode(code, body))
        (void)session_send(m, p, PEER_SEC_ERROR, 1, body, sizeof(body), now);
}

/* ---- setup and teardown ------------------------------------------------ */

static void pair_cleanup_start(struct pair *p, enum peer_pair_command_type first)
{
    for (unsigned i = 0; i < PEER_SEC_LANES_MAX; i++)
        if (ref_set(p->lane[i].ref) && !p->lane[i].quarantined && !p->lane[i].stuck) {
            p->lane[i].cleanup = first; p->lane[i].op = 0;
        }
}
/* Setup aborted before the registry owns anything. */
static void pair_cancel(struct peer_manager *m, struct pair *p, uint16_t code, uint64_t now)
{
    if (p->state >= PAIR_OPEN) return;
    TRACE("%s: cancel pair %s<->%s state=%d code=%u", m->cfg.node, p->pair.local_uid, p->pair.remote_uid, p->state, code);
    if (code && p->state != PAIR_WAIT_SESSION) session_error(m, p, code, now);
    m->stats.refused++;
    p->state = PAIR_CANCEL; p->round = 0;
    pair_cleanup_start(p, PEER_PAIR_BLOCK);
}
static void pair_revoke(struct peer_manager *m, struct pair *p, uint64_t now)
{
    if (p->state >= PAIR_OPEN) (void)peer_association_revoke(m->registry, p->token);
    else pair_cancel(m, p, PEER_SEC_ERR_REVOKED, now);
}
static int pair_propose(struct peer_manager *m, struct pair *p, struct session *s, uint64_t now)
{
    const struct peer_sec_hello *remote = peer_control_remote(s->control);
    if (!remote) return 0;
    unsigned n = remote->workers < m->cfg.workers ? remote->workers : m->cfg.workers;
    if (!n || p->origin >= n) return -1;
    struct peer_sec_propose body = {0};
    strcpy(body.cluster, m->cfg.cluster);
    strcpy(body.source_pod, p->pair.local_uid); strcpy(body.target_pod, p->pair.remote_uid);
    body.source = p->pair.local; memcpy(body.association, p->association, 16);
    body.rx_spi = p->spi[1-p->local]; body.incarnation = p->incarnation;
    strcpy(body.service, p->intent.src_service_key);
    body.port = p->intent.dst_port; body.mtu = m->cfg.mtu; body.lane_count = (uint16_t)n;
    if (m->lane_generation == UINT64_MAX) return -1;
    uint64_t generation = m->lane_generation + 1;
    for (unsigned i = 0; i < n; i++) {
        body.lane[i].id = i + 1; body.lane[i].generation = generation;
        body.lane[i].worker[0] = body.lane[i].worker[1] = (uint16_t)i;
    }
    uint8_t bytes[PEER_SEC_BODY_MAX]; size_t len;
    if (peer_sec_propose_encode(&body, bytes, sizeof(bytes), &len)) return -1;
    int rc = peer_control_send(s->control, PEER_SEC_PROPOSE, p->association, 1, bytes, len, now);
    if (rc != 1) return rc;
    m->lane_generation = generation;
    p->lane_count = n;
    p->deadline[2] = peer_control_lease_deadline(s->control);
    for (unsigned i = 0; i < n; i++) {
        p->lanes[i] = (struct peer_sec_lane){.id = i + 1, .generation = generation, .mtu = m->cfg.mtu};
        p->lanes[i].worker[0] = p->lanes[i].worker[1] = (uint16_t)i;
        p->lane[i].worker = i;
    }
    return 1;
}
static void pair_fill_half(struct pair *p, unsigned side, const struct peer_sec_half_lane *h, unsigned i)
{
    p->lanes[i].qpn[side] = h->qpn; p->lanes[i].psn[side] = h->psn;
    memcpy(p->lanes[i].gid[side], h->gid, 16);
}
static int pair_halves_match(const struct pair *p, const struct peer_sec_lane_params *h)
{
    if (h->lane_count != p->lane_count || h->mtu != p->lanes[0].mtu) return 0;
    for (unsigned i = 0; i < p->lane_count; i++)
        if (h->lane[i].id != p->lanes[i].id || h->lane[i].generation != p->lanes[i].generation) return 0;
    return 1;
}
static int pair_local_halves(struct pair *p, struct peer_sec_lane_params *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->association, p->association, 16);
    out->mtu = p->lanes[0].mtu; out->lane_count = (uint16_t)p->lane_count;
    for (unsigned i = 0; i < p->lane_count; i++) {
        struct lane *l = &p->lane[i];
        if (!l->prepared) return -1;
        out->lane[i] = (struct peer_sec_half_lane){.id = p->lanes[i].id, .generation = p->lanes[i].generation,
            .qpn = l->endpoint.qpn, .psn = l->endpoint.psn};
        memcpy(out->lane[i].gid, l->endpoint.gid, 16);
        pair_fill_half(p, p->local, &out->lane[i], i);
    }
    return 0;
}
static int pair_open_association(struct peer_manager *m, struct pair *p, uint64_t now)
{
    struct session *s = pair_session(m, p);
    struct peer_sec_binding b = {0};
    struct peer_sec_lane_params local_halves;
    const struct peer_sec_hello *remote = s ? peer_control_remote(s->control) : NULL;
    if (!remote || pair_local_halves(p, &local_halves)) return -1;
    strcpy(b.cluster, m->cfg.cluster);
    struct peer_sec_endpoint *e[2] = {&b.endpoint[p->local], &b.endpoint[1-p->local]};
    const struct dmesh_peer_registration *r[2] = {&p->pair.local, &p->pair.remote};
    const char *pods[2] = {p->pair.local_uid, p->pair.remote_uid};
    strcpy(e[0]->node, m->cfg.node); memcpy(e[0]->public_key, m->public_key, 32); memcpy(e[0]->boot, m->cfg.boot, 16);
    strcpy(e[1]->node, remote->node); memcpy(e[1]->public_key, remote->public_key, 32); memcpy(e[1]->boot, remote->boot, 16);
    for (unsigned i = 0; i < 2; i++) {
        strcpy(e[i]->pod, pods[i]); memcpy(e[i]->daemon, r[i]->daemon, sizeof(e[i]->daemon));
        e[i]->channel_slot = r[i]->slot; e[i]->channel_generation = r[i]->generation;
        memcpy(e[i]->registration_nonce, r[i]->nonce, sizeof(e[i]->registration_nonce));
    }
    memcpy(b.session, peer_control_session(s->control), 16); memcpy(b.association, p->association, 16);
    b.epoch = 1; b.spi[0] = p->spi[0]; b.spi[1] = p->spi[1];
    if (!peer_control_binding_valid(s->control, &b, p->local, now)) {
        TRACE("%s: binding invalid (valid=%d)", m->cfg.node, peer_sec_binding_valid(&b)); return -1;
    }
    if (peer_association_open(m->registry, peer_control_tls(s->control, now), &b, p->local,
                              p->lanes, p->lane_count, p->deadline, now, &p->token)) {
        TRACE("%s: association open refused: deadlines %llu %llu %llu now %llu, lane0 qpn %u/%u gid %d/%d",
              m->cfg.node, (unsigned long long)p->deadline[0], (unsigned long long)p->deadline[1],
              (unsigned long long)p->deadline[2], (unsigned long long)now, p->lanes[0].qpn[0], p->lanes[0].qpn[1],
              peer_sec_nonzero(p->lanes[0].gid[0], 16), peer_sec_nonzero(p->lanes[0].gid[1], 16));
        return -1;
    }
    TRACE("%s: association open %s<->%s local=%u lanes=%u", m->cfg.node, p->pair.local_uid, p->pair.remote_uid, p->local, p->lane_count);
    p->state = PAIR_OPEN; p->epoch = 1; return 0;
}
static int pair_send_halves(struct peer_manager *m, struct pair *p, int accept, uint64_t now)
{
    struct peer_sec_lane_params h;
    uint8_t bytes[PEER_SEC_BODY_MAX]; size_t len;
    if (pair_local_halves(p, &h)) return -1;
    if (accept) { h.rx_spi = p->spi[1-p->local]; h.registration = p->pair.local; }
    if (peer_sec_lane_params_encode(&h, accept, bytes, sizeof(bytes), &len)) return -1;
    return session_send(m, p, accept ? PEER_SEC_ACCEPT : PEER_SEC_LANE_PARAMS, 1, bytes, len, now);
}

/* A worker's stream opened a Pod-pair lane: start a proposal, or attach the
 * lane to a proposal already under way for the same pair. */
static void pair_open_event(struct peer_manager *m, unsigned w, const struct peer_pair_event *e, uint64_t now)
{
    const struct peer_pair_offer *o = &e->offer;
    struct pair *p = pair_find(m, o->node, o->pair.local_uid, o->pair.remote_uid);
    if (p) {
        struct lane *l = NULL;
        if (p->state == PAIR_WAIT_SESSION && w < PEER_SEC_LANES_MAX) l = &p->lane[w];
        else if (p->state == PAIR_PROPOSED || p->state == PAIR_PREPARING) l = pair_lane_for_worker(p, w);
        if (!p->proposer || !l || ref_set(l->ref) || l->prepared ||
            !dmesh_peer_registration_equal(&p->pair.local, &o->pair.local)) return;
        l->ref = e->ref; l->incarnation = o->incarnation; l->intent = o->intent; l->worker = w;
        /* A zero-ref PREPARE already in flight on this worker loses to this
         * conn; its failure is retried with the ref (lane_completed). */
        return;
    }
    p = pair_alloc(m, now);
    if (!p) { m->stats.refused++; return; }
    p->proposer = 1; p->state = PAIR_WAIT_SESSION; p->origin = w;
    p->local = strcmp(m->cfg.node, o->node) > 0;
    strcpy(p->node, o->node); memcpy(p->key, o->key, 32);
    p->ip_be = o->ip_be; p->port = o->port; p->incarnation = o->incarnation;
    p->pair = o->pair; memset(p->pair.association, 0, 16); p->pair.lane_id = p->pair.lane_generation = 0;
    p->intent = o->intent;
    uint32_t spi = spi_reserve(m);
    if (RAND_bytes(p->association, 16) != 1 || !peer_sec_nonzero(p->association, 16) || !spi ||
        w >= PEER_SEC_LANES_MAX) { pair_free(m, p); m->stats.refused++; return; }
    p->spi[1-p->local] = spi;
    p->deadline[0] = m->authority_deadline; p->deadline[1] = after(now, m->cfg.lease_ns);
    /* Lane bookkeeping before PROPOSE fixes the count: the origin worker's
     * conn is remembered by worker index. */
    p->lane_count = 0;
    p->lane[w].ref = e->ref; p->lane[w].incarnation = o->incarnation; p->lane[w].intent = o->intent;
    p->lane[w].worker = w;
}

/* ---- control messages -------------------------------------------------- */

static int handle_propose(struct peer_manager *m, struct session *s, const struct peer_sec_header *h,
                          const uint8_t *body, uint64_t now)
{
    struct peer_sec_propose in;
    const uint8_t *key = NULL; uint32_t ip = 0; uint16_t port = 0;
    const struct peer_sec_hello *remote = peer_control_remote(s->control);
    if (peer_sec_propose_decode(body, h->body_len, &in) || !remote || h->epoch != 1 ||
        memcmp(in.association, h->association, 16) || strcmp(in.cluster, m->cfg.cluster)) return -1;
    uint16_t code = 0;
    struct pair *existing = pair_find(m, s->node, in.target_pod, in.source_pod);
    unsigned local = strcmp(m->cfg.node, s->node) > 0;
    if (in.mtu != m->cfg.mtu) code = PEER_SEC_ERR_MISMATCH;
    else if (in.lane_count > m->cfg.workers || in.lane_count > remote->workers) code = PEER_SEC_ERR_MISMATCH;
    else if (pair_by_association(m, in.association)) code = PEER_SEC_ERR_CONFLICT;
    else if (m->cfg.ops->node_binding(m->cfg.ops_ctx, s->node, &key, &ip, &port) != 1 || !key ||
             memcmp(key, remote->public_key, 32)) code = PEER_SEC_ERR_REFUSED;
    uint32_t spi = code ? 0 : spi_reserve(m);
    if (!code && !spi) code = PEER_SEC_ERR_BUSY;
    for (unsigned i = 0; !code && i < in.lane_count; i++)
        if (in.lane[i].worker[local] >= m->cfg.workers || in.lane[i].worker[1-local] >= remote->workers)
            code = PEER_SEC_ERR_MISMATCH;
    if (!code && existing) {
        /* Both ends proposed the same pair: canonical endpoint 0 keeps its own.
         * The loser's lanes drain first; the frame waits, not the session. */
        if (existing->state == PAIR_CANCEL || existing->state == PAIR_FORGET) return 1;
        if (existing->state >= PAIR_OPEN || local == 0) code = PEER_SEC_ERR_CONFLICT;
        else { pair_cancel(m, existing, PEER_SEC_ERR_CONFLICT, now); return 1; }
    }
    struct pair *p = code ? NULL : pair_alloc(m, now);
    if (!code && !p) return 1;
    if (code) {
        uint8_t err[PEER_SEC_ERROR_LEN];
        m->stats.refused++;
        if (peer_sec_error_encode(code, err)) return -1;
        return peer_control_send(s->control, PEER_SEC_ERROR, in.association, 1, err, sizeof(err), now) < 0 ? -1 : 0;
    }
    p->state = PAIR_AUTHORIZING; p->local = local;
    p->session = (unsigned)(s - m->sessions); p->session_id = s->id;
    strcpy(p->node, s->node); memcpy(p->key, key, 32); p->ip_be = ip; p->port = port;
    p->incarnation = in.incarnation;
    strcpy(p->pair.local_uid, in.target_pod); strcpy(p->pair.remote_uid, in.source_pod);
    p->pair.remote = in.source;
    strcpy(p->intent.src_pod_uid, in.source_pod); strcpy(p->intent.dst_pod_uid, in.target_pod);
    strcpy(p->intent.src_service_key, in.service);
    p->intent.dst_port = in.port; p->intent.src_generation = in.source.generation;
    memcpy(p->association, in.association, 16);
    p->spi[local] = in.rx_spi; p->spi[1-local] = spi;
    p->lane_count = in.lane_count;
    for (unsigned i = 0; i < in.lane_count; i++) {
        p->lanes[i] = (struct peer_sec_lane){.id = in.lane[i].id, .generation = in.lane[i].generation, .mtu = in.mtu};
        p->lanes[i].worker[0] = in.lane[i].worker[0]; p->lanes[i].worker[1] = in.lane[i].worker[1];
        p->lane[i].worker = in.lane[i].worker[local];
    }
    p->deadline[0] = m->authority_deadline; p->deadline[1] = after(now, m->cfg.lease_ns);
    p->deadline[2] = peer_control_lease_deadline(s->control);
    if (!p->spi[1-local] || p->deadline[0] <= now || p->deadline[2] <= now) { pair_free(m, p); return 1; }
    return 0;
}
/* A late answer for a pair this node already gave up on: tell the peer, keep
 * the session. */
static int unknown_association(struct session *s, const struct peer_sec_header *h, uint64_t now)
{
    uint8_t err[PEER_SEC_ERROR_LEN];
    if (peer_sec_error_encode(PEER_SEC_ERR_REVOKED, err)) return -1;
    return peer_control_send(s->control, PEER_SEC_ERROR, h->association, 1, err, sizeof(err), now) < 0 ? -1 : 0;
}
static int handle_accept(struct peer_manager *m, struct session *s, const struct peer_sec_header *h,
                         const uint8_t *body, uint64_t now)
{
    struct peer_sec_lane_params in;
    struct pair *p = pair_by_association(m, h->association);
    if (!p) return unknown_association(s, h, now);
    if (pair_session(m, p) != s) return -1;
    if (p->state != PAIR_PROPOSED || h->epoch != 1 || peer_sec_lane_params_decode(body, h->body_len, 1, &in) ||
        memcmp(in.association, p->association, 16) || !pair_halves_match(p, &in)) {
        pair_cancel(m, p, PEER_SEC_ERR_MALFORMED, now); return 0;
    }
    p->pair.remote = in.registration; p->spi[p->local] = in.rx_spi;
    for (unsigned i = 0; i < p->lane_count; i++) pair_fill_half(p, 1-p->local, &in.lane[i], i);
    p->state = PAIR_PREPARING; round_start(m, p, PEER_PAIR_PREPARE);
    for (unsigned i = 0; i < p->lane_count; i++) p->lane[i].done = 0;
    return 0;
}
static int handle_params(struct peer_manager *m, struct session *s, const struct peer_sec_header *h,
                         const uint8_t *body, uint64_t now)
{
    struct peer_sec_lane_params in;
    struct pair *p = pair_by_association(m, h->association);
    if (!p) return unknown_association(s, h, now);
    if (pair_session(m, p) != s) return -1;
    if (p->state != PAIR_WAIT_PARAMS || h->epoch != 1 || peer_sec_lane_params_decode(body, h->body_len, 0, &in) ||
        memcmp(in.association, p->association, 16) || !pair_halves_match(p, &in)) {
        pair_cancel(m, p, PEER_SEC_ERR_MALFORMED, now); return 0;
    }
    for (unsigned i = 0; i < p->lane_count; i++) pair_fill_half(p, 1-p->local, &in.lane[i], i);
    if (pair_open_association(m, p, now)) pair_cancel(m, p, PEER_SEC_ERR_BUSY, now);
    return 0;
}
static int handle_rekey(struct peer_manager *m, struct session *s, const struct peer_sec_header *h,
                        const uint8_t *body, uint64_t now, int commit)
{
    uint32_t spi[2];
    struct pair *p = pair_by_association(m, h->association);
    if (!p || pair_session(m, p) != s) return -1;
    if (p->state != PAIR_OPEN || peer_sec_rekey_decode(body, h->body_len, spi)) return -1;
    unsigned mine = 1-p->local, theirs = p->local;
    if (commit) {
        if (!p->rekeying || spi[mine] != p->rekey_spi[mine] || !spi[theirs]) return -1;
    } else {
        if (p->rekeying || spi[mine] || !spi[theirs]) return -1;
        p->rekey_spi[theirs] = spi[theirs]; p->rekey_spi[mine] = spi_reserve(m);
        if (!p->rekey_spi[mine]) { memset(p->rekey_spi, 0, sizeof(p->rekey_spi)); return -1; }
        spi[mine] = p->rekey_spi[mine];
    }
    p->rekey_spi[theirs] = spi[theirs];
    if (peer_association_rekey(m->registry, p->token, spi, now)) {
        memset(p->rekey_spi, 0, sizeof(p->rekey_spi)); p->rekeying = 0;
        session_error(m, p, PEER_SEC_ERR_REFUSED, now); return 0;
    }
    p->rekeying = 1;
    if (!commit) {
        uint8_t bytes[PEER_SEC_REKEY_LEN];
        if (peer_sec_rekey_encode(spi, bytes) ||
            session_send(m, p, PEER_SEC_COMMIT, p->epoch, bytes, sizeof(bytes), now) < 0) return -1;
    }
    return 0;
}
static int message_cb(void *ctx, const struct peer_sec_header *h, const uint8_t *body)
{
    struct session *s = ctx;
    struct peer_manager *m = s->m;
    uint64_t now = m->last_now;
    if (!s->node[0]) {
        const struct peer_sec_hello *remote = peer_control_remote(s->control);
        if (!remote) return -1;
        strcpy(s->node, remote->node);
    }
    TRACE("%s: <- type=%u epoch=%llu from '%s'", m->cfg.node, h->type, (unsigned long long)h->epoch, s->node);
    switch (h->type) {
    case PEER_SEC_PROPOSE: return handle_propose(m, s, h, body, now);
    case PEER_SEC_ACCEPT: return handle_accept(m, s, h, body, now);
    case PEER_SEC_LANE_PARAMS: return handle_params(m, s, h, body, now);
    case PEER_SEC_REKEY: return handle_rekey(m, s, h, body, now, 0);
    case PEER_SEC_COMMIT: return handle_rekey(m, s, h, body, now, 1);
    case PEER_SEC_ERROR: {
        uint16_t code;
        struct pair *p = pair_by_association(m, h->association);
        if (peer_sec_error_decode(body, h->body_len, &code)) return -1;
        if (p && pair_session(m, p) == s) { p->rekeying = 0; pair_revoke(m, p, now); }
        return 0;
    }
    case PEER_SEC_RX_READY: case PEER_SEC_TX_READY: case PEER_SEC_LANE_READY:
    case PEER_SEC_DRAINED: case PEER_SEC_ACTIVE_ACK: case PEER_SEC_REVOKE: {
        struct peer_assoc_token token;
        if (h->body_len != 32) return -1;
        int rc = peer_associations_lookup(m->registry, h->session, h->association, &token);
        if (rc != 1) return rc < 0 ? -1 : 0;
        return peer_association_signal(m->registry, token, h->type, h->epoch, body, now) < 0 ? -1 : 0;
    }
    default: return -1;
    }
}

/* ---- turns ------------------------------------------------------------- */

static void notices_drain(struct peer_manager *m, uint64_t now)
{
    uint64_t drained; ssize_t n;
    do { n = read(m->notify_fd, &drained, sizeof(drained)); } while (n < 0 && errno == EINTR);
    for (;;) {
        struct notice note;
        pthread_mutex_lock(&m->notify_mu);
        if (!m->notice_count) { pthread_mutex_unlock(&m->notify_mu); return; }
        note = m->notices[m->notice_head]; m->notice_head = (m->notice_head + 1) % NOTICE_CAP; m->notice_count--;
        pthread_mutex_unlock(&m->notify_mu);
        if (note.kind == 1) {
            for (unsigned i = 0; i < m->pair_cap; i++) {
                struct pair *p = &m->pairs[i];
                if (p->used && !strcmp(p->pair.local_uid, note.uid)) pair_revoke(m, p, now);
            }
        } else if (note.kind == 2 && note.generation > m->authority_generation && note.deadline > now) {
            m->authority_generation = note.generation; m->authority_deadline = note.deadline;
            for (unsigned i = 0; i < PEER_MANAGER_SESSIONS_MAX; i++)
                if (m->sessions[i].used)
                    (void)peer_control_authority(m->sessions[i].control, note.generation, note.deadline, now);
        }
    }
}
static int sessions_progress(struct peer_manager *m, uint64_t now)
{
    int moved = 0;
    void *accepted[4];
    int count = 0;
    if (m->cfg.wire->progress(m->cfg.wire_ctx, accepted, 4, &count) > 0) moved = 1;
    for (int i = 0; i < count; i++)
        if (!session_open(m, NULL, NULL, accepted[i], 0, now)) { m->cfg.wire->close(accepted[i]); m->stats.refused++; }
    for (unsigned i = 0; i < PEER_MANAGER_SESSIONS_MAX; i++) {
        struct session *s = &m->sessions[i];
        if (!s->used) continue;
        int faulted = peer_control_faulted(s->control);
        if (!faulted) {
            m->last_now = now;
            for (unsigned turn = 0; turn < 2 && !faulted; turn++)
                faulted = peer_control_progress(s->control, now) < 0;
        }
        if (!faulted && !s->node[0]) {
            const struct peer_sec_hello *remote = peer_control_remote(s->control);
            if (remote) { strcpy(s->node, remote->node); moved = 1; }
        }
        if (faulted) {
            TRACE("%s: session to '%s' faulted (initiator=%d)", m->cfg.node, s->node, s->initiator);
            m->stats.faults++;
            for (unsigned k = 0; k < m->pair_cap; k++) {
                struct pair *p = &m->pairs[k];
                if (p->used && p->session == i && p->session_id == s->id && p->state < PAIR_OPEN)
                    pair_cancel(m, p, 0, now);
            }
            if (!session_referenced(m, s)) { session_close(m, s); moved = 1; }
        }
    }
    return moved;
}
static void lane_completed(struct peer_manager *m, struct pair *p, struct lane *l,
                           const struct peer_pair_event *e, uint64_t now)
{
    l->op = 0;
    TRACE("%s: lane worker=%u command=%d status=%d (cleanup=%d round=%d state=%d)", m->cfg.node, l->worker,
          (int)e->command, e->status, (int)l->cleanup, (int)p->round, p->state);
    if (l->cleanup) {
        if (e->status == -2) { l->quarantined = 1; l->cleanup = 0; m->stats.quarantined++; return; }
        if (e->status < 0) { l->stuck = 1; l->cleanup = 0; m->stats.faults++; return; }
        if (l->cleanup == PEER_PAIR_BLOCK) l->cleanup = PEER_PAIR_DESTROY;
        else if (l->cleanup == PEER_PAIR_DESTROY) l->cleanup = PEER_PAIR_FORGET;
        else { l->cleanup = 0; memset(&l->ref, 0, sizeof(l->ref)); }
        return;
    }
    if (e->command == PEER_PAIR_AUTHORIZE && p->policy_op == e->operation) {
        p->policy_op = 0;
        if (p->state == PAIR_AUTHORIZING) {
            if (e->status) { pair_cancel(m, p, PEER_SEC_ERR_REFUSED, now); return; }
            p->pair.local = e->offer.pair.local;
            p->state = PAIR_PREPARING; round_start(m, p, PEER_PAIR_PREPARE);
            for (unsigned i = 0; i < p->lane_count; i++) p->lane[i].done = 0;
        } else if (p->state == PAIR_OPEN) {
            if (e->status) { pair_revoke(m, p, now); return; }
            uint64_t deadline = after(now, m->cfg.lease_ns);
            if (!peer_association_renew(m->registry, p->token, 1, ++p->generation[1], deadline, now)) {
                p->deadline[1] = deadline; p->regrant = 1;
            }
        }
        return;
    }
    if (e->command != p->round) return;
    l->done = 1; l->status = e->status;
    if (e->command == PEER_PAIR_PREPARE) {
        if (e->status == 0) { l->prepared = 1; l->endpoint = e->endpoint; }
        if (e->status == -1 && !ref_set(e->ref) && ref_set(l->ref) && !l->prepared) { l->done = 0; return; }
        if (ref_set(e->ref)) l->ref = e->ref;
        if (e->status == -2) l->quarantined = 1;
    } else if (e->command == PEER_PAIR_ENABLE && e->status == 0) l->enabled = 1;
    else if (e->command == PEER_PAIR_GRANT && e->status == 1) l->status = 0;
}
static int workers_poll(struct peer_manager *m, uint64_t now)
{
    int moved = 0;
    for (unsigned w = 0; w < m->cfg.workers; w++) {
        struct peer_pair_event e;
        if (!m->workers[w].rt) continue;
        while (peer_pair_manager_poll(m->workers[w].rt, &e) == 1) {
            moved = 1;
            if (e.type == PEER_PAIR_OPEN) { pair_open_event(m, w, &e, now); continue; }
            struct pair *found = NULL; struct lane *lane = NULL;
            for (unsigned i = 0; i < m->pair_cap && !found; i++) {
                struct pair *p = &m->pairs[i];
                if (!p->used) continue;
                struct lane *l = pair_lane_for_worker(p, w);
                if (e.type == PEER_PAIR_COMPLETED) {
                    if (l && l->op == e.operation) { found = p; lane = l; }
                    else if (p->policy_op == e.operation && p->lane[0].worker == w) { found = p; lane = &p->lane[0]; }
                } else if (l && ref_set(l->ref) && l->ref.runtime == e.ref.runtime &&
                           l->ref.generation == e.ref.generation && l->ref.slot == e.ref.slot) found = p;
            }
            if (!found) continue;
            if (e.type == PEER_PAIR_COMPLETED) lane_completed(m, found, lane, &e, now);
            else if (found->state < PAIR_FORGET) pair_revoke(m, found, now);
        }
    }
    return moved;
}
static int crypto_poll(struct peer_manager *m, uint64_t now)
{
    struct peer_crypto_completion c;
    int moved = 0;
    while (m->cfg.crypto.poll(m->cfg.crypto.ctx, &c) == 1) {
        moved = 1;
        struct pair *p = pair_by_token(m, c.token);
        if (!p || !p->crypto_inflight || p->crypto.operation != c.operation || p->crypto.action != c.action) continue;
        p->hw_done = 1; p->hw_status = c.status; (void)now;
    }
    return moved;
}
static void pair_request_done(struct peer_manager *m, struct pair *p, int status, uint64_t now)
{
    struct peer_crypto_request r = p->crypto;
    peer_crypto_request_cleanse(&r);
    TRACE("%s: request action=%d epoch=%llu done status=%d", m->cfg.node, (int)r.action,
          (unsigned long long)r.binding.epoch, status);
    p->crypto_inflight = p->crypto_submitted = p->hw_done = 0; p->round = 0;
    (void)peer_association_complete(m->registry, &r, status, now);
    if (!status && r.action == PEER_CRYPTO_RESUME) { p->epoch = r.binding.epoch; p->rekey_at = 0; }
    if (!status && r.action == PEER_CRYPTO_RESUME && r.old_epoch) { p->regrant = 1; p->rekeying = 0; }
    if (!status && r.action == PEER_CRYPTO_REMOVE_OLD) memset(p->rekey_spi, 0, sizeof(p->rekey_spi));
}
static int registry_drive(struct peer_manager *m, uint64_t now)
{
    int moved = 0;
    struct peer_crypto_request r;
    struct peer_assoc_event e;
    for (unsigned n = 0; n < m->pair_cap && peer_associations_next(m->registry, now, &r) == 1; n++) {
        struct pair *p = pair_by_token(m, r.token);
        moved = 1;
        if (!p || p->crypto_inflight) { peer_crypto_request_cleanse(&r); (void)peer_association_complete(m->registry, &r, -1, now); continue; }
        p->crypto = r; peer_crypto_request_cleanse(&r);
        p->crypto_inflight = 1; p->crypto_submitted = p->hw_done = 0;
        switch (r.action) {
        case PEER_CRYPTO_ENABLE_QPS: round_start(m, p, PEER_PAIR_ENABLE); break;
        case PEER_CRYPTO_QUIESCE: round_start(m, p, PEER_PAIR_QUIESCE); break;
        case PEER_CRYPTO_RESUME:
            if (r.old_epoch) round_start(m, p, PEER_PAIR_RESUME);
            else pair_request_done(m, p, 0, now);
            break;
        case PEER_CRYPTO_DESTROY_QPS: round_start(m, p, PEER_PAIR_DESTROY); break;
        case PEER_CRYPTO_BLOCK: round_start(m, p, PEER_PAIR_BLOCK); break;
        default: break;
        }
    }
    for (unsigned n = 0; n < m->pair_cap && peer_associations_event(m->registry, &e) == 1; n++) {
        struct pair *p = pair_by_token(m, e.token);
        /* The peer must hold the association before any barrier reaches it. */
        if (p && p->send_params) {
            int sent = pair_send_halves(m, p, 0, now);
            if (sent < 0) { pair_revoke(m, p, now); continue; }
            if (!sent) break;
            p->send_params = 0;
        }
        int rc = p ? session_send(m, p, e.type, e.epoch, e.digest, 32, now) : -1;
        if (rc == 0) break;                      /* bounded queue; retry next turn */
        (void)peer_association_event_sent(m->registry, &e); moved = 1;
    }
    return moved;
}
static void pair_progress_open(struct peer_manager *m, struct pair *p, uint64_t now)
{
    enum peer_assoc_phase phase = peer_association_phase(m->registry, p->token);
    struct session *s = pair_session(m, p);
    if (p->send_params) {
        int rc = pair_send_halves(m, p, 0, now);
        if (rc < 0) { pair_revoke(m, p, now); return; }
        if (rc == 1) p->send_params = 0;
    }
    if (p->crypto_inflight) {
        int hw_needed = p->crypto.action == PEER_CRYPTO_INSTALL_RX || p->crypto.action == PEER_CRYPTO_INSTALL_TX ||
            p->crypto.action == PEER_CRYPTO_REMOVE_OLD || p->crypto.action == PEER_CRYPTO_REMOVE_ALL ||
            p->crypto.action == PEER_CRYPTO_BLOCK;
        if (hw_needed && !p->crypto_submitted) {
            int rc = m->cfg.crypto.submit(m->cfg.crypto.ctx, &p->crypto);
            if (rc < 0) { pair_request_done(m, p, -1, now); return; }
            if (rc == 1) { p->crypto_submitted = 1; peer_crypto_request_cleanse(&p->crypto); }
        }
        int lanes_done = 1, lane_status = 0;
        if (p->round) lanes_done = round_progress(m, p, now, &lane_status);
        int hw_ok = !hw_needed || p->hw_done;
        if (lanes_done && hw_ok) {
            int status = lane_status;
            if (hw_needed && (p->hw_status == -2 || (p->hw_status && status != -2))) status = p->hw_status;
            pair_request_done(m, p, status, now);
        }
        return;
    }
    if (phase == PEER_ASSOC_DEAD) {
        p->state = PAIR_FORGET; pair_cleanup_start(p, PEER_PAIR_FORGET); return;
    }
    if (phase == PEER_ASSOC_QUARANTINED) return;
    /* Leases: the control lease follows the challenge heartbeat, authority
     * follows the newest notification, and a destination re-asks its policy
     * before its lease runs out. Every renewal reaches workers as a new grant. */
    uint64_t control = s ? peer_control_lease_deadline(s->control) : 0;
    if (control > p->deadline[2] &&
        !peer_association_renew(m->registry, p->token, 2, ++p->generation[2], control, now)) {
        p->deadline[2] = control; p->regrant = 1;
    }
    if (m->authority_generation > p->generation[0] &&
        !peer_association_renew(m->registry, p->token, 0, m->authority_generation, m->authority_deadline, now)) {
        p->generation[0] = m->authority_generation; p->deadline[0] = m->authority_deadline; p->regrant = 1;
        if (p->proposer && !peer_association_renew(m->registry, p->token, 1, ++p->generation[1], m->authority_deadline, now))
            p->deadline[1] = m->authority_deadline;
    }
    if (!p->proposer && !p->policy_op && p->deadline[1] > now && p->deadline[1] - now < m->cfg.lease_ns / 2) {
        struct lane *l = &p->lane[0];
        if (!l->op && lane_submit(m, p, 0, PEER_PAIR_AUTHORIZE, now) == 1) { p->policy_op = l->op; l->op = 0; }
    }
    if (phase != PEER_ASSOC_ACTIVE || !peer_association_admit(m->registry, p->token, now)) {
        if (p->round == PEER_PAIR_GRANT) p->round = 0;
        return;
    }
    if (!p->round && (!p->granted || p->regrant)) { round_start(m, p, PEER_PAIR_GRANT); p->regrant = 0; }
    if (p->round == PEER_PAIR_GRANT) {
        int status = 0;
        if (round_progress(m, p, now, &status)) {
            p->round = 0;
            if (status < 0) pair_revoke(m, p, now); else p->granted = 1;
        }
    }
    if (m->cfg.rekey_ns && p->proposer && p->granted && !p->round) {
        if (!p->rekey_at) p->rekey_at = after(now, m->cfg.rekey_ns);
        else if (now >= p->rekey_at && !pair_rekey_start(m, p, now)) p->rekey_at = 0;
    }
}
static int pairs_progress(struct peer_manager *m, uint64_t now)
{
    int moved = 0;
    for (unsigned i = 0; i < m->pair_cap; i++) {
        struct pair *p = &m->pairs[i];
        if (!p->used) continue;
        struct session *s = pair_session(m, p);
        int status = 0;
        if (p->state < PAIR_OPEN && now - p->created >= m->cfg.setup_timeout_ns) {
            pair_cancel(m, p, PEER_SEC_ERR_BUSY, now); moved = 1;
        }
        switch (p->state) {
        case PAIR_WAIT_SESSION:
            if (!s) {
                s = session_find(m, p->node, 1);
                if (!s) s = session_dial(m, p->node, now);
                if (!s) break;
                p->session = (unsigned)(s - m->sessions); p->session_id = s->id;
            }
            if (!peer_control_ready(s->control, now)) break;
            {
                int rc = pair_propose(m, p, s, now);
                if (rc < 0) pair_cancel(m, p, 0, now);
                else if (rc == 1) { p->state = PAIR_PROPOSED; moved = 1; }
            }
            break;
        case PAIR_AUTHORIZING:
            if (!p->policy_op) {
                struct lane *l = &p->lane[0];
                if (lane_submit(m, p, 0, PEER_PAIR_AUTHORIZE, now) == 1) { p->policy_op = l->op; l->op = 0; moved = 1; }
            }
            break;
        case PAIR_PREPARING:
            if (!round_progress(m, p, now, &status)) break;
            p->round = 0; moved = 1;
            if (status < 0) { pair_cancel(m, p, PEER_SEC_ERR_BUSY, now); break; }
            if (p->proposer) {
                if (pair_open_association(m, p, now)) pair_cancel(m, p, PEER_SEC_ERR_BUSY, now);
                else p->send_params = 1;
            } else {
                int rc = pair_send_halves(m, p, 1, now);
                if (rc < 0) pair_cancel(m, p, PEER_SEC_ERR_BUSY, now);
                else if (rc == 1) p->state = PAIR_WAIT_PARAMS;
                else { p->round = PEER_PAIR_PREPARE; }   /* queue full; answer next turn */
            }
            break;
        case PAIR_OPEN: pair_progress_open(m, p, now); break;
        case PAIR_FORGET: case PAIR_CANCEL: {
            int remaining = 0;
            for (unsigned k = 0; k < PEER_SEC_LANES_MAX; k++) {
                struct lane *l = &p->lane[k];
                if (!ref_set(l->ref)) continue;
                if (l->quarantined || l->stuck) { remaining = 1; continue; }
                if (l->cleanup && !l->op) { (void)lane_submit(m, p, k, l->cleanup, now); moved = 1; }
                remaining = 1;
            }
            if (!remaining) { pair_free(m, p); moved = 1; }
            break;
        }
        default: break;
        }
    }
    return moved;
}
int peer_manager_progress(struct peer_manager *m, uint64_t now)
{
    if (!m || now < m->last_now) return -1;
    m->last_now = now;
    int moved = 0;
    notices_drain(m, now);
    if (m->cfg.crypto.healthy) {
        int down = !m->cfg.crypto.healthy(m->cfg.crypto.ctx);
        if (down && !m->crypto_down) {
            /* Protection left with the hardware owner: nothing it enforced
             * can be trusted, so every open pair retires now. */
            TRACE("%s: crypto adapter unreachable; revoking every open pair", m->cfg.node);
            for (unsigned i = 0; i < m->pair_cap; i++)
                if (m->pairs[i].used && m->pairs[i].state == PAIR_OPEN) pair_revoke(m, &m->pairs[i], now);
            m->stats.faults++;
        }
        m->crypto_down = down;
    }
    moved |= sessions_progress(m, now);
    moved |= workers_poll(m, now);
    moved |= crypto_poll(m, now);
    moved |= registry_drive(m, now);
    moved |= pairs_progress(m, now);
    moved |= registry_drive(m, now);
    struct peer_assoc_stats st; peer_associations_stats(m->registry, &st);
    m->stats.active = 0;
    for (unsigned i = 0; i < m->pair_cap; i++)
        if (m->pairs[i].used && m->pairs[i].state == PAIR_OPEN &&
            peer_association_phase(m->registry, m->pairs[i].token) == PEER_ASSOC_ACTIVE) m->stats.active++;
    m->stats.quarantined = st.quarantined;
    return moved;
}

/* ---- lifecycle --------------------------------------------------------- */

static int pair_rekey_start(struct peer_manager *m, struct pair *p, uint64_t now)
{
    if (!p || p->state != PAIR_OPEN || p->rekeying || p->crypto_inflight ||
        !peer_association_admit(m->registry, p->token, now)) return -1;
    uint32_t spi[2] = {0, 0};
    spi[1-p->local] = spi_reserve(m);
    if (!spi[1-p->local]) return -1;
    uint8_t bytes[PEER_SEC_REKEY_LEN];
    if (peer_sec_rekey_encode(spi, bytes) ||
        session_send(m, p, PEER_SEC_REKEY, p->epoch, bytes, sizeof(bytes), now) != 1) return -1;
    memset(p->rekey_spi, 0, sizeof(p->rekey_spi)); p->rekey_spi[1-p->local] = spi[1-p->local];
    p->rekeying = 1; return 0;
}
int peer_manager_rekey(struct peer_manager *m, const uint8_t association[16])
{
    if (!m || !association) return -1;
    return pair_rekey_start(m, pair_by_association(m, association), m->last_now);
}
static int notify(struct peer_manager *m, const struct notice *note)
{
    if (!m) return -1;
    pthread_mutex_lock(&m->notify_mu);
    if (m->notice_count == NOTICE_CAP) { pthread_mutex_unlock(&m->notify_mu); return 0; }
    m->notices[(m->notice_head + m->notice_count) % NOTICE_CAP] = *note; m->notice_count++;
    pthread_mutex_unlock(&m->notify_mu);
    wake(m->notify_fd); return 1;
}
int peer_manager_notify_unregister(struct peer_manager *m, const char *pod_uid)
{
    struct notice note = {.kind = 1};
    if (!pod_uid || !*pod_uid || strlen(pod_uid) >= sizeof(note.uid)) return -1;
    strcpy(note.uid, pod_uid); return notify(m, &note);
}
int peer_manager_notify_authority(struct peer_manager *m, uint64_t generation, uint64_t deadline_ns)
{
    struct notice note = {.kind = 2, .generation = generation, .deadline = deadline_ns};
    return generation && deadline_ns ? notify(m, &note) : -1;
}
void peer_manager_stats(struct peer_manager *m, struct peer_manager_stats *out)
{ if (m && out) *out = m->stats; }
int peer_manager_fd(struct peer_manager *m) { return m ? m->epfd : -1; }
int peer_manager_attach_worker(struct peer_manager *m, unsigned worker, struct peer_pair_transport *rt)
{
    if (!m || !rt || worker >= m->cfg.workers || m->workers[worker].rt || m->running) return -1;
    struct epoll_event e = {.events = EPOLLIN};
    int fd = peer_pair_manager_fd(rt);
    if (fd < 0 || epoll_ctl(m->epfd, EPOLL_CTL_ADD, fd, &e)) return -1;
    m->workers[worker].rt = rt; return 0;
}
int peer_manager_new(const struct peer_manager_config *cfg, struct peer_manager **out,
                     char *error, size_t error_len)
{
    if (error && error_len) error[0] = 0;
    if (!out) return -1;
    *out = NULL;
    const char *why = "invalid configuration";
    if (!cfg || !cfg->seed || !cfg->wire || !cfg->wire_ctx || !cfg->ops || !cfg->ops->node_binding ||
        !cfg->ops->node_by_key || !peer_crypto_adapter_valid(&cfg->crypto) || !cfg->workers ||
        cfg->workers > PEER_ASSOC_LANES_MAX || !cfg->node[0] || !cfg->cluster[0] ||
        !memchr(cfg->node, 0, sizeof(cfg->node)) || !memchr(cfg->cluster, 0, sizeof(cfg->cluster)) ||
        !peer_sec_nonzero(cfg->boot, 16) || !cfg->lease_ns || !cfg->wire->connect ||
        !cfg->wire->progress || !cfg->wire->close || !cfg->wire->ctx_free) goto fail_early;
    struct peer_manager *m = calloc(1, sizeof(*m));
    if (!m) { why = "out of memory"; goto fail_early; }
    m->cfg = *cfg; m->epfd = m->notify_fd = -1;
    if (!m->cfg.now_ns) m->cfg.now_ns = clock_ns;
    if (!m->cfg.control_lease_ns) m->cfg.control_lease_ns = 5 * SECOND_NS;
    if (!m->cfg.setup_timeout_ns) m->cfg.setup_timeout_ns = 5 * SECOND_NS;
    if (!m->cfg.mtu) m->cfg.mtu = 1024;
    if (!m->cfg.limits.pairs) m->cfg.limits = (struct peer_assoc_limits){.pairs = 256, .lanes = 320, .sas = 1024,
        .setup_timeout_ns = m->cfg.setup_timeout_ns, .overlap_ns = 2 * SECOND_NS};
    m->cfg.seed = NULL;
    m->pair_cap = m->cfg.limits.pairs;
    m->pairs = calloc(m->pair_cap, sizeof(*m->pairs));
    if (pthread_mutex_init(&m->notify_mu, NULL)) { free(m->pairs); free(m); why = "mutex"; goto fail_early; }
    char tls_error[128] = {0};
    if (!m->pairs || peer_tls_ctx_new(cfg->seed, cfg->node, &m->tls, tls_error, sizeof(tls_error))) {
        why = tls_error[0] ? tls_error : "TLS context"; goto fail;
    }
    peer_tls_ctx_public_key(m->tls, m->public_key);
    if (peer_associations_new(&m->cfg.limits, &m->registry)) { why = "association registry"; goto fail; }
    m->notify_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    m->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (m->notify_fd < 0 || m->epfd < 0) { why = "descriptors"; goto fail; }
    struct epoll_event e = {.events = EPOLLIN};
    int carrier = cfg->wire->epfd ? cfg->wire->epfd(cfg->wire_ctx) : -1, hw = cfg->crypto.fd(cfg->crypto.ctx);
    if (epoll_ctl(m->epfd, EPOLL_CTL_ADD, m->notify_fd, &e) ||
        (carrier >= 0 && epoll_ctl(m->epfd, EPOLL_CTL_ADD, carrier, &e)) ||
        (hw >= 0 && epoll_ctl(m->epfd, EPOLL_CTL_ADD, hw, &e))) { why = "epoll"; goto fail; }
    atomic_init(&m->stop, 0);
    m->last_now = m->cfg.now_ns(m->cfg.now_ctx);
    *out = m; return 0;
fail:
    if (m->registry) peer_associations_free(m->registry);
    if (m->tls) peer_tls_ctx_free(m->tls);
    if (m->epfd >= 0) close(m->epfd);
    if (m->notify_fd >= 0) close(m->notify_fd);
    pthread_mutex_destroy(&m->notify_mu);
    free(m->pairs); free(m);
fail_early:
    if (error && error_len) snprintf(error, error_len, "peer manager: %s", why);
    return -1;
}
int peer_manager_free(struct peer_manager *m)
{
    if (!m) return 0;
    if (m->running) return -1;
    for (unsigned i = 0; i < m->pair_cap; i++) if (m->pairs[i].used) return -1;
    for (unsigned i = 0; i < PEER_MANAGER_SESSIONS_MAX; i++)
        if (m->sessions[i].used) session_close(m, &m->sessions[i]);
    if (peer_associations_free(m->registry)) return -1;
    peer_tls_ctx_free(m->tls);
    close(m->epfd); close(m->notify_fd);
    pthread_mutex_destroy(&m->notify_mu);
    m->cfg.wire->ctx_free(m->cfg.wire_ctx);
    OPENSSL_cleanse(m->pairs, m->pair_cap * sizeof(*m->pairs));
    free(m->pairs); free(m); return 0;
}
static void *manager_main(void *arg)
{
    struct peer_manager *m = arg;
    while (!atomic_load_explicit(&m->stop, memory_order_acquire)) {
        struct epoll_event evs[8];
        (void)epoll_wait(m->epfd, evs, 8, 100);
        (void)peer_manager_progress(m, m->cfg.now_ns(m->cfg.now_ctx));
    }
    return NULL;
}
int peer_manager_start(struct peer_manager *m)
{
    if (!m || m->running) return -1;
    atomic_store_explicit(&m->stop, 0, memory_order_release);
    if (pthread_create(&m->thread, NULL, manager_main, m)) return -1;
    m->running = 1; return 0;
}
void peer_manager_stop(struct peer_manager *m)
{
    if (!m || !m->running) return;
    atomic_store_explicit(&m->stop, 1, memory_order_release);
    wake(m->notify_fd);
    pthread_join(m->thread, NULL); m->running = 0;
}
