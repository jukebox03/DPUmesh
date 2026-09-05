#include "workload_grant.h"

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
identifier_text(const char *text, size_t cap)
{
    size_t len;
    if (!canonical_text(text, cap, &len))
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
              c == ':' || c == '/'))
            return 0;
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

static enum dmesh_grant_result
validate_canonical(const struct dmesh_workload_assert_msg *assertion)
{
    if (assertion->type != DMESH_MSG_WORKLOAD_ASSERT)
        return DMESH_GRANT_BAD_TYPE;
    if (assertion->flags != 0 || assertion->reserved != 0 ||
        all_zero(assertion->assert_id, sizeof(assertion->assert_id)) ||
        all_zero(assertion->nonce, sizeof(assertion->nonce)) ||
        all_zero(assertion->daemon_incarnation,
                 sizeof(assertion->daemon_incarnation)) ||
        dmesh_grant_get_u64_le(assertion->channel_generation_le) == 0 ||
        !identifier_text(assertion->key_id, sizeof(assertion->key_id)) ||
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
        return DMESH_GRANT_NONCANONICAL;
    if (assertion->version != DMESH_ASSERT_VERSION)
        return DMESH_GRANT_BAD_VERSION;
    return DMESH_GRANT_OK;
}

const char *
dmesh_grant_result_name(enum dmesh_grant_result result)
{
    switch (result) {
    case DMESH_GRANT_OK: return "ok";
    case DMESH_GRANT_BAD_TYPE: return "bad-type";
    case DMESH_GRANT_BAD_VERSION: return "bad-version";
    case DMESH_GRANT_NONCANONICAL: return "noncanonical";
    case DMESH_GRANT_WRONG_NODE: return "wrong-node";
    case DMESH_GRANT_BAD_KEY_ID: return "bad-key-id";
    case DMESH_GRANT_BAD_TIME: return "bad-time";
    case DMESH_GRANT_BAD_NONCE: return "bad-nonce";
    case DMESH_GRANT_BAD_SIG: return "bad-sig";
    case DMESH_GRANT_REPLAY: return "replay";
    case DMESH_GRANT_WRONG_CHANNEL: return "wrong-channel";
    case DMESH_GRANT_WRONG_INCARNATION: return "wrong-incarnation";
    case DMESH_GRANT_INTERNAL: return "internal";
    }
    return "unknown";
}

static int hex_nibble(unsigned char c);

/* A feed names its key, so the name becomes a filename. It may not contain a
 * separator or start with a dot. */
static int
feed_key_id(const char *text, size_t length, char out[DMESH_GRANT_KEY_ID_MAX])
{
    if (length == 0 || length >= DMESH_GRANT_KEY_ID_MAX || text[0] == '.')
        return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return 0;
    }
    memset(out, 0, DMESH_GRANT_KEY_ID_MAX);
    memcpy(out, text, length);
    return 1;
}

/* Split the trailing `signature=<key-id>,<hex>` envelope off a document. The
 * signed prefix ends at the newline that introduces the envelope, and nothing
 * may follow it: appended bytes would be unsigned. */
