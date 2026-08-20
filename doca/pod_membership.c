#include "pod_membership.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dpumesh/dmesh_common.h>

#include "object.h"
#include "workload_grant.h"

/* A generation larger than this cannot be a membership document. */
#define MEMBERSHIP_MAX_BYTES (256u * 1024u)
/* Seconds a generation must have been installed before its stamp is trusted. */
#define MEMBERSHIP_STAMP_SETTLE_SEC 2

const char *
dmesh_membership_result_name(enum dmesh_membership_result result)
{
    switch (result) {
    case DMESH_MEMBERSHIP_UNCHANGED: return "unchanged";
    case DMESH_MEMBERSHIP_ADOPTED: return "adopted";
    case DMESH_MEMBERSHIP_UNREADABLE: return "unreadable";
    case DMESH_MEMBERSHIP_MALFORMED: return "malformed";
    case DMESH_MEMBERSHIP_ROLLBACK: return "rollback";
    case DMESH_MEMBERSHIP_OVERFLOW: return "overflow";
    case DMESH_MEMBERSHIP_UNSIGNED: return "unsigned";
    }
    return "unknown";
}

static int
pod_uid_char(unsigned char c)
{
    return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9') || c == '-';
}

static int
service_label_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

/* `member=<pod-uid>,<service-name>`; the name `-` states that the Pod exists
 * on this node without Service membership, which is what a pure client holds.
 * Names, not node-local numbers, cross this feed: the compact id is the DPU's
 * own interning of the topology generation. */
static enum dmesh_membership_result
parse_member(const char *line, size_t length,
             struct dmesh_membership_entry *entry)
{
    const char *comma = memchr(line, ',', length);
    if (comma == NULL)
        return DMESH_MEMBERSHIP_MALFORMED;
    size_t uid_len = (size_t)(comma - line);
    if (uid_len < 8 || uid_len >= DMESH_POD_UID_MAX)
        return DMESH_MEMBERSHIP_MALFORMED;
    for (size_t i = 0; i < uid_len; i++)
        if (!pod_uid_char((unsigned char)line[i]))
            return DMESH_MEMBERSHIP_MALFORMED;

    const char *name = comma + 1;
    size_t name_len = length - uid_len - 1;
    memset(entry->pod_uid, 0, sizeof(entry->pod_uid));
    memcpy(entry->pod_uid, line, uid_len);
    memset(entry->service_name, 0, sizeof(entry->service_name));
    if (name_len == 1 && name[0] == '-')
        return DMESH_MEMBERSHIP_ADOPTED;     /* bare membership */
    if (name_len == 0 || name_len > 63 ||
        name[0] == '-' || name[name_len - 1] == '-')
        return DMESH_MEMBERSHIP_MALFORMED;
    for (size_t i = 0; i < name_len; i++)
        if (!service_label_char((unsigned char)name[i]))
            return DMESH_MEMBERSHIP_MALFORMED;
    memcpy(entry->service_name, name, name_len);
    return DMESH_MEMBERSHIP_ADOPTED;
}

