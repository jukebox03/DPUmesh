#include "workload_identity.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <dpumesh/dmesh_common.h>

#include "object.h"

static int
all_zero(const uint8_t *bytes, size_t len)
{
    uint8_t combined = 0;
    for (size_t i = 0; i < len; i++)
        combined |= bytes[i];
    return combined == 0;
}

static int
canonical_text(const char *text, size_t cap, size_t *length)
{
    const char *end = memchr(text, '\0', cap);
    if (end == NULL || end == text)
        return 0;
    size_t len = (size_t)(end - text);
    for (size_t i = len + 1; i < cap; i++)
        if ((unsigned char)text[i] != 0)
            return 0;
    if (length != NULL)
        *length = len;
    return 1;
}

static int
dns_label_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

static int
dns_subdomain(const char *text, size_t cap, size_t max_len)
{
    size_t len;
    if (!canonical_text(text, cap, &len) || len > max_len ||
        !dns_label_char((unsigned char)text[0]) ||
        !dns_label_char((unsigned char)text[len - 1]) ||
        text[0] == '-' || text[len - 1] == '-')
        return 0;

    int label_start = 1;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '.') {
            if (label_start || i == len - 1 || text[i - 1] == '-')
                return 0;
            label_start = 1;
            continue;
        }
        if (!dns_label_char(c) || (label_start && c == '-'))
            return 0;
        label_start = 0;
    }
    return 1;
}

static int
pod_uid_text(const char *text, size_t cap)
{
    size_t len;
    if (!canonical_text(text, cap, &len) || len != 36)
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        int hyphen = i == 8 || i == 13 || i == 18 || i == 23;
        if ((hyphen && c != '-') ||
            (!hyphen && !((c >= 'a' && c <= 'f') ||
                           (c >= '0' && c <= '9'))))
            return 0;
    }
    return 1;
}

static int
container_id_text(const char *text, size_t cap)
{
    size_t len;
    if (!canonical_text(text, cap, &len) || len != 64)
        return 0;
    for (size_t i = 0; i < len; i++)
        if (!((text[i] >= '0' && text[i] <= '9') ||
              (text[i] >= 'a' && text[i] <= 'f')))
            return 0;
    return 1;
}

/* Dotted-quad IPv4 text: four decimal octets, no leading zeros beyond "0". */
static int
ipv4_text(const char *text, size_t cap)
{
    size_t len;
    if (!canonical_text(text, cap, &len) || len > 15)
        return 0;
    int octets = 0;
    size_t i = 0;
    while (i < len) {
        if (text[i] < '0' || text[i] > '9')
            return 0;
        unsigned int value = 0;
        size_t digits = 0;
        while (i < len && text[i] >= '0' && text[i] <= '9') {
            value = value * 10u + (unsigned int)(text[i] - '0');
            digits++;
            i++;
        }
        if (digits == 0 || digits > 3 || value > 255 ||
            (digits > 1 && text[i - digits] == '0'))
            return 0;
        octets++;
        if (i < len) {
            if (text[i] != '.' || octets == 4)
                return 0;
            i++;
            if (i == len)
                return 0;
        }
    }
    return octets == 4;
}

/* Empty is a valid Service claim (a client-only Pod advertises nothing). */
static int
service_name_text(const char *text, size_t cap)
{
    if (text[0] == '\0') {
        for (size_t i = 1; i < cap; i++)
            if ((unsigned char)text[i] != 0)
                return 0;
        return 1;
    }
    return dns_subdomain(text, cap, 63);
}

static enum dmesh_identity_result
validate_canonical(const struct dmesh_workload_identity *assertion)
{
    if (assertion->type != DMESH_IDENTITY_TYPE)
        return DMESH_IDENTITY_BAD_TYPE;
    if (assertion->flags != 0 || assertion->reserved != 0 ||

        all_zero(assertion->nonce, sizeof(assertion->nonce)) ||
        all_zero(assertion->daemon_incarnation,
                 sizeof(assertion->daemon_incarnation)) ||
        dmesh_wire_get_u64_le(assertion->channel_generation_le) == 0 ||

        !dns_subdomain(assertion->cluster_id,
                       sizeof(assertion->cluster_id), 63) ||
        !pod_uid_text(assertion->pod_uid, sizeof(assertion->pod_uid)) ||
        !dns_subdomain(assertion->namespace_name,
                       sizeof(assertion->namespace_name), 63) ||
        !dns_subdomain(assertion->pod_name, sizeof(assertion->pod_name), 253) ||
        !dns_subdomain(assertion->service_account,
                       sizeof(assertion->service_account), 253) ||
        !dns_subdomain(assertion->container_name,
                       sizeof(assertion->container_name), 253) ||
        !container_id_text(assertion->container_id,
                           sizeof(assertion->container_id)) ||
        !dns_subdomain(assertion->node_name,
                       sizeof(assertion->node_name), 253) ||
        !service_name_text(assertion->service_name,
                           sizeof(assertion->service_name)) ||
        !ipv4_text(assertion->pod_ip, sizeof(assertion->pod_ip)))
        return DMESH_IDENTITY_NONCANONICAL;
    if (assertion->version != DMESH_IDENTITY_VERSION)
        return DMESH_IDENTITY_BAD_VERSION;
    return DMESH_IDENTITY_OK;
}