static enum dmesh_feed_result
feed_envelope_split(const char *document, size_t length, size_t sig_size,
                    char key_id[DMESH_GRANT_KEY_ID_MAX], uint8_t *signature,
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
    uint8_t key[DMESH_GRANT_KEY_SIZE];
    uint8_t expected[DMESH_GRANT_MAC_SIZE];
    uint8_t signature[DMESH_GRANT_MAC_SIZE];
    char key_id[DMESH_GRANT_KEY_ID_MAX];
    char path[4096];
    unsigned int mac_len = 0;
    size_t prefix_len = 0;
    enum dmesh_feed_result result;

    if (key_dir == NULL || *key_dir == '\0' || signed_length == NULL)
        return DMESH_FEED_UNSIGNED;
    result = feed_envelope_split(document, length, DMESH_GRANT_MAC_SIZE,
                                 key_id, signature, &prefix_len);
    if (result != DMESH_FEED_OK)
        return result;

    int written = snprintf(path, sizeof(path), "%s/%s.key", key_dir, key_id);
    if (written < 0 || (size_t)written >= sizeof(path))
        return DMESH_FEED_BAD_KEY_ID;
    if (dmesh_grant_load_key(path, key, NULL, 0) != 0)
        return DMESH_FEED_BAD_KEY_ID;

    if (HMAC(EVP_sha256(), key, DMESH_GRANT_KEY_SIZE,
             (const unsigned char *)document, prefix_len,
             expected, &mac_len) == NULL || mac_len != DMESH_GRANT_MAC_SIZE) {
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
    uint8_t public_key[DMESH_GRANT_KEY_SIZE];
    uint8_t signature[64];
    char key_id[DMESH_GRANT_KEY_ID_MAX];
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
    if (dmesh_grant_load_key(path, public_key, NULL, 0) != 0)
        return DMESH_FEED_BAD_KEY_ID;

    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL,
                                                 public_key,
                                                 DMESH_GRANT_KEY_SIZE);
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
dmesh_registration_configure(struct objects *objs, char *error, size_t error_len)
{
    const char *key_dir;
    DIR *directory = NULL;

#define CONFIG_ERROR(...) do { if (error && error_len) snprintf(error, error_len, __VA_ARGS__); } while (0)
    if (objs == NULL) {
        CONFIG_ERROR("registration objects are null");
        return -1;
    }
    OPENSSL_cleanse(objs->registration_keys, sizeof(objs->registration_keys));
    objs->registration_key_count = 0;
    objs->registration_key_dir[0] = '\0';
    memset(objs->consumed_grant_ids, 0, sizeof(objs->consumed_grant_ids));
    objs->consumed_grant_count = 0;
    objs->consumed_grant_cursor = 0;
    objs->registration_grants_accepted = 0;
    objs->registration_grants_rejected = 0;
    objs->registration_grants_replayed = 0;

    key_dir = getenv("DPUMESH_REGISTRATION_KEY_DIR");
    if (key_dir == NULL || *key_dir == '\0') {
        CONFIG_ERROR("registration needs DPUMESH_REGISTRATION_KEY_DIR");
        return -1;
    }
    if (strlen(key_dir) + DMESH_GRANT_KEY_ID_MAX + 8 >= sizeof(objs->registration_key_dir)) {
        CONFIG_ERROR("DPUMESH_REGISTRATION_KEY_DIR is too long");
        return -1;
    }

    /* The feed keyring signs authoritative feeds only. Keeping it apart from
     * the registration keyring keeps identity minting out of the publishers'
     * hands; the two directories must hold disjoint key files. */
    objs->feed_key_dir[0] = '\0';
    const char *feed_dir = getenv("DPUMESH_FEED_KEY_DIR");
    if (feed_dir != NULL && *feed_dir != '\0') {
        if (strlen(feed_dir) + DMESH_GRANT_KEY_ID_MAX + 8 >=
            sizeof(objs->feed_key_dir)) {
            CONFIG_ERROR("DPUMESH_FEED_KEY_DIR is too long");
            return -1;
        }
        DIR *feed_directory = opendir(feed_dir);
        if (feed_directory == NULL) {
            CONFIG_ERROR("opendir(%s): %s", feed_dir, strerror(errno));
            return -1;
        }
        struct stat feed_stat;
        int feed_dir_ok = fstat(dirfd(feed_directory), &feed_stat) == 0 &&
                          S_ISDIR(feed_stat.st_mode) &&
                          feed_stat.st_uid == geteuid() &&
                          (feed_stat.st_mode & 077) == 0;
        closedir(feed_directory);
        if (!feed_dir_ok) {
            CONFIG_ERROR("%s must be owned by uid %u with mode 0700", feed_dir,
                         (unsigned int)geteuid());
            return -1;
        }
        snprintf(objs->feed_key_dir, sizeof(objs->feed_key_dir), "%s", feed_dir);
    }

    directory = opendir(key_dir);
    if (directory == NULL) {
        CONFIG_ERROR("opendir(%s): %s", key_dir, strerror(errno));
        return -1;
    }
    struct stat dir_stat;
    if (fstat(dirfd(directory), &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode) ||
        dir_stat.st_uid != geteuid() || (dir_stat.st_mode & 077) != 0) {
        CONFIG_ERROR("%s must be owned by uid %u with mode 0700", key_dir,
                     (unsigned int)geteuid());
        closedir(directory);
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        if (name_len <= 4 || strcmp(entry->d_name + name_len - 4, ".key") != 0)
            continue;
        size_t key_id_len = name_len - 4;
        if (key_id_len >= DMESH_GRANT_KEY_ID_MAX ||
            objs->registration_key_count >= DMESH_REGISTRATION_MAX_KEYS) {
            CONFIG_ERROR("%s contains too many keys or an overlong key id", key_dir);
            goto keyring_error;
        }
        char key_id[DMESH_GRANT_KEY_ID_MAX];
        memset(key_id, 0, sizeof(key_id));
        memcpy(key_id, entry->d_name, key_id_len);
        key_id[key_id_len] = '\0';
        if (!identifier_text(key_id, sizeof(key_id))) {
            CONFIG_ERROR("invalid registration key filename: %s", entry->d_name);
            goto keyring_error;
        }
        char path[4096];
        int written = snprintf(path, sizeof(path), "%s/%s", key_dir, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            CONFIG_ERROR("registration key path is too long");
            goto keyring_error;
        }
        struct dmesh_registration_key *key =
            &objs->registration_keys[objs->registration_key_count];
        if (dmesh_grant_load_key(path, key->bytes, error, error_len) != 0)
            goto keyring_error;
        snprintf(key->key_id, sizeof(key->key_id), "%s", key_id);
        objs->registration_key_count++;
    }
    closedir(directory);
    directory = NULL;
    if (objs->registration_key_count == 0) {
        CONFIG_ERROR("%s contains no registration .key files", key_dir);
        return -1;
    }

    snprintf(objs->registration_key_dir, sizeof(objs->registration_key_dir),
             "%s", key_dir);
    return 0;

keyring_error:
    if (directory != NULL)
        closedir(directory);
    OPENSSL_cleanse(objs->registration_keys, sizeof(objs->registration_keys));
    objs->registration_key_count = 0;
    return -1;
#undef CONFIG_ERROR
}

const uint8_t *
dmesh_registration_find_key(const struct objects *objs, const char *key_id)
{
    if (objs == NULL || key_id == NULL)
        return NULL;
    const char *end = memchr(key_id, '\0', DMESH_GRANT_KEY_ID_MAX);
    if (end == NULL || end == key_id)
        return NULL;
    for (size_t i = 0; i < objs->registration_key_count; i++)
        if (strncmp(objs->registration_keys[i].key_id, key_id,
                    DMESH_GRANT_KEY_ID_MAX) == 0)
            return objs->registration_keys[i].bytes;
    return NULL;
}

int
dmesh_registration_consume_grant(struct objects *objs,
                                 const uint8_t grant_id[DMESH_GRANT_ID_SIZE])
{
    if (objs == NULL || grant_id == NULL)
        return -1;
    for (size_t i = 0; i < objs->consumed_grant_count; i++)
        if (CRYPTO_memcmp(objs->consumed_grant_ids[i], grant_id,
                          DMESH_GRANT_ID_SIZE) == 0)
            return -1;

    size_t slot;
    if (objs->consumed_grant_count < DMESH_REGISTRATION_REPLAY_SLOTS) {
        slot = objs->consumed_grant_count++;
    } else {
        slot = objs->consumed_grant_cursor;
        objs->consumed_grant_cursor =
            (objs->consumed_grant_cursor + 1) % DMESH_REGISTRATION_REPLAY_SLOTS;
    }
    memcpy(objs->consumed_grant_ids[slot], grant_id, DMESH_GRANT_ID_SIZE);
    return 0;
}

void
dmesh_grant_put_u64_le(uint8_t out[8], uint64_t value)
{
    for (unsigned int i = 0; i < 8; i++)
        out[i] = (uint8_t)(value >> (8u * i));
}

uint64_t
dmesh_grant_get_u64_le(const uint8_t in[8])
{
    uint64_t v = 0;
    for (unsigned int i = 0; i < 8; i++)
        v |= (uint64_t)in[i] << (8u * i);
    return v;
}

void
dmesh_grant_put_u32_le(uint8_t out[4], uint32_t value)
{
    for (size_t i = 0; i < 4; i++)
        out[i] = (uint8_t)(value >> (8u * i));
}

uint32_t
dmesh_grant_get_u32_le(const uint8_t in[4])
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
dmesh_grant_load_key(const char *path, uint8_t key[DMESH_GRANT_KEY_SIZE],
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
    if (all_zero(key, DMESH_GRANT_KEY_SIZE)) {
        KEY_ERROR("%s contains an all-zero key", path);
        OPENSSL_cleanse(key, DMESH_GRANT_KEY_SIZE);
        goto out;
    }
    rc = 0;
out:
    OPENSSL_cleanse(input, sizeof(input));
    close(fd);
    return rc;
#undef KEY_ERROR
}

int
dmesh_assert_public_key(const uint8_t seed[DMESH_GRANT_KEY_SIZE],
                        uint8_t public_key[DMESH_GRANT_KEY_SIZE])
{
    int rc = -1;
    size_t public_len = DMESH_GRANT_KEY_SIZE;
    if (seed == NULL || public_key == NULL)
        return -1;
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL,
                                                  seed, DMESH_GRANT_KEY_SIZE);
    if (pkey == NULL)
        return -1;
    if (EVP_PKEY_get_raw_public_key(pkey, public_key, &public_len) == 1 &&
        public_len == DMESH_GRANT_KEY_SIZE)
        rc = 0;
    EVP_PKEY_free(pkey);
    return rc;
}

