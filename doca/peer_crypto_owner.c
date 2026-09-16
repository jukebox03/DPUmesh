/* dpumesh_crypto_owner: the one process on a node that holds the NIC's inline
 * IPsec state for DPUmesh Pod pairs.
 *
 * It opens the uplink PF and the runtime's SF representor in DOCA Flow switch
 * mode, keeps the existing OVS/TC path as the miss target for every packet it
 * does not own, and installs per-association SAs and rules on request from the
 * runtime's crypto manager over a UNIX socket (peer_crypto_ipc). It never
 * forwards a protected RoCE packet in plaintext: a RoCE packet from the SF to
 * a peer without a TX rule drops, an ESP packet without an RX rule drops, and a
 * plaintext RoCE packet from the wire to this node's fabric address drops.
 *
 * Graph on the switch port (S = SF representor, P = uplink PF):
 *
 *   ingress root  S, local->peer, UDP  -> tx_select: dst peer + dest QPN -> meta{encrypt, sa} -> egress root
 *                 P, peer->local, ESP  -> decrypt:   dst local + SPI      -> decrypt(sa), meta{decrypt, sa} -> decap -> egress root
 *                 P, peer->local, UDP  -> rx_plain:  UDP/4791 -> DROP, else kernel
 *                 miss                 -> kernel (existing OVS/TC)
 *   egress root   meta{encrypt}, local->peer -> encrypt: meta sa -> ESP encap+encrypt(sa) -> port P
 *                 meta{decrypt}, peer->local -> rx_gate: meta sa + dest QPN -> port S, miss DROP
 *
 * One SA per direction per epoch; the lanes of one association share it. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <doca_bitfield.h>
#include <doca_dev.h>
#include <doca_dpdk.h>
#include <doca_flow.h>
#include <doca_flow_net.h>
#include <doca_log.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_errno.h>
#include <rte_mbuf.h>
#include <openssl/crypto.h>
#include "peer_crypto_ipc.h"

DOCA_LOG_REGISTER(DPUMESH_CRYPTO_OWNER);

#ifndef DMESH_DOCA_MAJOR
#define DMESH_DOCA_MAJOR 3u
#endif
#ifndef DMESH_DOCA_MINOR
#ifdef DMESH_DOCA35
#define DMESH_DOCA_MINOR 5u
#else
#define DMESH_DOCA_MINOR 1u
#endif
#endif

#ifdef DMESH_DOCA35
#define FLAG_NO_WAIT DOCA_FLOW_ENTRY_FLAGS_NO_WAIT
#define ADD_ENTRY(pipe, match, actions, fwd, ctx, entry) \
    doca_flow_pipe_basic_add_entry(0, pipe, match, 0, actions, NULL, fwd, FLAG_NO_WAIT, ctx, entry)
#define SA_SET_CFG(port, id, cfg) doca_flow_port_shared_resource_set_cfg(port, DOCA_FLOW_SHARED_RESOURCE_IPSEC_SA, id, cfg)
#define CONTROL_ADD(priority, pipe, match, mask, fwd, ctx, entry) \
    doca_flow_pipe_control_add_entry(0, pipe, match, mask, NULL, NULL, NULL, NULL, NULL, priority, fwd, ctx, entry)
#else
#define FLAG_NO_WAIT DOCA_FLOW_NO_WAIT
#define ADD_ENTRY(pipe, match, actions, fwd, ctx, entry) \
    doca_flow_pipe_add_entry(0, pipe, match, actions, NULL, fwd, FLAG_NO_WAIT, ctx, entry)
#define SA_SET_CFG(port, id, cfg) doca_flow_shared_resource_set_cfg(DOCA_FLOW_SHARED_RESOURCE_IPSEC_SA, id, cfg)
#define CONTROL_ADD(priority, pipe, match, mask, fwd, ctx, entry) \
    doca_flow_pipe_control_add_entry(0, priority, pipe, match, mask, NULL, NULL, NULL, NULL, NULL, fwd, ctx, entry)
#endif

#define ASSOC_MAX 256u
#define PEERS_MAX 64u
#define SA_DEFAULT 4096u
#define ROCE_PORT 4791u
#define PROCESS_TIMEOUT_US 10000
#define CLIENT_RX_MAX (PEER_CRYPTO_IPC_FRAME_MAX * 4)

/* meta.pkt_meta layout shared by every rule: two path bits and the SA id. */
union pkt_meta {
    uint32_t u32;
    struct { uint32_t encrypt : 1, decrypt : 1, rsvd : 10, sa : 20; };
};
static uint32_t meta_be(int encrypt, int decrypt, uint32_t sa)
{ union pkt_meta m = {0}; m.encrypt = encrypt; m.decrypt = decrypt; m.sa = sa; return DOCA_HTOBE32(m.u32); }

struct epoch {
    int used;
    uint64_t epoch;
    uint32_t rx_sa, tx_sa;
    int rx_set, tx_set;
    struct doca_flow_pipe_entry *decrypt, *encrypt;
    struct doca_flow_pipe_entry *gate[PEER_ASSOC_LANES_MAX], *select[PEER_ASSOC_LANES_MAX];
    unsigned lanes;
    uint32_t local_qpn[PEER_ASSOC_LANES_MAX], remote_qpn[PEER_ASSOC_LANES_MAX];
};
struct peer {
    int used;
    uint32_t local_ip, peer_ip;   /* network order */
    unsigned refs;
    struct doca_flow_pipe_entry *root[3], *egress[2];
};
struct assoc {
    int used;
    uint8_t session[16], association[16];
    unsigned peer;
    struct epoch ep[2];
};
struct owner {
    /* configuration */
    char socket_path[108], pci[64], sf_iface[64], ovs_bridge[64];
    unsigned sa_limit;
    /* devices */
    struct doca_dev *dev; struct doca_dev_rep *rep;
    uint16_t pf_port_id, sf_port_id;
    struct rte_mempool *pool;
    struct doca_flow_port *pf_port, *sf_port, *sw;
    struct doca_flow_target *kernel;
    struct doca_flow_pipe *kernel_pipe, *rx_plain, *encrypt_pipe, *rx_gate, *egress_root;
    struct doca_flow_pipe *decap_pipe, *decrypt_pipe, *tx_select, *ingress_root;
    struct doca_flow_pipe_entry *kernel_entry, *rx_plain_entry, *rx_plain_catch, *decap_entry;
    /* The one entry operation in flight: completions for any other entry or
     * operation are stale and ignored, never counted against this one. */
    struct doca_flow_pipe_entry *await_entry;
    enum doca_flow_entry_op await_op;
    int await_any, await_done, await_failed;
    enum doca_flow_entry_status await_status;
    /* state: SA ids are never reused within one owner lifetime, because
     * reconfiguring a shared resource the device already holds leaves the port
     * unable to stop; exhaustion refuses new installs until a restart. */
    uint32_t sa_next;
    struct peer peers[PEERS_MAX];
    struct assoc assocs[ASSOC_MAX];
    int eal, flow;
    /* IPC */
    int listen_fd, client_fd;
    uint8_t rx[CLIENT_RX_MAX]; size_t rx_len;
};
static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }
/* The graceful DOCA teardown did not finish in time. The unsafe flow entries
 * are already gone, so the datapath is fail-closed; exit success rather than
 * hang. Async-signal-safe: write then _exit. */
