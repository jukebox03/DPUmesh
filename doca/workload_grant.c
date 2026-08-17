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
    if (!canonical_text(text, cap, &len) || len < 8)
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!((c >= 'a' && c <= 'f') || (c >= '0' && c <= '9') || c == '-'))
            return 0;
    }
    return 1;
}

static enum dmesh_grant_result
validate_canonical(const struct dmesh_workload_grant_msg *grant)
{
    if (grant->type != DMESH_MSG_WORKLOAD_GRANT)
        return DMESH_GRANT_BAD_TYPE;
    if (grant->version != DMESH_GRANT_VERSION)
        return DMESH_GRANT_BAD_VERSION;
    if (grant->flags != 0 || grant->reserved != 0 ||
        all_zero(grant->grant_id, sizeof(grant->grant_id)) ||
        all_zero(grant->nonce, sizeof(grant->nonce)) ||
        !identifier_text(grant->issuer, sizeof(grant->issuer)) ||
        !identifier_text(grant->key_id, sizeof(grant->key_id)) ||
        !pod_uid_text(grant->pod_uid, sizeof(grant->pod_uid)) ||
        !dns_subdomain(grant->namespace_name,
                       sizeof(grant->namespace_name), 63) ||
        !dns_subdomain(grant->pod_name, sizeof(grant->pod_name), 253) ||
        !dns_subdomain(grant->service_account,
                       sizeof(grant->service_account), 253) ||
        !dns_subdomain(grant->node_name, sizeof(grant->node_name), 253))
        return DMESH_GRANT_NONCANONICAL;
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
    case DMESH_GRANT_BAD_ISSUER: return "bad-issuer";
    case DMESH_GRANT_BAD_KEY_ID: return "bad-key-id";
    case DMESH_GRANT_BAD_SERVICE: return "bad-service";
    case DMESH_GRANT_BAD_TIME: return "bad-time";
    case DMESH_GRANT_BAD_NONCE: return "bad-nonce";
    case DMESH_GRANT_BAD_MAC: return "bad-mac";
    case DMESH_GRANT_REPLAY: return "replay";
    case DMESH_GRANT_INTERNAL: return "internal";
    }
    return "unknown";
}