int
dmesh_assert_sign_v3(struct dmesh_workload_assert_msg *assertion,
                     const uint8_t seed[DMESH_GRANT_KEY_SIZE])
{
    int rc = -1;
    size_t sig_len = DMESH_ASSERT_SIG_SIZE;
    EVP_MD_CTX *md = NULL;
    if (assertion == NULL || seed == NULL ||
        validate_canonical(assertion) != DMESH_GRANT_OK)
        return -1;
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL,
                                                  seed, DMESH_GRANT_KEY_SIZE);
    if (pkey == NULL)
        return -1;
    md = EVP_MD_CTX_new();
    if (md != NULL &&
        EVP_DigestSignInit(md, NULL, NULL, NULL, pkey) == 1 &&
        EVP_DigestSign(md, assertion->sig, &sig_len,
                       (const unsigned char *)assertion,
                       offsetof(struct dmesh_workload_assert_msg, sig)) == 1 &&
        sig_len == DMESH_ASSERT_SIG_SIZE)
        rc = 0;
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    return rc;
}

enum dmesh_grant_result
dmesh_assert_verify_v3(const struct dmesh_workload_assert_msg *assertion,
                       const uint8_t public_key[DMESH_GRANT_KEY_SIZE],
                       const char *expected_cluster,
                       const char *expected_node,
                       const uint8_t expected_nonce[DMESH_REG_NONCE_SIZE],
                       uint64_t now_sec,
                       struct dmesh_assert_claims *claims)
{
    enum dmesh_grant_result result = validate_canonical(assertion);
    if (result != DMESH_GRANT_OK)
        return result;
    if (expected_cluster == NULL || *expected_cluster == '\0' ||
        strcmp(assertion->cluster_id, expected_cluster) != 0 ||
        expected_node == NULL || *expected_node == '\0' ||
        strcmp(assertion->node_name, expected_node) != 0)
        return DMESH_GRANT_WRONG_NODE;

