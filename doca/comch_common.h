#ifndef COMCH_COMMON_H
#define COMCH_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <doca_error.h>
#include <doca_mmap.h>

struct objects;

/* Number of Comch control-path send tasks (HW max ~65536). Defined once here
 * and shared by BOTH the client and server send pools (each sizes its pool to
 * this) so the two sides can never drift out of lockstep. */
#define CC_SEND_TASK_NUM 8192

/* Control-channel message types (Host ↔ DPU ARM, over the DOCA Comch control
 * path). Explicit values, contiguous from 1; 0 is reserved INVALID so a zeroed
 * buffer never decodes to a live type. The verb vocabulary (POD_, MMAP_, FWD_,
 * REV_) is shared with the DPU<->DPA datapath enum (enum dpa_msg_type) for a
 * consistent naming scheme across both channels. The type travels on the wire
 * as the low byte of this field; the host dispatches by reading a single byte
 * (little-endian), so values must stay < 256. */
enum dmesh_msg_type {
    DMESH_MSG_INVALID      = 0, /* reserved: zeroed buffer is never a live type */
    DMESH_MSG_POD_REGISTER = 1, /* Host→DPU: register (service_id; pod_id=-1 → DPU assigns) */
    DMESH_MSG_MMAP_EXPORT  = 2, /* Host→DPU: export an mmap region (ring / TX buf / RX buf) */
    DMESH_MSG_BATCH_FWD_ACK= 3, /* DPU→Host: batch of (port,seq) keys whose forward DMA is done — free all */
    DMESH_MSG_BATCH_REV_DONE=4, /* DPU→Host: batch of reverse-DMA completions — deliver all */
    DMESH_MSG_POD_ASSIGNED = 5, /* DPU→Host: the pod_id the DPU allocated for this registration */
    DMESH_MSG_POD_INIT_RESULT=6,/* DPU→Host: all pod DMA resources are READY, or init failed */
    DMESH_MSG_POD_UNREGISTER=7, /* Host→DPU: stop routing and quiesce every remote DMA reference */
    DMESH_MSG_POD_QUIESCED=8,  /* DPU→Host: remote mappings reclaimed; host may destroy exports */
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

/* DPU→Host: batched TX_ACK. Coalesces up to BATCH_TXACK_MAX per-request
 * FWD_ACKs into one comch message so the host PE thread processes 1 message
 * instead of K. Flushed when full or on the idle (drain-empty) flush. */
#define BATCH_TXACK_MAX 16
/* DPU->Host TX_ACK frees the SENDER's TX slot. Keyed by the SOURCE endpoint
 * (port,seq) of the acked forward leg. The port's range (client-ephemeral vs
 * server-accepted, both from one host-unique pool) keeps client/server keys
 * disjoint even when both roles live on the same (loopback) host. */
struct dmesh_tx_ack_entry {
    uint16_t port;       /* source endpoint port of the acked leg */
    uint16_t seq;        /* its sequence */
};
struct dmesh_batch_tx_ack_msg {
    uint8_t  type;       /* = DMESH_MSG_BATCH_FWD_ACK */
    uint8_t  count;      /* number of valid entries in acks[] (1..BATCH_TXACK_MAX) */
    uint8_t  _pad[2];    /* align acks to 4B */
    struct dmesh_tx_ack_entry acks[BATCH_TXACK_MAX];
};
_Static_assert(sizeof(struct dmesh_batch_tx_ack_msg) == 4 + 4 * BATCH_TXACK_MAX,
               "dmesh_batch_tx_ack_msg must pack tightly");

/* DPU→Host: batched REV_DONE. Coalesces up to BATCH_REVDONE_MAX per-response
 * reverse-DMA completions into one comch message so the host PE thread reaps 1
 * message per K responses instead of K — the per-RTT PE reap is the 2-pod cap.
 * Each entry mirrors the comch_dma_comp_msg payload minus the type byte. */
#define BATCH_REVDONE_MAX 16
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
};  /* dst_pod omitted: host demuxes by dst_port; peer is captured from src_* */
_Static_assert(sizeof(struct dmesh_rev_done_entry) == 16, "dmesh_rev_done_entry must pack to 16B");
struct dmesh_batch_rev_done_msg {
    uint8_t  type;       /* = DMESH_MSG_BATCH_REV_DONE */
    uint8_t  count;      /* number of valid entries (1..BATCH_REVDONE_MAX) */
    uint8_t  _pad[2];    /* align entries to 4B */
    struct dmesh_rev_done_entry entries[BATCH_REVDONE_MAX];
};
_Static_assert(sizeof(struct dmesh_batch_rev_done_msg) == 4 + 16 * BATCH_REVDONE_MAX,
               "dmesh_batch_rev_done_msg must pack tightly");

enum mmap_type {
    DMA_BUFFER = 1,
    DMA_RING = 2,
    DMA_HOST_RX_BUFFER = 3, /* Host RX buffer for DPU→CPU reverse DMA */
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

/* Host→DPU: register this connection. pod_id == -1 asks the DPU to ALLOCATE a
 * free pod_id and return it in a DMESH_MSG_POD_ASSIGNED reply. */
struct dmesh_register_msg {
    enum dmesh_msg_type type;   /* = DMESH_MSG_POD_REGISTER */
    int32_t pod_id;             /* -1 → DPU assigns (the host no longer picks its address) */
    int32_t service_id;         /* this node's service id; the DPU adds this pod to the
                                 * service's live backend set (an LB candidate for that
                                 * service). SVC_NONE = client-only, no service to advertise. */
};

/* DPU→Host: the pod_id the DPU allocated for a pod_id==-1 registration. Byte
 * `type` at offset 0 (the host dispatches DPU→host messages by recv_buffer[0]). */
struct dmesh_pod_assigned_msg {
    uint8_t  type;              /* = DMESH_MSG_POD_ASSIGNED */
    uint8_t  _pad[3];
    int32_t  pod_id;            /* the assigned pod id (>= 0) */
};

/* DPU→Host terminal result for the second phase of channel initialization.
 * Keep the type byte at offset 0 because the host dispatches on recv_buffer[0]. */
struct dmesh_pod_init_result_msg {
    uint8_t  type;              /* = DMESH_MSG_POD_INIT_RESULT */
    uint8_t  _pad[3];
    int32_t  pod_id;            /* assigned pod, or -1 when registration itself failed */
    int32_t  result;            /* enum dmesh_pod_init_result; never PENDING on the wire */
};
_Static_assert(sizeof(struct dmesh_pod_init_result_msg) == 12,
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
_Static_assert(sizeof(struct dmesh_pod_unregister_msg) == 8,
               "dmesh_pod_unregister_msg ABI drift");
_Static_assert(sizeof(struct dmesh_pod_quiesced_msg) == 8,
               "dmesh_pod_quiesced_msg ABI drift");



/* Type-peek wrapper: control-path recv buffers are cast to this to read the
 * leading type, then re-cast to the concrete message struct (mmap/register). */
struct dmesh_comch_msg {
    enum dmesh_msg_type type;
};
doca_error_t
export_mmap_to_remote(struct objects *objs, struct doca_mmap *mmap, void *buffer, size_t buf_size, enum mmap_type mmap_type);
struct doca_comch_connection;
doca_error_t
process_mmap_msg(struct objects *objs, struct doca_comch_connection *conn,
                 struct dmesh_mmap_msg *mmap_msg, size_t msg_len);
#endif // COMCH_COMMON_H
