#include "peer_association.h"
#include <stdlib.h>
#include <string.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

struct association {
    struct peer_assoc_token token;
    struct peer_tls_conn *tls;
    struct peer_sec_binding binding;
    struct peer_sec_lane lanes[PEER_ASSOC_LANES_MAX];
    unsigned lane_count, local, reserved_sas;
    enum peer_assoc_phase phase;
    uint64_t pending, deadline[3], authority_generation[3], setup_deadline;
    uint64_t active_epoch, old_epoch, retire_deadline;
    uint32_t old_spi[2];
    enum peer_crypto_action action;
    int used, revoking, quiesced, peer_rx, peer_tx, peer_qp, peer_drained, peer_active;
    uint8_t digest[32];
    struct peer_assoc_event event;
};
struct peer_associations {
    struct peer_assoc_limits limits;
    struct peer_assoc_stats stats;
    struct association *pairs;
    uint64_t id, generation, operation, last_now, grant_revision;
    unsigned cursor, event_cursor;
};
static uint64_t after(uint64_t now, uint64_t delta)
{ return delta > UINT64_MAX - now ? UINT64_MAX : now + delta; }
static struct association *find(struct peer_associations *m, struct peer_assoc_token t)
{
    if (!m || t.manager != m->id || t.slot >= m->limits.pairs) return NULL;
    struct association *a = &m->pairs[t.slot];
    return a->used && a->token.generation == t.generation ? a : NULL;
}
static int live(struct association *a, uint64_t now)
{
    return !a->revoking && a->phase != PEER_ASSOC_QUARANTINED && a->phase != PEER_ASSOC_DEAD &&
        now < a->deadline[0] && now < a->deadline[1] && now < a->deadline[2] &&
        !peer_tls_faulted(a->tls);
}
static int digest(struct association *a)
{
    uint8_t bytes[PEER_TLS_IPSEC_CONTEXT_MAX + PEER_ASSOC_LANES_MAX * PEER_SEC_LANE_LEN];
    size_t n; unsigned len;
    if (peer_sec_binding_encode(&a->binding, bytes, sizeof(bytes), &n)) return -1;
    for (unsigned i = 0; i < a->lane_count; i++) {
        if (peer_sec_lane_encode(&a->lanes[i], bytes + n)) return -1;
        n += PEER_SEC_LANE_LEN;
    }
    return EVP_Digest(bytes, n, a->digest, &len, EVP_sha256(), NULL) == 1 && len == 32 ? 0 : -1;
}
static void emit(struct association *a, uint16_t type)
{
    a->event = (struct peer_assoc_event){.token = a->token, .type = type, .epoch = a->binding.epoch};
    memcpy(a->event.association, a->binding.association, 16);
    memcpy(a->event.digest, a->digest, 32);
}
int peer_associations_new(const struct peer_assoc_limits *limits, struct peer_associations **out)
{
    if (!out) return -1;
    *out = NULL;
    if (!limits || !limits->pairs || limits->pairs > 4096 || !limits->lanes ||
        limits->sas < 2 || !limits->setup_timeout_ns || !limits->overlap_ns) return -1;
    struct peer_associations *m = calloc(1, sizeof(*m));
    if (!m) return -1;
    m->pairs = calloc(limits->pairs, sizeof(*m->pairs)); m->limits = *limits;
    if (!m->pairs || RAND_bytes((uint8_t *)&m->id, sizeof(m->id)) != 1 || !m->id) {
        free(m->pairs); free(m); return -1;
    }
    *out = m; return 0;
}
int peer_associations_free(struct peer_associations *m)
{
    if (!m) return 0;
    if (m->stats.pairs) return -1;
    free(m->pairs); OPENSSL_cleanse(m, sizeof(*m)); free(m); return 0;
}
/* SPI is a receive namespace: a shared manager owns one fabric endpoint. This
 * conservatively disallows reuse of a local RX SPI across remote nodes. */
