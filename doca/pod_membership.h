#ifndef DMESH_POD_MEMBERSHIP_H
#define DMESH_POD_MEMBERSHIP_H

#include <stddef.h>
#include <stdint.h>

#include "comch_common.h"

struct objects;
struct pod_state;

/* One node's authorized (Pod UID, Service) pairs fit in this table; a larger
 * generation is refused rather than truncated, because a truncated membership
 * list would read as a withdrawal. The bound is the namespace's Pods on this
 * node times the Services they may select, plus one bare pair each. */
#define DMESH_MEMBERSHIP_MAX_ENTRIES 4096
/* A registration is revoked once it has been absent from this many consecutive
 * generations. One absence can be a generation whose snapshot predates the
 * registration, so a single miss is not authority to tear a Pod down. */
#define DMESH_MEMBERSHIP_ABSENCES_TO_REVOKE 2

struct dmesh_membership_entry {
    char pod_uid[DMESH_POD_UID_MAX];
    int32_t service_id;
};

enum dmesh_membership_result {
    DMESH_MEMBERSHIP_UNCHANGED = 0,
    DMESH_MEMBERSHIP_ADOPTED,
    DMESH_MEMBERSHIP_UNREADABLE,
    DMESH_MEMBERSHIP_MALFORMED,
    DMESH_MEMBERSHIP_ROLLBACK,
    DMESH_MEMBERSHIP_OVERFLOW,
    DMESH_MEMBERSHIP_UNSIGNED,
};

const char *dmesh_membership_result_name(enum dmesh_membership_result result);

/* Parse DPUMESH_MEMBERSHIP_FILE. An unset path leaves revocation disabled;
 * required trusted registration is what makes it meaningful. */
int dmesh_membership_configure(struct objects *objs, char *error, size_t error_len);

/* Parse DPUMESH_ADMISSION_FILE. An unset path leaves admission always open. */
int dmesh_admission_configure(struct objects *objs, char *error, size_t error_len);

/* Adopt the current generation if the publisher installed a newer one. Only a
 * strictly newer, completely parsed generation replaces the live table, so a
 * missing, truncated or rolled-back file never withdraws membership. */
enum dmesh_membership_result dmesh_membership_refresh(struct objects *objs);

/* Whether the live generation authorizes this exact pair. */
int dmesh_membership_contains(const struct objects *objs, const char *pod_uid,
                              int32_t service_id);

/* Parse one document into a caller-owned table. Exposed for unit tests. */
enum dmesh_membership_result
dmesh_membership_parse(const char *document, size_t length, uint64_t *version,
                       struct dmesh_membership_entry *entries, size_t max_entries,
                       size_t *count);

#endif /* DMESH_POD_MEMBERSHIP_H */