enum dmesh_membership_result
dmesh_membership_parse(const char *document, size_t length, uint64_t *version,
                       struct dmesh_membership_entry *entries, size_t max_entries,
                       size_t *count)
{
    int have_version = 0;
    uint64_t parsed_version = 0;
    size_t entry_count = 0;

    if (document == NULL || version == NULL || entries == NULL || count == NULL)
        return DMESH_MEMBERSHIP_MALFORMED;

    size_t offset = 0;
    while (offset < length) {
        const char *line = document + offset;
        const char *newline = memchr(line, '\n', length - offset);
        size_t line_len = newline ? (size_t)(newline - line) : length - offset;
        offset += line_len + (newline ? 1 : 0);

        const char *hash = memchr(line, '#', line_len);
        if (hash != NULL)
            line_len = (size_t)(hash - line);
        while (line_len > 0 && (line[line_len - 1] == ' ' ||
                                line[line_len - 1] == '\r' ||
                                line[line_len - 1] == '\t'))
            line_len--;
        while (line_len > 0 && (*line == ' ' || *line == '\t')) {
            line++;
            line_len--;
        }
        if (line_len == 0)
            continue;

        if (line_len > 8 && memcmp(line, "version=", 8) == 0) {
            if (have_version)
                return DMESH_MEMBERSHIP_MALFORMED;
            const char *digits = line + 8;
            size_t digits_len = line_len - 8;
            if (digits_len > 20)
                return DMESH_MEMBERSHIP_MALFORMED;
            for (size_t i = 0; i < digits_len; i++) {
                if (digits[i] < '0' || digits[i] > '9')
                    return DMESH_MEMBERSHIP_MALFORMED;
                if (parsed_version > (UINT64_MAX - (uint64_t)(digits[i] - '0')) / 10u)
                    return DMESH_MEMBERSHIP_MALFORMED;
                parsed_version = parsed_version * 10u + (uint64_t)(digits[i] - '0');
            }
            if (parsed_version == 0)
                return DMESH_MEMBERSHIP_MALFORMED;
            have_version = 1;
            continue;
        }
        if (line_len > 7 && memcmp(line, "member=", 7) == 0) {
            if (entry_count >= max_entries)
                return DMESH_MEMBERSHIP_OVERFLOW;
            enum dmesh_membership_result member =
                parse_member(line + 7, line_len - 7, &entries[entry_count]);
            if (member != DMESH_MEMBERSHIP_ADOPTED)
                return member;
            entry_count++;
            continue;
        }
        return DMESH_MEMBERSHIP_MALFORMED;
    }

    if (!have_version)
        return DMESH_MEMBERSHIP_MALFORMED;
    *version = parsed_version;
    *count = entry_count;
    return DMESH_MEMBERSHIP_ADOPTED;
}

int
dmesh_membership_configure(struct objects *objs, char *error, size_t error_len)
{
    const char *path = getenv("DPUMESH_MEMBERSHIP_FILE");

    if (objs == NULL) {
        if (error && error_len)
            snprintf(error, error_len, "membership objects are null");
        return -1;
    }
    objs->membership_path[0] = '\0';
    objs->membership_enabled = 0;
    objs->membership_generation = 0;
    objs->membership_count = 0;
    objs->membership_stamp_ino = 0;
    objs->membership_stamp_sec = 0;
    objs->membership_stamp_nsec = 0;
    objs->membership_stamp_size = 0;
    objs->membership_rejected = 0;
    objs->membership_revocations = 0;

    if (path == NULL || *path == '\0')
        return 0;
    if (strlen(path) >= sizeof(objs->membership_path)) {
        if (error && error_len)
            snprintf(error, error_len, "DPUMESH_MEMBERSHIP_FILE is too long");
        return -1;
    }
    if (objs->feed_key_dir[0] == '\0') {
        if (error && error_len)
            snprintf(error, error_len,
                     "DPUMESH_MEMBERSHIP_FILE needs DPUMESH_FEED_KEY_DIR to verify it");
        return -1;
    }
    snprintf(objs->membership_path, sizeof(objs->membership_path), "%s", path);
    objs->membership_enabled = 1;
    return 0;
}

int
dmesh_admission_configure(struct objects *objs, char *error, size_t error_len)
{
    const char *path = getenv("DPUMESH_ADMISSION_FILE");

    if (objs == NULL)
        return -1;
    objs->admission_path[0] = '\0';
    objs->admission_enabled = 0;
    objs->admission_drain = 0;
    objs->admission_drain_refusals = 0;
    objs->admission_next_check_ns = 0;

    if (path == NULL || *path == '\0')
        return 0;
    if (strlen(path) >= sizeof(objs->admission_path)) {
        if (error && error_len)
            snprintf(error, error_len, "DPUMESH_ADMISSION_FILE is too long");
        return -1;
    }
    snprintf(objs->admission_path, sizeof(objs->admission_path), "%s", path);
    objs->admission_enabled = 1;
    return 0;
}

