#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
    int32_t service_id = -99;

    fill_grant(&grant, nonce, now);
    assert(dmesh_grant_sign_v1(&grant, key) == 0);
    assert(dmesh_grant_verify_v1(
               &grant, key, "dpumesh-node-agent", "node-hmac-v1", nonce,
               now, &service_id, workload) == DMESH_GRANT_OK);
    assert(service_id == 11);
    assert(strcmp(workload,
                  "{\"ns\":\"test-bench\",\"pod\":\"bench-dpumesh-abc123\"}") == 0);

    struct dmesh_workload_grant_msg changed = grant;
    changed.pod_name[0] = 'x';
    assert(dmesh_grant_verify_v1(
               &changed, key, "dpumesh-node-agent", "node-hmac-v1", nonce,
               now, &service_id, workload) == DMESH_GRANT_BAD_MAC);

    uint8_t wrong_nonce[DMESH_REG_NONCE_SIZE];
    memcpy(wrong_nonce, nonce, sizeof(wrong_nonce));
    wrong_nonce[0] ^= 1;
    assert(dmesh_grant_verify_v1(
               &grant, key, "dpumesh-node-agent", "node-hmac-v1", wrong_nonce,
               now, &service_id, workload) == DMESH_GRANT_BAD_NONCE);
    assert(dmesh_grant_verify_v1(
               &grant, key, "another-agent", "node-hmac-v1", nonce,
               now, &service_id, workload) == DMESH_GRANT_BAD_ISSUER);

    fill_grant(&changed, nonce, now);
    dmesh_grant_put_u64_le(changed.issued_at_le, now - 400);
    dmesh_grant_put_u64_le(changed.expires_at_le, now - 100);
    assert(dmesh_grant_sign_v1(&changed, key) == 0);
    assert(dmesh_grant_verify_v1(
               &changed, key, "dpumesh-node-agent", "node-hmac-v1", nonce,
               now, &service_id, workload) == DMESH_GRANT_BAD_TIME);

    fill_grant(&changed, nonce, now);
    dmesh_grant_put_i32_le(changed.service_id_le, 128);
    assert(dmesh_grant_sign_v1(&changed, key) == 0);
    assert(dmesh_grant_verify_v1(
               &changed, key, "dpumesh-node-agent", "node-hmac-v1", nonce,
               now, &service_id, workload) == DMESH_GRANT_BAD_SERVICE);

    changed = grant;
    changed.namespace_name[strlen(changed.namespace_name) + 1] = 'x';
    assert(dmesh_grant_verify_v1(
               &changed, key, "dpumesh-node-agent", "node-hmac-v1", nonce,
               now, &service_id, workload) == DMESH_GRANT_NONCANONICAL);

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

    puts("workload_grant_test: PASS");
    return 0;
}
