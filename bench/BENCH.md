# DPUmesh RPC benchmark

A closed-loop RPC request/response benchmark for DPUmesh and a TCP+Envoy
service-mesh baseline, measured with the identical methodology so the two line
up. The measurement structure (closed-loop concurrency window, warmup, greeter
big-request/tiny-reply, and the latency/bandwidth/rate families) is modeled on
the microbenchmark in the mRPC paper (phoenix-dataplane/phoenix).

---

## 1. What it measures

A Greeter-style RPC:

```
SayHello(request[req_size])  ->  reply[reply_size]     (reply_size fixed, default 8 B)
```

Each client thread keeps `W = concurrency` requests in flight on ONE connection,
issuing a new one on every completion (closed loop, no pacing):

```
   client thread ─┐   keep W outstanding on one conn
                  ▼
       req ─► [ ··· W requests in flight ··· ] ─► Greeter: reassemble req,
       ▲                                                    reply reply_size B
       └──────────── reply ◄── record latency ─────────────┘
              issue one new req per reply   (first `warmup` replies excluded)
```

The request carries `req_size` bytes; the reply is a fixed small `reply_size`
(default 8 B) — **asymmetric** (big request, tiny reply), so throughput counts
request-side bytes. The client is:

* **closed-loop with a fixed concurrency window**: each client thread keeps
  `concurrency` requests outstanding on one connection, issuing a new request on
  every completion. There is **no target rate and no pacing** — offered load is
  whatever the closed loop sustains.
* **warmup-aware**: the first `warmup` (1000) completions are excluded; the
  measurement window opens at the warmup boundary.
* **multi-threaded, share-nothing**: `threads` client threads, each with its own
  **CQ** and its own connection on it, so no thread touches another's RX path.
  The aggregate rate is the **sum of per-thread rates**; latency percentiles come
  from the merged histogram.

Reported metrics: **p50/p95/p99/avg/min/max latency (µs)**, **goodput Gb/s** =
`8e-9 · request_bytes / dura`, **rate Mrps** = `1e-6 · completions / dura`.

The cases tested — three families, each sweeping ONE axis of the same RPC:

| family    | question              | fixed             | swept                          | output          |
|-----------|-----------------------|-------------------|--------------------------------|-----------------|
| latency   | how fast is one RPC?  | concurrency **1** | `req_size` 64,128,256,512,1024 | latency vs size |
| bandwidth | how much data/s?      | concurrency 32    | `req_size` 32 B … 8 MB         | Gb/s vs size    |
| rate      | how many small RPC/s? | req 32 B, conc 32 | client threads 1,2,4,8         | Mrps vs threads |

Every case is run on **both** transports (`dpumesh` and `tcp`) so the two curves
overlay — that side-by-side is the comparison.

---

## 2. Topology — what's compared

Both sides serve the SAME Greeter RPC and are measured identically. The ONLY
difference is the transport, and that difference is the point: DPUmesh runs the
transport on the DPU (the app keeps its whole host core), while the TCP baseline
runs an Envoy sidecar in the app's pod (app + proxy share one core — the
service-mesh "tax"). The default `fair` pin gives **1 host core per pod** on both
sides, so the comparison is apples-to-apples.

```
DPUmesh  — transport offloaded to the DPU; the app owns its whole core.
           Service `echo-dpumesh` has THREE backends (cores 1, 6, 7).

     host core 0                                      core 1  ┌────────────┐
   ┌────────────────┐  request(req_size)                    ┌─┤  Greeter   │
   │  bench-dpumesh │ ──────────┐                           │ └────────────┘
   │     client     │           │                   core 6  │ ┌────────────┐
   │  (full core)   │ ◄───────┐ │                           ├─┤  Greeter   │
   └────────────────┘ reply   │ │                           │ └────────────┘
        dmesh.h               │ ▼                   core 7  │ ┌────────────┐
                        ┌─────┴──────────┐                  ├─┤  Greeter   │
                        │ DPU / DPA      │──────────────────┘ └────────────┘
                        │ the sidecar    │   one upstream per backend (uP)
                        └────────────────┘   (off host CPU: DOCA Comch + DMA)

   no codec  → the client's ONE conn pins to ONE Greeter; the other two idle
   codec     → every message is load-balanced; all three serve the same conn


TCP + Envoy sidecar  — the service-mesh baseline; app shares its core with a proxy

     host core 2  (shared)                  host core 3  (shared)
   ┌────────────────────────┐   k8s svc   ┌────────────────────────┐
   │ bench-tcp ─► sidecar1 ──┼───────────►┼─► sidecar2 ─► echo-tcp  │
   │  client      (Envoy)    │            │   (Envoy)      Greeter  │
   └────────────────────────┘            └────────────────────────┘
       app + proxy contend for 1 core  =  the sidecar tax DPUmesh removes
```