static int spi_busy(struct peer_associations *m, unsigned local, uint32_t spi)
{
    (void)local;
    for (unsigned i = 0; i < m->limits.pairs; i++) {
        struct association *a = &m->pairs[i];
        if (a->used && (a->binding.spi[1-a->local] == spi ||
            (a->old_epoch && a->old_spi[1-a->local] == spi))) return 1;
    }
    return 0;
}
static int endpoints_same(const struct peer_sec_binding *a, const struct peer_sec_binding *b)
{
    /* Only one association for an ordered Pod UID pair per node manager. A
     * re-registration waits for old QP/DMA teardown rather than overlapping. */
    return !strcmp(a->cluster, b->cluster) &&
        !strcmp(a->endpoint[0].pod, b->endpoint[0].pod) &&
        !strcmp(a->endpoint[1].pod, b->endpoint[1].pod);
}
int peer_association_open(struct peer_associations *m, struct peer_tls_conn *tls,
                          const struct peer_sec_binding *b, unsigned local,
                          const struct peer_sec_lane *lanes, unsigned count,
                          const uint64_t deadlines[3], uint64_t now, struct peer_assoc_token *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!m || !out || !lanes || !deadlines || !count || count > PEER_ASSOC_LANES_MAX ||
        local > 1 || !peer_sec_binding_valid(b) || b->epoch != 1 || now < m->last_now ||
        deadlines[0] <= now || deadlines[1] <= now || deadlines[2] <= now ||
        m->stats.pairs == m->limits.pairs || count > m->limits.lanes - m->stats.lanes ||
        m->limits.sas - m->stats.sas < 2 || m->generation == UINT64_MAX ||
        spi_busy(m, local, b->spi[1-local])) return -1;
    uint8_t material[20], lane_bytes[PEER_SEC_LANE_LEN];
    if (peer_sec_derive(tls, b, local, local, material)) return -1;
    OPENSSL_cleanse(material, sizeof(material));
    unsigned slot = m->limits.pairs;
    for (unsigned i = 0; i < count; i++) {
        if (peer_sec_lane_encode(&lanes[i], lane_bytes)) return -1;
        if (i && lanes[i-1].id >= lanes[i].id) return -1;
        for (unsigned j = 0; j < i; j++)
            if (lanes[i].worker[local] == lanes[j].worker[local] ||
                lanes[i].qpn[local] == lanes[j].qpn[local]) return -1;
    }
    for (unsigned i = 0; i < m->limits.pairs; i++) {
        struct association *a = &m->pairs[i];
        if (!a->used) { if (slot == m->limits.pairs) slot = i; continue; }
        if (!memcmp(a->binding.association, b->association, 16) || endpoints_same(&a->binding, b)) return -1;
        for (unsigned j = 0; j < a->lane_count; j++)
            for (unsigned k = 0; k < count; k++)
                if (a->lanes[j].qpn[a->local] == lanes[k].qpn[local]) return -1;
    }
    struct association *a = &m->pairs[slot];
    memset(a, 0, sizeof(*a));
    a->token = (struct peer_assoc_token){m->id, ++m->generation, slot};
    a->binding = *b; a->tls = tls; a->local = local; a->lane_count = count;
    memcpy(a->lanes, lanes, count * sizeof(*lanes)); memcpy(a->deadline, deadlines, sizeof(a->deadline));
    a->setup_deadline = after(now, m->limits.setup_timeout_ns);
    a->phase = PEER_ASSOC_RX; a->reserved_sas = 2;
    if (digest(a)) { memset(a, 0, sizeof(*a)); return -1; }
    a->used = 1; m->stats.pairs++; m->stats.lanes += count; m->stats.sas += 2;
    *out = a->token; m->last_now = now; return 0;
}
static void revoke(struct association *a)
{
    if (!a->revoking && a->phase != PEER_ASSOC_DEAD && a->phase != PEER_ASSOC_QUARANTINED)
        emit(a, PEER_SEC_REVOKE);
    a->revoking = 1;
    if (!a->pending && a->phase != PEER_ASSOC_DEAD && a->phase != PEER_ASSOC_QUARANTINED &&
        a->phase != PEER_ASSOC_DESTROY && a->phase != PEER_ASSOC_REMOVE)
        a->phase = PEER_ASSOC_BLOCK;
}
int peer_associations_lookup(struct peer_associations *m, const uint8_t session[16],
                             const uint8_t association[16], struct peer_assoc_token *out)
{
    if (!m || !session || !association || !out) return -1;
    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < m->limits.pairs; i++) {
        struct association *a = &m->pairs[i];
        if (a->used && !memcmp(a->binding.session, session, 16) &&
            !memcmp(a->binding.association, association, 16)) { *out = a->token; return 1; }
    }
    return 0;
}
void peer_associations_tick(struct peer_associations *m, uint64_t now)
{
    if (!m) return;
    int backwards = now < m->last_now;
    if (!backwards) m->last_now = now;
    for (unsigned i = 0; i < m->limits.pairs; i++) {
        struct association *a = &m->pairs[i];
        if (!a->used || a->phase == PEER_ASSOC_DEAD || a->phase == PEER_ASSOC_QUARANTINED) continue;
        if (backwards || !live(a, now) ||
            ((a->phase != PEER_ASSOC_ACTIVE || !a->peer_active || a->event.type) &&
             now >= a->setup_deadline)) revoke(a);
    }
}
static enum peer_crypto_action next_action(struct association *a, uint64_t now)
{
    switch (a->phase) {
    case PEER_ASSOC_RX: return PEER_CRYPTO_INSTALL_RX;
    case PEER_ASSOC_WAIT_RX:
        if (a->peer_rx) {
            a->phase = a->active_epoch ? PEER_ASSOC_DRAIN : PEER_ASSOC_TX;
            return a->active_epoch ? PEER_CRYPTO_QUIESCE : PEER_CRYPTO_INSTALL_TX;
        }
        break;
    case PEER_ASSOC_WAIT_DRAIN:
        if (a->peer_drained) { a->phase = PEER_ASSOC_TX; return PEER_CRYPTO_INSTALL_TX; }
        break;
    case PEER_ASSOC_WAIT_TX:
        if (a->peer_tx) {
            a->phase = a->active_epoch ? PEER_ASSOC_RESUME : PEER_ASSOC_QP;
            return a->active_epoch ? PEER_CRYPTO_RESUME : PEER_CRYPTO_ENABLE_QPS;
        }
        break;
    case PEER_ASSOC_WAIT_QP:
        if (a->peer_qp) { a->phase = PEER_ASSOC_RESUME; return PEER_CRYPTO_RESUME; }
        break;
    case PEER_ASSOC_ACTIVE:
        if (a->old_epoch && a->peer_active && now >= a->retire_deadline) return PEER_CRYPTO_REMOVE_OLD;
        break;
    case PEER_ASSOC_BLOCK: return PEER_CRYPTO_BLOCK;
    case PEER_ASSOC_DESTROY: return PEER_CRYPTO_DESTROY_QPS;
    case PEER_ASSOC_REMOVE: return PEER_CRYPTO_REMOVE_ALL;
    default: break;
    }
    return 0;
}
void peer_crypto_request_cleanse(struct peer_crypto_request *r)
{ if (r) OPENSSL_cleanse(r->material, sizeof(r->material)); }
int peer_associations_next(struct peer_associations *m, uint64_t now, struct peer_crypto_request *out)
{
    if (!m || !out) return -1;
    memset(out, 0, sizeof(*out)); peer_associations_tick(m, now);
    for (unsigned n = 0; n < m->limits.pairs; n++) {
        unsigned i = m->cursor++ % m->limits.pairs;
        struct association *a = &m->pairs[i];
        if (!a->used || a->pending || (a->event.type && !a->revoking)) continue;
        enum peer_crypto_action action = next_action(a, now);
        if (!action) continue;
        if (m->operation == UINT64_MAX) { a->phase = PEER_ASSOC_QUARANTINED; m->stats.quarantined++; continue; }
        out->token = a->token; out->action = action; out->binding = a->binding;
        out->lane_count = a->lane_count; out->local_endpoint = a->local;
        memcpy(out->lanes, a->lanes, a->lane_count * sizeof(*a->lanes));
        out->old_epoch = a->old_epoch;
        if ((action == PEER_CRYPTO_INSTALL_RX || action == PEER_CRYPTO_INSTALL_TX) &&
            peer_sec_derive(a->tls, &a->binding, a->local,
                            action == PEER_CRYPTO_INSTALL_TX ? a->local : 1-a->local, out->material)) {
            revoke(a); memset(out, 0, sizeof(*out)); continue;
        }
        if (action == PEER_CRYPTO_QUIESCE) a->quiesced = 1;
        out->operation = a->pending = ++m->operation; a->action = action; return 1;
    }
    return 0;
}
int peer_association_complete(struct peer_associations *m, const struct peer_crypto_request *r,
                              int status, uint64_t now)
{
    if (!m || !r || status > 0 || status < -2) return -1;
    peer_associations_tick(m, now);
    struct association *a = find(m, r->token);
    if (!a || !a->pending || r->operation != a->pending || r->action != a->action ||
        r->binding.epoch != a->binding.epoch ||
        memcmp(r->binding.association, a->binding.association, 16) ||
        memcmp(r->binding.session, a->binding.session, 16)) return 1;
    a->pending = 0;
    if (status == -2 || (status && a->revoking)) {
        a->phase = PEER_ASSOC_QUARANTINED; a->revoking = 1; a->event.type = 0;
        m->stats.quarantined++; return 0;
    }
    if (status) { revoke(a); return 0; }
    if (a->revoking && r->action < PEER_CRYPTO_BLOCK) { revoke(a); return 0; }
    switch (r->action) {
    case PEER_CRYPTO_INSTALL_RX: a->phase = PEER_ASSOC_WAIT_RX; emit(a, PEER_SEC_RX_READY); break;
    case PEER_CRYPTO_INSTALL_TX: a->phase = PEER_ASSOC_WAIT_TX; emit(a, PEER_SEC_TX_READY); break;
    case PEER_CRYPTO_ENABLE_QPS: a->phase = PEER_ASSOC_WAIT_QP; emit(a, PEER_SEC_LANE_READY); break;
    case PEER_CRYPTO_QUIESCE: a->phase = PEER_ASSOC_WAIT_DRAIN; emit(a, PEER_SEC_DRAINED); break;
    case PEER_CRYPTO_RESUME:
        a->active_epoch = a->binding.epoch; a->quiesced = 0; a->phase = PEER_ASSOC_ACTIVE;
        a->retire_deadline = after(now, m->limits.overlap_ns); emit(a, PEER_SEC_ACTIVE_ACK); break;
    case PEER_CRYPTO_REMOVE_OLD:
        a->old_epoch = 0; memset(a->old_spi, 0, sizeof(a->old_spi));
        a->reserved_sas -= 2; m->stats.sas -= 2; break;
    case PEER_CRYPTO_BLOCK: a->phase = PEER_ASSOC_DESTROY; break;
    case PEER_CRYPTO_DESTROY_QPS: a->phase = PEER_ASSOC_REMOVE; break;
    case PEER_CRYPTO_REMOVE_ALL: a->phase = PEER_ASSOC_DEAD; break;
    default: return -1;
    }
    return 0;
}
int peer_associations_event(struct peer_associations *m, struct peer_assoc_event *out)
{
    if (!m || !out) return -1;
    memset(out, 0, sizeof(*out));
    for (unsigned n = 0; n < m->limits.pairs; n++) {
        unsigned i = m->event_cursor++ % m->limits.pairs;
        if (m->pairs[i].used && m->pairs[i].event.type) { *out = m->pairs[i].event; return 1; }
    }
    return 0;
}
int peer_association_event_sent(struct peer_associations *m, const struct peer_assoc_event *e)
{
    if (!e) return -1;
    struct association *a = find(m, e->token);
    if (!a || !a->event.type || a->event.type != e->type || a->event.epoch != e->epoch ||
        memcmp(a->event.digest, e->digest, 32)) return 1;
    a->event.type = 0; return 0;
}
int peer_association_signal(struct peer_associations *m, struct peer_assoc_token token,
                            uint16_t type, uint64_t epoch, const uint8_t d[32], uint64_t now)
{
    if (!m || !d) return -1;
    peer_associations_tick(m, now);
    struct association *a = find(m, token);
    if (!a || !live(a, now) || epoch < a->binding.epoch) return 1;
    if (epoch != a->binding.epoch || CRYPTO_memcmp(d, a->digest, 32)) return -1;
    switch (type) {
    case PEER_SEC_RX_READY: a->peer_rx = 1; break;
    case PEER_SEC_TX_READY:
        if (!a->peer_rx) return -1;
        a->peer_tx = 1; break;
    case PEER_SEC_LANE_READY:
        if (!a->peer_tx) return -1;
        a->peer_qp = 1; break;
    case PEER_SEC_DRAINED:
        if (!a->active_epoch || !a->peer_rx) return -1;
        a->peer_drained = 1; break;
    case PEER_SEC_ACTIVE_ACK:
        if (!a->peer_tx || (!a->active_epoch && !a->peer_qp)) return -1;
        a->peer_active = 1; break;
    case PEER_SEC_REVOKE: revoke(a); break;
    default: return -1;
    }
    return 0;
}
int peer_association_admit(struct peer_associations *m, struct peer_assoc_token t, uint64_t now)
{
    if (!m) return 0;
    peer_associations_tick(m, now); struct association *a = find(m, t);
    /* During preparation of a rekey the old epoch can continue until QUIESCE.
     * peer_active is reset for the candidate, so deliberately pause earlier. */
    return a && live(a, now) && a->phase == PEER_ASSOC_ACTIVE && a->peer_active &&
        !a->quiesced && !a->event.type && a->active_epoch == a->binding.epoch;
}

