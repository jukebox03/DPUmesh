/* What the mTLS layer owes the seam above it: a completed mutual handshake,
 * the peer's raw static key read back for the caller to pin, an all-or-nothing
 * plaintext write at the largest frame the channel can build, and a fault that
 * is permanent once a byte on the wire has been touched. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <openssl/evp.h>

#include "../doca/peer_tls.h"

#define FRAME_MAX 65616u          /* header + DMESH_PEER_FRAME_MAX */

static const uint8_t SEED_A[32] = {
    0xa1, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};
static const uint8_t SEED_B[32] = {
    0xb2, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b,
    0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
};

/* Derived independently of the module under test, so a wrong key type or a
 * wrong seed cannot agree with itself. */
static void expected_public(const uint8_t seed[32], uint8_t out[32])
{
    EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
    size_t len = 32;
    assert(key);
    assert(EVP_PKEY_get_raw_public_key(key, out, &len) == 1);
    assert(len == 32);
    EVP_PKEY_free(key);
}

static void pump(struct peer_tls_conn *from, struct peer_tls_conn *to)
{
    uint8_t buf[4096];
    long n;
    while ((n = peer_tls_out(from, buf, sizeof(buf))) > 0)
        assert(peer_tls_in(to, buf, (size_t)n) == 0);
    assert(n == 0);
}

static void handshake(struct peer_tls_conn *a, struct peer_tls_conn *b)
{
    for (int round = 0; round < 16; round++) {
        assert(peer_tls_handshake(a) >= 0);
        pump(a, b);
        assert(peer_tls_handshake(b) >= 0);
        pump(b, a);
        if (peer_tls_established(a) && peer_tls_established(b))
            return;
    }
    assert(0 && "handshake did not converge");
}

/* Drain `want` plaintext bytes, which arrive over as many records as the
 * record layer chose. */
static void read_exactly(struct peer_tls_conn *conn, uint8_t *out, size_t want)
{
    size_t got = 0;
    while (got < want) {
        long n = peer_tls_read(conn, out + got, want - got);
        assert(n > 0);
        got += (size_t)n;
    }
}

static void test_credential_is_the_node_key(void)
{
    struct peer_tls_ctx *ctx = NULL;
    char err[128] = {0};
    assert(peer_tls_ctx_new(SEED_A, "node-a", &ctx, err, sizeof(err)) == 0);
    uint8_t presented[32], expected[32];
    peer_tls_ctx_public_key(ctx, presented);
    expected_public(SEED_A, expected);
    assert(memcmp(presented, expected, 32) == 0);
    peer_tls_ctx_free(ctx);
}