static void on_teardown_timeout(int sig)
{
    (void)sig;
    static const char msg[] = "crypto owner: teardown watchdog fired; fail-closed exit\n";
    ssize_t n = write(STDERR_FILENO, msg, sizeof(msg) - 1); (void)n;
    _exit(0);
}

static void on_entry(struct doca_flow_pipe_entry *entry, uint16_t queue, enum doca_flow_entry_status status,
                     enum doca_flow_entry_op op, void *ctx)
{
    (void)queue;
    struct owner *o = ctx;
    if (!o || op != o->await_op || (!o->await_any && entry != o->await_entry)) {
        DOCA_LOG_WARN("stray completion entry %p op %d status %d", (void *)entry, (int)op, (int)status);
        return;
    }
    o->await_done = 1; o->await_failed = status != DOCA_FLOW_ENTRY_STATUS_SUCCESS; o->await_status = status;
}
/* Whatever the device still has queued: completions nobody awaits (logged as
 * stray) must not stay behind, or the port cannot stop. */
static void drain(struct owner *o, const char *why)
{
    o->await_any = 0; o->await_entry = NULL;
    for (unsigned n = 0; n < 50; n++) {
        doca_error_t r = doca_flow_entries_process(o->sw, 0, PROCESS_TIMEOUT_US, 64);
        if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("drain %s: %s", why, doca_error_get_descr(r)); return; }
    }
}
/* Some entries complete inside the call that issues them (control pipes), the
 * rest through entries_process: accept either, for this operation only. */
static void begin_await(struct owner *o, enum doca_flow_entry_op op)
{ o->await_any = 1; o->await_entry = NULL; o->await_op = op; o->await_done = o->await_failed = 0; }
/* 0 completed, -1 the device refused it, -2 the device did not answer. */
static int end_await(struct owner *o, struct doca_flow_pipe_entry *entry)
{
    o->await_any = 0; o->await_entry = entry;
    for (unsigned n = 0; n < 200 && !o->await_done; n++) {
        doca_error_t r = doca_flow_entries_process(o->sw, 0, PROCESS_TIMEOUT_US, 8);
        if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("entries_process: %s", doca_error_get_descr(r)); break; }
    }
    int rc = !o->await_done ? -2 : o->await_failed ? -1 : 0;
    if (rc == -1) DOCA_LOG_ERR("entry %p op %d status %d", (void *)entry, (int)o->await_op, (int)o->await_status);
    o->await_entry = NULL; o->await_done = o->await_failed = 0;
    return rc;
}
static int add(struct owner *o, struct doca_flow_pipe *pipe, const struct doca_flow_match *match,
               const struct doca_flow_actions *actions, const struct doca_flow_fwd *fwd,
               struct doca_flow_pipe_entry **entry)
{
    begin_await(o, DOCA_FLOW_ENTRY_OP_ADD);
    doca_error_t r = ADD_ENTRY(pipe, match, actions, fwd, o, entry);
    if (r != DOCA_SUCCESS) { o->await_any = 0; DOCA_LOG_ERR("add entry: %s", doca_error_get_descr(r)); *entry = NULL; return -1; }
    int rc = end_await(o, *entry);
    if (rc) { DOCA_LOG_ERR("add entry completion %d", rc); if (rc < -1) *entry = NULL; }
    return rc;
}
/* Once the device accepted the removal the pointer is dead whatever the
 * completion says; it is never retried. */
static int remove_entry(struct owner *o, struct doca_flow_pipe_entry **entry)
{
    if (!*entry) return 0;
    begin_await(o, DOCA_FLOW_ENTRY_OP_DEL);
    doca_error_t r = doca_flow_pipe_remove_entry(0, FLAG_NO_WAIT, *entry);
    if (r != DOCA_SUCCESS) { o->await_any = 0; DOCA_LOG_ERR("remove entry: %s", doca_error_get_descr(r)); return -2; }
    int rc = end_await(o, *entry);
    if (rc) DOCA_LOG_ERR("remove entry completion %d", rc);
    *entry = NULL;
    return rc;
}

/* ---- pipes ------------------------------------------------------------- */

static doca_error_t pipe_cfg(struct owner *o, const char *name, int root, enum doca_flow_pipe_domain domain,
                             unsigned entries, struct doca_flow_pipe_cfg **out)
{
    struct doca_flow_pipe_cfg *cfg;
    doca_error_t r = doca_flow_pipe_cfg_create(&cfg, o->sw);
    if (r != DOCA_SUCCESS) return r;
    if ((r = doca_flow_pipe_cfg_set_name(cfg, name)) != DOCA_SUCCESS ||
        (r = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC)) != DOCA_SUCCESS ||
        (r = doca_flow_pipe_cfg_set_is_root(cfg, root)) != DOCA_SUCCESS ||
        (r = doca_flow_pipe_cfg_set_domain(cfg, domain)) != DOCA_SUCCESS ||
        (r = doca_flow_pipe_cfg_set_nr_entries(cfg, entries)) != DOCA_SUCCESS) {
        doca_flow_pipe_cfg_destroy(cfg); return r;
    }
    *out = cfg; return DOCA_SUCCESS;
}
static void roce_match(struct doca_flow_match *m, int changeable_qpn)
{
    m->outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_ROCE_V2;
    m->outer.roce_v2.udp.l4_port.dst_port = htons(ROCE_PORT);
    if (changeable_qpn) memset(m->outer.roce_v2.bth.dest_qp, 0xff, 3);
}
/* The mask a pipe declares next to its match: without one, meta is not part of
 * the key at all and two entries that differ only in SA collapse into one. */
static void roce_mask(struct doca_flow_match *m)
{
    m->outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
    m->outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_ROCE_V2;
    m->outer.roce_v2.udp.l4_port.dst_port = 0xffff;
    memset(m->outer.roce_v2.bth.dest_qp, 0xff, 3);
}
static void qpn_bytes(uint8_t out[3], uint32_t qpn) { out[0] = qpn >> 16; out[1] = qpn >> 8; out[2] = qpn; }

#define TRY(call) do { doca_error_t _r = (call); if (_r != DOCA_SUCCESS) { \
    DOCA_LOG_ERR("%s: %s", #call, doca_error_get_descr(_r)); if (cfg) doca_flow_pipe_cfg_destroy(cfg); return -1; } } while (0)

static int build_kernel_pipe(struct owner *o)
{
    struct doca_flow_pipe_cfg *cfg = NULL;
    struct doca_flow_match match = {0};
    struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_TARGET, .target = o->kernel};
    TRY(pipe_cfg(o, "DMESH_KERNEL", 0, DOCA_FLOW_PIPE_DOMAIN_DEFAULT, 1, &cfg));
    TRY(doca_flow_pipe_cfg_set_match(cfg, &match, NULL));
    TRY(doca_flow_pipe_create(cfg, &fwd, NULL, &o->kernel_pipe));
    doca_flow_pipe_cfg_destroy(cfg); cfg = NULL;
    return add(o, o->kernel_pipe, &match, NULL, NULL, &o->kernel_entry);
}
/* Plaintext RoCE from the wire to this node drops; every other packet on the
 * same root entry keeps the kernel path. A control pipe carries the two
 * differently shaped matches with explicit priorities. */
