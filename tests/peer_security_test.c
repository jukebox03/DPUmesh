#include "peer_security_fixture.h"
#include "../doca/peer_association.h"
#include <sys/wait.h>
#include <unistd.h>

static void test_codec_and_exporter(void)
{
    struct security_fixture f; security_identity(&f);
    uint8_t key[2][20], saved[20], bytes[4096], encoded[4096]; size_t n, n2;
    assert(!peer_sec_binding_encode(&f.binding, bytes, sizeof(bytes), &n));
    assert(bytes[0] == 0 && bytes[1] == 2 && bytes[2] == 0 && bytes[3] == 1);
    struct peer_sec_binding decoded;
    for (size_t cut = 0; cut < n; cut++) assert(peer_sec_binding_decode(bytes, cut, &decoded) < 0);
    assert(!peer_sec_binding_decode(bytes, n, &decoded));
    assert(!peer_sec_binding_encode(&decoded, encoded, sizeof(encoded), &n2));
    assert(n == n2 && !memcmp(bytes, encoded, n));
    bytes[n] = 0; assert(peer_sec_binding_decode(bytes, n + 1, &decoded) < 0);
    bytes[6] = 0; assert(peer_sec_binding_decode(bytes, n, &decoded) < 0);
    uint8_t lane[PEER_SEC_LANE_LEN]; struct peer_sec_lane l;
    assert(!peer_sec_lane_encode(&f.lane, lane));
    assert(!peer_sec_lane_decode(lane, sizeof(lane), &l) && l.qpn[0] == 100 && l.mtu == 1024);
    f.lane.qpn[0] = 0x1000000; assert(peer_sec_lane_encode(&f.lane, lane) < 0);
    struct peer_sec_header h = {.type = PEER_SEC_RX_READY, .operation = 0x0102030405060708ull,
                                .body_len = 32, .epoch = 1}, back;
    h.session[0] = h.association[0] = 1;
    assert(!peer_sec_header_encode(&h, bytes));
    assert(bytes[32] == 1 && bytes[39] == 8 && bytes[15] == 32);
    assert(peer_sec_header_decode(bytes, 63, &back) == 1);
    assert(!peer_sec_header_decode(bytes, 64, &back));
    bytes[8] = 1; assert(peer_sec_header_decode(bytes, 64, &back) < 0); bytes[8] = 0;
    bytes[12] = 0x7f; assert(peer_sec_header_decode(bytes, 64, &back) < 0);
    memset(key[0], 0xaa, 20);
    assert(peer_sec_derive(NULL, &f.binding, 0, 0, key[0]) < 0 && !peer_sec_nonzero(key[0], 20));
    security_handshake(&f, 0);
    assert(peer_sec_derive(f.tls[0], &f.binding, 0, 0, key[0]) < 0);
    for (unsigned i = 0; i < 2; i++) assert(!peer_tls_pin(f.tls[i], f.binding.endpoint[1-i].public_key));
    for (unsigned i = 0; i < 2; i++) assert(!peer_sec_derive(f.tls[i], &f.binding, i, 0, key[i]));
    assert(!memcmp(key[0], key[1], 20)); memcpy(saved, key[0], 20);
    assert(!peer_sec_derive(f.tls[0], &f.binding, 0, 1, key[0])); assert(memcmp(saved, key[0], 20));
    for (unsigned change = 0; change < 7; change++) {
        struct peer_sec_binding b = f.binding;
        switch (change) {
        case 0: strcpy(b.endpoint[1].pod, "another-pod"); break;
        case 1: b.epoch++; break;
        case 2: b.endpoint[1].channel_generation++; break;
        case 3: b.association[0]++; break;
        case 4: b.session[0]++; break;
        case 5: b.spi[0]++; break;
        case 6: b.endpoint[1].registration_nonce[31]++; break;
        }
        assert(!peer_sec_derive(f.tls[0], &b, 0, 0, key[0])); assert(memcmp(saved, key[0], 20));
    }
    struct security_fixture other; security_identity(&other); security_handshake(&other, 1);
    assert(!peer_sec_derive(other.tls[0], &other.binding, 0, 0, key[0]));
    assert(memcmp(saved, key[0], 20)); security_free(&other);
    uint8_t wrong[32] = {1};
    assert(peer_tls_pin(f.tls[0], wrong) < 0 && peer_tls_faulted(f.tls[0]));
    assert(peer_sec_derive(f.tls[0], &f.binding, 0, 0, key[0]) < 0 && !peer_sec_nonzero(key[0], 20));
    security_free(&f);
}

