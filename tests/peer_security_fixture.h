#ifndef PEER_SECURITY_FIXTURE_H
#define PEER_SECURITY_FIXTURE_H
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../doca/peer_security_wire.h"
struct security_fixture {
    struct peer_tls_ctx *ctx[2];
    struct peer_tls_conn *tls[2];
    struct peer_sec_binding binding;
    struct peer_sec_lane lane;
};
static inline void security_identity(struct security_fixture *f)
{
    memset(f, 0, sizeof(*f));
    strcpy(f->binding.cluster, "test-cluster");
    for (unsigned i = 0; i < 2; i++) {
        uint8_t seed[32] = {0}; seed[0] = (uint8_t)(i + 1);
        struct peer_sec_endpoint *e = &f->binding.endpoint[i];
        snprintf(e->node, sizeof(e->node), "node-%c", 'a' + i);
        snprintf(e->pod, sizeof(e->pod), "pod-%c", 'a' + i);
        assert(!peer_tls_ctx_new(seed, e->node, &f->ctx[i], NULL, 0));
        peer_tls_ctx_public_key(f->ctx[i], e->public_key);
        e->boot[0] = (uint8_t)(i + 1); e->daemon[0] = (uint8_t)(i + 11);
        e->registration_nonce[0] = (uint8_t)(i + 21); e->channel_generation = 1;
        f->binding.spi[i] = 1024 + i; f->lane.qpn[i] = 100 + i;
        f->lane.psn[i] = 20 + i; f->lane.gid[i][15] = (uint8_t)(i + 1);
    }
    f->binding.session[0] = 1; f->binding.association[0] = 2; f->binding.epoch = 1;
    f->lane.id = f->lane.generation = 1; f->lane.mtu = 1024;
}
static inline void security_pump(struct peer_tls_conn *from, struct peer_tls_conn *to)
{
    uint8_t buf[71]; long n;
    while ((n = peer_tls_out(from, buf, sizeof(buf))) > 0)
        assert(!peer_tls_in(to, buf, (size_t)n));
    assert(n == 0);
}
static inline void security_handshake(struct security_fixture *f, int pin)
{
    for (unsigned i = 0; i < 2; i++) assert(!peer_tls_conn_new(f->ctx[i], !i, &f->tls[i]));
    for (unsigned n = 0; n < 100; n++) {
        for (unsigned i = 0; i < 2; i++) {
            assert(peer_tls_handshake(f->tls[i]) >= 0);
            security_pump(f->tls[i], f->tls[1-i]);
        }
        if (peer_tls_established(f->tls[0]) && peer_tls_established(f->tls[1])) break;
    }
    for (unsigned i = 0; i < 2; i++) {
        assert(peer_tls_established(f->tls[i]));
        if (pin) assert(!peer_tls_pin(f->tls[i], f->binding.endpoint[1-i].public_key));
    }
}
static inline void security_free(struct security_fixture *f)
{
    for (unsigned i = 0; i < 2; i++) { peer_tls_conn_free(f->tls[i]); peer_tls_ctx_free(f->ctx[i]); }
}
#endif