int peer_association_grant(struct peer_associations *m, struct peer_assoc_token token,
                           unsigned worker, uint64_t now, struct peer_assoc_grant *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!m) return -1;
    if (!peer_association_admit(m, token, now)) return 0;
    struct association *a = find(m, token);
    const struct peer_sec_lane *lane = NULL;
    for (unsigned i = 0; i < a->lane_count; i++)
        if (a->lanes[i].worker[a->local] == worker) { lane = &a->lanes[i]; break; }
    if (!lane) return 0;
    if (m->grant_revision == UINT64_MAX) { revoke(a); return -1; }
    const struct peer_sec_endpoint *local = &a->binding.endpoint[a->local];
    const struct peer_sec_endpoint *remote = &a->binding.endpoint[1-a->local];
    if (strlen(local->pod) >= sizeof(out->pair.local_uid) ||
        strlen(remote->pod) >= sizeof(out->pair.remote_uid)) { revoke(a); return -1; }
    strcpy(out->pair.local_uid, local->pod); strcpy(out->pair.remote_uid, remote->pod);
    struct dmesh_peer_registration *registrations[2] = {&out->pair.local, &out->pair.remote};
    const struct peer_sec_endpoint *endpoints[2] = {local, remote};
    for (unsigned i = 0; i < 2; i++) {
        registrations[i]->slot = endpoints[i]->channel_slot;
        registrations[i]->generation = endpoints[i]->channel_generation;
        memcpy(registrations[i]->daemon, endpoints[i]->daemon, sizeof(registrations[i]->daemon));
        _Static_assert(sizeof(registrations[i]->nonce) == sizeof(endpoints[i]->registration_nonce),
                       "SA binding must include the complete registration nonce");
        memcpy(registrations[i]->nonce, endpoints[i]->registration_nonce, sizeof(registrations[i]->nonce));
    }
    memcpy(out->pair.association, a->binding.association, 16);
    out->pair.lane_id = lane->id; out->pair.lane_generation = lane->generation;
    out->token = token; out->revision = ++m->grant_revision; out->epoch = a->active_epoch;
    out->lane = *lane;
    memcpy(out->deadline, a->deadline, sizeof(out->deadline));
    memcpy(out->lease_generation, a->authority_generation, sizeof(out->lease_generation));
    return 1;
}
int peer_association_rekey(struct peer_associations *m, struct peer_assoc_token t,
                           const uint32_t spi[2], uint64_t now)
{
    if (!m || !spi || !peer_association_admit(m, t, now)) return -1;
    struct association *a = find(m, t);
    if (a->pending || a->old_epoch || a->binding.epoch == UINT64_MAX ||
        spi[0] < 256 || spi[1] < 256 || spi[0] == a->binding.spi[0] || spi[1] == a->binding.spi[1] ||
        spi_busy(m, a->local, spi[1-a->local]) || m->limits.sas - m->stats.sas < 2) return -1;
    a->old_epoch = a->binding.epoch; memcpy(a->old_spi, a->binding.spi, sizeof(a->old_spi));
    a->binding.epoch++; memcpy(a->binding.spi, spi, sizeof(a->binding.spi));
    a->peer_rx = a->peer_tx = a->peer_qp = a->peer_drained = a->peer_active = 0;
    a->phase = PEER_ASSOC_RX; a->setup_deadline = after(now, m->limits.setup_timeout_ns);
    a->reserved_sas += 2; m->stats.sas += 2;
    if (digest(a)) { revoke(a); return -1; }
    return 0;
}
int peer_association_revoke(struct peer_associations *m, struct peer_assoc_token t)
{ struct association *a = find(m, t); if (!a) return -1; revoke(a); return 0; }
int peer_association_release(struct peer_associations *m, struct peer_assoc_token t)
{
    struct association *a = find(m, t);
    if (!a || a->pending || a->phase != PEER_ASSOC_DEAD) return -1;
    m->stats.pairs--; m->stats.lanes -= a->lane_count; m->stats.sas -= a->reserved_sas;
    OPENSSL_cleanse(a, sizeof(*a)); return 0;
}
int peer_association_renew(struct peer_associations *m, struct peer_assoc_token t, unsigned source,
                           uint64_t generation, uint64_t deadline, uint64_t now)
{
    if (!m || source > 2) return -1;
    peer_associations_tick(m, now); struct association *a = find(m, t);
    if (!a || !live(a, now) || generation <= a->authority_generation[source] || deadline <= now) return -1;
    a->authority_generation[source] = generation; a->deadline[source] = deadline; return 0;
}
enum peer_assoc_phase peer_association_phase(struct peer_associations *m, struct peer_assoc_token t)
{ struct association *a = find(m, t); return a ? a->phase : PEER_ASSOC_DEAD; }
void peer_associations_stats(const struct peer_associations *m, struct peer_assoc_stats *out)
{ if (m && out) *out = m->stats; }
