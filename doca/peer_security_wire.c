#include "peer_security_wire.h"

#include <string.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>

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
static void number(struct cursor *c, uint64_t n, unsigned width)
{
    uint8_t b[8];
    for (unsigned i = 0; i < width; i++) b[width - 1 - i] = (uint8_t)(n >> (8 * i));
    put(c, b, width);
}
static uint64_t take_number(struct cursor *c, unsigned width)
{
    uint8_t b[8] = {0}; uint64_t n = 0;
    get(c, b, width);
    for (unsigned i = 0; i < width; i++) n = (n << 8) | b[i];
    return n;
}
static size_t string_len(const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n]) {
        if ((unsigned char)s[n] < 0x21 || (unsigned char)s[n] > 0x7e) return 0;
        n++;
    }
    return n < cap ? n : 0;
}
static void string_put(struct cursor *c, const char *s, size_t cap)
{
    size_t n = string_len(s, cap);
    if (!n) { c->bad = 1; return; }
    number(c, n, 2); put(c, s, n);
}
static void string_get(struct cursor *c, char *s, size_t cap)
{
    size_t n = (size_t)take_number(c, 2);
    if (!n || n >= cap || n > c->left) { c->bad = 1; return; }
    get(c, s, n); s[n] = 0;
    if (string_len(s, cap) != n) c->bad = 1;
}
int peer_sec_nonzero(const void *bytes, size_t len)
{
    if (!bytes) return 0;
    const uint8_t *p = bytes; uint8_t n = 0;
    for (size_t i = 0; i < len; i++) n |= p[i];
    return n != 0;
}
static int header_valid(const struct peer_sec_header *h)
{
    if (!h || h->type < PEER_SEC_HELLO || h->type > PEER_SEC_PONG ||
        h->body_len > PEER_SEC_BODY_MAX || !h->operation) return 0;
    int assoc = peer_sec_nonzero(h->association, 16);
    int session = peer_sec_nonzero(h->session, 16);
    if (h->type == PEER_SEC_HELLO) return !session && !assoc && !h->epoch;
    if (h->type == PEER_SEC_PING || h->type == PEER_SEC_PONG)
        return session && !assoc && !h->epoch && h->body_len == 16;
    return session && assoc && h->epoch;
}
int peer_sec_header_encode(const struct peer_sec_header *h, uint8_t out[64])
{
    if (!out || !header_valid(h)) return -1;
    struct cursor c = {out, 64, 0};
    put(&c, "DMSC", 4); number(&c, PEER_SEC_VERSION, 2); number(&c, h->type, 2);
    number(&c, 0, 4); number(&c, h->body_len, 4); put(&c, h->session, 16);
    number(&c, h->operation, 8); put(&c, h->association, 16); number(&c, h->epoch, 8);
    return c.bad ? -1 : 0;
}
int peer_sec_header_decode(const void *buf, size_t len, struct peer_sec_header *out)
{
    if (!out || (!buf && len)) return -1;
    memset(out, 0, sizeof(*out));
    if (len < 64) return 1;
    struct cursor c = {(uint8_t *)buf, 64, 0}; char magic[4];
    get(&c, magic, 4);
    unsigned version = (unsigned)take_number(&c, 2);
    out->type = (uint16_t)take_number(&c, 2);
    uint32_t flags = (uint32_t)take_number(&c, 4);
    out->body_len = (uint32_t)take_number(&c, 4); get(&c, out->session, 16);
    out->operation = take_number(&c, 8); get(&c, out->association, 16);
    out->epoch = take_number(&c, 8);
    if (memcmp(magic, "DMSC", 4) || version != 2 || flags || !header_valid(out)) {
        memset(out, 0, sizeof(*out)); return -1;
    }
    return 0;
}
static int hello_valid(const struct peer_sec_hello *h)
{
    return h && string_len(h->cluster, sizeof(h->cluster)) &&
        string_len(h->node, sizeof(h->node)) && h->workers && h->workers <= 16 &&
        peer_sec_nonzero(h->public_key, 32) && peer_sec_nonzero(h->boot, 16) &&
        peer_sec_nonzero(h->nonce, 16);
}
int peer_sec_hello_encode(const struct peer_sec_hello *h, void *buf, size_t cap, size_t *len)
{
    if (len) *len = 0;
    if (!buf || !len || !hello_valid(h)) return -1;
    struct cursor c = {buf, cap, 0};
    string_put(&c, h->cluster, sizeof(h->cluster)); string_put(&c, h->node, sizeof(h->node));
    put(&c, h->public_key, 32); put(&c, h->boot, 16); put(&c, h->nonce, 16);
    number(&c, h->workers, 2); number(&c, PEER_SEC_AES128_GCM, 2);
    if (c.bad) return -1;
    *len = cap - c.left; return 0;
}
int peer_sec_hello_decode(const void *buf, size_t len, struct peer_sec_hello *out)
{
    if (!buf || !out) return -1;
    struct peer_sec_hello h = {0}; struct cursor c = {(uint8_t *)buf, len, 0};
    string_get(&c, h.cluster, sizeof(h.cluster)); string_get(&c, h.node, sizeof(h.node));
    get(&c, h.public_key, 32); get(&c, h.boot, 16); get(&c, h.nonce, 16);
    h.workers = (uint16_t)take_number(&c, 2);
    unsigned suite = (unsigned)take_number(&c, 2);
    memset(out, 0, sizeof(*out));
    if (c.bad || c.left || suite != PEER_SEC_AES128_GCM || !hello_valid(&h)) return -1;
    *out = h; return 0;
}
static int endpoint_valid(const struct peer_sec_endpoint *e)
{
    return string_len(e->node, sizeof(e->node)) && string_len(e->pod, sizeof(e->pod)) &&
        peer_sec_nonzero(e->public_key, 32) && peer_sec_nonzero(e->boot, 16) &&
        peer_sec_nonzero(e->daemon, 16) && e->channel_generation &&
        peer_sec_nonzero(e->registration_nonce, sizeof(e->registration_nonce));
}
int peer_sec_binding_valid(const struct peer_sec_binding *b)
{
    return b && string_len(b->cluster, sizeof(b->cluster)) &&
        endpoint_valid(&b->endpoint[0]) && endpoint_valid(&b->endpoint[1]) &&
        strcmp(b->endpoint[0].node, b->endpoint[1].node) < 0 &&
        strcmp(b->endpoint[0].pod, b->endpoint[1].pod) != 0 &&
        peer_sec_nonzero(b->session, 16) && peer_sec_nonzero(b->association, 16) &&
        b->epoch && b->spi[0] >= 256 && b->spi[1] >= 256;
}
static void endpoint_put(struct cursor *c, const struct peer_sec_endpoint *e)
{
    string_put(c, e->node, sizeof(e->node)); put(c, e->public_key, 32); put(c, e->boot, 16);
    string_put(c, e->pod, sizeof(e->pod)); put(c, e->daemon, 16);
    number(c, e->channel_slot, 4); number(c, e->channel_generation, 8);
    put(c, e->registration_nonce, sizeof(e->registration_nonce));
}
static void endpoint_get(struct cursor *c, struct peer_sec_endpoint *e)
{
    string_get(c, e->node, sizeof(e->node)); get(c, e->public_key, 32); get(c, e->boot, 16);
    string_get(c, e->pod, sizeof(e->pod)); get(c, e->daemon, 16);
    e->channel_slot = (uint32_t)take_number(c, 4); e->channel_generation = take_number(c, 8);
    get(c, e->registration_nonce, sizeof(e->registration_nonce));
}
int peer_sec_binding_encode(const struct peer_sec_binding *b, void *buf, size_t cap, size_t *len)
{
    if (len) *len = 0;
    if (!buf || !len || !peer_sec_binding_valid(b)) return -1;
    struct cursor c = {buf, cap, 0};
    number(&c, 2, 2); number(&c, PEER_SEC_AES128_GCM, 2);
    string_put(&c, b->cluster, sizeof(b->cluster));
    endpoint_put(&c, &b->endpoint[0]); endpoint_put(&c, &b->endpoint[1]);
    put(&c, b->session, 16); put(&c, b->association, 16); number(&c, b->epoch, 8);
    number(&c, b->spi[0], 4); number(&c, b->spi[1], 4);
    if (c.bad) return -1;
    *len = cap - c.left; return 0;
}
int peer_sec_binding_decode(const void *buf, size_t len, struct peer_sec_binding *out)
{
    if (!buf || !out) return -1;
    struct peer_sec_binding b = {0}; struct cursor c = {(uint8_t *)buf, len, 0};
    unsigned version = (unsigned)take_number(&c, 2), suite = (unsigned)take_number(&c, 2);
    string_get(&c, b.cluster, sizeof(b.cluster));
    endpoint_get(&c, &b.endpoint[0]); endpoint_get(&c, &b.endpoint[1]);
    get(&c, b.session, 16); get(&c, b.association, 16); b.epoch = take_number(&c, 8);
    b.spi[0] = (uint32_t)take_number(&c, 4); b.spi[1] = (uint32_t)take_number(&c, 4);
    memset(out, 0, sizeof(*out));
    if (c.bad || c.left || version != 2 || suite != 1 || !peer_sec_binding_valid(&b)) return -1;
    *out = b; return 0;
}
static int lane_valid(const struct peer_sec_lane *l)
{
    if (!l || !l->id || !l->generation || l->worker[0] >= 16 || l->worker[1] >= 16 ||
        !l->qpn[0] || !l->qpn[1] || l->qpn[0] > 0xffffff || l->qpn[1] > 0xffffff ||
        l->psn[0] > 0xffffff || l->psn[1] > 0xffffff ||
        !peer_sec_nonzero(l->gid[0], 16) || !peer_sec_nonzero(l->gid[1], 16)) return 0;
    return l->mtu == 256 || l->mtu == 512 || l->mtu == 1024 || l->mtu == 2048 || l->mtu == 4096;
}
int peer_sec_lane_encode(const struct peer_sec_lane *l, uint8_t out[PEER_SEC_LANE_LEN])
{
    if (!out || !lane_valid(l)) return -1;
    struct cursor c = {out, PEER_SEC_LANE_LEN, 0};
    number(&c, l->id, 8); number(&c, l->generation, 8);
    for (unsigned i = 0; i < 2; i++) {
        number(&c, l->worker[i], 2); number(&c, l->qpn[i], 3); number(&c, l->psn[i], 3);
        put(&c, l->gid[i], 16);
    }
    number(&c, l->mtu, 2);
    return c.bad || c.left ? -1 : 0;
}
int peer_sec_lane_decode(const void *buf, size_t len, struct peer_sec_lane *out)
{
    if (!buf || !out || len != PEER_SEC_LANE_LEN) return -1;
    struct cursor c = {(uint8_t *)buf, len, 0}; struct peer_sec_lane l = {0};
    l.id = take_number(&c, 8); l.generation = take_number(&c, 8);
    for (unsigned i = 0; i < 2; i++) {
        l.worker[i] = (uint16_t)take_number(&c, 2); l.qpn[i] = (uint32_t)take_number(&c, 3);
        l.psn[i] = (uint32_t)take_number(&c, 3); get(&c, l.gid[i], 16);
    }
    l.mtu = (uint16_t)take_number(&c, 2); memset(out, 0, sizeof(*out));
    if (c.bad || c.left || !lane_valid(&l)) return -1;
    *out = l; return 0;
}
int peer_sec_derive(struct peer_tls_conn *tls, const struct peer_sec_binding *b,
                    unsigned local, unsigned direction, uint8_t out[20])
{
    uint8_t context[PEER_TLS_IPSEC_CONTEXT_MAX], key[32]; size_t len = 0;
    if (!out) return -1;
    OPENSSL_cleanse(out, 20);
    if (local > 1 || direction > 1 || !peer_sec_binding_valid(b) ||
        peer_tls_local_key(tls, key) || CRYPTO_memcmp(key, b->endpoint[local].public_key, 32) ||
        peer_tls_peer_key(tls, key) || CRYPTO_memcmp(key, b->endpoint[1-local].public_key, 32) ||
        peer_sec_binding_encode(b, context, sizeof(context) - 1, &len)) return -1;
    context[len++] = (uint8_t)direction;
    int rc = peer_tls_export_ipsec(tls, context, len, out);
    OPENSSL_cleanse(context, sizeof(context)); return rc;
}
int peer_sec_session_id(const struct peer_sec_hello *a, const struct peer_sec_hello *b,
                        uint8_t out[16])
{
    uint8_t bytes[2048], digest[32]; size_t x, y; unsigned n = 0;
    if (!out || !hello_valid(a) || !hello_valid(b) || strcmp(a->cluster, b->cluster) ||
        !strcmp(a->node, b->node)) return -1;
    if (strcmp(a->node, b->node) > 0) { const struct peer_sec_hello *t = a; a = b; b = t; }
    /* Domain plus two self-delimiting canonical HELLOs, inside authenticated TLS. */
    static const char domain[] = "DPUmesh-control-session-v2";
    memcpy(bytes, domain, sizeof(domain));
    if (peer_sec_hello_encode(a, bytes + sizeof(domain), sizeof(bytes) - sizeof(domain), &x) ||
        peer_sec_hello_encode(b, bytes + sizeof(domain) + x, sizeof(bytes) - sizeof(domain) - x, &y) ||
        EVP_Digest(bytes, sizeof(domain) + x + y, digest, &n, EVP_sha256(), NULL) != 1 || n != 32)
        return -1;
    memcpy(out, digest, 16); return 0;
}

