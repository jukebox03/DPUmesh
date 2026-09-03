#ifndef DMESH_WORKLOAD_GRANT_H
#define DMESH_WORKLOAD_GRANT_H

#include <stddef.h>
#include <stdint.h>

#include "comch_common.h"

struct objects;

#define DMESH_GRANT_KEY_SIZE 32u
#define DMESH_ASSERT_CLOCK_SKEW_SEC 30u
#define DMESH_ASSERT_MAX_LIFETIME_SEC 300u

enum dmesh_grant_result {
    DMESH_GRANT_OK = 0,
    DMESH_GRANT_BAD_TYPE,
    DMESH_GRANT_BAD_VERSION,
    DMESH_GRANT_NONCANONICAL,
    DMESH_GRANT_WRONG_NODE,
    DMESH_GRANT_BAD_KEY_ID,
    DMESH_GRANT_BAD_TIME,
    DMESH_GRANT_BAD_NONCE,
    DMESH_GRANT_BAD_SIG,
    DMESH_GRANT_REPLAY,
    DMESH_GRANT_INTERNAL,
};

const char *dmesh_grant_result_name(enum dmesh_grant_result result);

/* Authoritative feeds are signed by the feed keyring (DPUMESH_FEED_KEY_DIR),
 * which is disjoint from the registration keyring so a feed publisher holds no
 * key that can mint identity. The envelope is a final line
 * `signature=<key-id>,<64 hex>`; the MAC covers every byte before it. */
enum dmesh_feed_result {
    DMESH_FEED_OK = 0,
    DMESH_FEED_UNSIGNED,
    DMESH_FEED_BAD_KEY_ID,
    DMESH_FEED_BAD_MAC,
    DMESH_FEED_INTERNAL,
};

/* Verify a feed document against the keyring in `key_dir`. On success
 * `signed_length` is the prefix the caller may parse; bytes after the envelope
 * are refused rather than ignored, so nothing unsigned is ever read. */
enum dmesh_feed_result
dmesh_feed_verify(const char *document, size_t length, const char *key_dir,
                  size_t *signed_length);

/* Verify a topology generation's envelope: the same signed-prefix rule as
 * dmesh_feed_verify, but the signature is a 64-byte Ed25519 (128 hex) verified
 * against `key_dir` public keys the DPU cannot sign with. BAD_MAC reads as a
 * bad signature. */
enum dmesh_feed_result
dmesh_gen_verify(const char *document, size_t length, const char *key_dir,
                 size_t *signed_length);

/* Parse the DPU verifier configuration. A root-only
 * DPUMESH_REGISTRATION_KEY_DIR holding up to four KEY_ID.key files is required;
 * a DPU that cannot load one refuses to start. DPUMESH_FEED_KEY_DIR selects
 * the separate feed-verification keyring; consumers that need it refuse to
 * enable without it. */
int dmesh_registration_configure(struct objects *objs,
                                 char *error, size_t error_len);

/* Select by signed key_id from the verifier's overlap keyring. */
const uint8_t *dmesh_registration_find_key(const struct objects *objs,
                                           const char *key_id);

/* Atomically consume a successfully verified grant id. The Comch control PE is
 * the single caller, so this bounded replay window needs no lock. */
int dmesh_registration_consume_grant(struct objects *objs,
                                     const uint8_t grant_id[DMESH_GRANT_ID_SIZE]);

void dmesh_grant_put_u64_le(uint8_t out[8], uint64_t value);
uint64_t dmesh_grant_get_u64_le(const uint8_t in[8]);

/* Key files are either 32 raw bytes or 64 lowercase/uppercase hexadecimal
 * digits (an optional final newline is accepted). They must be regular,
 * owned by the effective uid and inaccessible to group/other users. */
int dmesh_grant_load_key(const char *path, uint8_t key[DMESH_GRANT_KEY_SIZE],
                         char *error, size_t error_len);

/* Derive the raw Ed25519 public key of a 32-byte private seed. Used by unit
 * tests. */
int dmesh_assert_public_key(const uint8_t seed[DMESH_GRANT_KEY_SIZE],
                            uint8_t public_key[DMESH_GRANT_KEY_SIZE]);

/* Used by unit tests to mint assertions. The caller must fill a canonical v2
 * message before signing; `seed` is the node's raw Ed25519 private key. */
int dmesh_assert_sign_v2(struct dmesh_workload_assert_msg *assertion,
                         const uint8_t seed[DMESH_GRANT_KEY_SIZE]);

/* Claims a successful verification hands to the registration. The Service pair
 * (namespace_name, service_name) is compared against the Service the
 * registration requests; the rest is retained on the Pod's state. */
struct dmesh_assert_claims {
    char workload[DMESH_WORKLOAD_MAX];
    char pod_uid[DMESH_POD_UID_MAX];
    char namespace_name[DMESH_K8S_NAMESPACE_MAX];
    char service_account[DMESH_K8S_NAME_MAX];
    char service_name[DMESH_SVC_NAME_MAX];
    char pod_ip[DMESH_POD_IP_MAX];
};

/* Verify an assertion for one exact connection challenge. `public_key` is the
 * raw Ed25519 public key the signed key id selected before this call, so an
 * assertion naming an unknown key id is rejected by that lookup; the DPU holds
 * no key that could have signed. The signed node_name must equal
 * `expected_node` (the verifying DPU's own node): the Pod only relays the
 * assertion, so it cannot present another node's. Replay of assert_id is the
 * caller's check, after this one succeeds. */
enum dmesh_grant_result
dmesh_assert_verify_v2(const struct dmesh_workload_assert_msg *assertion,
                       const uint8_t public_key[DMESH_GRANT_KEY_SIZE],
                       const char *expected_node,
                       const uint8_t expected_nonce[DMESH_REG_NONCE_SIZE],
                       uint64_t now_sec,
                       struct dmesh_assert_claims *claims);

#endif /* DMESH_WORKLOAD_GRANT_H */