The pod that answers is the **Greeter** (`echo-*`); the pod that drives load is
the **client** (`bench-*`). Both listen for the `RUN` control command on :9092
(§4); the client also opens the data connection(s) to the Greeter.

### Seeing the routing granularity

Every Greeter stamps its own `pod_id` into each reply, and the client tallies it. Without that
discriminator the load balancer is **unobservable** — all three backends answer identically. Two
fields in the `OK` line say which regime you measured:

| | `reorder` | `dist` |
|---|---|---|
| **no codec** (default) | `0` | one pod per conn — `dist=0:342541` at threads=1 |
| **codec** (`DPUMESH_PROXY_L7_SVC=11`) | `>0` | even across all three — `dist=0:103984,1:103983,2:103983` |

`reorder>0` is not a fault: load-balancing every message means replies come back from several
backends at once, so arrival order stops matching send order. That is why the client correlates
by `seq`. The threads=1 / no-codec row is the gRPC problem in one line — one long-lived
connection, one backend, two pods idle.

## 3. Pieces

```
bench/
  bench.sh         the ONE script: deploy + benchmark + validators + utils
  bench.h          wire frame + stream reframer + latency histogram (shared)
  bench_dpumesh.c  DPUmesh client   -> bench-dpumesh   (closed-loop, framed, CQ-per-thread)
  echo_dpumesh.c   DPUmesh server   -> echo-dpumesh    (Greeter: fixed reply, reassembly)
  bench_tcp.go     TCP client       -> bench-tcp       (same methodology, via sidecar)
  echo_tcp.go      TCP server       -> echo-tcp
  BENCH.md         this doc
  RESULT.md        the measurement log — every experiment lands here
  k8s/pods.yaml    declarative k8s manifest (applied by bench.sh via envsubst)
  docker/          the 4 benchmark Dockerfiles
  validators/      DPUmesh feature validators (see validators/README.md) + their Dockerfiles
```

Building is the `Makefile`'s job (`bench.sh deploy` calls `make`); everything else
— DPU bring-up, images, k8s pods, pinning, and driving the runs — is `bench.sh`.

**Wire frame** (`bench.h`, little-endian 16-byte header + payload):

```
u32 magic   (BENCH_REQ_MAGIC request | BENCH_REP_MAGIC reply)
u32 seq     (client-assigned; echoed in the reply for correlation)
u32 payload_len
u32 aux     (request: reply_size the server must return; reply: 0)
u8  payload[payload_len]
```

Framing lets the server reassemble a **>8 KB request** that arrives as several
RECV completions and reply only after the whole request is in, and lets the
client correlate replies to requests by `seq` under the concurrency window.

Both sides run the **native API**: `dmesh_alloc` hands a pointer straight into the
TX ring (filled in place — no staging buffer), and every `DMESH_WC_RECV` completion
points straight into the RX mmap. **Zero copy in both directions.** Replies are correlated
**by seq**, never by arrival order — whether they keep send order is the service's codec's
business, not the client's, and the run reports `reorder` and `dist` so you can see which
regime you measured. Since `dmesh_alloc` **never blocks** (`NULL`+`EAGAIN` on a full SQ), a
backpressured frame parks and resumes on a later loop pass instead of stalling
the thread — so the closed-loop window is credit-limited without a spin.

The DPUmesh feature validators live in `validators/` (self-routing, concurrency
depth, the L7 frame-proxy engine, LD_PRELOAD transparency) — separate concerns
this benchmark does not exercise. See `validators/README.md`.

---

## 4. Control protocol

The client daemons (`bench-dpumesh`, `bench-tcp`) listen on TCP **:9092**:

```
RUN <req_size> <reply_size> <concurrency> <duration_s> <warmup> <threads> [reconn]
  -> OK mrps=.. gbps=.. p50=.. p95=.. p99=.. avg=.. min=.. max=..
        rcnt=.. fail=.. conc=.. threads=.. reqsz=.. repsz=.. durs=..
        reconns=.. reconn_us=.. grabs=.. rets=.. recyc=.. waits=.. pads=..
     (latencies in microseconds; key=value fields)

PING -> PONG
```

`reconn` (dpumesh only, default 0 = off) is the CONNECTION-CHURN knob: every
`reconn` completions a worker drains its window to 0 outstanding, then
`dmesh_destroy_qp()` + `dmesh_create_qp()`s a fresh one. `reconns` = total
reconnects, `reconn_us` = mean wall cost of one local destroy+create+pin.