static int data_valid(const struct peer_sec_data_header *h)
{
    return h && h->type >= PEER_SEC_STREAM_OPEN && h->type <= PEER_SEC_STREAM_FIN &&
        h->body_len <= PEER_SEC_DATA_MAX && peer_sec_nonzero(h->association, 16) &&
        h->lane_id && h->lane_generation && h->handle && h->stream_generation &&
        (h->type != PEER_SEC_DATA || (h->body_len && h->sequence));
}
int peer_sec_data_encode(const struct peer_sec_data_header *h, uint8_t out[64])
{
    if (!out || !data_valid(h)) return -1;
    struct cursor c = {out, 64, 0};
    put(&c, "DMSD", 4); number(&c, PEER_SEC_VERSION, 2); number(&c, h->type, 2);
    number(&c, h->body_len, 4); put(&c, h->association, 16);
    number(&c, h->lane_id, 8); number(&c, h->lane_generation, 8);
    number(&c, h->handle, 4); number(&c, h->stream_generation, 8); number(&c, h->sequence, 8);
    return c.bad || c.left ? -1 : 0;
}
int peer_sec_data_decode(const void *buf, size_t len, const uint8_t association[16],
                         uint64_t lane_id, uint64_t lane_generation,
                         struct peer_sec_data_header *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!buf || !association || len < 64 || len > 64 + PEER_SEC_DATA_MAX) return -1;
    struct peer_sec_data_header h = {0}; struct cursor c = {(uint8_t *)buf, 64, 0};
    char magic[4]; get(&c, magic, 4); unsigned version = (unsigned)take_number(&c, 2);
    h.type = (uint16_t)take_number(&c, 2); h.body_len = (uint32_t)take_number(&c, 4);
    get(&c, h.association, 16); h.lane_id = take_number(&c, 8); h.lane_generation = take_number(&c, 8);
    h.handle = (uint32_t)take_number(&c, 4); h.stream_generation = take_number(&c, 8);
    h.sequence = take_number(&c, 8);
    if (c.bad || c.left || memcmp(magic, "DMSD", 4) || version != PEER_SEC_VERSION ||
        !data_valid(&h) || len != 64 + h.body_len || memcmp(h.association, association, 16) ||
        h.lane_id != lane_id || h.lane_generation != lane_generation) return -1;
    *out = h; return 0;
}

