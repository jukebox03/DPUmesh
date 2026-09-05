#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>

#include "doca/topology.h"
#include "doca/workload_grant.h"
#include "doca/object.h"

static uint8_t controller_seed[DMESH_GRANT_KEY_SIZE];
static char key_dir[] = "/tmp/dpumesh-topology-keys-XXXXXX";
static char gen_path[256];

static const char UID_A[] = "12345678-1234-1234-1234-123456789abc";
static const char UID_B[] = "abcdef01-2345-6789-abcd-ef0123456789";
static const char NODE_LINE_A[] =
    "node=rapids4,192.168.100.2:4791,node-ed25519-v1,"
    "62b8205a7e8ee63039adca07a1e9aa6d069605ce54648314ee77ca74b457b5bd,"
    "0000000000000000000000000000000000000000000000000000000000000000";

static void
make_keyring(void)
{
    assert(mkdtemp(key_dir) != NULL);
    assert(chmod(key_dir, 0700) == 0);
    for (size_t i = 0; i < sizeof(controller_seed); i++)
        controller_seed[i] = (uint8_t)(0x60 + i);
    uint8_t public_key[DMESH_GRANT_KEY_SIZE];
    assert(dmesh_assert_public_key(controller_seed, public_key) == 0);
    char path[512];
    snprintf(path, sizeof(path), "%s/controller-v1.key", key_dir);
    FILE *out = fopen(path, "w");
    assert(out != NULL);
    for (size_t i = 0; i < sizeof(public_key); i++)
        assert(fprintf(out, "%02x", public_key[i]) == 2);
    assert(fclose(out) == 0);
    assert(chmod(path, 0400) == 0);
}

/* Sign the way the controller publishes: Ed25519 over the whole body, hex128
 * in the trailing envelope. */
static void
sign_generation(char *out, size_t cap, const char *body, const char *key_id)
{
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, NULL, controller_seed, sizeof(controller_seed));
    assert(pkey != NULL);
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    assert(md != NULL);
    uint8_t signature[64];
    size_t sig_len = sizeof(signature);
    assert(EVP_DigestSignInit(md, NULL, NULL, NULL, pkey) == 1);
    assert(EVP_DigestSign(md, signature, &sig_len,
                          (const unsigned char *)body, strlen(body)) == 1);
    assert(sig_len == sizeof(signature));
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    char hex[2 * sizeof(signature) + 1];
    for (size_t i = 0; i < sizeof(signature); i++)
        snprintf(hex + 2 * i, 3, "%02x", signature[i]);
    int n = snprintf(out, cap, "%ssignature=%s,%s\n", body, key_id, hex);
    assert(n > 0 && (size_t)n < cap);
}

static void
install(const char *document)
{
    char temporary[512];
    snprintf(temporary, sizeof(temporary), "%s.new", gen_path);
    FILE *out = fopen(temporary, "w");
    assert(out != NULL);
    assert(fputs(document, out) >= 0);
    assert(fclose(out) == 0);
    assert(rename(temporary, gen_path) == 0);
}

static void
install_signed(const char *body)
{
    static char document[64 * 1024];
    sign_generation(document, sizeof(document), body, "controller-v1");
    install(document);
}

