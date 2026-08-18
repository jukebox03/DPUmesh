#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/hmac.h>

#include "doca/pod_membership.h"
#include "doca/workload_grant.h"
#include "doca/object.h"

static char key_dir[] = "/tmp/dpumesh-membership-keys-XXXXXX";
static uint8_t feed_key[DMESH_GRANT_KEY_SIZE];

/* The consumer refuses a generation it cannot verify, so every document the
 * test installs carries the same envelope a publisher writes. */
static void
make_keyring(void)
{
    assert(mkdtemp(key_dir) != NULL);
    assert(chmod(key_dir, 0700) == 0);
    for (size_t i = 0; i < sizeof(feed_key); i++)
        feed_key[i] = (uint8_t)(0x31 + i);
    char path[512];
    snprintf(path, sizeof(path), "%s/feed-key-v1.key", key_dir);
    FILE *out = fopen(path, "w");
    assert(out != NULL);
    assert(fwrite(feed_key, 1, sizeof(feed_key), out) == sizeof(feed_key));
    assert(fclose(out) == 0);
    assert(chmod(path, 0400) == 0);
}

static void
sign_into(char *out, size_t cap, const char *body)
{
    uint8_t mac[DMESH_GRANT_MAC_SIZE];
    unsigned int mac_len = 0;
    assert(HMAC(EVP_sha256(), feed_key, sizeof(feed_key),
                (const unsigned char *)body, strlen(body), mac, &mac_len) != NULL);
    char hex[2 * DMESH_GRANT_MAC_SIZE + 1];
    for (size_t i = 0; i < DMESH_GRANT_MAC_SIZE; i++)
        snprintf(hex + 2 * i, 3, "%02x", mac[i]);
    int n = snprintf(out, cap, "%ssignature=feed-key-v1,%s\n", body, hex);
    assert(n > 0 && (size_t)n < cap);
}

static const char UID_A[] = "12345678-1234-1234-1234-123456789abc";
static const char UID_B[] = "abcdef01-2345-6789-abcd-ef0123456789";

static void
write_document(const char *path, const char *body)
{
    char document[1024];
    sign_into(document, sizeof(document), body);
    char temporary[512];
    snprintf(temporary, sizeof(temporary), "%s.new", path);
    FILE *out = fopen(temporary, "w");
    assert(out != NULL);
    assert(fputs(document, out) >= 0);
    assert(fclose(out) == 0);
    /* The publisher installs a generation by rename, which is what makes the
     * consumer's stamp a reliable "already parsed" marker. */
    assert(rename(temporary, path) == 0);
}

static void
test_parse(void)
{
    struct dmesh_membership_entry entries[DMESH_MEMBERSHIP_MAX_ENTRIES];
    uint64_t version = 0;
    size_t count = 0;

    char document[512];
    int n = snprintf(document, sizeof(document),
                     "# node membership\nversion=7\nmember=%s,-1\nmember=%s,11\n",
                     UID_A, UID_A);
    assert(dmesh_membership_parse(document, (size_t)n, &version, entries,
                                  DMESH_MEMBERSHIP_MAX_ENTRIES,
                                  &count) == DMESH_MEMBERSHIP_ADOPTED);
    assert(version == 7 && count == 2);
    assert(strcmp(entries[0].pod_uid, UID_A) == 0 && entries[0].service_id == -1);
    assert(entries[1].service_id == 11);

    /* A document without a generation cannot be ordered against the live one. */
    n = snprintf(document, sizeof(document), "member=%s,-1\n", UID_A);
    assert(dmesh_membership_parse(document, (size_t)n, &version, entries,
                                  DMESH_MEMBERSHIP_MAX_ENTRIES,
                                  &count) == DMESH_MEMBERSHIP_MALFORMED);
    n = snprintf(document, sizeof(document), "version=0\n");
    assert(dmesh_membership_parse(document, (size_t)n, &version, entries,
                                  DMESH_MEMBERSHIP_MAX_ENTRIES,
                                  &count) == DMESH_MEMBERSHIP_MALFORMED);
    n = snprintf(document, sizeof(document), "version=1\nmember=%s,128\n", UID_A);
    assert(dmesh_membership_parse(document, (size_t)n, &version, entries,
                                  DMESH_MEMBERSHIP_MAX_ENTRIES,
                                  &count) == DMESH_MEMBERSHIP_MALFORMED);
    n = snprintf(document, sizeof(document), "version=1\nmember=short,11\n");
    assert(dmesh_membership_parse(document, (size_t)n, &version, entries,
                                  DMESH_MEMBERSHIP_MAX_ENTRIES,
                                  &count) == DMESH_MEMBERSHIP_MALFORMED);
    /* A table that does not fit is refused: a truncated list reads as a
     * withdrawal of everything past the cut. */
    n = snprintf(document, sizeof(document),
                 "version=1\nmember=%s,-1\nmember=%s,-1\n", UID_A, UID_B);
    assert(dmesh_membership_parse(document, (size_t)n, &version, entries, 1,
                                  &count) == DMESH_MEMBERSHIP_OVERFLOW);
}