static void test_data_binding(void)
{
    uint8_t bytes[64 + PEER_SEC_DATA_MAX];
    struct peer_sec_data_header h = {.type = PEER_SEC_DATA, .body_len = PEER_SEC_DATA_MAX,
        .lane_id = 15, .lane_generation = 40, .handle = 0x80001000u, .stream_generation = 9, .sequence = 1}, out;
    h.association[0] = 5;
    assert(!peer_sec_data_encode(&h, bytes));
    memset(bytes + 64, 0xa5, sizeof(bytes) - 64);
    assert(!peer_sec_data_decode(bytes, sizeof(bytes), h.association, 15, 40, &out));
    assert(out.handle == 0x80001000u && out.stream_generation == 9 && out.body_len == PEER_SEC_DATA_MAX);
    assert(peer_sec_data_decode(bytes, sizeof(bytes) - 1, h.association, 15, 40, &out) < 0);
    assert(peer_sec_data_decode(bytes, sizeof(bytes), h.association, 15, 41, &out) < 0);
    assert(peer_sec_data_decode(bytes, sizeof(bytes), h.association, 16, 40, &out) < 0);
    uint8_t other_pair[16] = {6};
    assert(peer_sec_data_decode(bytes, sizeof(bytes), other_pair, 15, 40, &out) < 0);
    bytes[8] = 0xff;
    assert(peer_sec_data_decode(bytes, sizeof(bytes), h.association, 15, 40, &out) < 0);
    h.stream_generation = 0; assert(peer_sec_data_encode(&h, bytes) < 0);
}

