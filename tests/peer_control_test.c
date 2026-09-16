#include "peer_security_fixture.h"
#include "../doca/peer_control.h"
#include "../doca/peer_association.h"
#include <arpa/inet.h>

struct receive_state {
    unsigned received, retry;
    struct peer_associations *manager;
    struct peer_assoc_token token;
    uint64_t now;
    struct peer_sec_header last;
};
static int message(void *arg, const struct peer_sec_header *h, const uint8_t *body)
{
    struct receive_state *r = arg;
    if (r->retry) return 1;
    if (r->manager) {
        assert(h->body_len == 32);
        struct peer_assoc_token token;
        int rc = peer_associations_lookup(r->manager, h->session, h->association, &token);
        if (rc != 1) return rc < 0 ? -1 : 0;
        rc = peer_association_signal(r->manager, token, h->type, h->epoch, body, r->now);
        return rc < 0 ? -1 : 0; /* A stale signal is consumed, never retried forever. */
    }
    assert(h->type == PEER_SEC_PROPOSE && h->body_len == PEER_SEC_BODY_MAX);
    for (unsigned i = 0; i < h->body_len; i++) assert(body[i] == (uint8_t)(r->received + i));
    r->last = *h; r->received++; return 0;
}
struct control_fixture {
    struct security_fixture security;
    const struct peer_wire_ops *ops[2];
    void *wire_ctx[2];
    struct peer_control *control[2];
    struct receive_state receive[2];
    uint64_t now;
};
static void progress(struct control_fixture *f, unsigned rounds)
{
    for (unsigned n = 0; n < rounds; n++) {
        for (unsigned i = 0; i < 2; i++) {
            int accepted = 0;
            f->ops[i]->progress(f->wire_ctx[i], NULL, 0, &accepted);
            f->receive[i].now = f->now;
            assert(peer_control_progress(f->control[i], f->now) == 0);
        }
    }
}
static int g_resolve, g_resolve_refuse;
static int resolve(void *v, const uint8_t key[32], char node[PEER_SEC_NAME_MAX])
{
    struct security_fixture *sec = v;
    if (g_resolve_refuse || memcmp(key, sec->binding.endpoint[0].public_key, 32)) return 0;
    strcpy(node, sec->binding.endpoint[0].node); return 1;
}
static void init(struct control_fixture *f, int wrong_pin)
{
    memset(f, 0, sizeof(*f)); security_identity(&f->security); f->now = 1;
    char error[256]; void *wire[2] = {0};
    for (unsigned i = 0; i < 2; i++)
        assert(!peer_wire_tcp_new(htonl(INADDR_LOOPBACK), 0, &f->ops[i], &f->wire_ctx[i], error, sizeof(error)));
    assert(!f->ops[0]->connect(f->wire_ctx[0], htonl(INADDR_LOOPBACK), peer_wire_tcp_port(f->wire_ctx[1]), &wire[0]));
    for (unsigned n = 0; n < 10000 && !wire[1]; n++) {
        int count = 0;
        f->ops[0]->progress(f->wire_ctx[0], NULL, 0, &count);
        f->ops[1]->progress(f->wire_ctx[1], &wire[1], 1, &count);
    }
    assert(wire[1]);
    for (unsigned i = 0; i < 2; i++) {
        struct peer_control_config cfg = {.authority_deadline_ns = 100000000000ull,
                                         .message = message, .message_ctx = &f->receive[i]};
        struct peer_sec_endpoint *e = &f->security.binding.endpoint[i];
        strcpy(cfg.local.cluster, f->security.binding.cluster); strcpy(cfg.local.node, e->node);
        memcpy(cfg.local.boot, e->boot, 16); memcpy(cfg.local.public_key, e->public_key, 32);
        cfg.local.nonce[0] = (uint8_t)(i + 10); cfg.local.workers = 2;
        strcpy(cfg.remote_node, f->security.binding.endpoint[1-i].node);
        memcpy(cfg.remote_key, f->security.binding.endpoint[1-i].public_key, 32);
        if (wrong_pin && !i) cfg.remote_key[0] ^= 1;
        if (g_resolve && i) {
            /* The responder learns who dialled from the authenticated key. */
            memset(cfg.remote_node, 0, sizeof(cfg.remote_node)); memset(cfg.remote_key, 0, sizeof(cfg.remote_key));
            cfg.resolve = resolve; cfg.resolve_ctx = &f->security;
        }
        assert(!peer_control_new(&cfg, f->security.ctx[i], !i, f->ops[i], wire[i], f->now, &f->control[i]));
    }
}
static void ready(struct control_fixture *f)
{
    for (unsigned n = 0; n < 10000; n++) {
        progress(f, 1);
        if (peer_control_ready(f->control[0], f->now) && peer_control_ready(f->control[1], f->now)) break;
    }
    for (unsigned i = 0; i < 2; i++) assert(peer_control_ready(f->control[i], f->now));
    assert(!memcmp(peer_control_session(f->control[0]), peer_control_session(f->control[1]), 16));
    memcpy(f->security.binding.session, peer_control_session(f->control[0]), 16);
    for (unsigned i = 0; i < 2; i++) assert(peer_control_binding_valid(f->control[i], &f->security.binding, i, f->now));
}
static void finish(struct control_fixture *f)
{
    for (unsigned i = 0; i < 2; i++) { peer_control_free(f->control[i]); f->ops[i]->ctx_free(f->wire_ctx[i]); }
    security_free(&f->security);
}
static void test_control_backpressure_and_liveness(void)
{
    struct control_fixture f; init(&f, 0); ready(&f);
    uint8_t body[PEER_SEC_BODY_MAX]; unsigned sent = 0; int blocked = 0;
    f.receive[1].retry = 1;
    /* Fill the bounded application queue without progressing it. */
    for (unsigned n = 0; n < 32; n++) {
        for (unsigned j = 0; j < sizeof(body); j++) body[j] = (uint8_t)(sent + j);
        int rc = peer_control_send(f.control[0], PEER_SEC_PROPOSE, f.security.binding.association,
                                    1, body, sizeof(body), f.now);
        assert(rc >= 0);
        if (!rc) { blocked = 1; break; }
        sent++;
    }
    assert(blocked && sent > 0 && sent <= 8);
    progress(&f, 1000); assert(f.receive[1].received == 0);
    f.receive[1].retry = 0;
    for (unsigned n = 0; n < 10000 && f.receive[1].received < sent; n++) progress(&f, 1);
    assert(f.receive[1].received == sent);
    /* Ten seconds of challenge/response keep a five-second control lease alive. */
    for (unsigned n = 0; n < 10; n++) { f.now += 1000000000ull; progress(&f, 1000); }
    assert(peer_control_ready(f.control[0], f.now));
    assert(!peer_control_authority(f.control[0], 1, f.now + 1000000000ull, f.now));
    assert(peer_control_authority(f.control[0], 1, f.now + 2000000000ull, f.now) < 0);
    assert(peer_control_progress(f.control[0], f.now + 1000000000ull) < 0);
    assert(!peer_control_tls(f.control[0], f.now + 1000000000ull));
    finish(&f);
}
static void test_bad_pin(void)
{
    struct control_fixture f; init(&f, 1); int failed = 0;
    for (unsigned n = 0; n < 10000 && !failed; n++) {
        for (unsigned i = 0; i < 2; i++) {
            int count;
            f.ops[i]->progress(f.wire_ctx[i], NULL, 0, &count);
            if (peer_control_progress(f.control[i], f.now) < 0) failed = 1;
        }
    }
    assert(failed && !peer_control_ready(f.control[0], f.now)); finish(&f);
}
static void test_resolve_inbound(void)
{
    g_resolve = 1;
    struct control_fixture f; init(&f, 0); ready(&f);
    assert(!strcmp(peer_control_remote(f.control[1])->node, f.security.binding.endpoint[0].node));
    finish(&f);
    /* A key the snapshot binds to no node gets no session. */
    g_resolve_refuse = 1;
    init(&f, 0); int failed = 0;
    for (unsigned n = 0; n < 10000 && !failed; n++) {
        for (unsigned i = 0; i < 2; i++) {
            int count;
            f.ops[i]->progress(f.wire_ctx[i], NULL, 0, &count);
            if (peer_control_progress(f.control[i], f.now) < 0) failed = 1;
        }
    }
    assert(failed && peer_control_faulted(f.control[1]) && !peer_control_ready(f.control[0], f.now));
    finish(&f);
    g_resolve = g_resolve_refuse = 0;
}
static void test_duplicate_control_frame(void)
{
    struct control_fixture f; init(&f, 0); ready(&f); progress(&f, 1000);
    uint8_t bytes[64 + PEER_SEC_BODY_MAX];
    for (unsigned i = 0; i < PEER_SEC_BODY_MAX; i++) bytes[64 + i] = (uint8_t)i;
    assert(peer_control_send(f.control[0], PEER_SEC_PROPOSE, f.security.binding.association,
                             1, bytes + 64, PEER_SEC_BODY_MAX, f.now) == 1);
    progress(&f, 1000); assert(f.receive[1].received == 1);
    assert(!peer_sec_header_encode(&f.receive[1].last, bytes));
    assert(!peer_tls_write(peer_control_tls(f.control[0], f.now), bytes, sizeof(bytes)));
    progress(&f, 1000); assert(f.receive[1].received == 1); /* identical duplicate is not re-executed */
    bytes[64] ^= 1;
    assert(!peer_tls_write(peer_control_tls(f.control[0], f.now), bytes, sizeof(bytes)));
    int failed = 0;
    for (unsigned n = 0; n < 1000 && !failed; n++) {
        for (unsigned i = 0; i < 2; i++) {
            int count; f.ops[i]->progress(f.wire_ctx[i], NULL, 0, &count);
            if (peer_control_progress(f.control[i], f.now) < 0) failed = 1;
        }
    }
    assert(failed && peer_control_faulted(f.control[1])); finish(&f);
}
static void drive_associations(struct control_fixture *f)
{
    for (unsigned round = 0; round < 5000; round++) {
        for (unsigned i = 0; i < 2; i++) {
            struct peer_crypto_request r; struct peer_assoc_event e;
            if (peer_associations_next(f->receive[i].manager, f->now, &r) == 1) {
                assert(!peer_association_complete(f->receive[i].manager, &r, 0, f->now));
                peer_crypto_request_cleanse(&r);
            }
            if (peer_associations_event(f->receive[i].manager, &e) == 1) {
                int rc = peer_control_send(f->control[i], e.type, e.association, e.epoch, e.digest, 32, f->now);
                assert(rc >= 0);
                if (rc) assert(!peer_association_event_sent(f->receive[i].manager, &e));
            }
        }
        progress(f, 1);
    }
}
static void test_association_barriers_over_real_tls(void)
{
    struct control_fixture f; init(&f, 0); ready(&f);
    struct peer_assoc_limits limits = {.pairs = 1, .lanes = 1, .sas = 4,
                                      .setup_timeout_ns = 5000000000ull, .overlap_ns = 100};
    uint64_t deadlines[3] = {100000000000ull, 100000000000ull, 100000000000ull};
    for (unsigned i = 0; i < 2; i++) {
        assert(!peer_associations_new(&limits, &f.receive[i].manager));
        assert(!peer_association_open(f.receive[i].manager, peer_control_tls(f.control[i], f.now),
                                      &f.security.binding, i, &f.security.lane, 1, deadlines, f.now,
                                      &f.receive[i].token));
    }
    drive_associations(&f);
    for (unsigned i = 0; i < 2; i++) assert(peer_association_admit(f.receive[i].manager, f.receive[i].token, f.now));
    uint32_t spi[2] = {7000, 7001};
    for (unsigned i = 0; i < 2; i++) assert(!peer_association_rekey(f.receive[i].manager, f.receive[i].token, spi, f.now));
    drive_associations(&f);
    for (unsigned i = 0; i < 2; i++) {
        assert(peer_association_admit(f.receive[i].manager, f.receive[i].token, f.now));
        assert(!peer_association_revoke(f.receive[i].manager, f.receive[i].token));
    }
    drive_associations(&f);
    for (unsigned i = 0; i < 2; i++) {
        assert(!peer_association_release(f.receive[i].manager, f.receive[i].token));
        assert(!peer_associations_free(f.receive[i].manager)); f.receive[i].manager = NULL;
    }
    finish(&f);
}
int main(void)
{
    test_control_backpressure_and_liveness(); test_bad_pin(); test_resolve_inbound(); test_duplicate_control_frame();
    test_association_barriers_over_real_tls();
    puts("peer_control_test: PASS (localhost TCP/TLS, pin, inbound resolve, backpressure, leases, pair/rekey barriers)"); return 0;
}