static void
test_parse_refusals(void)
{
    struct dmesh_topology_tables *tables = NULL;
    char body[4096];

    /* Version must be the first non-comment line, nonzero, decimal. */
    assert(dmesh_topology_parse("pod=x\n", 6, NULL, &tables) ==
           DMESH_TOPOLOGY_MALFORMED);
    assert(dmesh_topology_parse("version=0\n", 10, NULL, &tables) ==
           DMESH_TOPOLOGY_MALFORMED);
    /* Comments are permitted only before version=. */
    assert(dmesh_topology_parse("# a\nversion=1\n", 14, NULL, &tables) ==
           DMESH_TOPOLOGY_ADOPTED);
    dmesh_topology_tables_free(tables);
    tables = NULL;
    assert(dmesh_topology_parse("version=1\n# a\n", 14, NULL, &tables) ==
           DMESH_TOPOLOGY_MALFORMED);

    /* An unknown line kind refuses the whole document. */
    int n = snprintf(body, sizeof(body), "version=2\nbogus=1\n");
    assert(dmesh_topology_parse(body, (size_t)n, NULL, &tables) ==
           DMESH_TOPOLOGY_MALFORMED);

    /* A duplicate pod= for one UID refuses the whole document. */
    n = snprintf(body, sizeof(body),
                 "version=2\npod=%s,rapids4,ns-a,default,10.244.0.5\n"
                 "pod=%s,rapids4,ns-a,default,10.244.0.6\n", UID_A, UID_A);
    assert(dmesh_topology_parse(body, (size_t)n, NULL, &tables) ==
           DMESH_TOPOLOGY_MALFORMED);

    /* An endpoint naming an undefined Service or Pod refuses the document. */
    n = snprintf(body, sizeof(body),
                 "version=2\npod=%s,rapids4,ns-a,default,10.244.0.5\n"
                 "endpoint=ns-a/echo,%s\n", UID_A, UID_A);
    assert(dmesh_topology_parse(body, (size_t)n, NULL, &tables) ==
           DMESH_TOPOLOGY_MALFORMED);
    n = snprintf(body, sizeof(body),
                 "version=2\nservice=ns-a/echo,10.96.0.11:9091\n"
                 "endpoint=ns-a/echo,%s\n", UID_A);
    assert(dmesh_topology_parse(body, (size_t)n, NULL, &tables) ==
           DMESH_TOPOLOGY_MALFORMED);

    /* protected= must also name a defined Service: a dangling protection is a
     * typo that would otherwise fail open at enforcement time. */
    n = snprintf(body, sizeof(body), "version=2\nprotected=ns-a/echo\n");
    assert(dmesh_topology_parse(body, (size_t)n, NULL, &tables) ==
           DMESH_TOPOLOGY_MALFORMED);

    /* Field syntax is enforced before anything is adopted. */
    n = snprintf(body, sizeof(body),
                 "version=2\npod=%s,rapids4,ns-a,default,10.244.0.256\n", UID_A);
    assert(dmesh_topology_parse(body, (size_t)n, NULL, &tables) ==
           DMESH_TOPOLOGY_MALFORMED);
    n = snprintf(body, sizeof(body),
                 "version=2\npod=SHOUTY-uid,rapids4,ns-a,default,10.244.0.5\n");
    assert(dmesh_topology_parse(body, (size_t)n, NULL, &tables) ==
           DMESH_TOPOLOGY_MALFORMED);
    n = snprintf(body, sizeof(body), "version=2\nservice=bare-name,10.96.0.11:9091\n");
    assert(dmesh_topology_parse(body, (size_t)n, NULL, &tables) ==
           DMESH_TOPOLOGY_MALFORMED);
    /* Namespace and Service name are DNS labels: a dot is not a qualifier. */
    n = snprintf(body, sizeof(body), "version=2\nservice=ns.a/echo,10.96.0.11:9091\n");
    assert(dmesh_topology_parse(body, (size_t)n, NULL, &tables) ==
           DMESH_TOPOLOGY_MALFORMED);
    n = snprintf(body, sizeof(body), "version=2\nservice=ns-a/echo.v2,10.96.0.11:9091\n");
    assert(dmesh_topology_parse(body, (size_t)n, NULL, &tables) ==
           DMESH_TOPOLOGY_MALFORMED);
    n = snprintf(body, sizeof(body),
                 "version=2\npod=%s,rapids4,ns.a,default,10.244.0.5\n", UID_A);
    assert(dmesh_topology_parse(body, (size_t)n, NULL, &tables) ==
           DMESH_TOPOLOGY_MALFORMED);

    /* Bounds are refused rather than truncated: one node over GEN_NODE_MAX. */
    size_t cap = (DMESH_GEN_NODE_MAX + 2) * (sizeof(NODE_LINE_A) + 16);
    char *big = malloc(cap);
    assert(big != NULL);
    size_t off = (size_t)snprintf(big, cap, "version=2\n");
    for (size_t i = 0; i <= DMESH_GEN_NODE_MAX; i++)
        off += (size_t)snprintf(big + off, cap - off,
                                "node=n%zu,10.0.0.1:4791,k,"
                                "62b8205a7e8ee63039adca07a1e9aa6d069605ce54648314ee77ca74b457b5bd,"
                                "62b8205a7e8ee63039adca07a1e9aa6d069605ce54648314ee77ca74b457b5bd\n",
                                i);
    assert(dmesh_topology_parse(big, off, NULL, &tables) ==
           DMESH_TOPOLOGY_OVERFLOW);
    free(big);
}

