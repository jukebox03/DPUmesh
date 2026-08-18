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
fill_grant(struct dmesh_workload_grant_msg *grant,
           const uint8_t nonce[DMESH_REG_NONCE_SIZE], uint64_t now)
{
    memset(grant, 0, sizeof(*grant));
    grant->type = DMESH_MSG_WORKLOAD_GRANT;
    grant->version = DMESH_GRANT_VERSION;
    dmesh_grant_put_i32_le(grant->service_id_le, 11);
    dmesh_grant_put_u64_le(grant->issued_at_le, now - 1);
    dmesh_grant_put_u64_le(grant->expires_at_le, now + 60);
    for (size_t i = 0; i < sizeof(grant->grant_id); i++)
        grant->grant_id[i] = (uint8_t)(i + 1);
    memcpy(grant->nonce, nonce, sizeof(grant->nonce));
    snprintf(grant->issuer, sizeof(grant->issuer), "dpumesh-node-agent");
    snprintf(grant->key_id, sizeof(grant->key_id), "node-hmac-v1");
    snprintf(grant->pod_uid, sizeof(grant->pod_uid),
             "12345678-1234-1234-1234-123456789abc");
    snprintf(grant->namespace_name, sizeof(grant->namespace_name), "test-bench");
    snprintf(grant->pod_name, sizeof(grant->pod_name), "bench-dpumesh-abc123");
    snprintf(grant->service_account, sizeof(grant->service_account), "default");
    snprintf(grant->node_name, sizeof(grant->node_name), "worker-1");
}

/* An authoritative feed is signed by the registration keyring, so only its
 * signed prefix may be parsed. */
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
    uint8_t key[DMESH_GRANT_KEY_SIZE];
    uint8_t nonce[DMESH_REG_NONCE_SIZE];
    for (size_t i = 0; i < sizeof(key); i++) {
        key[i] = (uint8_t)(0xa0 + i);
        nonce[i] = (uint8_t)(0x20 + i);
    }
    uint64_t now = (uint64_t)time(NULL);
    struct dmesh_workload_grant_msg grant;
    char workload[DMESH_WORKLOAD_MAX];
    char pod_uid[DMESH_POD_UID_MAX];
    int32_t service_id = -99;

    fill_grant(&grant, nonce, now);
    assert(dmesh_grant_sign_v1(&grant, key) == 0);
    assert(dmesh_grant_verify_v1(
               &grant, key, "dpumesh-node-agent", nonce,
               now, &service_id, workload, pod_uid) == DMESH_GRANT_OK);
    assert(service_id == 11);
    assert(strcmp(workload,
                  "{\"ns\":\"test-bench\",\"pod\":\"bench-dpumesh-abc123\"}") == 0);
    assert(strcmp(pod_uid, "12345678-1234-1234-1234-123456789abc") == 0);

    struct dmesh_workload_grant_msg changed = grant;
    changed.pod_name[0] = 'x';
    assert(dmesh_grant_verify_v1(
               &changed, key, "dpumesh-node-agent", nonce,
               now, &service_id, workload, pod_uid) == DMESH_GRANT_BAD_MAC);

    uint8_t wrong_nonce[DMESH_REG_NONCE_SIZE];
    memcpy(wrong_nonce, nonce, sizeof(wrong_nonce));
    wrong_nonce[0] ^= 1;
    assert(dmesh_grant_verify_v1(
               &grant, key, "dpumesh-node-agent", wrong_nonce,
               now, &service_id, workload, pod_uid) == DMESH_GRANT_BAD_NONCE);
    assert(dmesh_grant_verify_v1(
               &grant, key, "another-agent", nonce,
               now, &service_id, workload, pod_uid) == DMESH_GRANT_BAD_ISSUER);

    fill_grant(&changed, nonce, now);
    dmesh_grant_put_u64_le(changed.issued_at_le, now - 400);
    dmesh_grant_put_u64_le(changed.expires_at_le, now - 100);
    assert(dmesh_grant_sign_v1(&changed, key) == 0);
    assert(dmesh_grant_verify_v1(
               &changed, key, "dpumesh-node-agent", nonce,
               now, &service_id, workload, pod_uid) == DMESH_GRANT_BAD_TIME);

    fill_grant(&changed, nonce, now);
    dmesh_grant_put_i32_le(changed.service_id_le, 128);
    assert(dmesh_grant_sign_v1(&changed, key) == 0);
    assert(dmesh_grant_verify_v1(
               &changed, key, "dpumesh-node-agent", nonce,
               now, &service_id, workload, pod_uid) == DMESH_GRANT_BAD_SERVICE);

    changed = grant;
    changed.namespace_name[strlen(changed.namespace_name) + 1] = 'x';
    assert(dmesh_grant_verify_v1(
               &changed, key, "dpumesh-node-agent", nonce,
               now, &service_id, workload, pod_uid) == DMESH_GRANT_NONCANONICAL);

    verifier.registration_key_count = 2;
    snprintf(verifier.registration_keys[0].key_id,
             sizeof(verifier.registration_keys[0].key_id), "old-key-v1");
    snprintf(verifier.registration_keys[1].key_id,
             sizeof(verifier.registration_keys[1].key_id), "new-key-v2");
    verifier.registration_keys[1].bytes[0] = 42;
    assert(dmesh_registration_find_key(&verifier, "missing") == NULL);
    assert(dmesh_registration_find_key(&verifier, "new-key-v2") ==
           verifier.registration_keys[1].bytes);
    assert(dmesh_registration_consume_grant(&verifier, grant.grant_id) == 0);
    assert(dmesh_registration_consume_grant(&verifier, grant.grant_id) == -1);
    grant.grant_id[0] ^= 0xff;
    assert(dmesh_registration_consume_grant(&verifier, grant.grant_id) == 0);

    test_feed_verify();
    puts("workload_grant_test: PASS");
    return 0;
}