struct pair_fixture {
    struct security_fixture security;
    struct peer_associations *manager[2];
    struct peer_assoc_token token[2][2];
    unsigned actions[2][10];
    uint8_t rx_key[2][2][20], tx_key[2][2][20];
    uint64_t now;
};
static void pair_init(struct pair_fixture *p)
{
    memset(p, 0, sizeof(*p)); security_identity(&p->security); security_handshake(&p->security, 1);
    struct peer_assoc_limits limits = {.pairs = 2, .lanes = 2, .sas = 8,
                                      .setup_timeout_ns = 1000, .overlap_ns = 100};
    p->now = 1; uint64_t deadlines[3] = {10000, 10000, 10000};
    for (unsigned i = 0; i < 2; i++) assert(!peer_associations_new(&limits, &p->manager[i]));
    for (unsigned pair = 0; pair < 2; pair++) {
        struct peer_sec_binding b = p->security.binding; struct peer_sec_lane l = p->security.lane;
        b.association[0] += pair; b.endpoint[0].pod[4] += pair * 2; b.endpoint[1].pod[4] += pair * 2;
        for (unsigned i = 0; i < 2; i++) { b.spi[i] += pair * 10; l.qpn[i] += pair * 10; }
        for (unsigned i = 0; i < 2; i++)
            assert(!peer_association_open(p->manager[i], p->security.tls[i], &b, i, &l, 1,
                                          deadlines, p->now, &p->token[i][pair]));
    }
}
static int relay(struct pair_fixture *p, unsigned side)
{
    struct peer_assoc_event e;
    if (peer_associations_event(p->manager[side], &e) != 1) return 0;
    unsigned pair = e.token.slot;
    assert(pair < 2);
    assert(peer_association_signal(p->manager[1-side], p->token[1-side][pair], e.type,
                                   e.epoch, e.digest, p->now) >= 0);
    assert(!peer_association_event_sent(p->manager[side], &e)); return 1;
}
static int execute(struct pair_fixture *p, unsigned side)
{
    struct peer_crypto_request r;
    if (peer_associations_next(p->manager[side], p->now, &r) != 1) return 0;
    p->actions[side][r.action]++;
    if (r.action == PEER_CRYPTO_INSTALL_RX) memcpy(p->rx_key[side][r.token.slot], r.material, 20);
    if (r.action == PEER_CRYPTO_INSTALL_TX) memcpy(p->tx_key[side][r.token.slot], r.material, 20);
    assert(!peer_association_complete(p->manager[side], &r, 0, p->now));
    assert(peer_association_complete(p->manager[side], &r, 0, p->now) == 1);
    peer_crypto_request_cleanse(&r); assert(!peer_sec_nonzero(r.material, 20)); return 1;
}
static void settle(struct pair_fixture *p)
{
    for (unsigned n = 0; n < 100; n++) {
        int work = 0;
        for (unsigned i = 0; i < 2; i++) { work |= relay(p, i); work |= execute(p, i); }
        if (!work) return;
    }
    assert(0 && "pair state machine failed to settle");
}
static void pair_free(struct pair_fixture *p)
{
    for (unsigned i = 0; i < 2; i++)
        for (unsigned k = 0; k < 2; k++) assert(!peer_association_revoke(p->manager[i], p->token[i][k]));
    settle(p);
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned k = 0; k < 2; k++) assert(!peer_association_release(p->manager[i], p->token[i][k]));
        struct peer_assoc_stats stats; peer_associations_stats(p->manager[i], &stats);
        assert(!stats.pairs && !stats.sas && !stats.lanes);
        assert(!peer_associations_free(p->manager[i]));
    }
    security_free(&p->security);
}
static void test_pairs_and_rekey(void)
{
    struct pair_fixture p; pair_init(&p);
    assert(!peer_association_admit(p.manager[0], p.token[0][0], p.now));
    struct peer_assoc_grant grant;
    assert(!peer_association_grant(p.manager[0], p.token[0][0], 0, p.now, &grant));
    assert(!grant.revision);
    settle(&p);
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned k = 0; k < 2; k++) {
            assert(peer_association_admit(p.manager[i], p.token[i][k], p.now));
            assert(peer_association_grant(p.manager[i], p.token[i][k], 0, p.now, &grant) == 1);
            assert(grant.token.generation == p.token[i][k].generation && grant.epoch == 1);
            assert(grant.pair.lane_id == 1 && grant.pair.lane_generation == 1);
            assert(grant.pair.local.generation == 1 && grant.pair.remote.generation == 1);
            assert(!memcmp(grant.pair.local.nonce, p.security.binding.endpoint[i].registration_nonce, 32));
            assert(!memcmp(p.tx_key[i][k], p.rx_key[1-i][k], 20));
        }
        assert(memcmp(p.tx_key[i][0], p.tx_key[i][1], 20));
    }
    uint8_t old_key[20]; memcpy(old_key, p.tx_key[0][0], 20);
    uint32_t spi[2] = {5000, 5001};
    for (unsigned i = 0; i < 2; i++) assert(!peer_association_rekey(p.manager[i], p.token[i][0], spi, p.now));
    assert(!peer_association_admit(p.manager[0], p.token[0][0], p.now));
    assert(!peer_association_grant(p.manager[0], p.token[0][0], 0, p.now, &grant));
    assert(!grant.revision);
    assert(peer_association_admit(p.manager[0], p.token[0][1], p.now));
    settle(&p);
    assert(p.actions[0][PEER_CRYPTO_QUIESCE] == 1);
    assert(memcmp(old_key, p.tx_key[0][0], 20));
    assert(!memcmp(p.tx_key[0][0], p.rx_key[1][0], 20));
    struct peer_assoc_stats stats; peer_associations_stats(p.manager[0], &stats); assert(stats.sas == 6);
    spi[0]++; spi[1]++; assert(peer_association_rekey(p.manager[0], p.token[0][0], spi, p.now) < 0);
    p.now = 102; settle(&p); peer_associations_stats(p.manager[0], &stats); assert(stats.sas == 4);
    assert(p.actions[0][PEER_CRYPTO_REMOVE_OLD] == 1);
    assert(!peer_association_renew(p.manager[0], p.token[0][0], 1, 1, 200, p.now));
    assert(peer_association_renew(p.manager[0], p.token[0][0], 1, 1, 300, p.now) < 0);
    p.now = 200;
    assert(!peer_association_admit(p.manager[0], p.token[0][0], p.now));
    assert(peer_association_admit(p.manager[0], p.token[0][1], p.now));
    assert(peer_association_renew(p.manager[0], p.token[0][0], 1, 2, 300, p.now) < 0);
    pair_free(&p);
}
static void test_delayed_completion_and_stale_slot(void)
{
    struct pair_fixture p; pair_init(&p); struct peer_crypto_request r;
    assert(peer_associations_next(p.manager[0], p.now, &r) == 1);
    assert(r.action == PEER_CRYPTO_INSTALL_RX);
    assert(!peer_association_revoke(p.manager[0], r.token));
    assert(peer_association_release(p.manager[0], r.token) < 0);
    assert(peer_associations_free(p.manager[0]) < 0);
    assert(!peer_association_complete(p.manager[0], &r, 0, p.now));
    /* Revoked install does not publish a late RX_READY. */
    struct peer_assoc_event e;
    assert(peer_associations_event(p.manager[0], &e) == 1 && e.type == PEER_SEC_REVOKE);
    /* Revoke all halves before progressing notifications for this experiment. */
    pair_free(&p);
    peer_crypto_request_cleanse(&r);
}

