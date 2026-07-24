#ifndef DPA_COMMON_H
#define DPA_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <doca_mmap.h>

#include <dpumesh/dmesh_common.h>

typedef uint64_t doca_dpa_dev_uintptr_t;
typedef uint64_t doca_dpa_dev_buf_arr_t;

/* ====== Multi-ring DPA thread arg ====== */

struct dpa_ring_info {
	doca_dpa_dev_buf_arr_t buf_arr;
	uint32_t buf_arr_size;
	doca_dpa_dev_mmap_t host_mmap;   /* Host TX buffer mmap (forward DMA source) */
	uint64_t host_addr;              /* Host TX buffer base VA (moff = desc->addr - host_addr) */
	doca_dpa_dev_mmap_t dpu_mmap;    /* DPU staging buffer mmap (forward DMA dest) */
	uint64_t dpu_addr;               /* this ring's DPU staging region base VA */
	int32_t pod_id;
		/* Wire-ABI field fixed at zero. The staging base equals dpu_addr. */
	uint32_t region_off;
} __attribute__((__packed__, aligned(8)));

struct dpa_thread_arg {
	/* Shared comch msgq handles (CPU→DPU direction: DPA sends completions to DPU) */
	uint64_t dpa_consumer_comp;
	uint64_t dpa_producer_comp;
	uint64_t dpa_producer;
	uint64_t dpa_consumer;
	uint32_t dpu_consumer_id; /* DPU-side comch consumer ID for DPA->DPU sends */
	uint32_t eu_index; /* which EU this thread is (0..N-1) */
	/* Generation of rings[0]. The first ring is installed by h2d_memcpy before
	 * the EU starts rather than by RING_ADD; run_dma_manager echoes this in an
	 * ADD_ACK as its first action so ARM readiness proves the EU actually ran. */
	uint32_t initial_generation;
	/* doca_dpa_dev_thread_reschedule restarts the global entry function, so this
	 * persistent device-memory guard is required to emit the initial ACK once. */
	uint32_t initial_ack_sent;

	/* Forward rings (CPU→DPU, per-pod). Reverse (DPU→host) egress is the ARM
	 * SG-DMA engine (dpu_proxy.c), not a DPA ring — no reverse ring state here.
	 * Forward staging mirrors the host TX byte offset, so there is no per-ring
	 * landing cursor either (the completion pos == host offset). */
	volatile uint32_t num_rings;
	uint32_t _pad2;
	struct dpa_ring_info rings[MAX_DPA_RINGS];
	uint64_t consumer_head[MAX_DPA_RINGS];
	/* Incarnation paired with rings[] and echoed by FWD_DONE. */
	uint32_t ring_generation[MAX_DPA_RINGS];
} __attribute__((__packed__, aligned(8)));

/* DMA payloads contain only message bodies. Endpoint metadata travels in
 * comch_dma_comp_msg and dma_desc. Slot admission bounds buffer occupancy. */

/* DPU ARM and DPA EU messages. Zero is reserved for invalid buffers. */
enum dpa_msg_type {
	DPA_MSG_INVALID  = 0, /* reserved: zeroed buffer hits default-reject */
	DPA_MSG_RING_ADD = 1, /* DPU→DPA: add forward (CPU→DPU) ring */
	DPA_MSG_WAKE     = 2, /* DPU→DPA: wake up the EU thread (no payload) */
	DPA_MSG_FWD_DONE = 3, /* DPA→DPU: forward DMA completed (CPU→DPU) */
	DPA_MSG_RING_DEL = 4, /* DPU→DPA: drop this pod's forward ring from rings[] */
	DPA_MSG_RING_ADD_ACK = 5, /* DPA→DPU: ring installed (or rejected) */
	DPA_MSG_RING_DEL_ACK = 6, /* DPA→DPU: ring removed; prior DMA ops are fenced */
	/* Reverse egress uses the ARM SG-DMA engine. */
};

/* Each EU holds at most one ring per pod. RING_DEL removes it, and the ordered
 * DEL_ACK fences preceding DMA operations for that ring. */

enum dpa_ring_ack_status {
	DPA_RING_ACK_OK = 0,
	DPA_RING_ACK_FULL = 1,
};

/* Ring operation result. The generation rejects stale acknowledgements. */
struct dpa_ring_ack_msg {
	uint8_t  type;       /* DPA_MSG_RING_ADD_ACK or DPA_MSG_RING_DEL_ACK */
	int8_t   pod_id;
	uint8_t  status;     /* enum dpa_ring_ack_status */
	uint8_t  eu_index;
	uint32_t generation;
} __attribute__((__packed__, aligned(4)));
_Static_assert(sizeof(struct dpa_ring_ack_msg) == 8,
               "dpa_ring_ack_msg must be exactly 8 bytes");

/* Packed DPA-to-DPU completion. `type` remains at offset zero for dispatch, and
 * `generation` rejects stale completions. Endpoint fields support routing and
 * host demultiplexing. */
struct comch_dma_comp_msg {
	uint8_t  type;        /* DPA_MSG_FWD_DONE (offset 0 — peeked) */
	int8_t   src_pod_id;  /* originating pod (always concrete) */
	int8_t   dst_pod_id;  /* dest pod; DMESH_POD_BLANK -> DPU resolves dst_service */
	int8_t   dst_service; /* callee service id (routing input when dst_pod==BLANK) */
	uint16_t src_port;    /* sender port */
	uint16_t dst_port;    /* dest port; PORT_BLANK -> accept queue */
	uint16_t seq;         /* per-conn sequence (match key with port) */
	uint16_t length;      /* payload length (<= DPUMESH_SLOT_SIZE) */
	uint32_t pos;         /* buffer offset (forward: DPU dpu_buf; reverse: Host RX) */
	uint32_t generation;  /* source pod's dma_generation */
};
/* Sent as immediate via doca_dpa_dev_comch_producer_dma_copy() — HW max 32 bytes.
 * 20B remains below that limit. Routing carries nothing else on this wire: the
 * ARM pins a conn to its backend. */
