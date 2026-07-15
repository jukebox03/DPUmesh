# DPUmesh API — Whitepaper (user-facing)

A service-mesh **data plane** on NVIDIA DOCA (Comch + DMA). The transport runs on the
**BlueField DPU/DPA**, not the host CPU, so your application keeps its full host core (no
in-host sidecar tax).

The DPU is an **L7-style proxy that owns every connection** (think Envoy): your app addresses a
**`service_id`** — never a pod — and the DPU **load-balances across that service's backend pods**
(round robin), owns the connection to the chosen backend, and maps every reply back to you. **A
connection is sticky by default** — it keeps the backend the LB first picked (session affinity);
a service can opt into **per-message** load balancing instead (§5). Bodies move by DMA **host →
DPU → host**; the DPU touches only metadata unless a service selects the **L7 body-parsing hook**
(§8).

## Two surfaces, and only two

| | | for |
|---|---|---|
| **Native API** — `<dpumesh/dmesh.h>` | RDMA-verbs-shaped. **Zero-copy send AND receive.** No memcpy, no syscall, no thread on the data path. | new code |
| **POSIX socket ABI** — `libdmesh_preload.so` | `LD_PRELOAD`. You never call it; it impersonates libc (§7). | **unmodified** binaries |

Both sit directly on the internal core (`src/dmesh_core.h`, not installed). **Neither is built
on the other.**

**Why verbs-shaped and not socket-shaped — it is faster, not just more familiar.** DPUmesh's
substrate already *is* RDMA-shaped: a pre-registered mmap, a DMA engine, credit admission,
completions. `read(2)` semantics **mandate** a copy into caller memory, so a socket-shaped API
cannot be zero-copy. That copy is the price of the POSIX contract — and only the shim should pay
it.

---

## 1. Concepts — three handles

| Handle | What it is | ibverbs analogue | Lifetime / count |
|---|---|---|---|
| **`dmesh_channel_t`** | Your process's one link to the DPU: DOCA device, PE thread, TX/RX buffers, the port table. | `ibv_context` + `ibv_pd`, folded | **one per process**; heavy |
| **`dmesh_cq_t`** | A completion queue + its own ready list + its own event fd. **Single-consumer: poll from ONE thread.** | `ibv_cq` + comp_channel | **one per thread** |
| **`dmesh_qp_t`** | A logical stream. A **client** QP (`dmesh_create_qp`) addresses a service; a **server** QP arrives as a `DMESH_WC_CONN_REQ` completion. | `ibv_qp` | cheap; reused |

> `dpumesh_ctx_t` (the DOCA engine) is hidden inside the channel — **you never touch it.**

**One CQ per thread, exactly as in verbs.** A CQ is single-consumer; that is not a limitation,
it is *why you make several*. Each CQ owns its ready list and fd, and a QP belongs to the CQ that
created or accepted it — so N threads with N CQs receive genuinely in parallel, sharing nothing
on the RX path. One CQ for the whole process serializes every thread behind one ready list.

Inbound conns need no machinery: the accept queue is channel-wide and **multi-consumer**, so
every CQ may poll it and whichever accepts a QP owns it thereafter — SO_REUSEPORT-style
distribution for free.

### A QP is host ↔ DPU, addressed to a service

- **`dmesh_create_qp(cq, service)` is local** — no round-trip, **no pod chosen**. It is
  `ibv_create_qp`, not `rdma_connect`: it allocates queues and records "this QP talks to service
  S." Every post ships a message the DPU routes to a backend.
- The **client never learns or pins a pod.** The DPU owns the connection to each backend and
  correlates replies back to you.
- On the backend, the DPU delivers the message as a **new connection**: a `DMESH_WC_CONN_REQ`
  completion, immediately followed by that QP's already-landed messages. The backend is a
  **plain server** — it replies to its peer (which the DPU manages) and destroys the QP. It
  never sees or addresses the real client.
- **Full-duplex** once a message has been received. (A server QP can only reply to its peer, so
  it must receive before it can send.)
- **Teardown:** `dmesh_destroy_qp()` sends a **FIN** (zero-length, riding behind all prior
  data); the peer gets a `DMESH_WC_RECV_FIN` completion and destroys its side; the DPU frees the
  upstream.

```c
dmesh_channel_t *ch = dmesh_create_channel(DMESH_SVC_NONE);  // pure client
dmesh_cq_t      *cq = dmesh_create_cq(ch);                   // one per thread
dmesh_qp_t      *qp = dmesh_create_qp(cq, /*service*/11);    // address a service, not a pod

void *b = dmesh_alloc(qp, len);          // a pointer INTO the TX ring — fill in place
if (b) { fill(b, len); dmesh_post_send(qp, b, len, 0, 0); }

dmesh_wc_t wc[16];
int n = dmesh_poll_cq(cq, wc, 16);       // wc[i].buf points INTO the RX mmap
/* ... handle ... */ dmesh_wc_release(ch, &wc[i]);

dmesh_destroy_qp(qp);                    // sends a FIN so the DPU frees the upstream
```

---

## 2. How it differs

### From ibverbs — the object mapping

| ibverbs | DPUmesh |
|---|---|
| `ibv_open_device` + `ibv_alloc_pd` | `dmesh_create_channel` (folded — see below) |
| `ibv_create_cq` (+ comp_channel) | `dmesh_create_cq` (+ `dmesh_cq_fd`) |
| `ibv_create_qp(pd, {send_cq, recv_cq})` | `dmesh_create_qp(cq, service)` |
| RC (connected, ordered) | `dmesh_create_qp` + `dmesh_pin_route` |
| service-addressed, reliable, datagram-ish | `dmesh_create_qp` alone — closest to IB **RD** or Mellanox **DC**. *Not* UD (unreliable, needs an AH). |
| `rdma_cm` CONNECT_REQUEST | `DMESH_WC_CONN_REQ` completion |
| `ibv_post_send` (+ WR list) | `dmesh_post_send` (+ `DMESH_SEND_MORE`) |
| `ibv_poll_cq` | `dmesh_poll_cq` |
| SRQ recv-buffer repost | `dmesh_wc_release` |
| `ibv_req_notify_cq` | `dmesh_cq_fd` + drain-then-poll |