int
dmesh_registration_configure(struct objects *objs, char *error, size_t error_len)
{
    const char *mode = getenv("DPUMESH_TRUSTED_REGISTRATION");
    const char *issuer;
    const char *key_dir;
    DIR *directory = NULL;

#define CONFIG_ERROR(...) do { if (error && error_len) snprintf(error, error_len, __VA_ARGS__); } while (0)
    if (objs == NULL) {
        CONFIG_ERROR("registration objects are null");
        return -1;
    }
    objs->trusted_registration_required = 0;
    OPENSSL_cleanse(objs->registration_keys, sizeof(objs->registration_keys));
    objs->registration_key_count = 0;
    objs->registration_issuer[0] = '\0';
    memset(objs->consumed_grant_ids, 0, sizeof(objs->consumed_grant_ids));
    objs->consumed_grant_count = 0;
    objs->consumed_grant_cursor = 0;
    objs->registration_grants_accepted = 0;
    objs->registration_grants_rejected = 0;
    objs->registration_grants_replayed = 0;

    if (mode == NULL || *mode == '\0' || strcmp(mode, "0") == 0 ||
        strcmp(mode, "off") == 0 || strcmp(mode, "dev") == 0)
        return 0;
    if (strcmp(mode, "1") != 0 && strcmp(mode, "required") != 0) {
        CONFIG_ERROR("DPUMESH_TRUSTED_REGISTRATION must be off or required");
        return -1;
    }

    issuer = getenv("DPUMESH_REGISTRATION_ISSUER");
    if (issuer == NULL || *issuer == '\0')
        issuer = "dpumesh-node-agent";
    key_dir = getenv("DPUMESH_REGISTRATION_KEY_DIR");
    if (key_dir == NULL || *key_dir == '\0') {
        CONFIG_ERROR("required registration needs DPUMESH_REGISTRATION_KEY_DIR");
        return -1;
    }
    if (strlen(issuer) >= sizeof(objs->registration_issuer)) {
        CONFIG_ERROR("registration issuer is too long");
        return -1;
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

    snprintf(objs->registration_issuer, sizeof(objs->registration_issuer),
             "%s", issuer);
    objs->trusted_registration_required = 1;
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
dmesh_grant_put_i32_le(uint8_t out[4], int32_t value)
{
    uint32_t v = (uint32_t)value;
    for (unsigned int i = 0; i < 4; i++)
        out[i] = (uint8_t)(v >> (8u * i));
}

int32_t
dmesh_grant_get_i32_le(const uint8_t in[4])
{
    uint32_t v = 0;
    for (unsigned int i = 0; i < 4; i++)
        v |= (uint32_t)in[i] << (8u * i);
    return (int32_t)v;
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
dmesh_grant_sign_v1(struct dmesh_workload_grant_msg *grant,
                    const uint8_t key[DMESH_GRANT_KEY_SIZE])
{
    unsigned int mac_len = 0;
    if (grant == NULL || key == NULL ||
        validate_canonical(grant) != DMESH_GRANT_OK)
        return -1;
    if (HMAC(EVP_sha256(), key, DMESH_GRANT_KEY_SIZE,
             (const unsigned char *)grant,
             offsetof(struct dmesh_workload_grant_msg, mac),
             grant->mac, &mac_len) == NULL || mac_len != DMESH_GRANT_MAC_SIZE)
        return -1;
    return 0;
}

enum dmesh_grant_result
dmesh_grant_verify_v1(const struct dmesh_workload_grant_msg *grant,
                      const uint8_t key[DMESH_GRANT_KEY_SIZE],
                      const char *expected_issuer,
                      const char *expected_key_id,
                      const uint8_t expected_nonce[DMESH_REG_NONCE_SIZE],
                      uint64_t now_sec,
                      int32_t *service_id,
                      char workload[DMESH_WORKLOAD_MAX])
{
    uint8_t expected_mac[DMESH_GRANT_MAC_SIZE];
    unsigned int mac_len = 0;
    enum dmesh_grant_result result = validate_canonical(grant);
    if (result != DMESH_GRANT_OK)
        return result;
    if (expected_issuer == NULL || strcmp(grant->issuer, expected_issuer) != 0)
        return DMESH_GRANT_BAD_ISSUER;
    if (expected_key_id == NULL || strcmp(grant->key_id, expected_key_id) != 0)
        return DMESH_GRANT_BAD_KEY_ID;

    int32_t sid = dmesh_grant_get_i32_le(grant->service_id_le);
    if (sid != DMESH_SVC_NONE && (sid < 0 || sid >= POD_ID_SPACE))
        return DMESH_GRANT_BAD_SERVICE;
    uint64_t issued = dmesh_grant_get_u64_le(grant->issued_at_le);
    uint64_t expires = dmesh_grant_get_u64_le(grant->expires_at_le);
    if (issued > expires || expires - issued > DMESH_GRANT_MAX_LIFETIME_SEC ||
        issued > now_sec + DMESH_GRANT_CLOCK_SKEW_SEC || expires < now_sec)
        return DMESH_GRANT_BAD_TIME;
    if (CRYPTO_memcmp(grant->nonce, expected_nonce,
                      DMESH_REG_NONCE_SIZE) != 0)
        return DMESH_GRANT_BAD_NONCE;
    if (HMAC(EVP_sha256(), key, DMESH_GRANT_KEY_SIZE,
             (const unsigned char *)grant,
             offsetof(struct dmesh_workload_grant_msg, mac),
             expected_mac, &mac_len) == NULL ||
        mac_len != DMESH_GRANT_MAC_SIZE) {
        OPENSSL_cleanse(expected_mac, sizeof(expected_mac));
        return DMESH_GRANT_INTERNAL;
    }
    if (CRYPTO_memcmp(grant->mac, expected_mac, sizeof(expected_mac)) != 0) {
        OPENSSL_cleanse(expected_mac, sizeof(expected_mac));
        return DMESH_GRANT_BAD_MAC;
    }
    OPENSSL_cleanse(expected_mac, sizeof(expected_mac));

    int written = snprintf(workload, DMESH_WORKLOAD_MAX,
                           "{\"ns\":\"%s\",\"pod\":\"%s\"}",
                           grant->namespace_name, grant->pod_name);
    if (written < 0 || written >= DMESH_WORKLOAD_MAX) {
        workload[0] = '\0';
        return DMESH_GRANT_NONCANONICAL;
    }
    *service_id = sid;
    return DMESH_GRANT_OK;
}