static int build_rx_plain(struct owner *o)
{
    struct doca_flow_pipe_cfg *cfg = NULL;
    struct doca_flow_match match = {0}, mask = {0};
    struct doca_flow_fwd drop = {.type = DOCA_FLOW_FWD_DROP};
    struct doca_flow_fwd kernel = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = o->kernel_pipe};
    TRY(doca_flow_pipe_cfg_create(&cfg, o->sw));
    TRY(doca_flow_pipe_cfg_set_name(cfg, "DMESH_RX_PLAIN"));
    TRY(doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_CONTROL));
    TRY(doca_flow_pipe_cfg_set_is_root(cfg, false));
    TRY(doca_flow_pipe_create(cfg, NULL, NULL, &o->rx_plain));
    doca_flow_pipe_cfg_destroy(cfg); cfg = NULL;
    match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4; mask.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
    match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP; mask.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
    match.outer.udp.l4_port.dst_port = htons(ROCE_PORT); mask.outer.udp.l4_port.dst_port = 0xffff;
    begin_await(o, DOCA_FLOW_ENTRY_OP_ADD);
    doca_error_t r = CONTROL_ADD(0, o->rx_plain, &match, &mask, &drop, o, &o->rx_plain_entry);
    if (r != DOCA_SUCCESS) { o->await_any = 0; DOCA_LOG_ERR("rx_plain drop entry: %s", doca_error_get_descr(r)); return -1; }
    if (end_await(o, o->rx_plain_entry)) { DOCA_LOG_ERR("rx_plain drop entry completion"); return -1; }
    memset(&match, 0, sizeof(match));
    begin_await(o, DOCA_FLOW_ENTRY_OP_ADD);
    r = CONTROL_ADD(1, o->rx_plain, &match, NULL, &kernel, o, &o->rx_plain_catch);
    if (r != DOCA_SUCCESS) { o->await_any = 0; DOCA_LOG_ERR("rx_plain kernel entry: %s", doca_error_get_descr(r)); return -1; }
    if (end_await(o, o->rx_plain_catch)) { DOCA_LOG_ERR("rx_plain kernel entry completion"); return -1; }
    return 0;
}
static int build_encrypt(struct owner *o)
{
    struct doca_flow_pipe_cfg *cfg = NULL;
    struct doca_flow_match match = {0}, mask = {0};
    struct doca_flow_actions actions = {0}, *list[1] = {&actions};
    struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = o->pf_port_id};
    mask.meta.pkt_meta = meta_be(0, 0, 0xfffff); match.meta.pkt_meta = 0xffffffff;
    actions.has_crypto_encap = true;
    actions.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_ENCAP;
    actions.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_ESP_OVER_IPV4;
    actions.crypto_encap.icv_size = 16;
    memset(actions.crypto_encap.encap_data, 0xff, 16); actions.crypto_encap.data_size = 16;
    actions.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_ENCRYPT;
    actions.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_IPSEC_SA;
    actions.crypto.ipsec_sa.sn_en = true;
    actions.crypto.crypto_id = UINT32_MAX;
    TRY(pipe_cfg(o, "DMESH_ENCRYPT", 0, DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS, o->sa_limit, &cfg));
    TRY(doca_flow_pipe_cfg_set_match(cfg, &match, &mask));
    TRY(doca_flow_pipe_cfg_set_actions(cfg, list, NULL, NULL, 1));
    TRY(doca_flow_pipe_create(cfg, &fwd, NULL, &o->encrypt_pipe));
    doca_flow_pipe_cfg_destroy(cfg); return 0;
}
static int build_rx_gate(struct owner *o)
{
    struct doca_flow_pipe_cfg *cfg = NULL;
    struct doca_flow_match match = {0}, mask = {0};
    struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PORT, .port_id = o->sf_port_id};
    struct doca_flow_fwd drop = {.type = DOCA_FLOW_FWD_DROP};
    match.meta.pkt_meta = UINT32_MAX; mask.meta.pkt_meta = UINT32_MAX;
    match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
    match.outer.ip4.src_ip = UINT32_MAX; match.outer.ip4.dst_ip = UINT32_MAX;
    mask.outer.ip4.src_ip = UINT32_MAX; mask.outer.ip4.dst_ip = UINT32_MAX;
    roce_match(&match, 1); roce_mask(&mask);
    TRY(pipe_cfg(o, "DMESH_RX_SA_QPN", 0, DOCA_FLOW_PIPE_DOMAIN_EGRESS, o->sa_limit * PEER_ASSOC_LANES_MAX, &cfg));
    TRY(doca_flow_pipe_cfg_set_match(cfg, &match, &mask));
    TRY(doca_flow_pipe_create(cfg, &fwd, &drop, &o->rx_gate));
    doca_flow_pipe_cfg_destroy(cfg); return 0;
}
static int build_egress_root(struct owner *o)
{
    struct doca_flow_pipe_cfg *cfg = NULL;
    struct doca_flow_match match = {0}, mask = {0};
    struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE};
    mask.meta.pkt_meta = meta_be(1, 1, 0); match.meta.pkt_meta = UINT32_MAX;
    match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4; mask.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
    match.outer.ip4.src_ip = UINT32_MAX; match.outer.ip4.dst_ip = UINT32_MAX;
    mask.outer.ip4.src_ip = UINT32_MAX; mask.outer.ip4.dst_ip = UINT32_MAX;
    TRY(pipe_cfg(o, "DMESH_EGRESS_ROOT", 1, DOCA_FLOW_PIPE_DOMAIN_EGRESS, PEERS_MAX * 2, &cfg));
    TRY(doca_flow_pipe_cfg_set_match(cfg, &match, &mask));
    TRY(doca_flow_pipe_create(cfg, &fwd, NULL, &o->egress_root));
    doca_flow_pipe_cfg_destroy(cfg); return 0;
}
static int build_decap(struct owner *o)
{
    struct doca_flow_pipe_cfg *cfg = NULL;
    struct doca_flow_match match = {0}, mask = {0};
    struct doca_flow_actions actions = {0}, *list[1] = {&actions};
    struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = o->egress_root};
    struct doca_flow_fwd drop = {.type = DOCA_FLOW_FWD_DROP};
    /* Only a clean decrypt (both syndromes zero) continues; anything else drops. */
    mask.parser_meta.ipsec_syndrome = 0xff; mask.parser_meta.ipsec_ar_syndrome = 0xff;
    match.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT32_MAX;
    mask.parser_meta.outer_l3_type = (enum doca_flow_l3_meta)UINT32_MAX;
    mask.meta.pkt_meta = meta_be(0, 1, 0); match.meta.pkt_meta = UINT32_MAX;
    actions.has_crypto_encap = true;
    actions.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_DECAP;
    actions.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_ESP_OVER_IPV4;
    actions.crypto_encap.icv_size = 16;
    TRY(pipe_cfg(o, "DMESH_DECAP", 0, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS, 1, &cfg));
    TRY(doca_flow_pipe_cfg_set_match(cfg, &match, &mask));
    TRY(doca_flow_pipe_cfg_set_actions(cfg, list, NULL, NULL, 1));
    TRY(doca_flow_pipe_create(cfg, &fwd, &drop, &o->decap_pipe));
    doca_flow_pipe_cfg_destroy(cfg); cfg = NULL;
    memset(&match, 0, sizeof(match));
    match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
    match.meta.pkt_meta = meta_be(0, 1, 0);
    struct doca_flow_actions entry_actions = {0};
    return add(o, o->decap_pipe, &match, &entry_actions, NULL, &o->decap_entry);
}
static int build_decrypt(struct owner *o)
{
    struct doca_flow_pipe_cfg *cfg = NULL;
    struct doca_flow_match match = {0};
    struct doca_flow_actions actions = {0}, *list[1] = {&actions};
    struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = o->decap_pipe};
    struct doca_flow_fwd drop = {.type = DOCA_FLOW_FWD_DROP};
    match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4; match.outer.ip4.dst_ip = UINT32_MAX;
    match.tun.type = DOCA_FLOW_TUN_ESP; match.tun.esp_spi = UINT32_MAX;
    actions.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_DECRYPT;
    actions.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_IPSEC_SA;
    actions.crypto.ipsec_sa.sn_en = true;
    actions.crypto.crypto_id = UINT32_MAX;
    actions.meta.pkt_meta = UINT32_MAX;
    TRY(pipe_cfg(o, "DMESH_DECRYPT", 0, DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS, o->sa_limit, &cfg));
    TRY(doca_flow_pipe_cfg_set_match(cfg, &match, NULL));
    TRY(doca_flow_pipe_cfg_set_actions(cfg, list, NULL, NULL, 1));
    TRY(doca_flow_pipe_create(cfg, &fwd, &drop, &o->decrypt_pipe));
    doca_flow_pipe_cfg_destroy(cfg); return 0;
}
static int build_tx_select(struct owner *o)
{
    struct doca_flow_pipe_cfg *cfg = NULL;
    struct doca_flow_match match = {0}, mask = {0};
    struct doca_flow_actions actions = {0}, *list[1] = {&actions};
    struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = o->egress_root};
    struct doca_flow_fwd drop = {.type = DOCA_FLOW_FWD_DROP};
    match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4; match.outer.ip4.dst_ip = UINT32_MAX;
    mask.outer.ip4.dst_ip = UINT32_MAX;
    roce_match(&match, 1); roce_mask(&mask);
    actions.meta.pkt_meta = UINT32_MAX;
    TRY(pipe_cfg(o, "DMESH_TX_SELECT", 0, DOCA_FLOW_PIPE_DOMAIN_DEFAULT, o->sa_limit * PEER_ASSOC_LANES_MAX, &cfg));
    TRY(doca_flow_pipe_cfg_set_match(cfg, &match, &mask));
    TRY(doca_flow_pipe_cfg_set_actions(cfg, list, NULL, NULL, 1));
    TRY(doca_flow_pipe_create(cfg, &fwd, &drop, &o->tx_select));
    doca_flow_pipe_cfg_destroy(cfg); return 0;
}
static int build_ingress_root(struct owner *o)
{
    struct doca_flow_pipe_cfg *cfg = NULL;
    struct doca_flow_match match = {0};
    struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_CHANGEABLE};
    match.parser_meta.port_id = UINT16_MAX;
    match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
    match.outer.ip4.src_ip = UINT32_MAX; match.outer.ip4.dst_ip = UINT32_MAX;
    match.outer.ip4.next_proto = UINT8_MAX;
    TRY(pipe_cfg(o, "DMESH_INGRESS_ROOT", 1, DOCA_FLOW_PIPE_DOMAIN_DEFAULT, PEERS_MAX * 3, &cfg));
    TRY(doca_flow_pipe_cfg_set_match(cfg, &match, NULL));
    TRY(doca_flow_pipe_create(cfg, &fwd, NULL, &o->ingress_root));
    doca_flow_pipe_cfg_destroy(cfg); return 0;
}
#undef TRY
static int build_graph(struct owner *o)
{
    return build_kernel_pipe(o) || build_rx_plain(o) || build_encrypt(o) || build_rx_gate(o) ||
        build_egress_root(o) || build_decap(o) || build_decrypt(o) || build_tx_select(o) ||
        build_ingress_root(o) ? -1 : 0;
}