**Deliberate deviations, stated rather than hidden:**
- **No one-sided READ/WRITE, no `rkey`.** Arbitrary remote-address translation needs a lookup
  table on the DPU datapath — the one function a real RDMA NIC implements in hardware. Out of
  scope by design.
- **No `ibv_reg_mr`.** The DPU DMAs only from the mmap exported at init. `dmesh_alloc` **is** the
  registered-buffer allocator.
- **context + PD folded into the channel.** PD's job is to scope MRs; with no arbitrary
  `reg_mr` there are no MRs to scope. **The CQ is *not* folded in** — it is the one object
  carrying a concurrency constraint.
- **No send completions.** Unsignaled is the verbs default and the high-performance idiom; here
  it is the only mode, so the signal that SQ space freed is simply that a later `dmesh_alloc`
  succeeds.

### From BSD sockets

| | BSD socket | DPUmesh |
|---|---|---|
| **Address** | IP:port (one peer) | **`service_id`** — the DPU routes it; no IP/DNS; self-routing OK |
| **Load balancing** | none | round-robin across the service's backends; **sticky per connection** by default |
| **Setup** | 3-way handshake | **none** — local; peer liveness is *not* checked (apply your own timeout) |
| **Send** | `write()` copies into a kernel buffer | `dmesh_alloc` → fill **in place** → `dmesh_post_send`. **Zero copy.** |
| **Receive** | `read()` copies into your buffer | completion carries a pointer **into the RX mmap**. **Zero copy** + `dmesh_wc_release`. |
| **Framing** | byte stream | a post ≤ `dmesh_msg_max` arrives as **exactly one** RECV; larger arrives as several in order — frame your own length |
| **Ordering** | in-order | in-order on a sticky/pinned QP; **none** under per-message LB → correlate by req-id |
| **RPC matching** | n/a | **none** — the transport delivers, you correlate |
| **EOF** | `read()==0` | `DMESH_WC_RECV_FIN` completion |
| **Backpressure** | blocks, or `EAGAIN` + `EPOLLOUT` | `dmesh_alloc` returns `NULL`+`EAGAIN`; **never blocks** |
| **Readiness** | one fd per connection | **one fd per CQ** + a ready list — no per-conn fd, no scan |

**Must-follow rules**
- **One CQ, one thread.** Poll a CQ from its owning thread only.
- **Release every RECV completion** (`dmesh_wc_release`) — credits bound RX admission.
- **Handle `dmesh_alloc` → `NULL`+`EAGAIN`**: do other work and retry. `EINVAL` is permanent.
- **Poll to 0** before sleeping on `dmesh_cq_fd` again (edge-triggered, like `ibv_req_notify_cq`).
- **Exactly one `dmesh_destroy_qp` per QP.** It **frees** the QP — and `poll_cq` can emit
  CONN_REQ plus that QP's landed messages in **one batch**, so destroying mid-batch dangles
  later `wc[]` entries. Defer destroys to a sweep after the batch is dispatched.
- **`service_id` must be live** — a dead/unregistered service is *not* detected; the message is
  dropped at the DPU and nothing comes back. Apply your own wall-clock timeout.
- **`slot_size` ≤ 8192** — the DPA `dma_copy` limit; leave `DPUMESH_SLOT_SIZE` at its default.

---

## 3. API reference — 16 calls

### Channel — `ibv_open_device` + `ibv_alloc_pd`
| Function | Returns / errno |
|---|---|
| `dmesh_channel_t *dmesh_create_channel(int service_id)` | Channel, or `NULL` on init failure. `service_id` = the service this node advertises (`DMESH_SVC_NONE` for a pure client). **The node's `pod_id` is assigned by the DPU** — read it back with `dmesh_pod_id()`. Blocks briefly on the register round-trip. |
| `void dmesh_destroy_channel(dmesh_channel_t *s)` | Releases all DOCA resources. Safe on `NULL`. |
| `int dmesh_pod_id(dmesh_channel_t *s)` | This node's DPU-assigned `pod_id`. |
| `int dmesh_msg_max(dmesh_channel_t *s)` | Max length arriving as **ONE** RECV completion (`slot_size`, 8 KB). |
| `int dmesh_post_max(dmesh_channel_t *s)` | Max length of one `alloc`/`post` (`block_size`, 64 KB). |

### CQ — `ibv_create_cq`
| Function | Returns / errno |
|---|---|
| `dmesh_cq_t *dmesh_create_cq(dmesh_channel_t *ch)` | A completion queue with its own ready list + event fd. **Make one per thread.** Readiness is live immediately. |
| `void dmesh_destroy_cq(dmesh_cq_t *cq)` | Destroy its QPs **first** (the `ibv_destroy_cq` EBUSY rule). |
| `int dmesh_cq_fd(dmesh_cq_t *cq)` | The completion-channel fd, for vanilla epoll/poll/select. **Purely optional** — the idle-sleep path, never a prerequisite: `dmesh_poll_cq` is complete on its own, so a spin-polling client need not call this. |