const char *
dmesh_identity_result_name(enum dmesh_identity_result result)
{
    switch (result) {
    case DMESH_IDENTITY_OK: return "ok";
    case DMESH_IDENTITY_BAD_TYPE: return "bad-type";
    case DMESH_IDENTITY_BAD_VERSION: return "bad-version";
    case DMESH_IDENTITY_NONCANONICAL: return "noncanonical";
    case DMESH_IDENTITY_WRONG_NODE: return "wrong-node";
    case DMESH_IDENTITY_BAD_TIME: return "bad-time";
    case DMESH_IDENTITY_BAD_NONCE: return "bad-nonce";
    case DMESH_IDENTITY_WRONG_CHANNEL: return "wrong-channel";
    case DMESH_IDENTITY_WRONG_INCARNATION: return "wrong-incarnation";
    }
    return "unknown";
}

static int hex_nibble(unsigned char c);

/* A feed names its key, so the name becomes a filename. It may not contain a
 * separator or start with a dot. */
static int
feed_key_id(const char *text, size_t length, char out[DMESH_KEY_ID_MAX])
{
    if (length == 0 || length >= DMESH_KEY_ID_MAX || text[0] == '.')
        return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return 0;
    }
    memset(out, 0, DMESH_KEY_ID_MAX);
    memcpy(out, text, length);
    return 1;
}

/* Split the trailing `signature=<key-id>,<hex>` envelope off a document. The
 * signed prefix ends at the newline that introduces the envelope, and nothing
 * may follow it: appended bytes would be unsigned. */