/* ---- per-peer and per-association rules ---------------------------------- */

static struct peer *peer_get(struct owner *o, uint32_t local_ip, uint32_t peer_ip, int create)
{
    struct peer *slot = NULL;
    for (unsigned i = 0; i < PEERS_MAX; i++) {
        struct peer *p = &o->peers[i];
        if (p->used && p->local_ip == local_ip && p->peer_ip == peer_ip) return p;   /* refs may be 0; entries persist */
        if (!p->used && !slot) slot = p;
    }
    if (!create || !slot) return NULL;
    memset(slot, 0, sizeof(*slot)); slot->used = 1; slot->local_ip = local_ip; slot->peer_ip = peer_ip;
    struct doca_flow_match m = {0};
    struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE};
    m.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
    /* SF, local -> peer, UDP: candidate RoCE for encryption */
    m.parser_meta.port_id = o->sf_port_id; m.outer.ip4.src_ip = local_ip; m.outer.ip4.dst_ip = peer_ip;
    m.outer.ip4.next_proto = IPPROTO_UDP; fwd.next_pipe = o->tx_select;
    if (add(o, o->ingress_root, &m, NULL, &fwd, &slot->root[0])) goto fail;
    /* PF, peer -> local, ESP: decrypt */
    m.parser_meta.port_id = o->pf_port_id; m.outer.ip4.src_ip = peer_ip; m.outer.ip4.dst_ip = local_ip;
    m.outer.ip4.next_proto = IPPROTO_ESP; fwd.next_pipe = o->decrypt_pipe;
    if (add(o, o->ingress_root, &m, NULL, &fwd, &slot->root[1])) goto fail;
    /* PF, peer -> local, UDP: plaintext RoCE never reaches the SF */
    m.outer.ip4.next_proto = IPPROTO_UDP; fwd.next_pipe = o->rx_plain;
    if (add(o, o->ingress_root, &m, NULL, &fwd, &slot->root[2])) goto fail;
    memset(&m, 0, sizeof(m));
    m.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
    m.meta.pkt_meta = meta_be(1, 0, 0); m.outer.ip4.src_ip = local_ip; m.outer.ip4.dst_ip = peer_ip;
    fwd.next_pipe = o->encrypt_pipe;
    if (add(o, o->egress_root, &m, NULL, &fwd, &slot->egress[0])) goto fail;
    m.meta.pkt_meta = meta_be(0, 1, 0); m.outer.ip4.src_ip = peer_ip; m.outer.ip4.dst_ip = local_ip;
    fwd.next_pipe = o->rx_gate;
    if (add(o, o->egress_root, &m, NULL, &fwd, &slot->egress[1])) goto fail;
    return slot;
fail:
    for (unsigned i = 0; i < 3; i++) remove_entry(o, &slot->root[i]);
    for (unsigned i = 0; i < 2; i++) remove_entry(o, &slot->egress[i]);
    memset(slot, 0, sizeof(*slot)); return NULL;
}
/* The classification entries for an IP pair outlive every association on it:
 * with no SA gate to match, their targets drop, so they are harmless idle and
 * removing then re-adding them on the shared root pipes wedges port teardown.
 * The reference count still tracks liveness for the stats and the slot stays
 * claimed; the entries are retired only when the port stops. */
