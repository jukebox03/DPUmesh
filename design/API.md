# DPUmesh Native API

This document specifies the public contract of `<dpumesh/dmesh.h>` in the
2026-07-19 working tree. The API resembles RDMA verbs in object lifecycle and
nonblocking completion semantics, but it is not a remote-memory API. It exposes
reliable, full-duplex byte streams addressed by service name.

## 1. Objects and ownership

```text
process
└─ dmesh_channel_t                 normally one
   ├─ dmesh_cq_t                  one per polling thread
   │  └─ dmesh_qp_t               streams connected or accepted by this CQ
   └─ registered TX/RX memory     owned by the channel
```

- `dmesh_channel_t` owns the DOCA device, Comch control path, exported memory,
  and transport progress thread.
- `dmesh_cq_t` owns a completion ready-list and eventfd. It is single-consumer;
  two threads must not poll the same CQ concurrently.
- `dmesh_qp_t` is one persistent full-duplex connection. The application may use
  `qp->user_data`; the transport does not read or modify it.
- `dmesh_wc_t` reports `RECV`, `RECV_FIN`, or `CONN_REQ`. A `RECV` buffer points
  into the channel RX mmap and remains valid only until its credit is released.

Create in channel → CQ → QP order and destroy in QP → CQ → channel order. The
library enforces reverse destruction with `EBUSY`. Public declarations use
`extern "C"`, so C++ can link the C implementation directly.

## 2. Connection and routing semantics

`dmesh_create_qp(cq, service_name)` resolves a Kubernetes Service name to an
internal service id. It creates a local object and performs no peer round trip.
There is no separate server `accept()` call: an inbound connection arrives as a
`DMESH_WC_CONN_REQ`, whose `qp` is immediately usable.

The default L4 passthrough mode pins a QP to one backend and preserves byte order.
With a service codec, replies on one QP may originate from distinct upstreams;
the application must reassemble fragments independently by `wc.stream`. The
gRPC/HTTP/2 integration uses L4 passthrough and therefore one upstream stream.

Identity is injected rather than chosen as an integer by the caller:

- `$DPUMESH_SERVICE` is this process's Service name; unset means client-only.
- `$DPUMESH_PORT` is the listen port used by the preload facade.
- `$DPUMESH_CONFIG` selects the registry; the default is `/etc/dpumesh/registry`.
- The DPU assigns the pod id during registration; `dmesh_pod_id()` only reads it.

## 3. API reference

### Channel

```c
dmesh_channel_t *dmesh_create_channel(void);
int dmesh_destroy_channel(dmesh_channel_t *ch);
int dmesh_pod_id(dmesh_channel_t *ch);
int dmesh_msg_max(dmesh_channel_t *ch);
int dmesh_post_max(dmesh_channel_t *ch);
```

`dmesh_create_channel()` returns only after this complete sequence succeeds:

```text
Comch connect → POD_ASSIGNED → K ring/mmap imports → DPA RING_ADD_ACK from every EU
              → ARM egress ready → POD_INIT_RESULT(READY)
```

It returns `NULL` if assignment or readiness exceeds 30 seconds or if any import,
DPA, or egress step fails. Partial resources are unwound. `dmesh_destroy_channel()`
returns `-1/EBUSY` without destroying anything while CQs remain. During graceful
shutdown it progresses the PE while waiting up to five seconds for
`POD_QUIESCED`, then destroys host exports.

`dmesh_msg_max()` is the largest body delivered in one RX completion, currently
8 KiB by default. Longer byte sequences arrive in multiple completions.
`dmesh_post_max()` is the largest contiguous alloc/post, normally 64 KiB.

### Completion queue

```c
dmesh_cq_t *dmesh_create_cq(dmesh_channel_t *ch);
int dmesh_destroy_cq(dmesh_cq_t *cq);
int dmesh_cq_fd(dmesh_cq_t *cq);
```

Destroying a CQ with bound QPs returns `EBUSY`. The eventfd returned by
`dmesh_cq_fd()` is an optional idle-sleep path. After wakeup, drain one `uint64_t`
and call `dmesh_poll_cq()` until it returns zero. A spin-polling application need
not use the fd.

### QP

```c
dmesh_qp_t *dmesh_create_qp(dmesh_cq_t *cq, const char *service_name);
int dmesh_destroy_qp(dmesh_qp_t *qp);
```

An unknown service returns `NULL/ENOENT`. All completions for a QP remain on the
CQ to which it was created or accepted. `dmesh_destroy_qp()` drops unflushed
bytes, sends FIN for an established connection, returns held RX credit, and frees
the object. A single poll batch may contain `CONN_REQ` followed by `RECV` entries
for the same QP. Defer QP destruction until the entire batch has been processed.

### TX

