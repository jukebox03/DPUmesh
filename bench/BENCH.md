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
* **multi-threaded**: `threads` client threads, each its own connection; the
  aggregate rate is the **sum of per-thread rates**, latency percentiles come
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
DPUmesh  — transport offloaded to the DPU; the app owns its whole core

     host core 0                            host core 1
   ┌────────────────┐  request(req_size)   ┌────────────────┐
   │  bench-dpumesh │ ───────────────────► │  echo-dpumesh  │
   │     client     │                      │    Greeter     │
   │  (full core)   │ ◄─────────────────── │  (full core)   │
   └───────┬────────┘  reply(8 B)          └───────▲────────┘
           │              dmesh.h                  │
           └──────►  DPU / DPA transport  ─────────┘
                     (off host CPU: DOCA Comch + DMA)


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

## 3. Pieces

```
bench/
  bench.sh         the ONE script: deploy + benchmark + validators + utils
  bench.h          wire frame + stream reframer + latency histogram (shared)
  bench_dpumesh.c  DPUmesh client   -> bench-dpumesh   (closed-loop, framed, multithread)
  echo_dpumesh.c   DPUmesh server   -> echo-dpumesh    (Greeter: fixed reply, reassembly)
  bench_tcp.go     TCP client       -> bench-tcp       (same methodology, via sidecar)
  echo_tcp.go      TCP server       -> echo-tcp
  BENCH.md         this doc
  scale_log.md     (historical experiment log — kept)
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

Framing lets the server reassemble a **>8 KB request** that the transport
auto-chunks across slots and reply only after the whole request arrives, and lets
the client correlate replies to requests by `seq` under the concurrency window.
The DPUmesh client `dmesh_pin_route()`s its connection so it stays on the one
backend (replies in send order). `dmesh_write` busy-spins on byte-ring
backpressure, so the closed-loop window is naturally credit-limited.

The DPUmesh feature validators live in `validators/` (self-routing, the L7
frame-proxy engine, LD_PRELOAD transparency) — separate concerns this benchmark
does not exercise. See `validators/README.md`.

---

## 4. Control protocol

The client daemons (`bench-dpumesh`, `bench-tcp`) listen on TCP **:9092**:

```
RUN <req_size> <reply_size> <concurrency> <duration_s> <warmup> <threads>
  -> OK mrps=.. gbps=.. p50=.. p95=.. p99=.. avg=.. min=.. max=..
        rcnt=.. fail=.. conc=.. threads=.. reqsz=.. repsz=.. durs=..
     (latencies in microseconds; key=value fields)

PING -> PONG
```

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
./bench/bench.sh point dpumesh 1024 8 32 10 1000 4   # one raw RUN
./bench/bench.sh loopback|stream|preload …    # feature validators
./bench/bench.sh pin [fair|hw|hw3|hw6] | status | logs | cleanup | dpulog | dpucpu
```

Each family prints a table and writes a CSV. Knobs (env): `OUT`, `LAT_DUR`,
`BW_DUR`, `RATE_DUR`, `WARMUP`, `BW_CONC`, `RATE_CONC`, `RATE_THREADS`,
`LAT_SIZES`, `BW_SIZES`. `deploy`/`pin` read `.env` at the repo root (DPU ssh
target, sudo passwords, PCIe addrs); benchmark runs need only `kubectl` + `nc`.

## 6. Layout / build

- **`Makefile`** (repo root) builds the host `libdpumesh.so` + all binaries;
  `bench.sh deploy` calls `make lib bench go`. Validator sources live in
  `validators/`, benchmark sources at `bench/` top — the Makefile paths point
  there. `make lib bench go` builds all 9 binaries clean.
- **`bench.sh`** is the only shell script: it builds/cross-builds the DPU control
  plane, builds+imports the container images, applies `k8s/pods.yaml` (via
  `envsubst`), starts the DPU process, brings up the pods, pins CPUs, and drives
  the runs.
- **`k8s/pods.yaml`** holds the pod/service/sidecar definitions declaratively
  (was 400 lines of inline heredoc); `bench.sh` renders it with `envsubst`.

### Notes
* **Rate/scalability.** The DPUmesh server delivers through a single-consumer
  ready-list (one PE thread), so only **client** threads scale; the client pod's
  core count is set by the pin profile. For a real core-scaling curve pin more
  cores first (`./bench/bench.sh pin hw6`) then run the thread sweep.
* **Closed loop, not paced.** There is no target-RPS knob — raise `concurrency`
  to push harder.
* **Validated in-session:** the TCP pair was smoke-tested over loopback
  (64 B … 8 MB, `fail=0`); the DPUmesh pair shares the identical framing +
  closed-loop code and compiles/links clean, but a live run needs the HW deploy
  (`bench.sh deploy`).