/* A node name longer than a common name may be is truncated, not refused. */
static void test_long_node_name(void)
{
    char name[300];
    memset(name, 'n', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    struct peer_tls_ctx *ctx = NULL;
    assert(peer_tls_ctx_new(SEED_A, name, &ctx, NULL, 0) == 0);
    peer_tls_ctx_free(ctx);
}

static void test_mutual_handshake_yields_peer_keys(void)
{
    struct peer_tls_ctx *ca = NULL, *cb = NULL;
    assert(peer_tls_ctx_new(SEED_A, "node-a", &ca, NULL, 0) == 0);
    assert(peer_tls_ctx_new(SEED_B, "node-b", &cb, NULL, 0) == 0);
    struct peer_tls_conn *a = NULL, *b = NULL;
    assert(peer_tls_conn_new(ca, 1, &a) == 0);
    assert(peer_tls_conn_new(cb, 0, &b) == 0);

    uint8_t key[32];
    /* Nothing is authenticated until the handshake says so. */
    assert(peer_tls_peer_key(a, key) < 0);
    assert(peer_tls_write(a, "x", 1) < 0);

    handshake(a, b);

    uint8_t pub_a[32], pub_b[32];
    expected_public(SEED_A, pub_a);
    expected_public(SEED_B, pub_b);
    assert(peer_tls_peer_key(a, key) == 0 && memcmp(key, pub_b, 32) == 0);
    assert(peer_tls_peer_key(b, key) == 0 && memcmp(key, pub_a, 32) == 0);

    peer_tls_conn_free(a);
    peer_tls_conn_free(b);
    peer_tls_ctx_free(ca);
    peer_tls_ctx_free(cb);
}

static void test_frames_round_trip(void)
{
    struct peer_tls_ctx *ca = NULL, *cb = NULL;
    assert(peer_tls_ctx_new(SEED_A, "node-a", &ca, NULL, 0) == 0);
    assert(peer_tls_ctx_new(SEED_B, "node-b", &cb, NULL, 0) == 0);
    struct peer_tls_conn *a = NULL, *b = NULL;
    assert(peer_tls_conn_new(ca, 1, &a) == 0);
    assert(peer_tls_conn_new(cb, 0, &b) == 0);
    handshake(a, b);

    /* The prologue, the first thing a real initiator sends. */
    const char *prologue = "dpumesh-peer-v1\nnode-a\nnode-b\n1\n";
    size_t plen = strlen(prologue);
    assert(peer_tls_write(a, prologue, plen) == 0);
    assert(peer_tls_out_pending(a) > 0);
    pump(a, b);
    uint8_t seen[64];
    read_exactly(b, seen, plen);
    assert(memcmp(seen, prologue, plen) == 0);
    assert(peer_tls_read(b, seen, sizeof(seen)) == 0);

    /* The largest frame the channel above can build, in one write. */
    static uint8_t sent[FRAME_MAX], back[FRAME_MAX];
    for (size_t i = 0; i < sizeof(sent); i++)
        sent[i] = (uint8_t)(i * 31u + 7u);
    assert(peer_tls_write(b, sent, sizeof(sent)) == 0);
    pump(b, a);
    read_exactly(a, back, sizeof(back));
    assert(memcmp(sent, back, sizeof(sent)) == 0);

    peer_tls_conn_free(a);
    peer_tls_conn_free(b);
    peer_tls_ctx_free(ca);
    peer_tls_ctx_free(cb);
}

static void test_tampered_record_faults(void)
{
    struct peer_tls_ctx *ca = NULL, *cb = NULL;
    assert(peer_tls_ctx_new(SEED_A, "node-a", &ca, NULL, 0) == 0);
    assert(peer_tls_ctx_new(SEED_B, "node-b", &cb, NULL, 0) == 0);
    struct peer_tls_conn *a = NULL, *b = NULL;
    assert(peer_tls_conn_new(ca, 1, &a) == 0);
    assert(peer_tls_conn_new(cb, 0, &b) == 0);
    handshake(a, b);

    assert(peer_tls_write(a, "the bytes that matter", 21) == 0);
    uint8_t record[4096];
    long n = peer_tls_out(a, record, sizeof(record));
    assert(n > 8);
    record[n - 1] ^= 0x01;             /* inside the sealed payload */
    assert(peer_tls_in(b, record, (size_t)n) == 0);
    uint8_t plain[64];
    assert(peer_tls_read(b, plain, sizeof(plain)) < 0);
    /* A fault does not heal: the channel above resets rather than retries. */
    assert(peer_tls_faulted(b));
    assert(peer_tls_read(b, plain, sizeof(plain)) < 0);
    assert(peer_tls_write(b, "x", 1) < 0);
    assert(!peer_tls_established(b));

    peer_tls_conn_free(a);
    peer_tls_conn_free(b);
    peer_tls_ctx_free(ca);
    peer_tls_ctx_free(cb);
}

/* Two ends that do not share a binding still complete a handshake — each one
 * reads back a key that is simply not the one it expects. That is the shape
 * the pin above depends on: refusal is a decision, not a TLS failure. */
static void test_impostor_is_visible_not_hidden(void)
{
    static const uint8_t SEED_C[32] = {
        0xc3, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b,
        0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
        0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
    };
    struct peer_tls_ctx *ca = NULL, *cc = NULL;
    assert(peer_tls_ctx_new(SEED_A, "node-a", &ca, NULL, 0) == 0);
    /* An impostor naming itself node-b holds a key node-b does not. */
    assert(peer_tls_ctx_new(SEED_C, "node-b", &cc, NULL, 0) == 0);
    struct peer_tls_conn *a = NULL, *c = NULL;
    assert(peer_tls_conn_new(ca, 1, &a) == 0);
    assert(peer_tls_conn_new(cc, 0, &c) == 0);
    handshake(a, c);

    uint8_t key[32], pub_b[32], pub_c[32];
    expected_public(SEED_B, pub_b);
    expected_public(SEED_C, pub_c);
    assert(peer_tls_peer_key(a, key) == 0);
    assert(memcmp(key, pub_b, 32) != 0);      /* not who it claimed to be */
    assert(memcmp(key, pub_c, 32) == 0);      /* and the truth is legible */

    peer_tls_conn_free(a);
    peer_tls_conn_free(c);
    peer_tls_ctx_free(ca);
    peer_tls_ctx_free(cc);
}

int main(void)
{
    test_credential_is_the_node_key();
    test_long_node_name();
    test_mutual_handshake_yields_peer_keys();
    test_frames_round_trip();
    test_tampered_record_faults();
    test_impostor_is_visible_not_hidden();
    printf("peer_tls_test: PASS\n");
    return 0;
}