The trailing five fields are the elastic TX block-pool deltas over the run
(`dmesh_get_tx_stats`): shared-pool grabs/returns (one CAS each), owner-local
recycle hits, **`waits`** = `dmesh_alloc` calls that hit the in-flight ceiling and
returned `EAGAIN`, and block-tail pads. In a steady small-message run grabs ≈ live
conns (the lazy first block) and **`waits` = 0** — the per-message path touches no
shared state and backpressure never occurs. `waits` is the number to watch: it is
the only observable proof of whether a send loop ever actually backpressures.

Pod bring-up (`bench.sh deploy`) does not parse the RUN reply, so a redeploy is
what makes the pods speak this protocol.

---

## 5. Running it

`bench.sh` is the single entry point — deploy, run, and validators:

```bash
./bench/bench.sh deploy                       # build + DPU + images + pods + pin
./bench/bench.sh latency   both               # p50/p95/p99 vs req_size (concurrency 1)
./bench/bench.sh bandwidth both               # Gb/s vs req_size (to 8 MB)
./bench/bench.sh rate      both               # Mrps vs client threads {1,2,4,8}
./bench/bench.sh all       both               # all three -> CSVs under $OUT (/tmp/dpumesh-bench)
./bench/bench.sh point dpumesh 1024 8 32 10 1000 4   # one raw RUN (reports reorder + dist)
./bench/bench.sh loopback|verbs|stream|preload …     # feature validators
./bench/bench.sh pin [fair|hw|hw3|hw6] | status | logs | cleanup | dpulog | dpucpu

DPUMESH_PROXY_L7_SVC=11 ./bench/bench.sh deploy      # codec on echo-dpumesh -> per-message LB
DPUMESH_PROXY=frame     ./bench/bench.sh deploy      # the frame codec (what `stream` needs)
```

Each family prints a table and writes a CSV. Knobs (env): `OUT`, `LAT_DUR`,
`BW_DUR`, `RATE_DUR`, `WARMUP`, `BW_CONC`, `RATE_CONC`, `RATE_THREADS`,
`LAT_SIZES`, `BW_SIZES`. `deploy`/`pin` read `.env` at the repo root (DPU ssh
target, sudo passwords, PCIe addrs); benchmark runs need only `kubectl` + `nc`.
DPU-side thread knobs pass straight through `deploy` to the DPU process —
`DPUMESH_INGEST_SHARDS=4 DPUMESH_ARM_EGRESS_THREADS=4 DPUMESH_SHARD_SHARED=0 ./bench/bench.sh deploy`
is the recommended config (design/CORE.md §4.1).

## 6. Layout / build

- **`Makefile`** (repo root) builds the host `libdpumesh.so` + all binaries;
  `bench.sh deploy` calls `make`. Validator sources live in `validators/`,
  benchmark sources at `bench/` top.
- **`bench.sh`** is the only shell script: it builds/cross-builds the DPU control
  plane, builds+imports the container images, applies `k8s/pods.yaml` (via
  `envsubst`), starts the DPU process, brings up the pods, pins CPUs, and drives
  the runs.
- **`k8s/pods.yaml`** holds the pod/service/sidecar definitions declaratively;
  `bench.sh` renders it with `envsubst`.

### Notes — read these before quoting a number

* **A `RESULT.md` number is meaningless without its section's config header.**
  Every measurement block names its deploy env and payload; a default (unsharded)
  deploy and a `shards=2 + egress2` deploy differ by ~2×, and the greeter payload
  (32 B/8 B vs 1 KB/1 KB) matters too. Comparing across them fabricates a
  regression exactly the size of the config delta. Match the config first.
* **The default deploy pins to ~0.10 Mrps** regardless of concurrency, threads,
  or payload, with p50 = N/0.10 (pure Little's law). That flat line is the
  **unsharded, single-emit-thread ceiling** — the proxy `pool_lock` serialized the
  emit thread — not a host bottleneck. `DPUMESH_INGEST_SHARDS=2
  DPUMESH_ARM_EGRESS_THREADS=2` lifts it to ~0.2, and `4 / 4` (with the batched
  object pools, design/CORE.md §4.1) to **~0.5**.
* **Thread scaling depends on the DPU config.** On the default/single-funnel
  deploy one thread already saturates the DPU and the sweep stays flat. With the
  recommended `4 / 4` config the emit thread is no longer the wall, so aggregate
  rate rises with client threads (t1×conc64 ~0.28 → t8×conc32 ~0.54 Mrps); past the
  DPU cap, extra threads only add queueing delay.
* **Client threads share nothing on RX** (one CQ each). The *server* still
  delivers through one PE thread. DPU-side transport parallelism is a deploy-time
  knob (`INGEST_SHARDS` + `ARM_EGRESS_THREADS`, design/CORE.md §4.1). For a real
  core-scaling curve pin more cores first (`./bench/bench.sh pin hw6`).
* **Closed loop, not paced.** There is no target-RPS knob — raise `concurrency`
  to push harder.
