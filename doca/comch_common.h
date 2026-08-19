#ifndef COMCH_COMMON_H
#define COMCH_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <doca_error.h>
#include <doca_mmap.h>

struct objects;
struct doca_comch_event_consumer;
struct doca_comch_connection;

void
dmesh_consumer_connected(struct doca_comch_event_consumer *event,
                         struct doca_comch_connection *connection,
                         uint32_t id);

void
dmesh_consumer_expired(struct doca_comch_event_consumer *event,
                       struct doca_comch_connection *connection,
                       uint32_t id);

/* Number of Comch control-path send tasks (HW max ~65536). Both the client and
 * the server size their send pool to this value. */
#define CC_SEND_TASK_NUM 8192

/* Host ↔ DPU ARM control messages. Values are wire ABI and remain below 256. */
enum dmesh_msg_type {
    DMESH_MSG_INVALID      = 0, /* reserved: zeroed buffer is never a live type */
    DMESH_MSG_POD_REGISTER = 1, /* Host→DPU: register (service_id; pod_id=-1 → DPU assigns) */
    DMESH_MSG_MMAP_EXPORT  = 2, /* Host→DPU: export an mmap region (ring / TX buf / RX buf) */
    DMESH_MSG_POD_ASSIGNED = 5, /* DPU→Host: the pod_id the DPU allocated for this registration */
    DMESH_MSG_POD_INIT_RESULT=6,/* DPU→Host: all pod DMA resources are READY, or init failed */
    DMESH_MSG_POD_UNREGISTER=7, /* Host→DPU: stop routing and quiesce every remote DMA reference */
    DMESH_MSG_POD_QUIESCED=8,  /* DPU→Host: remote mappings reclaimed; host may destroy exports */
    DMESH_MSG_REV_DOORBELL=9,  /* DPU→Host: reverse-ring wake notification */
    DMESH_MSG_REG_CHALLENGE=11,/* DPU→Host: connection-bound trusted-registration nonce */
    DMESH_MSG_WORKLOAD_GRANT=12,/* Host→DPU: node-agent-authorized workload grant */
};

/* POD_ASSIGNED only reserves an address. A channel is usable only after the DPU
 * reports DMESH_POD_INIT_READY, which means all K forward rings, the host TX
 * mmap, the host RX mmap, an installation ACK from every target DPA EU, and the
 * ARM egress engine are ready. Values other than PENDING/READY are terminal
 * failures. */
enum dmesh_pod_init_result {
    DMESH_POD_INIT_PENDING         = 0,
    DMESH_POD_INIT_READY           = 1,
    DMESH_POD_INIT_REGISTER_FAILED = 2,
    DMESH_POD_INIT_MMAP_FAILED     = 3,
    DMESH_POD_INIT_DPA_FAILED      = 4,
};

/* TX_ACK frees the sender's TX slots by source endpoint and sequence. One entry
 * names the consecutive run [seq, seq + seq_count) one staging extent holds. A
 * zero count is read as one. */
struct dmesh_tx_ack_entry {
    uint16_t port;
    uint16_t seq;
    uint16_t seq_count;
};
_Static_assert(sizeof(struct dmesh_tx_ack_entry) <= 16,
               "dmesh_tx_ack_entry must fit the reverse-entry payload union");

struct dmesh_rev_done_entry {
    int8_t   src_pod_id;   /* sender pod (the peer, for the receiving conn) */
    int8_t   src_service;  /* caller service */
    int8_t   dst_service;  /* callee service (selects local accept queue when dst_port==BLANK) */
    uint8_t  _pad;
    uint16_t src_port;     /* sender port */
    uint16_t dst_port;     /* dest port: PORT_BLANK -> accept queue, else socket lookup */
    uint16_t seq;          /* per-conn sequence (match key with dst_port) */
    uint16_t length;       /* payload length (<= slot_size) */
    uint32_t pos;          /* landing byte-offset in host RX buffer */
};
_Static_assert(sizeof(struct dmesh_rev_done_entry) == 16, "dmesh_rev_done_entry must pack to 16B");

enum dmesh_rev_entry_kind {
    DMESH_REV_ENTRY_INVALID = 0,
    DMESH_REV_ENTRY_DONE = 1,
    DMESH_REV_ENTRY_TX_ACK = 2,
};

/* One SPSC reverse-completion ring is owned by each destination region. */
#define DMA_REV_RING_SIZE 8192u
struct dmesh_rev_ring_entry {
    uint8_t kind;
    uint8_t reserved0[7];
    union {
        struct dmesh_rev_done_entry done;
        struct dmesh_tx_ack_entry ack;
        uint8_t bytes[16];
    } payload;
    volatile uint64_t publish_seq; /* producer ticket + 1 */
} __attribute__((aligned(8)));
_Static_assert(sizeof(struct dmesh_rev_ring_entry) == 32,
               "dmesh_rev_ring_entry ABI drift");
