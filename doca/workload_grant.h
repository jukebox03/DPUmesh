#ifndef DMESH_WORKLOAD_GRANT_H
#define DMESH_WORKLOAD_GRANT_H

#include <stddef.h>
#include <stdint.h>

#include "comch_common.h"

struct objects;

#define DMESH_GRANT_KEY_SIZE 32u
#define DMESH_GRANT_CLOCK_SKEW_SEC 30u
#define DMESH_GRANT_MAX_LIFETIME_SEC 300u

enum dmesh_grant_result {
    DMESH_GRANT_OK = 0,
    DMESH_GRANT_BAD_TYPE,
    DMESH_GRANT_BAD_VERSION,
    DMESH_GRANT_NONCANONICAL,
    DMESH_GRANT_BAD_ISSUER,
    DMESH_GRANT_BAD_KEY_ID,
    DMESH_GRANT_BAD_SERVICE,
    DMESH_GRANT_BAD_TIME,
    DMESH_GRANT_BAD_NONCE,
    DMESH_GRANT_BAD_MAC,
    DMESH_GRANT_REPLAY,
    DMESH_GRANT_INTERNAL,
};

const char *dmesh_grant_result_name(enum dmesh_grant_result result);

/* Authoritative feeds carry the same authority as a grant, so they are signed
 * by the same keyring. The envelope is a final line
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

/* Parse the DPU verifier configuration. DPUMESH_TRUSTED_REGISTRATION=required
 * requires a root-only DPUMESH_REGISTRATION_KEY_DIR containing up to four
 * KEY_ID.key files; unset/off selects development compatibility. */
int dmesh_registration_configure(struct objects *objs,
                                 char *error, size_t error_len);

/* Select by signed key_id from the verifier's overlap keyring. */
const uint8_t *dmesh_registration_find_key(const struct objects *objs,
                                           const char *key_id);

/* Atomically consume a successfully verified grant id. The Comch control PE is
 * the single caller, so this bounded replay window needs no lock. */
int dmesh_registration_consume_grant(struct objects *objs,
                                     const uint8_t grant_id[DMESH_GRANT_ID_SIZE]);

void dmesh_grant_put_i32_le(uint8_t out[4], int32_t value);
int32_t dmesh_grant_get_i32_le(const uint8_t in[4]);
void dmesh_grant_put_u64_le(uint8_t out[8], uint64_t value);
uint64_t dmesh_grant_get_u64_le(const uint8_t in[8]);

/* Key files are either 32 raw bytes or 64 lowercase/uppercase hexadecimal
 * digits (an optional final newline is accepted). They must be regular,
 * owned by the effective uid and inaccessible to group/other users. */
int dmesh_grant_load_key(const char *path, uint8_t key[DMESH_GRANT_KEY_SIZE],
                         char *error, size_t error_len);

/* Used by unit tests and trusted-agent implementations. The caller must fill a
 * canonical v1 message before signing. */
int dmesh_grant_sign_v1(struct dmesh_workload_grant_msg *grant,
                        const uint8_t key[DMESH_GRANT_KEY_SIZE]);

/* Verify a grant for one exact connection challenge. The key is selected by the
 * signed key id before this call, so a grant naming an unknown key id is
 * rejected by that lookup. On success, returns the authorized service, the
 * signed Pod UID that names this registration for revocation, and the Linkerd
 * injector-compatible workload built from signed namespace/Pod claims. */
enum dmesh_grant_result
dmesh_grant_verify_v1(const struct dmesh_workload_grant_msg *grant,
                      const uint8_t key[DMESH_GRANT_KEY_SIZE],
                      const char *expected_issuer,
                      const uint8_t expected_nonce[DMESH_REG_NONCE_SIZE],
                      uint64_t now_sec,
                      int32_t *service_id,
                      char workload[DMESH_WORKLOAD_MAX],
                      char pod_uid[DMESH_POD_UID_MAX]);

#endif /* DMESH_WORKLOAD_GRANT_H */