/* ---- Pod-pair negotiation bodies ---------------------------------------- */

static int mtu_valid(unsigned mtu)
{ return mtu == 256 || mtu == 512 || mtu == 1024 || mtu == 2048 || mtu == 4096; }
static void registration_put(struct cursor *c, const struct dmesh_peer_registration *r)
{
    put(c, r->daemon, sizeof(r->daemon)); number(c, r->slot, 4);
    number(c, r->generation, 8); put(c, r->nonce, sizeof(r->nonce));
}
static void registration_get(struct cursor *c, struct dmesh_peer_registration *r)
{
    get(c, r->daemon, sizeof(r->daemon)); r->slot = (uint32_t)take_number(c, 4);
    r->generation = take_number(c, 8); get(c, r->nonce, sizeof(r->nonce));
}
static int lane_ids_valid(unsigned count, const uint64_t *ids, const uint64_t *generations)
{
    if (!count || count > PEER_SEC_LANES_MAX) return 0;
    for (unsigned i = 0; i < count; i++) {
        if (!ids[i] || !generations[i]) return 0;
        if (i && ids[i-1] >= ids[i]) return 0;
    }
    return 1;
}
static int propose_valid(const struct peer_sec_propose *p)
{
    uint64_t ids[PEER_SEC_LANES_MAX], generations[PEER_SEC_LANES_MAX];
    if (!p || !string_len(p->cluster, sizeof(p->cluster)) ||
        !string_len(p->source_pod, sizeof(p->source_pod)) ||
        !string_len(p->target_pod, sizeof(p->target_pod)) ||
        !strcmp(p->source_pod, p->target_pod) || !dmesh_peer_registration_valid(&p->source) ||
        !peer_sec_nonzero(p->association, 16) || p->rx_spi < 256 || !p->incarnation ||
        !string_len(p->service, sizeof(p->service)) || !p->port || !mtu_valid(p->mtu) ||
        !p->lane_count || p->lane_count > PEER_SEC_LANES_MAX) return 0;
    for (unsigned i = 0; i < p->lane_count; i++) {
        ids[i] = p->lane[i].id; generations[i] = p->lane[i].generation;
        if (p->lane[i].worker[0] >= 16 || p->lane[i].worker[1] >= 16) return 0;
        for (unsigned j = 0; j < i; j++)
            if (p->lane[i].worker[0] == p->lane[j].worker[0] ||
                p->lane[i].worker[1] == p->lane[j].worker[1]) return 0;
    }
    return lane_ids_valid(p->lane_count, ids, generations);
}
int peer_sec_propose_encode(const struct peer_sec_propose *p, void *buf, size_t cap, size_t *len)
{
    if (len) *len = 0;
    if (!buf || !len || !propose_valid(p)) return -1;
    struct cursor c = {buf, cap, 0};
    number(&c, PEER_SEC_VERSION, 2);
    string_put(&c, p->cluster, sizeof(p->cluster));
    string_put(&c, p->source_pod, sizeof(p->source_pod));
    string_put(&c, p->target_pod, sizeof(p->target_pod));
    registration_put(&c, &p->source);
    put(&c, p->association, 16); number(&c, p->rx_spi, 4); number(&c, p->incarnation, 4);
    string_put(&c, p->service, sizeof(p->service));
    number(&c, p->port, 2); number(&c, p->mtu, 2); number(&c, p->lane_count, 2);
    for (unsigned i = 0; i < p->lane_count; i++) {
        number(&c, p->lane[i].id, 8); number(&c, p->lane[i].generation, 8);
        number(&c, p->lane[i].worker[0], 2); number(&c, p->lane[i].worker[1], 2);
    }
    if (c.bad) return -1;
    *len = cap - c.left; return 0;
}
int peer_sec_propose_decode(const void *buf, size_t len, struct peer_sec_propose *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!buf) return -1;
    struct peer_sec_propose p = {0}; struct cursor c = {(uint8_t *)buf, len, 0};
    unsigned version = (unsigned)take_number(&c, 2);
    string_get(&c, p.cluster, sizeof(p.cluster));
    string_get(&c, p.source_pod, sizeof(p.source_pod));
    string_get(&c, p.target_pod, sizeof(p.target_pod));
    registration_get(&c, &p.source);
    get(&c, p.association, 16); p.rx_spi = (uint32_t)take_number(&c, 4);
    p.incarnation = (uint32_t)take_number(&c, 4);
    string_get(&c, p.service, sizeof(p.service));
    p.port = (uint16_t)take_number(&c, 2); p.mtu = (uint16_t)take_number(&c, 2);
    p.lane_count = (uint16_t)take_number(&c, 2);
    if (c.bad || p.lane_count > PEER_SEC_LANES_MAX) return -1;
    for (unsigned i = 0; i < p.lane_count; i++) {
        p.lane[i].id = take_number(&c, 8); p.lane[i].generation = take_number(&c, 8);
        p.lane[i].worker[0] = (uint16_t)take_number(&c, 2);
        p.lane[i].worker[1] = (uint16_t)take_number(&c, 2);
    }
    if (c.bad || c.left || version != PEER_SEC_VERSION || !propose_valid(&p)) return -1;
    *out = p; return 0;
}
static int lane_params_valid(const struct peer_sec_lane_params *p, int accept)
{
    uint64_t ids[PEER_SEC_LANES_MAX], generations[PEER_SEC_LANES_MAX];
    if (!p || !peer_sec_nonzero(p->association, 16) || !mtu_valid(p->mtu)) return 0;
    if (accept) {
        if (p->rx_spi < 256 || !dmesh_peer_registration_valid(&p->registration)) return 0;
    } else if (p->rx_spi || peer_sec_nonzero(&p->registration, sizeof(p->registration))) return 0;
    for (unsigned i = 0; i < p->lane_count && i < PEER_SEC_LANES_MAX; i++) {
        const struct peer_sec_half_lane *l = &p->lane[i];
        ids[i] = l->id; generations[i] = l->generation;
        if (!l->qpn || l->qpn > 0xffffff || l->psn > 0xffffff || !peer_sec_nonzero(l->gid, 16)) return 0;
    }
    return lane_ids_valid(p->lane_count, ids, generations);
}
int peer_sec_lane_params_encode(const struct peer_sec_lane_params *p, int accept,
                                void *buf, size_t cap, size_t *len)
{
    if (len) *len = 0;
    if (!buf || !len || !lane_params_valid(p, accept)) return -1;
    struct cursor c = {buf, cap, 0};
    number(&c, PEER_SEC_VERSION, 2); number(&c, accept ? 1 : 0, 2);
    put(&c, p->association, 16);
    if (accept) { number(&c, p->rx_spi, 4); registration_put(&c, &p->registration); }
    number(&c, p->mtu, 2); number(&c, p->lane_count, 2);
    for (unsigned i = 0; i < p->lane_count; i++) {
        const struct peer_sec_half_lane *l = &p->lane[i];
        number(&c, l->id, 8); number(&c, l->generation, 8);
        number(&c, l->qpn, 3); number(&c, l->psn, 3); put(&c, l->gid, 16);
    }
    if (c.bad) return -1;
    *len = cap - c.left; return 0;
}
int peer_sec_lane_params_decode(const void *buf, size_t len, int accept, struct peer_sec_lane_params *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!buf) return -1;
    struct peer_sec_lane_params p = {0}; struct cursor c = {(uint8_t *)buf, len, 0};
    unsigned version = (unsigned)take_number(&c, 2), kind = (unsigned)take_number(&c, 2);
    get(&c, p.association, 16);
    if (accept) { p.rx_spi = (uint32_t)take_number(&c, 4); registration_get(&c, &p.registration); }
    p.mtu = (uint16_t)take_number(&c, 2); p.lane_count = (uint16_t)take_number(&c, 2);
    if (c.bad || p.lane_count > PEER_SEC_LANES_MAX) return -1;
    for (unsigned i = 0; i < p.lane_count; i++) {
        struct peer_sec_half_lane *l = &p.lane[i];
        l->id = take_number(&c, 8); l->generation = take_number(&c, 8);
        l->qpn = (uint32_t)take_number(&c, 3); l->psn = (uint32_t)take_number(&c, 3); get(&c, l->gid, 16);
    }
    if (c.bad || c.left || version != PEER_SEC_VERSION || kind != (accept ? 1u : 0u) ||
        !lane_params_valid(&p, accept)) return -1;
    *out = p; return 0;
}
int peer_sec_rekey_encode(const uint32_t spi[2], uint8_t out[PEER_SEC_REKEY_LEN])
{
    if (!spi || !out || (spi[0] && spi[0] < 256) || (spi[1] && spi[1] < 256) || (!spi[0] && !spi[1]))
        return -1;
    struct cursor c = {out, PEER_SEC_REKEY_LEN, 0};
    number(&c, PEER_SEC_VERSION, 2); number(&c, spi[0], 4); number(&c, spi[1], 4);
    return c.bad || c.left ? -1 : 0;
}
int peer_sec_rekey_decode(const void *buf, size_t len, uint32_t spi[2])
{
    if (!spi) return -1;
    spi[0] = spi[1] = 0;
    if (!buf || len != PEER_SEC_REKEY_LEN) return -1;
    struct cursor c = {(uint8_t *)buf, len, 0};
    unsigned version = (unsigned)take_number(&c, 2);
    uint32_t a = (uint32_t)take_number(&c, 4), b = (uint32_t)take_number(&c, 4);
    if (c.bad || c.left || version != PEER_SEC_VERSION || (a && a < 256) || (b && b < 256) || (!a && !b))
        return -1;
    spi[0] = a; spi[1] = b; return 0;
}
int peer_sec_error_encode(uint16_t code, uint8_t out[PEER_SEC_ERROR_LEN])
{
    if (!out || code < PEER_SEC_ERR_REFUSED || code > PEER_SEC_ERR_MISMATCH) return -1;
    struct cursor c = {out, PEER_SEC_ERROR_LEN, 0};
    number(&c, PEER_SEC_VERSION, 2); number(&c, code, 2);
    return c.bad || c.left ? -1 : 0;
}
int peer_sec_error_decode(const void *buf, size_t len, uint16_t *code)
{
    if (!code) return -1;
    *code = 0;
    if (!buf || len != PEER_SEC_ERROR_LEN) return -1;
    struct cursor c = {(uint8_t *)buf, len, 0};
    unsigned version = (unsigned)take_number(&c, 2), value = (unsigned)take_number(&c, 2);
    if (c.bad || c.left || version != PEER_SEC_VERSION || value < PEER_SEC_ERR_REFUSED ||
        value > PEER_SEC_ERR_MISMATCH) return -1;
    *code = (uint16_t)value; return 0;
}