static int peer_release(struct owner *o, struct peer *p)
{
    (void)o; if (p->refs) p->refs--; return 0;
}
static int sa_alloc(struct owner *o, uint32_t *id)
{
    if (o->sa_next >= o->sa_limit) { DOCA_LOG_ERR("SA ids exhausted (%u); restart the owner", o->sa_limit); return -1; }
    *id = o->sa_next++; return 0;
}
static void sa_free(struct owner *o, uint32_t id) { (void)o; (void)id; }
static int sa_configure(struct owner *o, uint32_t id, const uint8_t material[20], int rx)
{
    struct doca_flow_shared_resource_cfg cfg = {0};
    uint32_t key[4], salt;
    memcpy(key, material, 16); memcpy(&salt, material + 16, 4);
    (void)o;
    cfg.ipsec_sa_cfg.icv_len = DOCA_FLOW_CRYPTO_ICV_LENGTH_16;
    cfg.ipsec_sa_cfg.salt = salt;
    cfg.ipsec_sa_cfg.key_cfg.key_type = DOCA_FLOW_CRYPTO_KEY_128;
    cfg.ipsec_sa_cfg.key_cfg.key = key;
    cfg.ipsec_sa_cfg.sn_initial = 0;
    cfg.ipsec_sa_cfg.esn_en = false;
    if (rx) {
        cfg.ipsec_sa_cfg.sn_offload_type = DOCA_FLOW_CRYPTO_SN_OFFLOAD_AR;
        cfg.ipsec_sa_cfg.win_size = DOCA_FLOW_CRYPTO_REPLAY_WIN_SIZE_128;
    } else {
        cfg.ipsec_sa_cfg.sn_offload_type = DOCA_FLOW_CRYPTO_SN_OFFLOAD_INC;
    }
    doca_error_t r = SA_SET_CFG(o->sw, id, &cfg);
    OPENSSL_cleanse(key, sizeof(key)); OPENSSL_cleanse(&cfg, sizeof(cfg));
    if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("SA %u cfg: %s", id, doca_error_get_descr(r)); return -1; }
    return 0;
}
static struct assoc *assoc_find(struct owner *o, const struct peer_crypto_request *r, int create)
{
    struct assoc *slot = NULL;
    for (unsigned i = 0; i < ASSOC_MAX; i++) {
        struct assoc *a = &o->assocs[i];
        if (a->used && !memcmp(a->session, r->binding.session, 16) &&
            !memcmp(a->association, r->binding.association, 16)) return a;
        if (!a->used && !slot) slot = a;
    }
    if (!create || !slot) return NULL;
    memset(slot, 0, sizeof(*slot)); slot->used = 1;
    memcpy(slot->session, r->binding.session, 16); memcpy(slot->association, r->binding.association, 16);
    return slot;
}
static struct epoch *epoch_find(struct assoc *a, uint64_t epoch, int create)
{
    for (unsigned i = 0; i < 2; i++) if (a->ep[i].used && a->ep[i].epoch == epoch) return &a->ep[i];
    if (!create) return NULL;
    for (unsigned i = 0; i < 2; i++) if (!a->ep[i].used) { memset(&a->ep[i], 0, sizeof(a->ep[i])); a->ep[i].used = 1; a->ep[i].epoch = epoch; return &a->ep[i]; }
    return NULL;
}
static uint32_t gid_ipv4(const uint8_t gid[16]) { uint32_t ip; memcpy(&ip, gid + 12, 4); return ip; }
static int epoch_remove(struct owner *o, struct assoc *a, struct epoch *e)
{
    int rc = 0;
    for (unsigned i = 0; i < e->lanes; i++) {
        if (remove_entry(o, &e->select[i])) rc = -2;
        if (remove_entry(o, &e->gate[i])) rc = -2;
    }
    if (remove_entry(o, &e->encrypt)) rc = -2;
    if (remove_entry(o, &e->decrypt)) rc = -2;
    if (rc) return rc;
    if (e->rx_set) sa_free(o, e->rx_sa);
    if (e->tx_set) sa_free(o, e->tx_sa);
    memset(e, 0, sizeof(*e)); (void)a;
    return 0;
}
static int install_rx(struct owner *o, const struct peer_crypto_request *r)
{
    unsigned local = r->local_endpoint;
    uint32_t local_ip = gid_ipv4(r->lanes[0].gid[local]), peer_ip = gid_ipv4(r->lanes[0].gid[1-local]);
    struct assoc *a = assoc_find(o, r, 1);
    if (!a) return -1;
    struct peer *p = peer_get(o, local_ip, peer_ip, 1);
    if (!p) { if (!a->ep[0].used && !a->ep[1].used) a->used = 0; return -1; }
    struct epoch *e = epoch_find(a, r->binding.epoch, 1);
    if (!e || e->rx_set) return -1;
    if (!a->ep[0].used || (a->ep[0].used && !a->ep[1].used && &a->ep[0] == e)) { a->peer = (unsigned)(p - o->peers); }
    if (a->ep[0].used + a->ep[1].used == 1) p->refs++;
    if (sa_alloc(o, &e->rx_sa) || sa_configure(o, e->rx_sa, r->material, 1)) goto fail;
    e->rx_set = 1;
    struct doca_flow_match m = {0};
    struct doca_flow_actions act = {0};
    m.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4; m.outer.ip4.dst_ip = local_ip;
    m.tun.type = DOCA_FLOW_TUN_ESP; m.tun.esp_spi = htonl(r->binding.spi[1-local]);
    act.crypto.crypto_id = e->rx_sa; act.meta.pkt_meta = meta_be(0, 1, e->rx_sa);
    if (add(o, o->decrypt_pipe, &m, &act, NULL, &e->decrypt)) goto fail;
    e->lanes = r->lane_count;
    for (unsigned i = 0; i < r->lane_count; i++) {
        e->local_qpn[i] = r->lanes[i].qpn[local]; e->remote_qpn[i] = r->lanes[i].qpn[1-local];
        memset(&m, 0, sizeof(m));
        m.meta.pkt_meta = meta_be(0, 1, e->rx_sa);
        m.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4; m.outer.ip4.src_ip = peer_ip; m.outer.ip4.dst_ip = local_ip;
        roce_match(&m, 0); qpn_bytes(m.outer.roce_v2.bth.dest_qp, e->local_qpn[i]);
        if (add(o, o->rx_gate, &m, NULL, NULL, &e->gate[i])) goto fail;
    }
    return 0;
fail:
    if (epoch_remove(o, a, e)) return -2;
    if (!a->ep[0].used && !a->ep[1].used) { peer_release(o, p); a->used = 0; }
    return -1;
}
static int install_tx(struct owner *o, const struct peer_crypto_request *r)
{
    unsigned local = r->local_endpoint;
    uint32_t peer_ip = gid_ipv4(r->lanes[0].gid[1-local]);
    struct assoc *a = assoc_find(o, r, 0);
    struct epoch *e = a ? epoch_find(a, r->binding.epoch, 0) : NULL;
    if (!e || !e->rx_set || e->tx_set) return -1;
    if (sa_alloc(o, &e->tx_sa) || sa_configure(o, e->tx_sa, r->material, 0)) { if (e->tx_set) sa_free(o, e->tx_sa); return -1; }
    e->tx_set = 1;
    struct doca_flow_match m = {0};
    struct doca_flow_actions act = {0};
    m.meta.pkt_meta = meta_be(0, 0, e->tx_sa);
    act.crypto.crypto_id = e->tx_sa;
    act.crypto_encap.data_size = 16; memset(act.crypto_encap.encap_data, 0, 16);
    uint32_t spi = htonl(r->binding.spi[local]); memcpy(act.crypto_encap.encap_data, &spi, 4);
    if (add(o, o->encrypt_pipe, &m, &act, NULL, &e->encrypt)) goto fail;
    /* A previous epoch's selection for the same lane gives way to this one. */
    for (unsigned i = 0; i < 2; i++) {
        struct epoch *old = &a->ep[i];
        if (!old->used || old == e) continue;
        for (unsigned k = 0; k < old->lanes; k++) if (remove_entry(o, &old->select[k])) return -2;
    }
    for (unsigned i = 0; i < r->lane_count; i++) {
        memset(&m, 0, sizeof(m)); memset(&act, 0, sizeof(act));
        m.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4; m.outer.ip4.dst_ip = peer_ip;
        roce_match(&m, 0); qpn_bytes(m.outer.roce_v2.bth.dest_qp, e->remote_qpn[i]);
        act.meta.pkt_meta = meta_be(1, 0, e->tx_sa);
        if (add(o, o->tx_select, &m, &act, NULL, &e->select[i])) goto fail;
    }
    return 0;
fail:
    for (unsigned i = 0; i < e->lanes; i++) if (remove_entry(o, &e->select[i])) return -2;
    if (remove_entry(o, &e->encrypt)) return -2;
    sa_free(o, e->tx_sa); e->tx_set = 0;
    return -1;
}
static int remove_old(struct owner *o, const struct peer_crypto_request *r)
{
    struct assoc *a = assoc_find(o, r, 0);
    struct epoch *e = a ? epoch_find(a, r->old_epoch, 0) : NULL;
    if (!e) return 0;
    return epoch_remove(o, a, e);
}
static int block(struct owner *o, const struct peer_crypto_request *r)
{
    struct assoc *a = assoc_find(o, r, 0);
    if (!a) return 0;
    int rc = 0;
    for (unsigned i = 0; i < 2; i++) {
        struct epoch *e = &a->ep[i];
        if (!e->used) continue;
        for (unsigned k = 0; k < e->lanes; k++) {
            if (remove_entry(o, &e->select[k])) { DOCA_LOG_ERR("block: select[%u] epoch %llu", k, (unsigned long long)e->epoch); rc = -2; }
            if (remove_entry(o, &e->gate[k])) { DOCA_LOG_ERR("block: gate[%u] epoch %llu", k, (unsigned long long)e->epoch); rc = -2; }
        }
    }
    return rc;
}
static int remove_all(struct owner *o, const struct peer_crypto_request *r)
{
    struct assoc *a = assoc_find(o, r, 0);
    if (!a) return 0;
    int rc = 0;
    for (unsigned i = 0; i < 2; i++) if (a->ep[i].used && epoch_remove(o, a, &a->ep[i])) rc = -2;
    if (rc) return rc;
    peer_release(o, &o->peers[a->peer]);
    memset(a, 0, sizeof(*a)); return 0;
}
static int serve(struct owner *o, const struct peer_crypto_request *r)
{
    switch (r->action) {
    case PEER_CRYPTO_INSTALL_RX: return install_rx(o, r);
    case PEER_CRYPTO_INSTALL_TX: return install_tx(o, r);
    case PEER_CRYPTO_REMOVE_OLD: return remove_old(o, r);
    case PEER_CRYPTO_BLOCK: return block(o, r);
    case PEER_CRYPTO_REMOVE_ALL: return remove_all(o, r);
    default: return 0;   /* worker-side actions never reach the owner */
    }
}