static void
test_refresh_and_revocation(void)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/dpumesh-membership-test-%d.v1", (int)getpid());
    setenv("DPUMESH_MEMBERSHIP_FILE", path, 1);

    struct objects *objs = calloc(1, sizeof(*objs));
    assert(objs != NULL);
    snprintf(objs->registration_key_dir, sizeof(objs->registration_key_dir),
             "%s", key_dir);
    for (int i = 0; i < POD_ID_SPACE; i++)
        objs->pod_id_to_slot[i] = -1;

    char document[512];
    snprintf(document, sizeof(document),
             "version=100\nmember=%s,-1\nmember=%s,11\n", UID_A, UID_A);
    write_document(path, document);
    assert(dmesh_membership_configure(objs, NULL, 0) == 0);
    assert(objs->membership_enabled == 1);
    assert(dmesh_membership_refresh(objs) == DMESH_MEMBERSHIP_ADOPTED);
    assert(objs->membership_generation == 100 && objs->membership_count == 2);
    assert(dmesh_membership_contains(objs, UID_A, 11));
    assert(!dmesh_membership_contains(objs, UID_A, 12));
    assert(!dmesh_membership_contains(objs, UID_B, -1));

    /* An unchanged generation is not re-read. */
    assert(dmesh_membership_refresh(objs) == DMESH_MEMBERSHIP_UNCHANGED);

    /* A generation older than the live one is refused and does not withdraw
     * the membership already held. */
    snprintf(document, sizeof(document), "version=99\n");
    write_document(path, document);
    assert(dmesh_membership_refresh(objs) == DMESH_MEMBERSHIP_ROLLBACK);
    assert(objs->membership_generation == 100);
    assert(dmesh_membership_contains(objs, UID_A, 11));

    /* An unsigned generation carries no authority, so it is not adopted. */
    {
        char temporary[512];
        snprintf(temporary, sizeof(temporary), "%s.new", path);
        FILE *out = fopen(temporary, "w");
        assert(out != NULL);
        assert(fputs("version=200\n", out) >= 0);
        assert(fclose(out) == 0);
        assert(rename(temporary, path) == 0);
    }
    assert(dmesh_membership_refresh(objs) == DMESH_MEMBERSHIP_UNSIGNED);
    assert(objs->membership_generation == 100);
    assert(dmesh_membership_contains(objs, UID_A, 11));

    /* A missing document never withdraws membership either. */
    assert(unlink(path) == 0);
    assert(dmesh_membership_refresh(objs) == DMESH_MEMBERSHIP_UNREADABLE);
    assert(dmesh_membership_contains(objs, UID_A, 11));

    /* Withdrawal takes two consecutive generations: one absence can be a
     * generation whose snapshot predates the registration. */
    struct pod_state *pod = &objs->pods[0];
    objs->num_pods = 1;
    pod->pod_id = 11;
    pod->service_id = 11;
    snprintf(pod->pod_uid, sizeof(pod->pod_uid), "%s", UID_A);
    pod->registered = 1;
    pod->membership_generation = objs->membership_generation;

    snprintf(document, sizeof(document), "version=101\nmember=%s,-1\n", UID_B);
    write_document(path, document);
    objs->membership_next_check_ns = 0;
    assert(server_progress_membership(objs) == 0);
    assert(pod->membership_absences == 1 && pod->registered == 1);

    snprintf(document, sizeof(document), "version=102\nmember=%s,-1\n", UID_B);
    write_document(path, document);
    objs->membership_next_check_ns = 0;
    assert(server_progress_membership(objs) == 1);
    assert(pod->revoked == 1);
    assert(pod->registered == 0);
    assert(objs->membership_revocations == 1);
    assert(objs->pod_id_to_slot[11] == -1);

    /* A present pair clears the absence rather than accumulating it. */
    struct pod_state *other = &objs->pods[1];
    objs->num_pods = 2;
    other->pod_id = 12;
    other->service_id = -1;
    snprintf(other->pod_uid, sizeof(other->pod_uid), "%s", UID_B);
    other->registered = 1;
    other->membership_generation = objs->membership_generation;
    snprintf(document, sizeof(document), "version=103\nmember=%s,-1\n", UID_B);
    write_document(path, document);
    objs->membership_next_check_ns = 0;
    assert(server_progress_membership(objs) == 0);
    assert(other->membership_absences == 0 && other->registered == 1);

    unlink(path);
    free(objs);
}

int
main(void)
{
    make_keyring();
    test_parse();
    test_refresh_and_revocation();
    char key_path[512];
    snprintf(key_path, sizeof(key_path), "%s/feed-key-v1.key", key_dir);
    assert(unlink(key_path) == 0);
    assert(rmdir(key_dir) == 0);
    puts("pod_membership_test: PASS");
    return 0;
}