### QP — `ibv_create_qp` / `rdma_disconnect`
| Function | Returns / errno |
|---|---|
| `dmesh_qp_t *dmesh_create_qp(dmesh_cq_t *cq, int dst_service_id)` | New **client** QP bound to a service (the id the backend passed to `dmesh_create_channel`) and to `cq`. Local, no round-trip; no pod chosen or learned. `NULL`+`ENOMEM`. |
| `void dmesh_pin_route(dmesh_qp_t *qp)` | **Pin to ONE backend** (RC-like). Every later message (and the FIN) carries one route-affinity key, so the DPU routes them all to the backend picked for the **first** — replies then arrive **in send order**. Call right after create, before any post; idempotent; no-op on a server QP. Keys are a per-channel rolling 255-id space scoped **by destination service**: two pinned QPs to the *same* service may share a backend (balance skew, never a correctness issue); cross-service redirection is impossible. |
| `int dmesh_destroy_qp(dmesh_qp_t *qp)` | **Graceful.** Sends a **FIN** (zero-length, behind all prior data) so the peer gets `DMESH_WC_RECV_FIN` and the DPU frees the upstream; then frees local state. Posted-but-unflushed bytes are discarded (flush first). Returns `0`; safe on `NULL`. |

### Send — `ibv_post_send`
| Function | Returns / errno |
|---|---|
| `void *dmesh_alloc(dmesh_qp_t *qp, uint32_t len)` | A pointer to **`len` contiguous bytes** in the QP's pre-registered TX ring — the DMA source; fill it directly. **NEVER BLOCKS.** `NULL`+`EAGAIN` = SQ full (retry later; a TX_ACK will free space). `NULL`+`EINVAL` = `len==0`, `len > dmesh_post_max()`, or the QP is not established. There is no separate commit — `post_send` finalizes. |
| `int dmesh_post_send(dmesh_qp_t *qp, const void *buf, uint32_t len, uint64_t wr_id, unsigned flags)` | Post the buffer from the QP's most recent `dmesh_alloc`; `len` ≤ that alloc's length. `wr_id` reserved, ignored. `flags`: `DMESH_SEND_MORE` defers the doorbell (the WR-list idiom) — the next post without it, or a `dmesh_flush`, ships everything in order. `0`, or `-1` `EINVAL` / `EBADMSG` (descriptor fault → destroy the QP). **Ownership transfers at post; do not touch `buf` after.** |
| `int dmesh_flush(dmesh_qp_t *qp)` | Ring the doorbell for everything posted with `DMESH_SEND_MORE`. Cannot run out of *per-QP* queue space (alloc reserved the slot), but **may back off briefly on the shared host→DPU descriptor ring** — a global resource that saturates only if the DPA falls behind. A ring wedged past its deadline fails `EBADMSG` rather than hanging. |