_Static_assert(offsetof(struct dmesh_rev_ring_entry, publish_seq) == 24,
               "dmesh_rev_ring_entry publication offset drift");

/* The host-owned control fields occupy a separate cache line from the slots. */
struct dmesh_rev_ring_ctrl {
    uint8_t reserved[64];
    /* The DPU egress engine reads this control block positionally through
     * PX_REV_CTRL_OFF (dpu_proxy.c): ctrl[0] is consumer_head and ctrl[1] is
     * arm_epoch. Neither field may be moved or dropped. */
    volatile uint64_t consumer_head; /* host publishes after a drain batch */
    volatile uint64_t arm_epoch;     /* host increments before blocking; a change
                                      * makes the DPU send REV_DOORBELL */
    uint8_t consumer_reserved[48];
} __attribute__((aligned(64)));
_Static_assert(sizeof(struct dmesh_rev_ring_ctrl) == 128,
               "dmesh_rev_ring_ctrl ABI drift");

#define DMA_REV_RING_BYTES \
    ((size_t)DMA_REV_RING_SIZE * sizeof(struct dmesh_rev_ring_entry) + \
     sizeof(struct dmesh_rev_ring_ctrl))

enum mmap_type {
    DMA_BUFFER = 1,
    DMA_RING = 2,
    DMA_HOST_RX_BUFFER = 3, /* Host RX buffer for DPU→CPU reverse DMA */
    DMA_REV_RING = 4,       /* DPU→Host reverse completion ring */
};

struct dmesh_mmap_msg {
    enum dmesh_msg_type type;
    enum mmap_type mmap_type;
    void *host_addr;
    size_t buf_size;
    size_t export_desc_len;
    uint8_t export_desc[];
};

typedef uint64_t doca_dpa_dev_comch_consumer_completion_t;
typedef uint64_t doca_dpa_dev_completion_t;
typedef uint64_t doca_dpa_dev_comch_producer_t;
typedef uint64_t doca_dpa_dev_comch_consumer_t;

/* Host→DPU: register this connection. pod_id == -1 asks the DPU to allocate a
 * free pod_id and return it in a DMESH_MSG_POD_ASSIGNED reply. */
struct dmesh_register_msg {
    enum dmesh_msg_type type;   /* = DMESH_MSG_POD_REGISTER */
    int32_t pod_id;             /* -1 requests DPU assignment */
    int32_t service_id;         /* this node's service id; the DPU adds this pod to the
                                 * service's live backend set (an LB candidate for that
                                 * service). SVC_NONE = client-only, no service to advertise. */
};
_Static_assert(sizeof(struct dmesh_register_msg) == 12,
               "dmesh_register_msg ABI drift");

/* Bound on the Linkerd workload a registration is attributed to: the grant's
 * namespace and Pod name as injector-compatible JSON. */
#define DMESH_WORKLOAD_MAX 384

/* Trusted workload registration is the only way a Pod enters the mesh. The DPU
 * creates a fresh challenge for every Comch connection. The Host relays it to a
 * root-owned node agent; it holds no HMAC key and cannot alter the claims. */
#define DMESH_GRANT_VERSION 1u
#define DMESH_REG_NONCE_SIZE 32u
#define DMESH_GRANT_ID_SIZE 16u
#define DMESH_GRANT_MAC_SIZE 32u
#define DMESH_GRANT_ISSUER_MAX 64u
#define DMESH_GRANT_KEY_ID_MAX 32u
#define DMESH_POD_UID_MAX 64u
#define DMESH_K8S_NAMESPACE_MAX 64u
#define DMESH_K8S_NAME_MAX 254u

struct dmesh_registration_challenge_msg {
    uint8_t type;               /* = DMESH_MSG_REG_CHALLENGE */
    uint8_t version;            /* = DMESH_GRANT_VERSION */
    uint8_t trusted_required;   /* always 1; a Host that reads 0 refuses to register */
    uint8_t reserved;           /* must be zero */
    uint8_t nonce[DMESH_REG_NONCE_SIZE];
};
_Static_assert(sizeof(struct dmesh_registration_challenge_msg) == 36,
               "dmesh_registration_challenge_msg ABI drift");

/* Canonical v1 grant. Numeric fields are explicit little-endian byte strings;
 * all text fields are NUL-terminated and zero-padded. The HMAC-SHA256 covers
 * every byte before `mac`, including the message type and version. */
