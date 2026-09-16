#include "peer_control.h"
#include <stdlib.h>
#include <string.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define CONTROL_QUEUE 8u
#define SECOND_NS 1000000000ull
struct frame { size_t len; uint8_t bytes[PEER_SEC_FRAME_MAX]; };
struct peer_control {
    struct peer_control_config config;
    struct peer_sec_hello remote;
    struct peer_tls_conn *tls;
    const struct peer_wire_ops *ops;
    void *wire;
    uint64_t setup_deadline, lease_deadline, next_ping, last_now, authority_generation;
    uint64_t sent, received;
    uint8_t session[16], challenge[16], last_digest[32];
    int faulted, hello_sent, ready, ping_pending, rx_blocked;
    struct frame queue[CONTROL_QUEUE];
    unsigned head, count;
    uint8_t cipher[PEER_WIRE_MSG_MAX];
    size_t cipher_len;
    uint8_t plain[PEER_SEC_FRAME_MAX];
    size_t plain_len;
};
static uint64_t after(uint64_t now, uint64_t delta)
{
    return delta > UINT64_MAX - now ? UINT64_MAX : now + delta;
}
static int fault(struct peer_control *c) { if (c) c->faulted = 1; return -1; }
int peer_control_faulted(const struct peer_control *c)
{
    return !c || c->faulted || peer_tls_faulted(c->tls) || c->ops->faulted(c->wire);
}
int peer_control_ready(const struct peer_control *c, uint64_t now)
{
    return !peer_control_faulted(c) && c->ready && now >= c->last_now &&
        now < c->lease_deadline && now < c->config.authority_deadline_ns;
}
const uint8_t *peer_control_session(const struct peer_control *c)
{ return c && c->ready ? c->session : NULL; }
const struct peer_sec_hello *peer_control_remote(const struct peer_control *c)
{ return c && c->ready ? &c->remote : NULL; }
struct peer_tls_conn *peer_control_tls(struct peer_control *c, uint64_t now)
{ return peer_control_ready(c, now) ? c->tls : NULL; }
int peer_control_binding_valid(const struct peer_control *c, const struct peer_sec_binding *b,
                               unsigned local, uint64_t now)
{
    if (local > 1 || !peer_control_ready(c, now) || !peer_sec_binding_valid(b) ||
        strcmp(b->cluster, c->config.local.cluster) || memcmp(b->session, c->session, 16)) return 0;
    const struct peer_sec_hello *h[2]; h[local] = &c->config.local; h[1-local] = &c->remote;
    for (unsigned i = 0; i < 2; i++)
        if (strcmp(b->endpoint[i].node, h[i]->node) || memcmp(b->endpoint[i].public_key, h[i]->public_key, 32) ||
            memcmp(b->endpoint[i].boot, h[i]->boot, 16)) return 0;
    return 1;
}
int peer_control_new(const struct peer_control_config *cfg, struct peer_tls_ctx *tls,
                     int initiator, const struct peer_wire_ops *ops, void *wire,
                     uint64_t now, struct peer_control **out)
{
    if (!out) return -1;
    *out = NULL;
    uint8_t body[1024], public_key[32]; size_t len;
    if (!cfg || !tls || !ops || !wire || !ops->established || !ops->faulted ||
        !ops->send_msg || !ops->recv_msg || !ops->close ||
        cfg->authority_deadline_ns <= now ||
        !memchr(cfg->remote_node, 0, sizeof(cfg->remote_node)) ||
        (cfg->remote_node[0] ? !peer_sec_nonzero(cfg->remote_key, 32) : !cfg->resolve)) return -1;
    struct peer_sec_hello local = cfg->local;
    if (RAND_bytes(local.nonce, sizeof(local.nonce)) != 1 ||
        peer_sec_hello_encode(&local, body, sizeof(body), &len) ||
        (cfg->remote_node[0] && !strcmp(local.node, cfg->remote_node))) return -1;
    peer_tls_ctx_public_key(tls, public_key);
    if (CRYPTO_memcmp(public_key, cfg->local.public_key, 32)) return -1;
    struct peer_control *c = calloc(1, sizeof(*c));
    if (!c) return -1;
    c->config = *cfg; c->ops = ops; c->wire = wire; c->last_now = now;
    c->config.local = local;
    if (!c->config.setup_timeout_ns) c->config.setup_timeout_ns = 5 * SECOND_NS;
    if (!c->config.control_lease_ns) c->config.control_lease_ns = 5 * SECOND_NS;
    if (c->config.control_lease_ns < 4) { free(c); return -1; }
    c->setup_deadline = after(now, c->config.setup_timeout_ns);
    if (peer_tls_conn_new(tls, initiator, &c->tls)) { free(c); return -1; }
    *out = c; return 0;
}
void peer_control_free(struct peer_control *c)
{
    if (!c) return;
    c->ops->close(c->wire); peer_tls_conn_free(c->tls);
    OPENSSL_cleanse(c, sizeof(*c)); free(c);
}
static int enqueue(struct peer_control *c, uint16_t type, const uint8_t *association,
                   uint64_t epoch, const void *body, size_t len)
{
    if (len > PEER_SEC_BODY_MAX || (len && !body) || c->sent == UINT64_MAX) return -1;
    if (c->count == CONTROL_QUEUE) return 0;
    struct peer_sec_header h = {.type = type, .body_len = (uint32_t)len,
                                .operation = c->sent + 1, .epoch = epoch};
    if (type != PEER_SEC_HELLO) memcpy(h.session, c->session, 16);
    if (association) memcpy(h.association, association, 16);
    struct frame *f = &c->queue[(c->head + c->count) % CONTROL_QUEUE];
    if (peer_sec_header_encode(&h, f->bytes)) return -1;
    if (len) memcpy(f->bytes + 64, body, len);
    f->len = 64 + len; c->count++; c->sent++; return 1;
}
int peer_control_send(struct peer_control *c, uint16_t type, const uint8_t association[16],
                      uint64_t epoch, const void *body, size_t len, uint64_t now)
{
    if (!peer_control_ready(c, now) || type == PEER_SEC_HELLO ||
        type == PEER_SEC_PING || type == PEER_SEC_PONG) return -1;
    return enqueue(c, type, association, epoch, body, len);
}
static int receive_frame(struct peer_control *c, const struct peer_sec_header *h,
                         const uint8_t *body, uint64_t now)
{
    if (!c->ready) {
        if (h->type != PEER_SEC_HELLO || h->operation != 1 ||
            peer_sec_hello_decode(body, h->body_len, &c->remote) ||
            strcmp(c->remote.cluster, c->config.local.cluster) ||
            strcmp(c->remote.node, c->config.remote_node) ||
            CRYPTO_memcmp(c->remote.public_key, c->config.remote_key, 32) ||
            peer_sec_session_id(&c->config.local, &c->remote, c->session)) return -1;
        c->ready = 1; c->lease_deadline = after(now, c->config.control_lease_ns);
        c->next_ping = now; return 0;
    }
    if (h->type == PEER_SEC_HELLO || CRYPTO_memcmp(h->session, c->session, 16)) return -1;
    if (h->type == PEER_SEC_PING) {
        int rc = enqueue(c, PEER_SEC_PONG, NULL, 0, body, 16);
        return rc < 0 ? -1 : rc == 0 ? 1 : 0;
    }
    if (h->type == PEER_SEC_PONG) {
        if (!c->ping_pending || CRYPTO_memcmp(body, c->challenge, 16)) return -1;
        c->ping_pending = 0;
        c->lease_deadline = after(now, c->config.control_lease_ns);
        c->next_ping = after(now, c->config.control_lease_ns / 3); return 0;
    }
    return c->config.message ? c->config.message(c->config.message_ctx, h, body) : -1;
}
static int consume(struct peer_control *c, uint64_t now)
{
    for (unsigned budget = 0; budget < 16; budget++) {
        struct peer_sec_header h;
        int rc = peer_sec_header_decode(c->plain, c->plain_len, &h);
        if (rc < 0) return -1;
        if (rc > 0 || c->plain_len < 64 + h.body_len) return 0;
        size_t len = 64 + h.body_len;
        uint8_t digest[32]; unsigned digest_len;
        if (EVP_Digest(c->plain, len, digest, &digest_len, EVP_sha256(), NULL) != 1 ||
            digest_len != 32) return -1;
        if (h.operation == c->received) {
            if (CRYPTO_memcmp(digest, c->last_digest, 32)) return -1;
        } else {
            if (c->received == UINT64_MAX || h.operation != c->received + 1) return -1;
            rc = receive_frame(c, &h, c->plain + 64, now);
            if (rc) return rc;
            c->received = h.operation; memcpy(c->last_digest, digest, 32);
        }
        c->plain_len -= len; memmove(c->plain, c->plain + len, c->plain_len);
    }
    return 0;
}
int peer_control_progress(struct peer_control *c, uint64_t now)
{
    if (peer_control_faulted(c) || now < c->last_now || now >= c->config.authority_deadline_ns ||
        (!c->ready && now >= c->setup_deadline) ||
        (c->ready && now >= c->lease_deadline)) return fault(c);
    c->last_now = now;
    if (!c->ops->established(c->wire)) return 0;
    /* One wire message per turn bounds both BIO growth and owner work. */
    uint8_t incoming[PEER_WIRE_MSG_MAX];
    if (!c->rx_blocked && !peer_tls_in_pending(c->tls) && c->plain_len < sizeof(c->plain)) {
        long n = c->ops->recv_msg(c->wire, incoming, sizeof(incoming));
        if (n < 0 || (n > 0 && peer_tls_in(c->tls, incoming, (size_t)n))) return fault(c);
    }
    if (!peer_tls_established(c->tls)) {
        int rc = peer_tls_handshake(c->tls);
        if (rc < 0) return fault(c);
        if (rc == 1 && !c->config.remote_node[0]) {
            /* The authenticated key names the peer; the pin below then checks
             * the same key, so only a key the snapshot binds gets a session. */
            uint8_t key[32];
            if (peer_tls_peer_key(c->tls, key) ||
                c->config.resolve(c->config.resolve_ctx, key, c->config.remote_node) != 1 ||
                !memchr(c->config.remote_node, 0, sizeof(c->config.remote_node)) ||
                !c->config.remote_node[0] ||
                !strcmp(c->config.remote_node, c->config.local.node)) return fault(c);
            memcpy(c->config.remote_key, key, 32);
        }
        if (rc == 1 && peer_tls_pin(c->tls, c->config.remote_key)) return fault(c);
    }
    if (peer_tls_established(c->tls)) {
        if (!c->hello_sent) {
            uint8_t body[1024]; size_t len;
            if (peer_sec_hello_encode(&c->config.local, body, sizeof(body), &len) ||
                enqueue(c, PEER_SEC_HELLO, NULL, 0, body, len) != 1) return fault(c);
            c->hello_sent = 1;
        }
        int rc = consume(c, now);
        if (rc < 0) return fault(c);
        c->rx_blocked = rc == 1;
        if (rc == 0 && c->plain_len < sizeof(c->plain)) {
            long n = peer_tls_read(c->tls, c->plain + c->plain_len,
                                    sizeof(c->plain) - c->plain_len);
            if (n < 0) return fault(c);
            c->plain_len += (size_t)n;
            rc = consume(c, now);
            if (rc < 0) return fault(c);
            c->rx_blocked = rc == 1;
        }
        if (c->ready && !c->ping_pending && now >= c->next_ping && c->count < CONTROL_QUEUE) {
            if (RAND_bytes(c->challenge, 16) != 1 ||
                enqueue(c, PEER_SEC_PING, NULL, 0, c->challenge, 16) != 1) return fault(c);
            c->ping_pending = 1;
        }
    }
    /* Keep refused ciphertext verbatim. Never re-encrypt after wire pressure. */
    if (!c->cipher_len) {
        long n = peer_tls_out(c->tls, c->cipher, sizeof(c->cipher));
        if (n < 0) return fault(c);
        c->cipher_len = (size_t)n;
    }
    if (c->cipher_len) {
        int rc = c->ops->send_msg(c->wire, c->cipher, c->cipher_len);
        if (rc < 0) return fault(c);
        if (!rc) return 0;
        c->cipher_len = 0;
    }
    if (peer_tls_established(c->tls) && !peer_tls_out_pending(c->tls) && c->count) {
        struct frame *f = &c->queue[c->head];
        if (peer_tls_write(c->tls, f->bytes, f->len)) return fault(c);
        c->head = (c->head + 1) % CONTROL_QUEUE; c->count--;
    }
    return 0;
}
int peer_control_authority(struct peer_control *c, uint64_t generation,
                           uint64_t deadline, uint64_t now)
{
    if (peer_control_faulted(c) || now < c->last_now || now >= c->config.authority_deadline_ns ||
        (c->ready && now >= c->lease_deadline) || generation <= c->authority_generation ||
        (!c->ready && now >= c->setup_deadline) ||
        deadline <= now) return -1;
    c->authority_generation = generation; c->config.authority_deadline_ns = deadline;
    c->last_now = now;
    return 0;
}
uint64_t peer_control_lease_deadline(const struct peer_control *c)
{ return c && c->ready && !c->faulted ? c->lease_deadline : 0; }
