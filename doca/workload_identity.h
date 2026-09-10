#ifndef DMESH_WORKLOAD_IDENTITY_H
#define DMESH_WORKLOAD_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

#include "comch_common.h"

struct objects;

#define DMESH_KEY_SIZE 32u
#define DMESH_ASSERT_CLOCK_SKEW_SEC 30u
#define DMESH_ASSERT_MAX_LIFETIME_SEC 300u

enum dmesh_identity_result {
    DMESH_IDENTITY_OK = 0,
    DMESH_IDENTITY_BAD_TYPE,
    DMESH_IDENTITY_BAD_VERSION,
    DMESH_IDENTITY_NONCANONICAL,
    DMESH_IDENTITY_WRONG_NODE,
    DMESH_IDENTITY_BAD_TIME,
    DMESH_IDENTITY_BAD_NONCE,
    DMESH_IDENTITY_WRONG_CHANNEL,
    DMESH_IDENTITY_WRONG_INCARNATION,
};

const char *dmesh_identity_result_name(enum dmesh_identity_result result);

/* Authoritative feeds are signed by the feed keyring (DPUMESH_FEED_KEY_DIR),
 * which is disjoint from the topology signing key so each signature has one
 * protocol role. The envelope is a final line
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

int dmesh_feed_configure(struct objects *, char *, size_t);

void dmesh_wire_put_u64_le(uint8_t out[8], uint64_t value);
uint64_t dmesh_wire_get_u64_le(const uint8_t in[8]);
void dmesh_wire_put_u32_le(uint8_t out[4], uint32_t value);
uint32_t dmesh_wire_get_u32_le(const uint8_t in[4]);

/* Key files are either 32 raw bytes or 64 lowercase/uppercase hexadecimal
 * digits (an optional final newline is accepted). They must be regular,
 * owned by the effective uid and inaccessible to group/other users. */
int dmesh_load_key(const char *path, uint8_t key[DMESH_KEY_SIZE],
                         char *error, size_t error_len);

/* Claims a successful verification hands to the registration. The Service pair
 * (namespace_name, service_name) is compared against the Service the
 * registration requests; the rest is retained on the Pod's state. */
struct dmesh_identity_claims {
    char workload[DMESH_WORKLOAD_MAX];
    char pod_uid[DMESH_POD_UID_MAX];
    char namespace_name[DMESH_K8S_NAMESPACE_MAX];
    char service_account[DMESH_K8S_NAME_MAX];
    char service_name[DMESH_SVC_NAME_MAX];
    char pod_ip[DMESH_POD_IP_MAX];
    uint8_t daemon_incarnation[DMESH_DAEMON_INCARNATION_SIZE];
    uint32_t channel_slot;
    uint64_t channel_generation;
};

/* Metadata decoder. Never authorizes a Comch message. The direct-registration
 * caller must already be an authenticated paired-host control session. */
enum dmesh_identity_result dmesh_identity_decode(
    const struct dmesh_workload_identity *, const char *, const char *,
    const uint8_t [DMESH_REG_NONCE_SIZE], uint64_t,
    struct dmesh_identity_claims *);

#endif /* DMESH_WORKLOAD_IDENTITY_H */