_Static_assert(sizeof(struct comch_dma_comp_msg) == 20,
               "comch_dma_comp_msg must be exactly 20 bytes");
_Static_assert(offsetof(struct comch_dma_comp_msg, type) == 0,
               "comch_dma_comp_msg.type must be at offset 0 (recv-cb peeks raw[0])");
/* src/dst_pod_id travel as int8 on the wire (dst==-1 = DMESH_POD_BLANK, the
 * unresolved-destination sentinel), so pod_id must fit in int8. Fail-fast if
 * MAX_PODS ever outgrows that. */
_Static_assert(MAX_PODS <= 127,
               "pod_id wire format is int8 in comch_dma_comp_msg; MAX_PODS must be <= 127");

typedef uint64_t doca_dpa_dev_completion_t;
typedef uint64_t doca_dpa_dev_comch_producer_t;

struct comch_add_ring_msg {
	enum dpa_msg_type type;
	uint32_t generation;
	struct dpa_ring_info ring;
} __attribute__((__packed__, aligned(8)));

struct comch_msg {
	enum dpa_msg_type type;
	union
	{
		struct comch_dma_comp_msg dma_comp_msg;
		struct comch_add_ring_msg add_ring_msg;
	};
} __attribute__((__packed__, aligned(4)));

/* These structs cross the host(x86) / DPU-ARM / DPA-EU toolchain boundary
 * (dpa_ring_info is the RING_ADD/REV_RING_ADD payload and is also h2d_memcpy'd
 * inside dpa_thread_arg; comch_msg is the configured msgq imm_data_len). Lock
 * their layout so any ABI drift between toolchains fails the build instead of
 * silently corrupting the wire. */
_Static_assert(sizeof(struct dpa_ring_info) == 48, "dpa_ring_info ABI drift");
_Static_assert(sizeof(struct comch_add_ring_msg) == 56, "comch_add_ring_msg ABI drift");
_Static_assert(offsetof(struct comch_add_ring_msg, ring) == 8, "comch_add_ring_msg.ring offset drift");
_Static_assert(sizeof(struct comch_msg) == 60, "comch_msg ABI drift");

/* ====== DMA ring descriptor ====== */

/* The shared ring contains descriptor slots, one RX-credit slot, and one
 * consumer-head control slot. */
#define DMA_RING_EXTRA_SLOTS       2u
#define DMA_RING_CREDIT_SLOT(size) (size)
#define DMA_RING_CTRL_SLOT(size)   ((size) + 1u)

struct dma_ring_ctrl {
	volatile uint64_t consumer_head;
	uint8_t reserved[56];
} __attribute__((aligned(64)));

/* Exactly 64 bytes = one cache line per descriptor. */
struct dma_desc {
	doca_dpa_dev_mmap_t mmap;      /* 4B */
	uint64_t addr;                 /* 8B */
	uint32_t size;                 /* 4B (fixed width for Host/DPA ABI stability) */
		/* Endpoint tuple copied verbatim into the completion. */
		uint16_t seq;                  /* 2B per-conn sequence */
	uint16_t src_port;             /* 2B sender port */
	uint16_t dst_port;             /* 2B dest port (PORT_BLANK=0 -> accept queue) */
	int8_t   src_service;          /* 1B caller service (SVC_NONE if none) */
	int8_t   dst_service;          /* 1B callee service (routing input when dst_pod==BLANK) */
	int32_t dst_pod_id;            /* 4B routing target; DMESH_POD_BLANK(-1) -> DPU resolves dst_service */
	uint8_t pad0[4];               /* 4B: aligns src_pod_id, and holds the struct at its fixed
	                                * 64B cache line (see above) — widen it, never shrink. */
	int32_t src_pod_id;            /* 4B host sender field; DPA uses ring->pod_id */
	uint8_t reserved[20];          /* fixed descriptor padding */
	volatile uint64_t publish_seq; /* ticket+1; generation-safe MPSC publication */
} __attribute__((__packed__, aligned(8)));

/* Keep Host/DPA descriptor ABI stable across toolchains. */
_Static_assert(sizeof(struct dma_ring_ctrl) == 64, "dma_ring_ctrl must be 64 bytes");
_Static_assert(sizeof(struct dma_desc) == 64, "dma_desc must be 64 bytes");
_Static_assert(offsetof(struct dma_desc, addr) == 4, "dma_desc.addr offset mismatch");
_Static_assert(offsetof(struct dma_desc, size) == 12, "dma_desc.size offset mismatch");
_Static_assert(offsetof(struct dma_desc, seq) == 16, "dma_desc.seq offset mismatch");
_Static_assert(offsetof(struct dma_desc, dst_pod_id) == 24, "dma_desc.dst_pod_id offset mismatch");
_Static_assert(offsetof(struct dma_desc, src_pod_id) == 32, "dma_desc.src_pod_id offset mismatch");
_Static_assert(offsetof(struct dma_desc, publish_seq) == 56, "dma_desc.publish_seq offset mismatch");

#endif