static enum dmesh_feed_result
feed_envelope_split(const char *document, size_t length, size_t sig_size,
                    char key_id[DMESH_KEY_ID_MAX], uint8_t *signature,
                    size_t *prefix_len)
{
    static const char MARKER[] = "\nsignature=";
    const size_t marker_len = sizeof(MARKER) - 1;

    if (document == NULL || length < marker_len)
        return DMESH_FEED_UNSIGNED;

    const char *marker = NULL;
    for (size_t i = length - marker_len + 1; i-- > 0; ) {
        if (memcmp(document + i, MARKER, marker_len) == 0) {
            marker = document + i;
            break;
        }
    }
    if (marker == NULL)
        return DMESH_FEED_UNSIGNED;

    *prefix_len = (size_t)(marker - document) + 1;
    const char *value = marker + marker_len;
    size_t value_len = length - (size_t)(value - document);
    if (value_len > 0 && value[value_len - 1] == '\n')
        value_len--;
    if (memchr(value, '\n', value_len) != NULL)
        return DMESH_FEED_UNSIGNED;

    const char *comma = memchr(value, ',', value_len);
    if (comma == NULL)
        return DMESH_FEED_UNSIGNED;
    if (!feed_key_id(value, (size_t)(comma - value), key_id))
        return DMESH_FEED_BAD_KEY_ID;

    const char *hex = comma + 1;
    size_t hex_len = value_len - (size_t)(hex - value);
    if (hex_len != 2u * sig_size)
        return DMESH_FEED_UNSIGNED;
    for (size_t i = 0; i < sig_size; i++) {
        int hi = hex_nibble((unsigned char)hex[2 * i]);
        int lo = hex_nibble((unsigned char)hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return DMESH_FEED_UNSIGNED;
        signature[i] = (uint8_t)((hi << 4) | lo);
    }
    return DMESH_FEED_OK;
}

enum dmesh_feed_result
dmesh_feed_verify(const char *document, size_t length, const char *key_dir,
                  size_t *signed_length)
{
    uint8_t key[DMESH_KEY_SIZE];
    uint8_t expected[DMESH_FEED_MAC_SIZE];
    uint8_t signature[DMESH_FEED_MAC_SIZE];
    char key_id[DMESH_KEY_ID_MAX];
    char path[4096];
    unsigned int mac_len = 0;
    size_t prefix_len = 0;
    enum dmesh_feed_result result;

    if (key_dir == NULL || *key_dir == '\0' || signed_length == NULL)
        return DMESH_FEED_UNSIGNED;
    result = feed_envelope_split(document, length, DMESH_FEED_MAC_SIZE,
                                 key_id, signature, &prefix_len);
    if (result != DMESH_FEED_OK)
        return result;

    int written = snprintf(path, sizeof(path), "%s/%s.key", key_dir, key_id);
    if (written < 0 || (size_t)written >= sizeof(path))
        return DMESH_FEED_BAD_KEY_ID;
    if (dmesh_load_key(path, key, NULL, 0) != 0)
        return DMESH_FEED_BAD_KEY_ID;

    if (HMAC(EVP_sha256(), key, DMESH_KEY_SIZE,
             (const unsigned char *)document, prefix_len,
             expected, &mac_len) == NULL || mac_len != DMESH_FEED_MAC_SIZE) {
        result = DMESH_FEED_INTERNAL;
        goto out;
    }
    if (CRYPTO_memcmp(signature, expected, sizeof(expected)) != 0) {
        result = DMESH_FEED_BAD_MAC;
        goto out;
    }
    *signed_length = prefix_len;
    result = DMESH_FEED_OK;
out:
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(expected, sizeof(expected));
    return result;
}

enum dmesh_feed_result
dmesh_gen_verify(const char *document, size_t length, const char *key_dir,
                 size_t *signed_length)
{
    uint8_t public_key[DMESH_KEY_SIZE];
    uint8_t signature[64];
    char key_id[DMESH_KEY_ID_MAX];
    char path[4096];
    size_t prefix_len = 0;
    enum dmesh_feed_result result;

    if (key_dir == NULL || *key_dir == '\0' || signed_length == NULL)
        return DMESH_FEED_UNSIGNED;
    result = feed_envelope_split(document, length, sizeof(signature),
                                 key_id, signature, &prefix_len);
    if (result != DMESH_FEED_OK)
        return result;

    int written = snprintf(path, sizeof(path), "%s/%s.key", key_dir, key_id);
    if (written < 0 || (size_t)written >= sizeof(path))
        return DMESH_FEED_BAD_KEY_ID;
    if (dmesh_load_key(path, public_key, NULL, 0) != 0)
        return DMESH_FEED_BAD_KEY_ID;

    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL,
                                                 public_key,
                                                 DMESH_KEY_SIZE);
    if (pkey == NULL)
        return DMESH_FEED_INTERNAL;
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (md == NULL) {
        EVP_PKEY_free(pkey);
        return DMESH_FEED_INTERNAL;
    }
    int verified =
        EVP_DigestVerifyInit(md, NULL, NULL, NULL, pkey) == 1 &&
        EVP_DigestVerify(md, signature, sizeof(signature),
                         (const unsigned char *)document, prefix_len) == 1;
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    if (!verified)
        return DMESH_FEED_BAD_MAC;
    *signed_length = prefix_len;
    return DMESH_FEED_OK;
}

int
dmesh_feed_configure(struct objects *objs, char *error, size_t error_len)
{
    const char *path = getenv("DPUMESH_FEED_KEY_DIR");
    if (!objs || !path || !*path || strlen(path) >= sizeof(objs->feed_key_dir)) {
        if (error && error_len) snprintf(error, error_len, "DPUMESH_FEED_KEY_DIR is required");
        return -1;
    }
    snprintf(objs->feed_key_dir, sizeof(objs->feed_key_dir), "%s", path);
    return 0;
}

void
dmesh_wire_put_u64_le(uint8_t out[8], uint64_t value)
{
    for (unsigned int i = 0; i < 8; i++)
        out[i] = (uint8_t)(value >> (8u * i));
}

uint64_t
dmesh_wire_get_u64_le(const uint8_t in[8])
{
    uint64_t v = 0;
    for (unsigned int i = 0; i < 8; i++)
        v |= (uint64_t)in[i] << (8u * i);
    return v;
}

void
dmesh_wire_put_u32_le(uint8_t out[4], uint32_t value)
{
    for (size_t i = 0; i < 4; i++)
        out[i] = (uint8_t)(value >> (8u * i));
}

uint32_t
dmesh_wire_get_u32_le(const uint8_t in[4])
{
    uint32_t value = 0;
    for (size_t i = 0; i < 4; i++)
        value |= (uint32_t)in[i] << (8u * i);
    return value;
}

