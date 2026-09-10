#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <openssl/hmac.h>

#include "doca/workload_identity.h"
#include "doca/object.h"

static void
fill_assert(struct dmesh_workload_identity *assertion,
            const uint8_t nonce[DMESH_REG_NONCE_SIZE], uint64_t now)
{
    memset(assertion, 0, sizeof(*assertion));
    assertion->type = DMESH_IDENTITY_TYPE;
    assertion->version = DMESH_IDENTITY_VERSION;
    dmesh_wire_put_u64_le(assertion->issued_at_le, now - 1);
    dmesh_wire_put_u64_le(assertion->expires_at_le, now + 60);


    memcpy(assertion->nonce, nonce, sizeof(assertion->nonce));
    dmesh_wire_put_u32_le(assertion->channel_slot_le, 3);
    dmesh_wire_put_u64_le(assertion->channel_generation_le, 7);
    for (size_t i = 0; i < sizeof(assertion->daemon_incarnation); i++)
        assertion->daemon_incarnation[i] = (uint8_t)(0x80 + i);

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

    uint8_t key[DMESH_KEY_SIZE];
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
    uint8_t mac[DMESH_FEED_MAC_SIZE];
    unsigned int mac_len = 0;
    assert(HMAC(EVP_sha256(), key, sizeof(key), (const unsigned char *)body,
                sizeof(body) - 1, mac, &mac_len) != NULL);
    assert(mac_len == DMESH_FEED_MAC_SIZE);
    char hex[2 * DMESH_FEED_MAC_SIZE + 1];
    for (size_t i = 0; i < DMESH_FEED_MAC_SIZE; i++)
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
    uint8_t nonce[DMESH_REG_NONCE_SIZE];
    memset(nonce, 0x42, sizeof(nonce));
    uint64_t now = (uint64_t)time(NULL);
    struct dmesh_workload_identity assertion;
    struct dmesh_identity_claims claims;
    fill_assert(&assertion, nonce, now);
    assert(dmesh_identity_decode(&assertion, "test-cluster", "worker-1", nonce,
                                    now, &claims) == DMESH_IDENTITY_OK);
    assert(claims.channel_slot == 3 && claims.channel_generation == 7);
    assert(strcmp(claims.service_name, "echo-dpumesh") == 0);
    assert(dmesh_identity_decode(&assertion, "wrong", "worker-1", nonce,
                                    now, &claims) == DMESH_IDENTITY_WRONG_NODE);
    assert(dmesh_identity_decode(&assertion, "test-cluster", "worker-2", nonce,
                                    now, &claims) == DMESH_IDENTITY_WRONG_NODE);
    nonce[0] ^= 1;
    assert(dmesh_identity_decode(&assertion, "test-cluster", "worker-1", nonce,
                                    now, &claims) == DMESH_IDENTITY_BAD_NONCE);
    nonce[0] ^= 1;
    assert(dmesh_identity_decode(&assertion, "test-cluster", "worker-1", nonce,
                                    now + 301, &claims) == DMESH_IDENTITY_BAD_TIME);
    assertion.pod_uid[0] = '!';
    assert(dmesh_identity_decode(&assertion, "test-cluster", "worker-1", nonce,
                                    now, &claims) == DMESH_IDENTITY_NONCANONICAL);
    fill_assert(&assertion, nonce, now);
    assertion.flags = 1;
    assert(dmesh_identity_decode(&assertion, "test-cluster", "worker-1", nonce,
                                    now, &claims) == DMESH_IDENTITY_NONCANONICAL);
    test_feed_verify();
    puts("workload_identity_test: PASS");
    return 0;
}