/* ---- IPC --------------------------------------------------------------- */

static int send_all(int fd, const uint8_t *buf, size_t len)
{
    while (len) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        buf += n; len -= (size_t)n;
    }
    return 0;
}
static int client_frame(struct owner *o, uint16_t type, const uint8_t *body, uint32_t len)
{
    struct peer_crypto_request r;
    struct peer_crypto_completion d;
    uint8_t frame[PEER_CRYPTO_IPC_FRAME_MAX]; size_t n;
    if (type != PEER_CRYPTO_IPC_REQUEST || peer_crypto_ipc_request_decode(body, len, &r)) return -1;
    d = (struct peer_crypto_completion){.token = r.token, .operation = r.operation, .action = r.action,
                                        .epoch = r.binding.epoch, .status = serve(o, &r)};
    memcpy(d.session, r.binding.session, 16); memcpy(d.association, r.binding.association, 16);
    OPENSSL_cleanse(&r, sizeof(r));
    drain(o, "request");
    DOCA_LOG_DBG("request op=%llu action=%d epoch=%llu -> %d", (unsigned long long)d.operation, (int)d.action,
                  (unsigned long long)d.epoch, d.status);
    if (peer_crypto_ipc_completion_encode(&d, frame, sizeof(frame), &n)) return -1;
    return send_all(o->client_fd, frame, n);
}
static void client_drop(struct owner *o)
{
    if (o->client_fd >= 0) close(o->client_fd);
    o->client_fd = -1; OPENSSL_cleanse(o->rx, sizeof(o->rx)); o->rx_len = 0;
}
static void client_accept(struct owner *o)
{
    int fd = accept4(o->listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) return;
    if (o->client_fd >= 0) { close(fd); return; }   /* one runtime per node */
    uint8_t frame[64]; size_t n;
    if (peer_crypto_ipc_hello_encode(DMESH_DOCA_MAJOR, DMESH_DOCA_MINOR, frame, sizeof(frame), &n) ||
        send_all(fd, frame, n)) { close(fd); return; }
    o->client_fd = fd; o->rx_len = 0;
    DOCA_LOG_INFO("runtime connected");
}
static void client_read(struct owner *o)
{
    ssize_t n = recv(o->client_fd, o->rx + o->rx_len, sizeof(o->rx) - o->rx_len, 0);
    if (n <= 0) { if (n < 0 && errno == EINTR) return; DOCA_LOG_INFO("runtime disconnected"); client_drop(o); return; }
    o->rx_len += (size_t)n;
    for (;;) {
        uint16_t type; uint32_t body_len;
        int rc = peer_crypto_ipc_header_decode(o->rx, o->rx_len, &type, &body_len);
        if (rc < 0) { client_drop(o); return; }
        if (rc > 0 || o->rx_len < PEER_CRYPTO_IPC_HEADER_LEN + body_len) break;
        if (client_frame(o, type, o->rx + PEER_CRYPTO_IPC_HEADER_LEN, body_len)) { client_drop(o); return; }
        size_t whole = PEER_CRYPTO_IPC_HEADER_LEN + body_len;
        OPENSSL_cleanse(o->rx, whole);
        memmove(o->rx, o->rx + whole, o->rx_len - whole); o->rx_len -= whole;
    }
    if (o->rx_len == sizeof(o->rx)) client_drop(o);
}
static int ipc_listen(struct owner *o)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(o->socket_path) >= sizeof(addr.sun_path)) { DOCA_LOG_ERR("socket path too long"); return -1; }
    strcpy(addr.sun_path, o->socket_path);
    unlink(o->socket_path);
    o->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    mode_t old = umask(0177);
    int rc = o->listen_fd < 0 || bind(o->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(o->listen_fd, 2);
    umask(old);
    if (rc) { DOCA_LOG_ERR("socket %s: %s", o->socket_path, strerror(errno)); return -1; }
    return 0;
}

/* ---- OVS guard --------------------------------------------------------- */