static int
hex_nibble(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int
dmesh_load_key(const char *path, uint8_t key[DMESH_KEY_SIZE],
                     char *error, size_t error_len)
{
    uint8_t input[66];
    int fd = -1;
    int rc = -1;
    ssize_t len;
    struct stat st;

#define KEY_ERROR(...) do { if (error && error_len) snprintf(error, error_len, __VA_ARGS__); } while (0)
    if (path == NULL || *path == '\0') {
        KEY_ERROR("key path is empty");
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        KEY_ERROR("open(%s): %s", path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        KEY_ERROR("%s is not a regular file", path);
        goto out;
    }
    if (st.st_uid != geteuid() || (st.st_mode & 077) != 0 ||
        (st.st_mode & S_IRUSR) == 0 || (st.st_mode & S_IXUSR) != 0 ||
        (st.st_mode & (S_ISUID | S_ISGID | S_ISVTX)) != 0) {
        KEY_ERROR("%s must be owned by uid %u with mode 0600/0400",
                  path, (unsigned int)geteuid());
        goto out;
    }
    len = read(fd, input, sizeof(input));
    if (len < 0) {
        KEY_ERROR("read(%s): %s", path, strerror(errno));
        goto out;
    }
    uint8_t extra;
    if (read(fd, &extra, 1) != 0) {
        KEY_ERROR("%s is longer than a v1 registration key", path);
        goto out;
    }
    if (len == 32) {
        memcpy(key, input, 32);
    } else {
        if (len == 65 && input[64] == '\n')
            len = 64;
        if (len != 64) {
            KEY_ERROR("%s must contain 32 raw bytes or 64 hex digits", path);
            goto out;
        }
        for (int i = 0; i < 32; i++) {
            int hi = hex_nibble(input[2 * i]);
            int lo = hex_nibble(input[2 * i + 1]);
            if (hi < 0 || lo < 0) {
                KEY_ERROR("%s contains non-hex key data", path);
                goto out;
            }
            key[i] = (uint8_t)((hi << 4) | lo);
        }
    }
    if (all_zero(key, DMESH_KEY_SIZE)) {
        KEY_ERROR("%s contains an all-zero key", path);
        OPENSSL_cleanse(key, DMESH_KEY_SIZE);
        goto out;
    }
    rc = 0;
out:
    OPENSSL_cleanse(input, sizeof(input));
    close(fd);
    return rc;
#undef KEY_ERROR
}




enum dmesh_identity_result
dmesh_identity_decode(const struct dmesh_workload_identity *assertion,
                         const char *expected_cluster, const char *expected_node,
                         const uint8_t expected_nonce[DMESH_REG_NONCE_SIZE],
                         uint64_t now_sec, struct dmesh_identity_claims *claims)
{
    enum dmesh_identity_result result = validate_canonical(assertion);
    if (result != DMESH_IDENTITY_OK) return result;
    if (!expected_cluster || !expected_node ||
        strcmp(assertion->cluster_id, expected_cluster) ||
        strcmp(assertion->node_name, expected_node)) return DMESH_IDENTITY_WRONG_NODE;
    uint64_t issued = dmesh_wire_get_u64_le(assertion->issued_at_le);
    uint64_t expires = dmesh_wire_get_u64_le(assertion->expires_at_le);
    if (issued > expires || expires - issued > DMESH_ASSERT_MAX_LIFETIME_SEC ||
        issued > now_sec + DMESH_ASSERT_CLOCK_SKEW_SEC || expires <= now_sec)
        return DMESH_IDENTITY_BAD_TIME;
    if (memcmp(assertion->nonce, expected_nonce, DMESH_REG_NONCE_SIZE))
        return DMESH_IDENTITY_BAD_NONCE;
    int written = snprintf(claims->workload, sizeof(claims->workload),
                           "{\"ns\":\"%s\",\"pod\":\"%s\"}",
                           assertion->namespace_name, assertion->pod_name);
    if (written < 0 || written >= (int)sizeof(claims->workload)) {
        claims->workload[0] = '\0';
        return DMESH_IDENTITY_NONCANONICAL;
    }
    memcpy(claims->pod_uid, assertion->pod_uid, sizeof(claims->pod_uid));
    memcpy(claims->namespace_name, assertion->namespace_name,
           sizeof(claims->namespace_name));
    memcpy(claims->service_account, assertion->service_account,
           sizeof(claims->service_account));
    memcpy(claims->service_name, assertion->service_name,
           sizeof(claims->service_name));
    memcpy(claims->pod_ip, assertion->pod_ip, sizeof(claims->pod_ip));
    memcpy(claims->daemon_incarnation, assertion->daemon_incarnation,
           sizeof(claims->daemon_incarnation));
    claims->channel_slot = dmesh_wire_get_u32_le(assertion->channel_slot_le);
    claims->channel_generation =
        dmesh_wire_get_u64_le(assertion->channel_generation_le);
    return DMESH_IDENTITY_OK;
}
