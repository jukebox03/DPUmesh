#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <openssl/hmac.h>

#include "doca/workload_grant.h"
#include "doca/object.h"

static void
fill_assert(struct dmesh_workload_assert_msg *assertion,
            const uint8_t nonce[DMESH_REG_NONCE_SIZE], uint64_t now)
{
    memset(assertion, 0, sizeof(*assertion));
    assertion->type = DMESH_MSG_WORKLOAD_ASSERT;
    assertion->version = DMESH_ASSERT_VERSION;
    dmesh_grant_put_u64_le(assertion->issued_at_le, now - 1);
    dmesh_grant_put_u64_le(assertion->expires_at_le, now + 60);
    for (size_t i = 0; i < sizeof(assertion->assert_id); i++)
        assertion->assert_id[i] = (uint8_t)(i + 1);
    memcpy(assertion->nonce, nonce, sizeof(assertion->nonce));
    dmesh_grant_put_u32_le(assertion->channel_slot_le, 3);
    dmesh_grant_put_u64_le(assertion->channel_generation_le, 7);
    for (size_t i = 0; i < sizeof(assertion->daemon_incarnation); i++)
        assertion->daemon_incarnation[i] = (uint8_t)(0x80 + i);
    snprintf(assertion->key_id, sizeof(assertion->key_id), "node-ed25519-v1");
    snprintf(assertion->cluster_id, sizeof(assertion->cluster_id), "test-cluster");
    snprintf(assertion->node_name, sizeof(assertion->node_name), "worker-1");
    snprintf(assertion->pod_uid, sizeof(assertion->pod_uid),
             "12345678-1234-1234-1234-123456789abc");
    snprintf(assertion->namespace_name, sizeof(assertion->namespace_name),
             "test-bench");
    snprintf(assertion->pod_name, sizeof(assertion->pod_name),
             "bench-dpumesh-abc123");
    snprintf(assertion->service_account, sizeof(assertion->service_account),
             "default");
    snprintf(assertion->container_name, sizeof(assertion->container_name),
             "app");
    snprintf(assertion->container_id, sizeof(assertion->container_id),
             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    snprintf(assertion->service_name, sizeof(assertion->service_name),
             "echo-dpumesh");
    snprintf(assertion->pod_ip, sizeof(assertion->pod_ip), "10.244.1.17");
}

/* An authoritative feed is signed by the feed keyring, so only its signed
 * prefix may be parsed. */
static void
test_feed_verify(void)
{
    char dir[] = "/tmp/dpumesh-feed-test-XXXXXX";
    assert(mkdtemp(dir) != NULL);
    assert(chmod(dir, 0700) == 0);

    uint8_t key[DMESH_GRANT_KEY_SIZE];
    for (size_t i = 0; i < sizeof(key); i++)
        key[i] = (uint8_t)(0x11 * (i % 15) + 1);
    char key_path[512];
    snprintf(key_path, sizeof(key_path), "%s/feed-key-v1.key", dir);
    FILE *out = fopen(key_path, "w");
    assert(out != NULL);
    assert(fwrite(key, 1, sizeof(key), out) == sizeof(key));
    assert(fclose(out) == 0);
    assert(chmod(key_path, 0400) == 0);

    const char body[] = "version=7\nmember=abc,-1\n";
    uint8_t mac[DMESH_GRANT_MAC_SIZE];
    unsigned int mac_len = 0;
    assert(HMAC(EVP_sha256(), key, sizeof(key), (const unsigned char *)body,
                sizeof(body) - 1, mac, &mac_len) != NULL);
    assert(mac_len == DMESH_GRANT_MAC_SIZE);
    char hex[2 * DMESH_GRANT_MAC_SIZE + 1];
    for (size_t i = 0; i < DMESH_GRANT_MAC_SIZE; i++)
        snprintf(hex + 2 * i, 3, "%02x", mac[i]);

    char document[512];
    size_t signed_length = 0;
    int n = snprintf(document, sizeof(document), "%ssignature=feed-key-v1,%s\n",
                     body, hex);
    assert(dmesh_feed_verify(document, (size_t)n, dir, &signed_length) ==
           DMESH_FEED_OK);
    assert(signed_length == sizeof(body) - 1);

    /* A document with no envelope is not a signed generation. */
    assert(dmesh_feed_verify(body, sizeof(body) - 1, dir, &signed_length) ==
           DMESH_FEED_UNSIGNED);

    /* Bytes appended after the envelope are outside the signature. */
    n = snprintf(document, sizeof(document),
                 "%ssignature=feed-key-v1,%s\nmember=def,-1\n", body, hex);
    assert(dmesh_feed_verify(document, (size_t)n, dir, &signed_length) ==
           DMESH_FEED_UNSIGNED);

    /* A key the keyring does not hold cannot authorize a generation. */
    n = snprintf(document, sizeof(document), "%ssignature=other-key,%s\n",
                 body, hex);
    assert(dmesh_feed_verify(document, (size_t)n, dir, &signed_length) ==
           DMESH_FEED_BAD_KEY_ID);

    /* A key id may not escape the keyring directory. */
    n = snprintf(document, sizeof(document), "%ssignature=../feed-key-v1,%s\n",
                 body, hex);
    assert(dmesh_feed_verify(document, (size_t)n, dir, &signed_length) ==
           DMESH_FEED_BAD_KEY_ID);

    /* One flipped body byte invalidates the generation. */
    char tampered[512];
    n = snprintf(tampered, sizeof(tampered),
                 "version=8\nmember=abc,-1\nsignature=feed-key-v1,%s\n", hex);
    assert(dmesh_feed_verify(tampered, (size_t)n, dir, &signed_length) ==
           DMESH_FEED_BAD_MAC);

    assert(unlink(key_path) == 0);
    assert(rmdir(dir) == 0);
}

int
main(void)
{
    static struct objects verifier;
    uint8_t seed[DMESH_GRANT_KEY_SIZE];
    uint8_t public_key[DMESH_GRANT_KEY_SIZE];
    uint8_t nonce[DMESH_REG_NONCE_SIZE];
    for (size_t i = 0; i < sizeof(seed); i++) {
        seed[i] = (uint8_t)(0xa0 + i);
        nonce[i] = (uint8_t)(0x20 + i);
    }
    assert(dmesh_assert_public_key(seed, public_key) == 0);
    uint64_t now = (uint64_t)time(NULL);
    struct dmesh_workload_assert_msg assertion;
    struct dmesh_assert_claims claims;

    fill_assert(&assertion, nonce, now);
    assert(dmesh_assert_sign_v3(&assertion, seed) == 0);
    assert(dmesh_assert_verify_v3(&assertion, public_key, "test-cluster", "worker-1", nonce,
                                  now, &claims) == DMESH_GRANT_OK);
    assert(strcmp(claims.workload,
                  "{\"ns\":\"test-bench\",\"pod\":\"bench-dpumesh-abc123\"}") == 0);
    assert(strcmp(claims.pod_uid, "12345678-1234-1234-1234-123456789abc") == 0);
    assert(claims.channel_slot == 3 && claims.channel_generation == 7);
    assert(strcmp(claims.namespace_name, "test-bench") == 0);
    assert(strcmp(claims.service_account, "default") == 0);
    assert(strcmp(claims.service_name, "echo-dpumesh") == 0);
    assert(strcmp(claims.pod_ip, "10.244.1.17") == 0);

    /* A forged claim fails the signature: the DPU verifies with a key that
     * cannot sign. */
    struct dmesh_workload_assert_msg changed = assertion;
    changed.pod_name[0] = 'x';
    assert(dmesh_assert_verify_v3(&changed, public_key, "test-cluster", "worker-1", nonce,
                                  now, &claims) == DMESH_GRANT_BAD_SIG);
    changed = assertion;
    changed.sig[0] ^= 1;
    assert(dmesh_assert_verify_v3(&changed, public_key, "test-cluster", "worker-1", nonce,
                                  now, &claims) == DMESH_GRANT_BAD_SIG);

    /* An assertion minted for another node's Pod is refused on this node even
     * though its signature is genuine: the Pod relays the assertion, and the
     * assertion names the node it may register on. */
    assert(dmesh_assert_verify_v3(&assertion, public_key, "test-cluster", "worker-2", nonce,
                                  now, &claims) == DMESH_GRANT_WRONG_NODE);
    assert(dmesh_assert_verify_v3(&assertion, public_key, "test-cluster", "", nonce,
                                  now, &claims) == DMESH_GRANT_WRONG_NODE);

    uint8_t wrong_nonce[DMESH_REG_NONCE_SIZE];
    memcpy(wrong_nonce, nonce, sizeof(wrong_nonce));
    wrong_nonce[0] ^= 1;
    assert(dmesh_assert_verify_v3(&assertion, public_key, "test-cluster", "worker-1",
                                  wrong_nonce, now,
                                  &claims) == DMESH_GRANT_BAD_NONCE);

    /* Expired, and over-long lifetimes: expiry gets no skew grace. */
    fill_assert(&changed, nonce, now);
    dmesh_grant_put_u64_le(changed.issued_at_le, now - 400);
    dmesh_grant_put_u64_le(changed.expires_at_le, now - 100);
    assert(dmesh_assert_sign_v3(&changed, seed) == 0);
    assert(dmesh_assert_verify_v3(&changed, public_key, "test-cluster", "worker-1", nonce,
                                  now, &claims) == DMESH_GRANT_BAD_TIME);
    fill_assert(&changed, nonce, now);
    dmesh_grant_put_u64_le(changed.expires_at_le,
                           now + DMESH_ASSERT_MAX_LIFETIME_SEC + 60);
    assert(dmesh_assert_sign_v3(&changed, seed) == 0);
    assert(dmesh_assert_verify_v3(&changed, public_key, "test-cluster", "worker-1", nonce,
                                  now, &claims) == DMESH_GRANT_BAD_TIME);

    /* A Pod IP that is not a dotted quad never reaches the policy input. */
    fill_assert(&changed, nonce, now);
    snprintf(changed.pod_ip, sizeof(changed.pod_ip), "10.244.1.");
    assert(dmesh_assert_sign_v3(&changed, seed) == -1);
    assert(dmesh_assert_verify_v3(&changed, public_key, "test-cluster", "worker-1", nonce,
                                  now, &claims) == DMESH_GRANT_NONCANONICAL);
    snprintf(changed.pod_ip, sizeof(changed.pod_ip), "10.244.1.256");
    assert(dmesh_assert_verify_v3(&changed, public_key, "test-cluster", "worker-1", nonce,
                                  now, &claims) == DMESH_GRANT_NONCANONICAL);

    /* An empty Service claim is canonical: a client-only Pod serves nothing. */
    fill_assert(&changed, nonce, now);
    memset(changed.service_name, 0, sizeof(changed.service_name));
    assert(dmesh_assert_sign_v3(&changed, seed) == 0);
    assert(dmesh_assert_verify_v3(&changed, public_key, "test-cluster", "worker-1", nonce,
                                  now, &claims) == DMESH_GRANT_OK);
    assert(claims.service_name[0] == '\0');

    /* A message of the wrong type, then of the wrong version, is refused before
     * any cryptography. */
    fill_assert(&changed, nonce, now);
    changed.type = 12; /* not DMESH_MSG_WORKLOAD_ASSERT */
    assert(dmesh_assert_verify_v3(&changed, public_key, "test-cluster", "worker-1", nonce,
                                  now, &claims) == DMESH_GRANT_BAD_TYPE);
    fill_assert(&changed, nonce, now);
    changed.version = 1;
    assert(dmesh_assert_verify_v3(&changed, public_key, "test-cluster", "worker-1", nonce,
                                  now, &claims) == DMESH_GRANT_BAD_VERSION);

    /* Text after the terminating NUL is hidden input, so it is refused. */
    changed = assertion;
    changed.namespace_name[strlen(changed.namespace_name) + 1] = 'x';
    assert(dmesh_assert_verify_v3(&changed, public_key, "test-cluster", "worker-1", nonce,
                                  now, &claims) == DMESH_GRANT_NONCANONICAL);

    /* The keyring lookup and the replay window bound reuse. */
    verifier.registration_key_count = 2;
    snprintf(verifier.registration_keys[0].key_id,
             sizeof(verifier.registration_keys[0].key_id), "old-key-v1");
    snprintf(verifier.registration_keys[1].key_id,
             sizeof(verifier.registration_keys[1].key_id), "new-key-v2");
    verifier.registration_keys[1].bytes[0] = 42;
    assert(dmesh_registration_find_key(&verifier, "missing") == NULL);
    assert(dmesh_registration_find_key(&verifier, "new-key-v2") ==
           verifier.registration_keys[1].bytes);
    assert(dmesh_registration_consume_grant(&verifier, assertion.assert_id) == 0);
    assert(dmesh_registration_consume_grant(&verifier, assertion.assert_id) == -1);
    assertion.assert_id[0] ^= 0xff;
    assert(dmesh_registration_consume_grant(&verifier, assertion.assert_id) == 0);

    test_feed_verify();
    puts("workload_grant_test: PASS");
    return 0;
}