    uint64_t issued = dmesh_grant_get_u64_le(assertion->issued_at_le);
    uint64_t expires = dmesh_grant_get_u64_le(assertion->expires_at_le);
    /* The skew grace is one-sided, on issued_at only; expiry gets no grace. */
    if (issued > expires ||
        expires - issued > DMESH_ASSERT_MAX_LIFETIME_SEC ||
        issued > now_sec + DMESH_ASSERT_CLOCK_SKEW_SEC || expires <= now_sec)
        return DMESH_GRANT_BAD_TIME;
    if (CRYPTO_memcmp(assertion->nonce, expected_nonce,
                      DMESH_REG_NONCE_SIZE) != 0)
        return DMESH_GRANT_BAD_NONCE;

    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL,
                                                 public_key,
                                                 DMESH_GRANT_KEY_SIZE);
    if (pkey == NULL)
        return DMESH_GRANT_INTERNAL;
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (md == NULL) {
        EVP_PKEY_free(pkey);
        return DMESH_GRANT_INTERNAL;
    }
    int verified =
        EVP_DigestVerifyInit(md, NULL, NULL, NULL, pkey) == 1 &&
        EVP_DigestVerify(md, assertion->sig, DMESH_ASSERT_SIG_SIZE,
                         (const unsigned char *)assertion,
                         offsetof(struct dmesh_workload_assert_msg, sig)) == 1;
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    if (!verified)
        return DMESH_GRANT_BAD_SIG;

    int written = snprintf(claims->workload, sizeof(claims->workload),
                           "{\"ns\":\"%s\",\"pod\":\"%s\"}",
                           assertion->namespace_name, assertion->pod_name);
    if (written < 0 || written >= (int)sizeof(claims->workload)) {
        claims->workload[0] = '\0';
        return DMESH_GRANT_NONCANONICAL;
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
    claims->channel_slot = dmesh_grant_get_u32_le(assertion->channel_slot_le);
    claims->channel_generation =
        dmesh_grant_get_u64_le(assertion->channel_generation_le);
    return DMESH_GRANT_OK;
}