enum dmesh_membership_result
dmesh_membership_refresh(struct objects *objs)
{
    if (objs == NULL || !objs->membership_enabled)
        return DMESH_MEMBERSHIP_UNCHANGED;

    struct stat st;
    if (stat(objs->membership_path, &st) != 0 || !S_ISREG(st.st_mode) ||
        (size_t)st.st_size > MEMBERSHIP_MAX_BYTES)
        return DMESH_MEMBERSHIP_UNREADABLE;
    /* Skipping the read is an optimization, never a decision: the filesystem
     * reuses inodes across a rename and stamps coarse timestamps, so two
     * generations installed within one tick can share a stamp. A generation is
     * therefore only trusted to be unchanged once it is older than that
     * granularity. */
    struct timespec wall;
    if (clock_gettime(CLOCK_REALTIME, &wall) == 0 &&
        wall.tv_sec - st.st_mtim.tv_sec > MEMBERSHIP_STAMP_SETTLE_SEC &&
        st.st_ino == objs->membership_stamp_ino &&
        st.st_mtim.tv_sec == objs->membership_stamp_sec &&
        st.st_mtim.tv_nsec == objs->membership_stamp_nsec &&
        (uint64_t)st.st_size == objs->membership_stamp_size)
        return DMESH_MEMBERSHIP_UNCHANGED;

    int fd = open(objs->membership_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return DMESH_MEMBERSHIP_UNREADABLE;
    char *document = malloc((size_t)st.st_size + 1u);
    if (document == NULL) {
        close(fd);
        return DMESH_MEMBERSHIP_UNREADABLE;
    }
    ssize_t got = read(fd, document, (size_t)st.st_size);
    close(fd);
    if (got < 0) {
        free(document);
        return DMESH_MEMBERSHIP_UNREADABLE;
    }

    /* Only the signed prefix is ever parsed, so an unsigned generation cannot
     * withdraw membership. */
    size_t signed_length = 0;
    enum dmesh_feed_result signature =
        dmesh_feed_verify(document, (size_t)got, objs->feed_key_dir,
                          &signed_length);
    if (signature != DMESH_FEED_OK) {
        free(document);
        return DMESH_MEMBERSHIP_UNSIGNED;
    }

    uint64_t version = 0;
    size_t count = 0;
    static struct dmesh_membership_entry staged[DMESH_MEMBERSHIP_MAX_ENTRIES];
    enum dmesh_membership_result result =
        dmesh_membership_parse(document, signed_length, &version, staged,
                               DMESH_MEMBERSHIP_MAX_ENTRIES, &count);
    free(document);
    if (result != DMESH_MEMBERSHIP_ADOPTED)
        return result;
    if (version < objs->membership_generation)
        return DMESH_MEMBERSHIP_ROLLBACK;

    /* Only a fully parsed generation is stamped, so a rejected document is
     * re-read until the publisher installs a newer one. */
    objs->membership_stamp_ino = (uint64_t)st.st_ino;
    objs->membership_stamp_sec = st.st_mtim.tv_sec;
    objs->membership_stamp_nsec = st.st_mtim.tv_nsec;
    objs->membership_stamp_size = (uint64_t)st.st_size;
    if (version == objs->membership_generation)
        return DMESH_MEMBERSHIP_UNCHANGED;

    memcpy(objs->membership, staged, count * sizeof(staged[0]));
    objs->membership_count = count;
    objs->membership_generation = version;
    return DMESH_MEMBERSHIP_ADOPTED;
}

int
dmesh_membership_contains(const struct objects *objs, const char *pod_uid,
                          const char *service_name)
{
    if (objs == NULL || pod_uid == NULL || *pod_uid == '\0' ||
        service_name == NULL)
        return 0;
    for (size_t i = 0; i < objs->membership_count; i++)
        if (strncmp(objs->membership[i].service_name, service_name,
                    DMESH_SVC_NAME_MAX) == 0 &&
            strncmp(objs->membership[i].pod_uid, pod_uid,
                    DMESH_POD_UID_MAX) == 0)
            return 1;
    return 0;
}