/* A persistent OpenFlow rule that drops plaintext RoCE from the runtime's SF
 * on the existing bridge. It is what keeps the miss path fail-closed when this
 * process is not running; it is deliberately not removed on exit. */
static int ovs_guard(struct owner *o)
{
    if (!o->ovs_bridge[0]) return 0;
    char cmd[512]; FILE *f; char port[32] = {0};
    snprintf(cmd, sizeof(cmd), "ovs-vsctl get Interface %s ofport", o->sf_iface);
    if (!(f = popen(cmd, "r")) || !fgets(port, sizeof(port), f)) { if (f) pclose(f); return -1; }
    pclose(f);
    long ofport = strtol(port, NULL, 10);
    if (ofport <= 0) return -1;
    snprintf(cmd, sizeof(cmd), "ovs-ofctl add-flow %s \"cookie=0x444d4553,priority=200,in_port=%ld,udp,tp_dst=%u,actions=drop\"",
             o->ovs_bridge, ofport, ROCE_PORT);
    int rc = system(cmd);
    if (rc) DOCA_LOG_ERR("OVS guard install failed (%d)", rc);
    else DOCA_LOG_INFO("OVS guard on %s in_port=%ld (cookie 0x444d4553)", o->ovs_bridge, ofport);
    return rc ? -1 : 0;
}

/* ---- device bring-up --------------------------------------------------- */

static int devices_open(struct owner *o)
{
    struct doca_devinfo **infos = NULL; uint32_t n = 0;
    doca_error_t r = doca_devinfo_create_list(&infos, &n);
    if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("devinfo list: %s", doca_error_get_descr(r)); return -1; }
    for (uint32_t i = 0; i < n && !o->dev; i++) {
        uint8_t equal = 0;
        if (doca_devinfo_is_equal_pci_addr(infos[i], o->pci, &equal) == DOCA_SUCCESS && equal &&
            doca_dev_open(infos[i], &o->dev) != DOCA_SUCCESS) o->dev = NULL;
    }
    doca_devinfo_destroy_list(infos);
    if (!o->dev) { DOCA_LOG_ERR("device %s not found", o->pci); return -1; }
    struct doca_devinfo_rep **reps = NULL; uint32_t nr = 0;
    r = doca_devinfo_rep_create_list(o->dev, DOCA_DEVINFO_REP_FILTER_NET, &reps, &nr);
    if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("representor list: %s", doca_error_get_descr(r)); return -1; }
    for (uint32_t i = 0; i < nr && !o->rep; i++) {
        char iface[DOCA_DEVINFO_IFACE_NAME_SIZE] = {0};
        if (doca_devinfo_rep_get_iface_name(reps[i], iface, sizeof(iface)) == DOCA_SUCCESS &&
            !strcmp(iface, o->sf_iface) && doca_dev_rep_open(reps[i], &o->rep) != DOCA_SUCCESS) o->rep = NULL;
    }
    doca_devinfo_rep_destroy_list(reps);
    if (!o->rep) { DOCA_LOG_ERR("representor %s not found", o->sf_iface); return -1; }
#ifdef DMESH_DOCA35
    const char *devargs = "dv_flow_en=2,dv_xmeta_en=4,fdb_def_rule_en=0";
#else
    const char *devargs = "dv_flow_en=2,dv_xmeta_en=4,fdb_def_rule_en=0,vport_match=1,repr_matching_en=0";
#endif
    r = doca_dpdk_port_probe_with_representors(o->dev, devargs, &o->rep, 1);
    if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("port probe: %s", doca_error_get_descr(r)); return -1; }
    uint16_t ids[2], count = 0;
    if (doca_dpdk_get_first_port_id(o->dev, &o->pf_port_id) != DOCA_SUCCESS ||
        doca_dpdk_get_port_ids(o->dev, ids, 2, &count) != DOCA_SUCCESS || count != 2) {
        DOCA_LOG_ERR("expected the PF and one representor port"); return -1;
    }
    o->sf_port_id = ids[0] == o->pf_port_id ? ids[1] : ids[0];
    uint16_t mtu;
    int rc = rte_eth_dev_get_mtu(o->pf_port_id, &mtu);
    if (rc) { DOCA_LOG_ERR("PF mtu: %d", rc); return -1; }
    struct rte_eth_conf conf = {0}; conf.rxmode.mtu = mtu;
    rc = rte_eth_dev_configure(o->pf_port_id, 1, 1, &conf);
    if (rc) { DOCA_LOG_ERR("PF configure: %d", rc); return -1; }
    char pool[64]; snprintf(pool, sizeof(pool), "dpumesh-crypto-%ld", (long)getpid());
    o->pool = rte_pktmbuf_pool_create(pool, 1024, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!o->pool) { DOCA_LOG_ERR("mbuf pool: %s", rte_strerror(rte_errno)); return -1; }
    if ((rc = rte_eth_rx_queue_setup(o->pf_port_id, 0, 128, rte_socket_id(), NULL, o->pool)) ||
        (rc = rte_eth_tx_queue_setup(o->pf_port_id, 0, 128, rte_socket_id(), NULL)) ||
        (rc = rte_eth_dev_start(o->pf_port_id))) {
        DOCA_LOG_ERR("PF queues/start: %d", rc); return -1;
    }
    return 0;
}
static int flow_start(struct owner *o)
{
    struct doca_flow_cfg *cfg = NULL;
    struct doca_flow_port_cfg *pcfg = NULL;
    doca_error_t r;
#define T(call) do { r = (call); if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("%s: %s", #call, doca_error_get_descr(r)); goto fail; } } while (0)
    T(doca_flow_cfg_create(&cfg));
    T(doca_flow_cfg_set_pipe_queues(cfg, 1));
    T(doca_flow_cfg_set_queue_depth(cfg, 64));
#ifdef DMESH_DOCA35
    T(doca_flow_cfg_set_mode_args(cfg, "switch,hws,expert"));
    T(doca_flow_cfg_set_resource_mode(cfg, DOCA_FLOW_RESOURCE_MODE_PORT));
#else
    T(doca_flow_cfg_set_mode_args(cfg, "switch,hws,isolated,expert"));
    T(doca_flow_cfg_set_nr_shared_resource(cfg, o->sa_limit, DOCA_FLOW_SHARED_RESOURCE_IPSEC_SA));
    uint16_t rss_q = 0;
    struct doca_flow_resource_rss_cfg rss = {.queues_array = &rss_q, .nr_queues = 1};
    T(doca_flow_cfg_set_default_rss(cfg, &rss));
#endif
    T(doca_flow_cfg_set_cb_entry_process(cfg, on_entry));
    T(doca_flow_init(cfg)); o->flow = 1;
    doca_flow_cfg_destroy(cfg); cfg = NULL;
    T(doca_flow_port_cfg_create(&pcfg));
    T(doca_flow_port_cfg_set_port_id(pcfg, o->pf_port_id));
    T(doca_flow_port_cfg_set_dev(pcfg, o->dev));
    T(doca_flow_port_cfg_set_actions_mem_size(pcfg, 1u << 20));
#ifdef DMESH_DOCA35
    T(doca_flow_port_cfg_set_nr_resources(pcfg, DOCA_FLOW_RESOURCE_IPSEC_SA, o->sa_limit));
#endif
    T(doca_flow_port_start(pcfg, &o->pf_port));
    doca_flow_port_cfg_destroy(pcfg); pcfg = NULL;
    T(doca_flow_port_cfg_create(&pcfg));
    T(doca_flow_port_cfg_set_port_id(pcfg, o->sf_port_id));
    T(doca_flow_port_cfg_set_dev_rep(pcfg, o->rep));
    T(doca_flow_port_cfg_set_actions_mem_size(pcfg, 1024));
    T(doca_flow_port_start(pcfg, &o->sf_port));
    doca_flow_port_cfg_destroy(pcfg); pcfg = NULL;
    o->sw = doca_flow_port_switch_get(o->pf_port);
    if (!o->sw) { DOCA_LOG_ERR("no switch port"); goto fail; }
#ifndef DMESH_DOCA35
    uint32_t *ids = calloc(o->sa_limit, sizeof(*ids));
    if (!ids) goto fail;
    for (uint32_t i = 0; i < o->sa_limit; i++) ids[i] = i;
    r = doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_IPSEC_SA, ids, o->sa_limit, o->sw);
    free(ids);
    if (r != DOCA_SUCCESS) { DOCA_LOG_ERR("SA bind: %s", doca_error_get_descr(r)); goto fail; }