### Completions — `ibv_poll_cq`
| Function | Returns / errno |
|---|---|
| `int dmesh_poll_cq(dmesh_cq_t *cq, dmesh_wc_t *wc, int nwc)` | Harvest up to `nwc` completions. **Non-blocking**; returns the count (`0` = drained) or `-1`+`EINVAL`. Single-consumer. Delivers in priority: the resumed partially-drained QP from the previous call, then new QPs (one `CONN_REQ` each, immediately followed by that QP's already-landed messages), then every ready QP's inbox drained to empty. A QP cut off by `wc[]` filling resumes **first** next call, so per-QP order always holds and no message is lost. |
| `void dmesh_wc_release(dmesh_channel_t *s, dmesh_wc_t *wc)` | Return a RECV completion's RX credit (≈ reposting to an SRQ). Idempotent; a no-op for FIN/CONN_REQ. Valid after the QP is destroyed — the credit is **channel**-level. |

```c
typedef enum { DMESH_WC_RECV = 1, DMESH_WC_RECV_FIN = 2, DMESH_WC_CONN_REQ = 3 } dmesh_wc_opcode_t;

typedef struct dmesh_wc {
    dmesh_qp_t       *qp;      /* use qp->user_data for your per-QP context */
    dmesh_wc_opcode_t opcode;
    const uint8_t    *buf;     /* RECV: points INTO the RX mmap (zero-copy) until wc_release */
    uint32_t          len;     /* RECV: message length (<= msg_max) */
    int32_t           rx_slot; /* internal release token */
} dmesh_wc_t;
```

### Diagnostics
| Function | Returns |
|---|---|
| `void dmesh_get_tx_stats(dmesh_channel_t *s, dmesh_tx_stats_t *out)` | Elastic TX pool counters: `pool_grabs`, `pool_returns`, `recycle_hits`, `grow_waits`, `block_pads`. Every field counts a **rare** path — steady state increments nothing but `recycle_hits`. **`grow_waits` is the observable counterpart of `EAGAIN`**: it is how you find out whether your send loop ever actually hits backpressure. The ring grows on demand, so non-zero means the QP reached its in-flight cap, not that memory ran out. |

### Accessor
- `void *qp->user_data` — **app-owned** (like a verbs `qp_context`): set it, and every completion
  hands the QP back so you read your context off it. The transport never touches it.

### Backpressure, in one paragraph
`dmesh_alloc` never sleeps, so **one backpressured QP can never stall the others on your
thread**. On `EAGAIN`: go do other work and retry — a later alloc succeeds once the DPU's
TX_ACKs free ring space. The TX ring is an **elastic block chain** that grows on demand
(`DPUMESH_TX_BLOCK` 64 KB blocks, up to `DPUMESH_TX_MAXB` = 4 → ≤ 256 KB in flight per QP), so
`EAGAIN` means the QP hit its in-flight ceiling, not that memory ran out. Measured on this
deploy, `grow_waits` is **0** — backpressure has not occurred in practice.

### Large payloads (> `msg_max`) — no special API
A post up to `dmesh_post_max` (64 KB) is legal and arrives as **several** ≤ `msg_max`
completions, in order on a server/pinned QP; the receiver reassembles (app framing, as with a
byte stream). Beyond `post_max`, loop: alloc + post per chunk. To a service that
load-balances across **several** backends, `dmesh_pin_route(qp)` first so the chunks stay on one
backend and in order.

---

## 4. Examples

### 4a. Backend server — a plain server behind the DPU proxy
`bench/echo_dpumesh.c`. The DPU creates connections to you and routes clients' messages here.
One fd per CQ; on wake, drain the CQ to 0.

```c
#include <dpumesh/dmesh.h>
#include <sys/epoll.h>

int main(void) {
    dmesh_channel_t *ch = dmesh_create_channel(/*service_id*/11);   // this backend serves svc 11
    dmesh_cq_t      *cq = dmesh_create_cq(ch);
    int fd = dmesh_cq_fd(cq);

    int epfd = epoll_create1(0);                                    // ── vanilla kernel epoll ──
    struct epoll_event ev = { .events = EPOLLIN, .data = { .fd = fd } };
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

    for (;;) {
        struct epoll_event out[8];
        epoll_wait(epfd, out, 8, -1);                               // SLEEP until activity
        uint64_t cnt; while (read(fd, &cnt, sizeof cnt) > 0) {}     // drain the counter

        dmesh_wc_t wc[64]; int n;
        while ((n = dmesh_poll_cq(cq, wc, 64)) > 0)                 // poll to 0
            for (int i = 0; i < n; i++) switch (wc[i].opcode) {
            case DMESH_WC_CONN_REQ:                                 // new conn, already usable
                break;
            case DMESH_WC_RECV: {                                   // echo it back, zero-copy
                void *b = dmesh_alloc(wc[i].qp, wc[i].len);
                if (b) { memcpy(b, wc[i].buf, wc[i].len);           // RX mmap -> TX ring
                         dmesh_post_send(wc[i].qp, b, wc[i].len, 0, 0); }
                dmesh_wc_release(ch, &wc[i]);                       // ALWAYS release
                break; }
            case DMESH_WC_RECV_FIN:                                 // peer closed
                dmesh_destroy_qp(wc[i].qp);
                break;
            }
    }
}
```
> `dmesh_alloc` can return `NULL`+`EAGAIN`. A real server parks the body and retries from the
> loop rather than dropping it — see `bench/validators/loopback_dpumesh.c`.

### 4b. Client — request/response, single-outstanding
With **one** request outstanding, the reply that arrives IS this request's reply — no
correlation needed.
```c
dmesh_channel_t *ch = dmesh_create_channel(DMESH_SVC_NONE);   // pure client
dmesh_cq_t      *cq = dmesh_create_cq(ch);
dmesh_qp_t      *qp = dmesh_create_qp(cq, /*service*/11);

for (int i = 0; i < N; i++) {
    void *b;
    while (!(b = dmesh_alloc(qp, len)))                       // EAGAIN: SQ full, keep the loop alive
        { if (errno != EAGAIN) goto dead; dmesh_poll_cq(cq, wc, 64); }
    fill(b, len);
    dmesh_post_send(qp, b, len, 0, 0);

    for (int got = 0; !got; ) {                               // await the reply
        dmesh_wc_t wc[16];
        int n = dmesh_poll_cq(cq, wc, 16);
        for (int j = 0; j < n; j++) {
            if (wc[j].opcode == DMESH_WC_RECV) { handle(wc[j].buf, wc[j].len); got = 1; }
            dmesh_wc_release(ch, &wc[j]);
        }
    }
}
dead:
dmesh_destroy_qp(qp);
```

### 4c. Pipelined — correlate by req-id
Fire many without waiting. Under **per-message** LB (opt-in, §5) replies on one QP can arrive
**out of order** — put a **req-id in the body** and match on it, never on arrival order. On a
sticky (default) or `dmesh_pin_route`'d QP replies stay in send order.
```c
for (uint32_t id = 0; id < N; id++) {                         // fire N, no waiting
    msg_t *m = dmesh_alloc(qp, sizeof *m);
    if (!m) { /* EAGAIN: drain the CQ, retry this id */ continue; }
    m->req_id = id; fill(m->payload);
    dmesh_post_send(qp, m, sizeof *m, 0, DMESH_SEND_MORE);    // batch the doorbell
}
dmesh_flush(qp);                                              // one doorbell for all of them

while (got < N) {                                             // harvest, matched by req_id
    dmesh_wc_t wc[64];
    int n = dmesh_poll_cq(cq, wc, 64);
    for (int i = 0; i < n; i++) {
        if (wc[i].opcode == DMESH_WC_RECV)
            { const msg_t *r = (const msg_t *)wc[i].buf; on_reply(r->req_id, r); got++; }
        dmesh_wc_release(ch, &wc[i]);
    }
}
```
> Many QPs at once? Set each `qp->user_data` to its context — every completion hands the QP
> back, so there is no scan and no conn table.

### 4d. Scaling to N threads
One CQ per thread, QPs created on that thread's CQ, each thread polls only its own CQ. Nothing
is shared on the RX path. New inbound conns distribute themselves: every CQ may poll the
channel-wide accept queue, and whichever accepts a QP owns it.
```c
void *worker(void *arg) {
    dmesh_cq_t *cq = dmesh_create_cq(g_ch);      // ── this thread's own CQ ──
    dmesh_qp_t *qp = dmesh_create_qp(cq, 11);    // its QPs live on it
    for (;;) { /* post / poll_cq(cq, …) — no locks, no sharing */ }
}
```

---

## 5. Addressing & routing model

- **The DPU owns every connection.** A client addresses a **service** (a cluster of backend
  pods); the DPU **round-robin load-balances** across the live backends, owns the "upstream"
  connection to the chosen one, and maps the reply back. The client never sees or names a pod.
- **Sticky by default; per-message is opt-in.** A connection keeps the backend the LB first
  picked (session affinity — replies stay in send order). This is Envoy's TCP-proxy behavior.
  `DPUMESH_LB_PER_REQUEST_SVC=<csv>` (§8) marks services that load-balance **every** message
  instead (Envoy's HTTP default). The shim additionally pins each conn (§7) for the socket
  contract.
- **Backend set is live (no blackhole).** Derived from the live pods: a backend that registers
  or disconnects is added/removed automatically; a pinned backend that dies is re-picked. New
  traffic never routes to a dead backend.
- **How it stays a connection.** The DPU assigns each upstream a private id `uP` and rewrites
  the message so the backend sees a connection from *the proxy* `(client_pod, uP)`, demuxes and
  replies by `uP`; the reply returns to the DPU, which maps `uP → (client, client_port)` and
  delivers it on your client QP. Invisible to both apps.
- **Routing granularity is one whole message.** The DPU makes **one** routing decision per
  message and delivers it to **exactly one** backend — a message cannot be split across
  destinations. A wire message is atomic at **≤ 8 KB**; there is no transport concept of a
  message spanning slots.
- **Route-affinity keeps a multi-slot post's chunks together.** Each wire message carries a
  one-byte **route-affinity key**: the DPU pins every message sharing a non-zero key to the
  **same** backend (a small `(dst_service, key) → backend` table). Key `0` = normal per-message
  LB. `dmesh_pin_route` stamps one key on a QP's whole life. Chunks reach the ARM **in send
  order** (a QP's messages are conn-sharded onto ONE forward ring, `src_port % K`, so they stay
  FIFO — the head chunk arrives first, a property a future L7 parser relies on); the table is
  nevertheless **overwrite-on-reuse**, so correctness never depends on that ordering. It is
  **collision-safe**: pins are scoped by destination service, so a shared key can only merge
  same-service traffic onto one backend (skew, still reassembling) — never a cross-service
  redirect. (An `is_last`-DELETE freeing the pin per message was considered and **rejected**:
  under either hazard it can free a pin mid-message and scatter the chunks.)
- **L7 routing (body-aware).** The byte-stream proxy engine that ships every reply is **always
  on** (§8); its default parser (`passthru`) routes on **metadata only** and never reads a body.
  To make the DPU an Envoy-like L7 proxy that parses the stream and content-routes, select the
  L7 parser for a service (`DPUMESH_PROXY_L7_SVC`, §8).
- **Response↔request matching is the app's job.** This DPU proxy routes at the **connection**
  level, not the request level (it never parses the body). Carry a req-id and match on it —
  especially because per-message LB can reorder replies (§4c).
- **Delivery & readiness.** Bodies DMA host→DPU→host straight into the receiver's RX buffer;
  only a small descriptor + a 20-byte completion ride the control path. The PE thread delivers
  each message to its QP's inbox and, on the inbox's empty→non-empty edge, publishes the QP to
  **its CQ's ready list** and wakes that CQ's fd. `dmesh_poll_cq` drains it — no per-conn fd, no
  scan.
- **Teardown (FIN).** `dmesh_destroy_qp` sends a zero-length FIN behind all prior data; the peer
  gets `DMESH_WC_RECV_FIN` and the DPU frees the upstream. A peer that crashes without a FIN
  leaves its conn + upstream allocated until that port/`uP` is reused (there is **no idle
  reaper**); apply your own wall-clock timeout.

---

## 6. Architecture — three pods exchanging messages

**Pod 10** is a client of **service 11**, which has two backends, **pod 11** and **pod 13**. The
DPU load-balances pod 10's messages across them and owns every connection.

```
   HOST pod 10  (client)                 BlueField DPU  (owns every connection)          HOST pod 11  (service 11)
   ═════════════════════                 ═══════════════════════════════════════         ══════════════════════════
   dmesh_channel_t  (1/process)          ┌── ARM control plane ──────────────────┐        dmesh_channel_t
    • dmesh_cq_t  (1 per thread)         │  svc 11 backends: { pod11, pod13 }    │         • ports[uP1] = server QP
    │   • ready list + event fd          │  lb_pick(11) → pod11 | pod13 (RR)     │         •   peer = (pod10, uP1)
    • ports[] (the QP table)             │                                       │         • RX buffer rx_dma_buffer
    │   [pC] = client QP → service 11    │  conntrack (the owned connections):   │         • PE thread
    • TX buffer  dma_buffer              │    upstream[uP1] = {pod10, pC, pod11} │
    │   elastic 64 KB blocks             │    upstream[uP2] = {pod10, pC, pod13} │        HOST pod 13  (service 11)
    • RX buffer  rx_dma_buffer           │    reuse (pod,port,backend) → uP      │        ══════════════════════════
    • PE thread  (RX → inbox → ready)    └───────────────────────────────────────┘         dmesh_channel_t
                                         ┌── DPA EUs (data plane) ───────────────┐          • ports[uP2] = server QP
                                         │  fwd dma_copy: host → DPU-staging     │          •   peer = (pod10, uP2)
                                         │  staging[pod]  (32 MB per pod)        │          • RX buffer + PE thread
                                         └───────────────────────────────────────┘

   descriptor posted per message (dma_desc, 64 B — the body is NOT in it, only a pointer + the tuple):
        { mmap, addr = &dma_buffer[off], size,
          src = (pod, port),  dst = (service, pod, port),  seq,  valid }     ← the "oriented tuple"
   completion returned per DMA (20 B on the control path): { type, src/dst pod, ports, seq, len, pos, route_group }
```

**One message — `pod10:pC → service 11`, LB'd to `pod 11`, then its reply:**
```
  FORWARD  (client request → backend)
  (1) host10  dmesh_alloc → fill in place;  post_send emits a dma_desc:
              src=(10, pC)   dst=(svc 11, BLANK, BLANK)   seq=s      ← dst_pod BLANK = "DPU, route me"
  (2) DPA EU  dma_copy  host10 dma_buffer  →  DPU staging[pod10]
  (3) ARM     dpu_route(11) → pod 11 ;  reuse or create upstream → uP1 ;  upstream[uP1]={10,pC,pod11}
              rewrite tuple → src=(10, uP1)  dst=(pod11, uP1)        ← backend will see the DPU id uP1
  (4) ARM     SG-DMA   DPU staging[pod10]  →  host11 rx_dma_buffer   (egress; in-place, no DPU CPU copy)
  (5) host11  PE: dst_port=uP1 not live → accept queue → DMESH_WC_CONN_REQ → server QP uP1
              app reads wc.buf straight out of the RX mmap
      ARM     TX_ACK to (pod10, pC, s)   ← uP1 translated back to pC → frees host10's ring bytes

  REPLY  (backend → its peer)
  (6) host11  alloc → post_send   src=(11, uP1)   dst=(pod10, uP1)   ← concrete dst → no LB, direct
  (7) DPA     dma_copy host11 → staging[pod11] ;  ARM SG-DMA staging → host10 rx_dma_buffer
  (8) ARM     dst_port=uP1 is an owned upstream → map uP1 → (pod10, pC) → rewrite dst
              TX_ACK to (pod11, uP1)  → frees host11's ring bytes
  (9) host10  PE: dst_port=pC → QP pC's inbox → pC's CQ ready list → dmesh_poll_cq → the reply
```
**Under per-message LB** (opt-in) pod 10's **next** message may route to **pod 13** instead — a
second upstream `uP2`, its replies mapping back to the same client QP `pC`. That is why replies
on `pC` can interleave and are matched by your **req-id**, not by order. By **default** (sticky)
`pC` keeps whichever backend its first message picked.

**Legend**
- **channel** — your process's single link to the DPU: DOCA device + PE thread + TX/RX buffers +
  the QP table.
- **cq** — a completion queue: its own ready list + event fd. One per thread.
- **qp** — an entry in `ports[]`: a **client** QP (addresses a service) or a **server** QP
  (created by the DPU, bound to a `uP`). Holds an inbox and an elastic TX block chain.
- **buffer** — **TX** `dma_buffer` (the DMA source, carved into 64 KB blocks) and **RX**
  `rx_dma_buffer` (where inbound bodies land) on each host; **DPU staging** per pod (the
  host→DPU→host hop; read in place, so the DPU never memcpy's).
- **uP** — the DPU-assigned upstream id: the backend's server-QP port, and the key the DPU maps
  ↔ `(client pod, client port, backend)` to route replies home.

---

## 7. Socket compatibility — `LD_PRELOAD` shim (`libdmesh_preload.so`)

Run an **unmodified, dynamically-linked POSIX socket application** over DPUmesh — no recompile,
no source change:

```sh
# backend: its listen(<port>) becomes a dmesh service listener
LD_PRELOAD=libdmesh_preload.so DMESH_PRELOAD_LISTEN=9095 DMESH_PRELOAD_SVC=15  ./my_server 9095
# client: connect() to a registry ClusterIP:port is routed over DPUmesh
LD_PRELOAD=libdmesh_preload.so DMESH_PRELOAD_REGISTRY=/etc/dpumesh/registry    ./my_client 10.96.0.15 9095
```

| env | meaning |
|---|---|
| `DMESH_PRELOAD_LISTEN=<port>` | `listen()` on this TCP port becomes the dmesh service listener |
| `DMESH_PRELOAD_SVC=<svc>` | the service this process advertises (required with `LISTEN`) |
| `DMESH_PRELOAD_REGISTRY=<file>` | control-plane registry, lines `ClusterIP:port svc`: `connect()` to a listed **ClusterIP:port** is routed to that service — the Envoy xDS/EDS equivalent (a static file today; controller-fed later). Keyed on IP:port, so same-port services on distinct ClusterIPs resolve apart. `getpeername()` then returns that ClusterIP:port. |
| `DMESH_PRELOAD_MAP=<port>=<svc>[,…]` | legacy port-only rule; superseded by `REGISTRY`'s ClusterIP key |
| `DMESH_PRELOAD_DEBUG=1` | per-connection diagnostics on stderr |

**The shim is a sibling of the native API, not a client of it.** It sits directly on the core
(`src/dmesh_core.h`) because it needs QP internals and the internal lifecycle. It also **owns the
POSIX byte-stream semantics** — `read()`'s copy + partial-consumption cursor, `write()`'s copy +
arbitrary-length carve — because `read(2)` mandates a copy and `write(2)` mandates any length.
That is precisely why the native path is zero-copy: the cost lives only where the contract
demands it.

**How it works.** When a socket becomes dmesh-backed, the shim `dup2()`s a private eventfd over
the app's fd number — the fd stays a **real kernel fd**, so `epoll`/`poll`/`select`/`close`/`dup`
work natively (kernel-TCP fds and dmesh fds mix freely in one epoll set). A dispatcher thread
(the single consumer of its one CQ) asserts per-fd readability. Reads are byte-stream (short
reads at message boundaries, exactly like TCP); each `send()`/`write()` ships one message; any
length auto-chunks. Blocking sockets are emulated (`SO_RCVTIMEO` honored). Every client conn is
`dmesh_pin_route()`d, so one connection's traffic stays on **one backend** and replies arrive
**in order** — the socket contract; load is still balanced **across** connections.

```
  UNMODIFIED app                     libdmesh_preload.so (interposes libc)          DPUmesh
  ══════════════                     ════════════════════════════════════          ═══════
  connect(fd, ip:port) ─intercept──▶ ip:port ∈ registry?  dmesh_create_qp(svc) + pin_route
  listen(port)       ──intercept──▶  port == LISTEN?  advertise service SVC
        fd  ◀───────── dup2() a private eventfd OVER the app's fd number ──────────┐
        │            (fd stays a REAL kernel fd → epoll/poll/select/close/dup work  │
        │             natively; kernel-TCP fds and dmesh fds share one epoll set)   │
  epoll_wait(fd)   ── native kernel ──▶ readable ⇐ eventfd asserted                 │
  read(fd)/write(fd) ──intercept──▶ the shim's OWN byte-stream statics:             │
                                     copy out of the RX mmap / into the TX ring     │
   ┌── dispatcher thread: the sole consumer of the shim's ONE CQ. Per delivered ────┘
   │   message it writes the conn's eventfd → the app's next epoll/read sees the fd
   └── ready (re-buys per-conn-fd readiness the native API avoids — the transparency tax).
```

**Cost.** The shim deliberately re-buys the per-conn-fd readiness model the native API avoids
(one eventfd signal per message) **and** the two copies the native API does not pay. Measured
(1 KB round trips, thread-per-conn vanilla client, 2-core pod): ~7K RPS per connection
(closed-loop RTT-bound, p50 ~130 µs), scaling near-linearly to 16 conns, then a **plateau of
~150K RPS at 64 conns** — ~76% of the native ceiling on the same deploy. The price of
transparency, paid only by preloaded apps. Native callers are unaffected.

**Limits (v1).** AF_INET `SOCK_STREAM` only; most `SO_*` options are accepted no-ops;
`shutdown(SHUT_WR)` sends the FIN (no true half-close); no fork-shared sockets (DOCA is not
fork-safe); statically-linked or raw-syscall binaries (e.g. Go) bypass `LD_PRELOAD` entirely,
and so does **stdio** (`FILE*` via `fdopen` — glibc stdio calls its internal `__read`/`__write`,
not the interposable symbols). **`O_NONBLOCK` is not honored on the write path** — it blocks
rather than returning `EAGAIN`. Fixing that needs honest `EPOLLOUT`, which the eventfd cannot
express (an eventfd is always writable), so an app would livelock on epoll→write→EAGAIN. Gated
on `grow_waits` ever being non-zero; measured at **0**, so the path never executes.

**Validation.** `bench/validators/tcp_echo.c` (vanilla epoll echo) + `tcp_client.c` (vanilla
thread-per-conn blocking client; `SO_RCVTIMEO=5s` so a lost message is a counted failure, never
a hang) run the SAME binaries over kernel TCP and over DPUmesh (`bench.sh preload <N> <SIZE>
<CONNS>`): 0-fail across 1 KB–32 KB and 1–256 connections — including 256-conn
simultaneous-connect storms and idle→storm cycles — p50 ~120–150 µs.

---

## 8. L7 proxy — byte-stream reframing (`DPUMESH_PROXY` = parser selector)

DPU→host egress is **always** this per-conn byte-stream engine — the sole reverse path. The
forward DMA lands a connection's bytes in DPU staging; an L7 function reframes the stream into
**routing segments**; the engine ships each to its backend by **scatter-gather DMA** (ARM). The
backend receives a byte stream and frames it **itself** (an ordinary server behind an envoy).
`DPUMESH_PROXY` does **not** toggle the engine — it selects the deploy-default **request
parser**. Replies always pass through (their dst is the conntrack peer).

**What an L7 author writes — the routing decision (`dpu_l7.h`):**
```c
struct dmesh_l7_ctx      { int32_t service, client_pod; uint16_t client_port;
                           const int32_t *hosts; int32_t n_hosts; };  // the cluster's live backend pods
struct dmesh_l7_decision { uint32_t total_len; int32_t cluster; int32_t host; };

// You implement this in dpu_l7.c. Shown the message HEAD (bounded window) + the cluster's live hosts.
int dmesh_l7_route(const uint8_t *head, uint32_t len,
                   const struct dmesh_l7_ctx *ctx, struct dmesh_l7_decision *out);
//  return >0 : decided (fill out).  0 : head not fully here (grow).  <0 : malformed (poison).
//  out.total_len : whole message length (body streams from staging via SG — never linearized)
//  out.cluster   : service to route to (default = ctx->service; overwrite to content-route)
//  out.host      : DMESH_LB_DEFER (engine load-balances the cluster) or a ctx->hosts pod (override)
```

**The L4 engine (request AND reply run the SAME machinery):**
```
 forward DMA lands the body in DPU staging (in place)
   │
   ▼  per-conn INPUT WINDOW ── bytes in arrival order; zero-copy views over staging + a cursor.
   │   A SEAM buffer aligns the unconsumed tail into ONE contiguous run only when a parse stalls
   │   across staging extents (e.g. a >8 KB frame split across arrivals).
   ▼  parser (passthru / frame / L7 hook) → segs {off,len,dst}   (consumed = how far the cursor advances)
   ▼  per-(dst pod, region) LANE ── segments to one backend gathered into ONE chained-buf SG-DMA
   │   (ARM generic doca_dma: staging → the backend's host RX buffer) + one batched notify
   ▼  BYTE-STREAM delivery ── the backend frames itself; per receiving conn, delivery order =
   │   segment order (lane FIFO).
   └─ CUSTODY ── the sender's ring bytes are held until the egress DMA has READ its staging bytes,
      then a batched TX_ACK frees them (never released early — the host would overwrite mid-read).
```

**Key properties**
- **Byte stream both ends.** Message boundaries are the L7 parser's business — the app may pack
  several frames into one post, or let a big post auto-chunk, and the parser reframes from the
  content either way. (The receiver-side one-post-one-RECV contract of §3 is thus dropped for a
  proxied stream; the backend frames its own bytes, exactly like an envoy upstream.)
- **`dst` is a concrete pod** (the L7 hook / LB already picked), or **`DEFER`** = fall through to
  the §5 default route (LB + stickiness + route-affinity).
- **Custody at egress**, not at receipt — the batched `TX_ACK` fires only when the SG-DMA has
  read the staging bytes, so the sender never overwrites a body mid-flight.
- **Ordering.** One receiving conn always lands in ONE lane (FIFO), so a conn's delivery order =
  segment order; one unit's chunks are never interleaved with another's.

**The parser is chosen per connection** from the addressed service — **not** a single global
choice. So a vanilla (shim) app, the frame demo, and a real L7 service **coexist in one deploy**.

| env | behavior | purpose |
|---|---|---|
| `DPUMESH_PROXY` *(unset)* | same as `passthru` — the engine is **always on** | production default, **bit-identical** to legacy per-message LB |
| `DPUMESH_PROXY=passthru` \| `1` | deploy default = **passthru**: one segment per arrived message; `dst` = the §5 L4 route. Works with **any** byte stream. | parity / regression + the vanilla (shim) path |
| `DPUMESH_PROXY=frame` | deploy default = **frame** for every request stream | the byte-stream demo |
| `DPUMESH_PROXY_FRAME_SVC=<csv>` | services whose **request** streams use the frame demo parser `[u32 len][u8 svc][payload]` | mix app kinds: `DPUMESH_PROXY=passthru DPUMESH_PROXY_FRAME_SVC=16` |
| `DPUMESH_PROXY_L7_SVC=<csv>` | services whose **request** streams run the **real L7 hook** (checked before frame) | the production L7 slot |
| `DPUMESH_LB_PER_REQUEST_SVC=<csv>` | services that load-balance **every** message (Envoy HTTP default) instead of the connection-sticky default (Envoy TCP-proxy) | opt out of session affinity per service |

**Writing an L7 parser.** Implement `dmesh_l7_route` in `dpu_l7.c`: you are shown only the
**head** of the front message (a bounded ≤`PX_HEAD_MAX` window) plus the cluster's live backend
pods. Fill a decision and return `>0` (decided), `0` ("head not fully here" → grow), or `<0`
(malformed). The engine **streams the body from staging via SG** — never linearized, so a large
message costs **no per-slot memcpy**; the only copy is the head, and only when it straddles a
slot. The hook is **stateless, no malloc, no locks**. A head larger than `PX_HEAD_MAX` poisons
the conn (cf. Envoy `max_request_headers_kb`).

Drive `frame` with `bench/validators/stream_dpumesh.c` (`bench.sh stream <N> <SIZE> [<SVC_LIST>]
[<FPW>]`): length-prefixed frames, **byte-exact** echo + a **served-byte exact-count**. Validated
for self-loopback, a >8 KB frame (seam), several frames per write, and fan-out across backends —
0 drops. **Status:** the substrate — cluster registry, round-robin LB, stickiness, connection
pool, and the `dmesh_l7_route` decision hook — is validated; writing a real body parser is the
author's step and needs **no transport changes**.

---

## 9. Baked data-plane configuration (reference)

Most data-plane tuning is **compiled in** at the values measured as best. The runtime env knobs
are `DPUMESH_PROXY` + `_PROXY_FRAME_SVC` + `_PROXY_L7_SVC` + `_LB_PER_REQUEST_SVC` (§8),
`DPUMESH_INGEST_SHARDS` + `_ARM_EGRESS_THREADS` + `_DPA_THREADS` + `_RINGS_PER_POD` (below),
`DPUMESH_DIAG`, `DPUMESH_PCI_ADDR`, `DPUMESH_SERVICE_ID`, `DPUMESH_TX_BLOCK` / `_TX_MAXB` /
`_TX_H`, and the `DMESH_PRELOAD_*` shim vars (§7). **To change a *baked* value, edit the named
constant and rebuild.**

| Setting | Baked value | Constant / assignment | File |
|---|---|---|---|
| TX/RX slots per pod | `4096` | `DPUMESH_NUM_SLOTS_DEFAULT` → `ctx->num_slots` | `src/dmesh_core.{h,c}` |
| Slot size (max wire DMA) | `8192` (DPA `dma_copy` cap) | `DPUMESH_SLOT_SIZE_DEFAULT` → `ctx->slot_size` | `src/dmesh_core.{h,c}` |
| Per-QP TX elastic block chain | `block_size 64 KB`, `maxb 4` (≤ 256 KB/QP), pool `n_blocks 512`, `cushion_h 1` | `DPUMESH_TX_BLOCK` / `_TX_MAXB` / `_TX_H` env | `src/dmesh_core.c` |
| Send-unit FIFO depth | `64` — **clamps `maxb`** so `maxb × ceil(block/slot) ≤ 64`; makes the block window bind first, so the FIFO can never fill and `tx_next_send` needs no capacity check | `TX_SU_DEPTH` | `src/dmesh_core.c` |
| DPA EU threads (N) | **auto-detected** = min(device EUs, `MAX_DPA_EU`=8); BF3 → `8` | `DPUMESH_DPA_THREADS` env | `doca/dpa.c` |
| Concurrent meshed pods / DPU | live cap `MAX_DPA_RINGS × N / K` (BF3 → `32`) | — | `dmesh_common.h` + `doca/comch_server.c` |
| Forward rings per pod / EU-sharding (K) | `2` — env `DPUMESH_RINGS_PER_POD` | `DPUMESH_RINGS_PER_POD_DEFAULT` | `dmesh_common.h` (DPU + host) |
| ARM ingest shards (M) | `1` — env `DPUMESH_INGEST_SHARDS` (use `2`: parse/route on M threads, per-shard conntrack; with egress `2` ≈ 2× small-RPC rate) | `getenv` → `n_ingest_shards` | `doca/dpu_worker.c` |
| ARM SG-DMA egress workers | `1` — env `DPUMESH_ARM_EGRESS_THREADS` (use `2`) | `getenv` → `px->n_eng` | `doca/dpu_proxy.c` |
| DPU main loop | event-driven epoll (busy-poll auto-fallback) | — | `doca/dpu_worker.c` |
| Host RX (PE) thread | sleep on the notification fd | `want_epoll = 1` | `src/dmesh_core.c` |
| Proxy seam cap | `512 KB` | `PX_SEAM_MAX_DEFAULT` | `doca/dpu_proxy.c` |

Constraints when changing: `K ≤ N ≤ MAX_DPA_EU` (8); each EU holds `MAX_DPA_RINGS` (8) forward
rings, so concurrent meshed pods are capped at `MAX_DPA_RINGS × N / K` — going past that needs a
bigger `MAX_DPA_EU` + `MAX_DPA_RINGS` (a DPA-kernel change). The **host's K must equal the
DPU's K** (forward rings pair 1:1); `slot_size ≤ 8192` (the DPA limit); `num_slots × slot_size`
is the per-pod staging (4096 × 8 KB = 32 MB).