struct dmesh_workload_grant_msg {
    uint8_t type;               /* = DMESH_MSG_WORKLOAD_GRANT */
    uint8_t version;            /* = DMESH_GRANT_VERSION */
    uint8_t flags;              /* v1: zero */
    uint8_t reserved;           /* zero */
    uint8_t service_id_le[4];
    uint8_t issued_at_le[8];
    uint8_t expires_at_le[8];
    uint8_t grant_id[DMESH_GRANT_ID_SIZE];
    uint8_t nonce[DMESH_REG_NONCE_SIZE];
    char issuer[DMESH_GRANT_ISSUER_MAX];
    char key_id[DMESH_GRANT_KEY_ID_MAX];
    char pod_uid[DMESH_POD_UID_MAX];
    char namespace_name[DMESH_K8S_NAMESPACE_MAX];
    char pod_name[DMESH_K8S_NAME_MAX];
    char service_account[DMESH_K8S_NAME_MAX];
    char node_name[DMESH_K8S_NAME_MAX];
    uint8_t mac[DMESH_GRANT_MAC_SIZE];
};
_Static_assert(sizeof(struct dmesh_workload_grant_msg) == 1090,
               "dmesh_workload_grant_msg ABI drift");

/* Host→trusted-node-agent request, transported over a root-owned AF_UNIX
 * SOCK_SEQPACKET socket. SO_PEERCRED, not request data, identifies the caller. */
#define DMESH_ATTEST_MAGIC "DMESHAR1"
struct dmesh_attest_request {
    uint8_t magic[8];
    uint8_t version;
    uint8_t reserved[3];
    uint8_t service_id_le[4];
    uint8_t nonce[DMESH_REG_NONCE_SIZE];
};
_Static_assert(sizeof(struct dmesh_attest_request) == 48,
               "dmesh_attest_request ABI drift");

/* DPU→Host: the pod_id the DPU allocated for a pod_id==-1 registration. Byte
 * `type` at offset 0 (the host dispatches DPU→host messages by recv_buffer[0]). */
struct dmesh_pod_assigned_msg {
    uint8_t  type;              /* = DMESH_MSG_POD_ASSIGNED */
    uint8_t  _pad[3];
    int32_t  pod_id;            /* the assigned pod id (>= 0) */
    int32_t  landing_stripes;   /* host RX partitions and reverse rings to export */
};
_Static_assert(sizeof(struct dmesh_pod_assigned_msg) == 12,
               "dmesh_pod_assigned_msg ABI drift");

/* DPU→Host terminal result for the second phase of channel initialization.
 * Keep the type byte at offset 0 because the host dispatches on recv_buffer[0]. */
struct dmesh_pod_init_result_msg {
    uint8_t  type;              /* = DMESH_MSG_POD_INIT_RESULT */
    uint8_t  _pad[3];
    int32_t  pod_id;            /* assigned pod, or -1 when registration itself failed */
    int32_t  result;            /* enum dmesh_pod_init_result; never PENDING on the wire */
    int32_t  landing_stripes;   /* host RX buffer partitions */
};
_Static_assert(sizeof(struct dmesh_pod_init_result_msg) == 16,
               "dmesh_pod_init_result_msg ABI drift");

/* Graceful teardown is a protocol barrier, not merely a Comch disconnect. The
 * host keeps its exported mmaps alive after UNREGISTER until QUIESCED arrives.
 * The DPU sends QUIESCED only after DPA DEL_ACK fences and ARM SG-DMA quiescence,
 * and after destroying its imported buf_arr/mmap handles. */
struct dmesh_pod_unregister_msg {
    uint8_t type;               /* = DMESH_MSG_POD_UNREGISTER */
    uint8_t _pad[3];
    int32_t pod_id;
};
struct dmesh_pod_quiesced_msg {
    uint8_t type;               /* = DMESH_MSG_POD_QUIESCED */
    uint8_t _pad[3];
    int32_t pod_id;
};
struct dmesh_rev_doorbell_msg {
    uint8_t type;
    uint8_t _pad[3];
};
_Static_assert(sizeof(struct dmesh_rev_doorbell_msg) == 4,
               "dmesh_rev_doorbell_msg ABI drift");
_Static_assert(sizeof(struct dmesh_pod_unregister_msg) == 8,
               "dmesh_pod_unregister_msg ABI drift");
_Static_assert(sizeof(struct dmesh_pod_quiesced_msg) == 8,
               "dmesh_pod_quiesced_msg ABI drift");

doca_error_t
export_mmap_to_remote(struct objects *objs, struct doca_mmap *mmap, void *buffer, size_t buf_size, enum mmap_type mmap_type);
doca_error_t
process_mmap_msg(struct objects *objs, struct doca_comch_connection *conn,
                 struct dmesh_mmap_msg *mmap_msg, size_t msg_len);
#endif // COMCH_COMMON_H