#endif
    T(doca_flow_get_target(DOCA_FLOW_TARGET_KERNEL, &o->kernel));
#undef T
    return 0;
fail:
    if (pcfg) doca_flow_port_cfg_destroy(pcfg);
    if (cfg) doca_flow_cfg_destroy(cfg);
    return -1;
}
static void teardown(struct owner *o)
{
    for (unsigned i = 0; i < ASSOC_MAX; i++) {
        struct assoc *a = &o->assocs[i];
        if (!a->used) continue;
        for (unsigned k = 0; k < 2; k++) if (a->ep[k].used) epoch_remove(o, a, &a->ep[k]);
        a->used = 0;
    }
    /* The port stop retired every entry and pipe, persistent peer entries
     * included; only descriptors and the socket remain. */
    /* Every entry and pipe is retired by stopping the switch port. Removing the
     * static entries by hand first, or destroying the crypto pipes explicitly,
     * both wedge doca_flow_port_stop; letting the port own the teardown works. */
    /* The switch port_stop can wedge after many associations have churned on one
     * IP pair; a watchdog turns that into a fast, fail-closed exit instead of a
     * hang. Every flow entry a live association needs was already removed, so
     * nothing plaintext survives, and the next start rebuilds a fresh graph. */
    if (o->listen_fd >= 0) unlink(o->socket_path);   /* before the watchdog window */
    drain(o, "teardown");
    signal(SIGALRM, on_teardown_timeout);
    alarm(8);
    if (o->sf_port) doca_flow_port_stop(o->sf_port);
    if (o->pf_port) doca_flow_port_stop(o->pf_port);
    if (o->flow) doca_flow_destroy();
    alarm(0);
    if (o->pf_port_id != UINT16_MAX) { rte_eth_dev_stop(o->pf_port_id); rte_eth_dev_close(o->pf_port_id); }
    if (o->sf_port_id != UINT16_MAX) rte_eth_dev_close(o->sf_port_id);
    if (o->pool) rte_mempool_free(o->pool);
    if (o->rep) doca_dev_rep_close(o->rep);
    if (o->dev) doca_dev_close(o->dev);
    if (o->eal) rte_eal_cleanup();
    if (o->client_fd >= 0) close(o->client_fd);
    if (o->listen_fd >= 0) { close(o->listen_fd); unlink(o->socket_path); }
}
static int usage(const char *argv0)
{
    fprintf(stderr, "usage: %s --socket PATH --pci PCI --sf-iface IFACE [--sa-limit N] [--ovs-guard BRIDGE]\n", argv0);
    return 2;
}
int main(int argc, char **argv)
{
    static struct owner o;
    o.pf_port_id = o.sf_port_id = UINT16_MAX; o.listen_fd = o.client_fd = -1; o.sa_limit = SA_DEFAULT;
    static const struct option options[] = {
        {"socket", required_argument, NULL, 's'}, {"pci", required_argument, NULL, 'p'},
        {"sf-iface", required_argument, NULL, 'i'}, {"sa-limit", required_argument, NULL, 'n'},
        {"ovs-guard", required_argument, NULL, 'g'}, {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "s:p:i:n:g:", options, NULL)) != -1) {
        switch (c) {
        case 's': snprintf(o.socket_path, sizeof(o.socket_path), "%s", optarg); break;
        case 'p': snprintf(o.pci, sizeof(o.pci), "%s", optarg); break;
        case 'i': snprintf(o.sf_iface, sizeof(o.sf_iface), "%s", optarg); break;
        case 'n': o.sa_limit = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'g': snprintf(o.ovs_bridge, sizeof(o.ovs_bridge), "%s", optarg); break;
        default: return usage(argv[0]);
        }
    }
    if (!o.socket_path[0] || !o.pci[0] || !o.sf_iface[0] || o.sa_limit < 2 || o.sa_limit > 65536) return usage(argv[0]);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);
    cpu_set_t allowed; CPU_ZERO(&allowed);
    int cpu = -1;
    if (!sched_getaffinity(0, sizeof(allowed), &allowed))
        for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &allowed)) cpu = i;
    if (cpu < 0) return 1;
    char cpustr[16], prefix[64];
    snprintf(cpustr, sizeof(cpustr), "%d", cpu);
    snprintf(prefix, sizeof(prefix), "dpumesh-crypto-%ld", (long)getpid());
    char *eal_argv[] = {argv[0], "-l", cpustr, "-a", "pci:00:00.0", "--no-huge", "--no-shconf",
#ifdef DMESH_DOCA35
                        "-a", "auxiliary:",
#endif
                        "--no-telemetry", "--iova-mode=va", "-m", "128", "--file-prefix", prefix};
    int rc = 1;
    if (doca_log_backend_create_standard() != DOCA_SUCCESS) return 1;
    if (rte_eal_init(sizeof(eal_argv) / sizeof(eal_argv[0]), eal_argv) < 0) { DOCA_LOG_ERR("EAL init failed"); goto out; }
    o.eal = 1;
    if (devices_open(&o)) { DOCA_LOG_ERR("devices failed"); goto out; }
    if (flow_start(&o)) { DOCA_LOG_ERR("flow start failed"); goto out; }
    if (build_graph(&o)) { DOCA_LOG_ERR("graph build failed"); goto out; }
    if (ovs_guard(&o)) { DOCA_LOG_ERR("ovs guard failed"); goto out; }
    if (ipc_listen(&o)) { DOCA_LOG_ERR("socket failed"); goto out; }
    DOCA_LOG_INFO("ready: pf port %u, sf port %u (%s), %u SAs, socket %s", o.pf_port_id, o.sf_port_id, o.sf_iface,
                  o.sa_limit, o.socket_path);
    while (!g_stop) {
        struct pollfd fds[2] = {{.fd = o.listen_fd, .events = POLLIN}, {.fd = o.client_fd, .events = POLLIN}};
        int n = poll(fds, o.client_fd >= 0 ? 2 : 1, 500);
        if (n < 0 && errno != EINTR) break;
        if (n <= 0) continue;
        if (fds[0].revents & POLLIN) client_accept(&o);
        if (o.client_fd >= 0 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) client_read(&o);
    }
    rc = 0;
out:
    DOCA_LOG_INFO("shutting down");
    teardown(&o);
    return rc;
}