int
main(void)
{
    make_keyring();
    snprintf(gen_path, sizeof(gen_path), "/tmp/dpumesh-topology-test-%d.gen",
             (int)getpid());
    setenv("DPUMESH_TOPOLOGY_FILE", gen_path, 1);
    setenv("DPUMESH_CONTROLLER_KEY_DIR", key_dir, 1);

    test_parse_refusals();

    struct objects *objs = calloc(1, sizeof(*objs));
    assert(objs != NULL);

    char body[8192];
    int n = snprintf(body, sizeof(body),
                     "# cluster topology\n"
                     "version=100\n"
                     "%s\n"
                     "service=ns-a/echo,10.96.0.11:9091\n"
                     "service=ns-a/bench,10.96.0.12:9091\n"
                     "pod=%s,rapids4,ns-a,default,10.244.0.5\n"
                     "pod=%s,worker-2,ns-a,default,10.244.1.6\n"
                     "endpoint=ns-a/echo,%s\n"
                     "endpoint=ns-a/echo,%s\n"
                     "protected=ns-a/echo\n",
                     NODE_LINE_A, UID_A, UID_B, UID_A, UID_B);
    assert(n > 0 && (size_t)n < sizeof(body));
    install_signed(body);

    assert(dmesh_topology_configure(objs, NULL, 0) == 0);
    assert(objs->topology.enabled == 1);
    assert(dmesh_topology_refresh(objs) == DMESH_TOPOLOGY_ADOPTED);
    assert(objs->topology.tables->version == 100);
    assert(objs->topology.tables->node_count == 1);
    assert(objs->topology.tables->pod_count == 2);
    assert(objs->topology.tables->service_count == 2);
    assert(objs->topology.tables->endpoint_count == 2);

    const struct dmesh_gen_pod *pod = dmesh_topology_pod(objs, UID_A);
    assert(pod != NULL && strcmp(pod->node_name, "rapids4") == 0 &&
           strcmp(pod->service_account, "default") == 0);
    assert(dmesh_topology_pod(objs, "00000000-0000-0000-0000-000000000000") ==
           NULL);
    const struct dmesh_gen_service *service =
        dmesh_topology_service(objs, "ns-a/echo");
    assert(service != NULL && service->is_protected == 1 &&
           service->endpoint_count == 2 && service->port == 9091);
    assert(dmesh_topology_service(objs, "ns-a/bench")->is_protected == 0);
    int echo_id = dmesh_topology_interned_id(objs, "ns-a/echo");
    int bench_id = dmesh_topology_interned_id(objs, "ns-a/bench");
    assert(echo_id >= 0 && bench_id >= 0 && echo_id != bench_id);

    /* The generation resolves this node's grant key by key id. */
    const uint8_t *grant_key = NULL;
    assert(dmesh_topology_grant_key(objs, "rapids4", "node-ed25519-v1",
                                    &grant_key) == 1);
    assert(grant_key != NULL && grant_key[0] == 0x62);
    assert(dmesh_topology_grant_key(objs, "rapids4", "unknown-key",
                                    &grant_key) == 1);
    assert(grant_key == NULL);
    assert(dmesh_topology_grant_key(objs, "worker-9", "node-ed25519-v1",
                                    &grant_key) == 0);

    /* Reach: a Service with replicas elsewhere is routable across the boundary
     * rather than unroutable. The local half stays this node's own live
     * registrations, so only the remote endpoints come from here. */
    struct dmesh_endpoint_ref remote[8];
    snprintf(objs->node_name, sizeof(objs->node_name), "rapids4");
    int n_remote = dmesh_topology_remote_endpoints(objs, (int16_t)echo_id,
                                                   objs->node_name, remote, 8);
    assert(n_remote == 1);
    assert(strcmp(remote[0].pod_uid, UID_B) == 0);
    assert(strcmp(remote[0].node_name, "worker-2") == 0);
    /* Asked from the other node, the split is the mirror image. */
    assert(dmesh_topology_remote_endpoints(objs, (int16_t)echo_id, "worker-2",
                                           remote, 8) == 1);
    assert(strcmp(remote[0].pod_uid, UID_A) == 0);
    /* A Service the generation names with no endpoints has no remote half. */
    assert(dmesh_topology_remote_endpoints(objs, (int16_t)bench_id, "rapids4",
                                           remote, 8) == 0);

    /* The identity check a destination applies to a peer's claim: a lookup in
     * the signed table, and nothing the peer says. */
    assert(dmesh_topology_pod_on_node(objs, UID_B, "worker-2") == 1);
    assert(dmesh_topology_pod_on_node(objs, UID_B, "rapids4") == 0);
    assert(dmesh_topology_pod_on_node(objs, "00000000-0000-0000-0000-000000000000",
                                      "worker-2") == 0);

    /* A node whose DPU has not reported a static key carries the all-zero
     * placeholder, which binds nothing: it is refused here rather than at the
     * handshake, where a zero key would look like a key. */
    const uint8_t *static_key = NULL;
    uint32_t peer_ip = 0;
    uint16_t peer_port = 0;
    assert(dmesh_topology_node_peer(objs, "rapids4", &static_key, &peer_ip,
                                    &peer_port) == 0);
    assert(dmesh_topology_node_peer(objs, "worker-9", &static_key, &peer_ip,
                                    &peer_port) == 0);

    /* A republished document carrying the held version is read, verified and
     * parsed, and adopts nothing: the countable "unchanged" outcome, distinct
     * from a file the stamp says was never touched. */
    assert(dmesh_topology_refresh(objs) == DMESH_TOPOLOGY_SAME_VERSION);

    /* A rolled-back generation changes nothing. */
    n = snprintf(body, sizeof(body), "version=99\n%s\n", NODE_LINE_A);
    install_signed(body);
    assert(dmesh_topology_refresh(objs) == DMESH_TOPOLOGY_ROLLBACK);
    assert(objs->topology.tables->version == 100);

    /* An unsigned generation carries no authority. */
    install("version=200\n");
    assert(dmesh_topology_refresh(objs) == DMESH_TOPOLOGY_UNSIGNED);
    assert(objs->topology.tables->version == 100);

    /* A generation signed with a key the directory does not hold is refused,
     * and one whose body was altered after signing fails the signature. */
    {
        static char document[64 * 1024];
        sign_generation(document, sizeof(document), "version=201\n",
                        "other-key");
        install(document);
        assert(dmesh_topology_refresh(objs) == DMESH_TOPOLOGY_BAD_KEY_ID);
        sign_generation(document, sizeof(document), "version=202\n",
                        "controller-v1");
        document[8] = '3';   /* version=202 -> version=302, after signing */
        install(document);
        assert(dmesh_topology_refresh(objs) == DMESH_TOPOLOGY_BAD_SIG);
        assert(objs->topology.tables->version == 100);
    }

    /* A malformed newer generation changes nothing, including interning. */
    n = snprintf(body, sizeof(body), "version=300\nbogus=1\n");
    install_signed(body);
    assert(dmesh_topology_refresh(objs) == DMESH_TOPOLOGY_MALFORMED);
    assert(objs->topology.tables->version == 100);
    assert(dmesh_topology_interned_id(objs, "ns-a/echo") == echo_id);

    /* Interning is stable across adoptions while a Service persists; a
     * departed Service frees its id and a new one takes a free id. */
    n = snprintf(body, sizeof(body),
                 "version=400\n%s\n"
                 "service=ns-a/echo,10.96.0.11:9091\n"
                 "service=ns-a/fresh,10.96.0.13:9091\n",
                 NODE_LINE_A);
    install_signed(body);
    assert(dmesh_topology_refresh(objs) == DMESH_TOPOLOGY_ADOPTED);
    assert(dmesh_topology_interned_id(objs, "ns-a/echo") == echo_id);
    int fresh_id = dmesh_topology_interned_id(objs, "ns-a/fresh");
    assert(fresh_id == bench_id);   /* the freed id is the lowest free one */
    assert(dmesh_topology_service(objs, "ns-a/bench") == NULL);

    /* Once a node's DPU has generated a credential and its runtime has reported
     * it, the generation binds a key and a transport address, and that pair is
     * what a handshake is checked against. */
    n = snprintf(body, sizeof(body),
                 "version=500\n"
                 "node=worker-2,192.168.100.9:4791,node-ed25519-v1,"
                 "62b8205a7e8ee63039adca07a1e9aa6d069605ce54648314ee77ca74b457b5bd,"
                 "aabbccddeeff00112233445566778899aabbccddeeff001122334455667788aa\n");
    assert(n > 0 && (size_t)n < sizeof(body));
    install_signed(body);
    assert(dmesh_topology_refresh(objs) == DMESH_TOPOLOGY_ADOPTED);
    assert(dmesh_topology_node_peer(objs, "worker-2", &static_key, &peer_ip,
                                    &peer_port) == 1);
    assert(static_key != NULL && static_key[0] == 0xaa && static_key[31] == 0xaa);
    assert(peer_port == 4791);

    /* A document over the byte bound is refused as overflow before it is
     * read, and withdraws nothing. */
    {
        char temporary[512];
        snprintf(temporary, sizeof(temporary), "%s.new", gen_path);
        FILE *out = fopen(temporary, "w");
        assert(out != NULL);
        assert(fputs("version=600\n", out) >= 0);
        assert(fseek(out, (long)DMESH_TOPOLOGY_MAX_BYTES, SEEK_SET) == 0);
        assert(fputc('\n', out) != EOF);
        assert(fclose(out) == 0);
        assert(rename(temporary, gen_path) == 0);
    }
    assert(dmesh_topology_refresh(objs) == DMESH_TOPOLOGY_OVERFLOW);
    assert(objs->topology.tables->version == 500);

    /* A missing document never withdraws the held generation. */
    assert(unlink(gen_path) == 0);
    assert(dmesh_topology_refresh(objs) == DMESH_TOPOLOGY_UNREADABLE);
    assert(objs->topology.tables->version == 500);

    dmesh_topology_tables_free(objs->topology.tables);
    dmesh_topology_tables_free(objs->topology.retired);
    free(objs);
    char key_path[512];
    snprintf(key_path, sizeof(key_path), "%s/controller-v1.key", key_dir);
    assert(unlink(key_path) == 0);
    assert(rmdir(key_dir) == 0);
    puts("topology_gen_test: PASS");
    return 0;
}