static void test_failure_timeout_and_budget(void)
{
    struct pair_fixture p; pair_init(&p);
    struct peer_assoc_stats stats; peer_associations_stats(p.manager[0], &stats);
    assert(stats.pairs == 2 && stats.lanes == 2 && stats.sas == 4);
    struct peer_assoc_token token; uint64_t deadlines[3] = {10000, 10000, 10000};
    assert(peer_association_open(p.manager[0], p.security.tls[0], &p.security.binding, 0,
                                 &p.security.lane, 1, deadlines, p.now, &token) < 0);
    struct peer_crypto_request r;
    assert(peer_associations_next(p.manager[0], p.now, &r) == 1);
    assert(!peer_association_complete(p.manager[0], &r, -1, p.now));
    assert(!peer_association_admit(p.manager[0], r.token, p.now));
    peer_crypto_request_cleanse(&r);
    settle(&p);
    assert(p.actions[0][PEER_CRYPTO_INSTALL_TX] == 1); /* only the unaffected pair */
    assert(peer_association_admit(p.manager[0], p.token[0][1], p.now));
    /* A DEAD slot cannot be released until BLOCK -> QP/DMA fence -> SA removal. */
    struct peer_assoc_token old = p.token[0][0];
    assert(!peer_association_release(p.manager[0], old));
    p.security.binding.association[0] = 44;
    assert(!peer_association_open(p.manager[0], p.security.tls[0], &p.security.binding, 0,
                                  &p.security.lane, 1, deadlines, p.now, &p.token[0][0]));
    assert(p.token[0][0].slot == old.slot && p.token[0][0].generation != old.generation);
    assert(peer_association_complete(p.manager[0], &r, 0, p.now) == 1);
    assert(peer_association_revoke(p.manager[0], old) < 0);
    assert(peer_associations_next(p.manager[0], p.now, &r) == 1);
    p.now = 1002; peer_associations_tick(p.manager[0], p.now);
    assert(peer_association_release(p.manager[0], p.token[0][0]) < 0);
    assert(!peer_association_complete(p.manager[0], &r, 0, p.now));
    assert(!peer_association_admit(p.manager[0], p.token[0][0], p.now));
    peer_crypto_request_cleanse(&r); pair_free(&p);
}
static void test_unknown_completion_quarantines(void)
{
    /* A quarantined manager deliberately cannot be freed while hardware may
     * reference it. Isolate this terminal fault in a child process; no device
     * or external state exists in this provider-free test. */
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        struct pair_fixture p; pair_init(&p); struct peer_crypto_request r;
        assert(peer_associations_next(p.manager[0], p.now, &r) == 1);
        assert(!peer_association_complete(p.manager[0], &r, -2, p.now));
        struct peer_assoc_stats stats; peer_associations_stats(p.manager[0], &stats);
        assert(stats.quarantined == 1 && stats.sas == 4 && stats.lanes == 2);
        assert(peer_association_phase(p.manager[0], r.token) == PEER_ASSOC_QUARANTINED);
        assert(peer_association_release(p.manager[0], r.token) < 0);
        assert(peer_associations_free(p.manager[0]) < 0);
        peer_crypto_request_cleanse(&r); _exit(0);
    }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void test_identity_and_selector_collisions(void)
{
    struct security_fixture f; security_identity(&f); security_handshake(&f, 1);
    struct peer_assoc_limits limits = {.pairs = 4, .lanes = 4, .sas = 8, .setup_timeout_ns = 1000, .overlap_ns = 10};
    struct peer_associations *m; assert(!peer_associations_new(&limits, &m));
    struct peer_assoc_token original, unused; uint64_t deadlines[3] = {10000, 10000, 10000};
    assert(!peer_association_open(m, f.tls[0], &f.binding, 0, &f.lane, 1, deadlines, 1, &original));
    struct peer_sec_binding b = f.binding; struct peer_sec_lane l = f.lane;
    b.association[0]++; strcpy(b.endpoint[0].pod, "different-pod"); l.qpn[0]++;
    /* Distinct pair and QP still cannot claim another pair's local receive SPI. */
    assert(peer_association_open(m, f.tls[0], &b, 0, &l, 1, deadlines, 1, &unused) < 0);
    b.spi[1]++; l.qpn[0] = f.lane.qpn[0];
    assert(peer_association_open(m, f.tls[0], &b, 0, &l, 1, deadlines, 1, &unused) < 0);
    l.qpn[0]++; strcpy(b.endpoint[0].pod, f.binding.endpoint[0].pod);
    assert(peer_association_open(m, f.tls[0], &b, 0, &l, 1, deadlines, 1, &unused) < 0);
    struct peer_assoc_stats stats; peer_associations_stats(m, &stats);
    assert(stats.pairs == 1 && stats.lanes == 1 && stats.sas == 2);
    assert(!peer_association_revoke(m, original));
    struct peer_crypto_request r;
    while (peer_associations_next(m, 1, &r) == 1) assert(!peer_association_complete(m, &r, 0, 1));
    assert(!peer_association_release(m, original)); assert(!peer_associations_free(m)); security_free(&f);
}
int main(void)
{
    test_codec_and_exporter(); test_data_binding(); test_pairs_and_rekey(); test_delayed_completion_and_stale_slot();
    test_failure_timeout_and_budget(); test_unknown_completion_quarantines();
    test_identity_and_selector_collisions();
    puts("peer_security_test: PASS (TLS exporter, codec, pair isolation, rekey, leases, teardown)");
    return 0;
}