```c
void *dmesh_alloc(dmesh_qp_t *qp, uint32_t len);
int dmesh_post_send(dmesh_qp_t *qp, const void *buf, uint32_t len,
                    uint64_t wr_id, unsigned flags);
int dmesh_flush(dmesh_qp_t *qp);
#define DMESH_SEND_MORE 0x1u
```

`dmesh_alloc()` reserves a contiguous region in the registered TX ring. Fill that
region directly, then call `dmesh_post_send()` exactly once. Ownership transfers
to the transport at post time. `wr_id` is reserved for future send completions and
is currently ignored.

`dmesh_alloc()` never blocks:

| Result | Meaning | Required action |
|---|---|---|
| pointer | Reservation succeeded | Fill and post |
| `NULL/EAGAIN` | Per-QP inflight or shared-pool pressure | Progress other QPs/CQs, then retry |
| `NULL/EINVAL` | Zero/oversized length or unusable QP | Treat as a call or connection error |

The API has no notification that TX capacity became writable. A reactor must keep
one pending write per QP, continue CQ progress, and retry on a short timer. The
C++ gRPC adapter defaults to 50 microseconds. This is correct but remains a
performance and idle-CPU limitation.

`DMESH_SEND_MORE` commits a message while deferring the shared descriptor
doorbell. A final post without the flag, or `dmesh_flush()`, ships committed
messages in order. `flush()` cannot exhaust the already reserved per-QP units, but
it may briefly back off on a saturated host-to-DPU descriptor ring and returns
`EBADMSG` if its bounded deadline expires.

### Completion and RX credit

```c
int dmesh_poll_cq(dmesh_cq_t *cq, dmesh_wc_t *wc, int nwc);
void dmesh_wc_release(dmesh_channel_t *ch, dmesh_wc_t *wc);
```

`dmesh_poll_cq()` is nonblocking and returns at most `nwc` entries. It preserves
per-QP order. If the batch ends in the middle of a QP's ready list, the next call
resumes that QP first.

| Opcode | Meaning | Buffer and credit |
|---|---|---|
| `DMESH_WC_CONN_REQ` | New inbound QP | None |
| `DMESH_WC_RECV` | One RX fragment | `wc.buf`, `wc.len`; release is mandatory |
| `DMESH_WC_RECV_FIN` | Peer EOF | None |

`dmesh_wc_release()` is idempotent and remains valid after the QP closes. Copy RX
bytes into application-owned memory before release if they must outlive the call.
The gRPC adapter follows this baseline: it copies into gRPC slices and immediately
returns the DPUmesh credit.

### Diagnostics

```c
void dmesh_get_tx_stats(dmesh_channel_t *ch, dmesh_tx_stats_t *out);
```

The cumulative fields are `pool_grabs`, `pool_returns`, `recycle_hits`,
`grow_waits`, and `block_pads`. `grow_waits` is the observable count of actual
`dmesh_alloc()` backpressure. Record the before/after delta in performance tests.

## 4. Minimal reactor pattern

```c
dmesh_channel_t *ch = dmesh_create_channel();
dmesh_cq_t *cq = dmesh_create_cq(ch);
dmesh_qp_t *qp = dmesh_create_qp(cq, "echo-dpumesh");

void *p;
while ((p = dmesh_alloc(qp, len)) == NULL && errno == EAGAIN) {
    dmesh_wc_t wc[64];
    int n = dmesh_poll_cq(cq, wc, 64);
    for (int i = 0; i < n; ++i) {
        if (wc[i].opcode == DMESH_WC_RECV) dmesh_wc_release(ch, &wc[i]);
    }
}
memcpy(p, body, len);
if (dmesh_post_send(qp, p, len, 0, 0) != 0) /* close qp */;

dmesh_destroy_qp(qp);
dmesh_destroy_cq(cq);
dmesh_destroy_channel(ch);
```

A real reactor must also handle connection requests, FIN, fragmented messages,
per-QP pending writes, and post-batch close. Reference implementations are
`integrations/grpc/src/dmesh_reactor.*` and `bench/apps/echo_dpumesh.c`.

## 5. POSIX facade

`libdmesh_preload.so` is a separate public facade. To preserve
`socket/connect/listen/accept/read/write` behavior, it hides native zero-copy and
performs the copies and partial-read bookkeeping required by POSIX. Destinations
absent from the registry remain on kernel TCP. Native and preload are sibling
facades over `src/dmesh_core.c`; neither is implemented on top of the other.
Because the Go runtime does not consistently route network operations through
libc, LD_PRELOAD is not the Go integration strategy.

## 6. Support boundary

- The transport is a reliable ordered byte stream, not a complete implementation
  of every POSIX socket option.
- There are no send completions; protocol acknowledgements reclaim TX capacity.
- DPUmesh does not terminate TLS. gRPC TLS ciphertext is transported unchanged.
- Graceful shutdown is protected by the remote-reclaim barrier. Full containment
  of in-flight DMA across SIGKILL or host failure remains production work.
- The registry loads once at first use. Live reload is not implemented.
