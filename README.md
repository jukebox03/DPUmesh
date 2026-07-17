# DPUmesh

A service-mesh **data plane** on NVIDIA DOCA (Comch + DMA). The transport runs on the
**BlueField DPU/DPA**, not the host CPU — your application keeps its full host core (no
in-host sidecar tax).

**The DPU is the sidecar.** Your app addresses a **service by name**, never a pod. Its
connection is a submission queue that **ends at the DPU**; the DPU owns and re-originates the
connections to the backends, and maps every reply back. That is Envoy's shape, without the
host-CPU tax and without L7 parsing on the common path — the descriptor already carries the
routing key.

Whitepapers: **[design/API.md](design/API.md)** (what you call) ·
**[design/CORE.md](design/CORE.md)** (how it works) ·
**[design/NAMING.md](design/NAMING.md)** (name/identity control plane) ·
**[bench/RESULT.md](bench/RESULT.md)** (every experiment, dated).

## The layering rule

```
L4 — the transport   host <-> DPU. A byte stream. No message boundaries, no routing keys,
                     nothing L7 on the wire. Exactly what TCP gives you.
L7 — the DPU's codec, chosen per service in the proxy config (never by the app):
       codec  (_FRAME_SVC / _L7_SVC)  knows message boundaries  ->  LB picks PER MESSAGE
       none   (passthru, the default) bytes only, no boundaries ->  conn pinned for life
```

Pinning is what you get when nothing can see where messages end — not a setting. To
load-balance a long-lived connection, give the service a codec. Routing policy lives in the
DPU's config; the app only names a service. (Envoy parity: `tcp_proxy` vs
`http_connection_manager`.)

Under a codec one QP holds several upstreams at once, so its inbound carries several
independent byte streams — `dmesh_wc_t.stream` says which, exactly as TCP would give you one
socket per peer.

## Two surfaces

Exactly two public APIs. Neither is built on the other — both sit directly on the internal core.

| | | for |
|---|---|---|
| **Native API** — `<dpumesh/dmesh.h>` | RDMA-verbs-shaped (`dmesh_create_cq` / `dmesh_create_qp` / `dmesh_alloc` / `dmesh_post_send` / `dmesh_poll_cq`). **Zero-copy send AND receive.** 16 calls. | new code |
| **POSIX socket ABI** — `libdmesh_preload.so` | `LD_PRELOAD`; you never call it, it impersonates libc. It owns the copies `read(2)`/`write(2)` mandate — which is why the native path pays none. | **unmodified** binaries |

## Layout

```
include/dpumesh/    public headers — #include <dpumesh/dmesh.h>
    dmesh.h             the native API (the one you call — dmesh_*)
    dmesh_common.h      wire ABI + config shared by host and DPU
src/                host transport library sources
    dmesh_core.{h,c}    INTERNAL core: transport + connection lifecycle (not installed)
    dmesh_resolve.c     name/identity resolver — the file-backed registry both surfaces use
    dmesh_api.c         the native API impl — the verbs-shaped data path (→ libdpumesh.so)
    dmesh_preload.c     LD_PRELOAD socket shim (→ libdmesh_preload.so)
doca/               DPU-side control plane (ARM) + DPA kernel — built with meson
    dpu_proxy.c         the proxy engine: window, parse, route, SG-DMA egress, custody
    dpu_l7.c            THE L7 SLOT — write your codec here (dpu_l7.h is the contract)
    device/dpa_kernel.c the DPA (EU) data-plane kernel — built by dpacc
bench/              benchmark + validators + bench.sh (deploy+run) + k8s/ + Dockerfiles
design/             API.md · CORE.md · NAMING.md
Makefile            host build (libdpumesh.so + bench binaries)
```

`src/dmesh_core.h` is **internal and deliberately not installed** — nothing outside this repo
may include it. The shim is a *sibling façade*, not a client of the native API, so it compiles
against the core (`-Isrc`) for the QP internals it needs.

## Build

```sh
make            # build/lib/libdpumesh.so, build/lib/libdmesh_preload.so, build/bin/*
make clean
```

Needs DOCA on the host (`pkg-config` finds `doca-common`/`doca-comch`/`doca-dpa`) and `go` for
the TCP baseline binaries. Header dependencies are tracked (`-MMD`), so editing a public header
relinks everything that includes it — mixing a fresh `libdpumesh.so` with a stale binary is an
ABI skew that only shows up as a run-time SIGSEGV.

The **DPU** side (ARM control plane + DPA kernel) builds with meson/ninja on the DPU;
`bench/bench.sh deploy` does it over ssh, and `build_dpacc.sh` cross-compiles
`device/dpa_kernel.c` for BF3 via `dpacc`.

## Deploy & test

`bench/bench.sh` reads `.env` (copy `.env.example`, fill in the DPU ssh target, sudo passwords,
PCI addresses). It is the single entry point, and `deploy` is the **only** bring-up path — it
starts the DPU and every pod together, in order. See **[bench/BENCH.md](bench/BENCH.md)**.

```sh
./bench/bench.sh deploy                 # build host+DPU, images, (re)start DPU, launch pods
DPUMESH_PROXY_L7_SVC=11 ./bench/bench.sh deploy    # run the L7 codec on echo-dpumesh → per-message LB
DPUMESH_INGEST_SHARDS=2 DPUMESH_ARM_EGRESS_THREADS=2 \
./bench/bench.sh deploy                 # measured DPU thread config (≈2× small-RPC rate; CORE.md §4.1)

./bench/bench.sh latency|bandwidth|rate|all [dpumesh|tcp|both]   # RPC benchmark vs TCP+Envoy
./bench/bench.sh point <sol> <req> <reply> <conc> <dur> <warmup> <threads> [reconn]
./bench/bench.sh loopback <N> <SIZE> [<ZC>]            # self-routing validator
./bench/bench.sh verbs    <N> <SIZE> <ZC> <WIN> <PIPE> # concurrency depth: window × pipeline
./bench/bench.sh stream   <N> <SIZE> [<FPW>]           # byte-stream / frame-codec validator
./bench/bench.sh preload  <N> <SIZE> <CONNS>           # LD_PRELOAD shim (vanilla TCP apps)
./bench/bench.sh status | logs | cleanup | dpulog [n] | dpucpu
```

`point` reports `dist=pod:count` (which backend served each reply) and `reorder` — together they
show the routing granularity: one pod and `reorder=0` means the connection is pinned; an even
spread with `reorder>0` means every message is load-balanced.

The DPU checkout defaults to `~/DPUmesh` on `$DPU_HOST`; override with `DPU_PROJ`.

## Include-path note

Consumers include the transport as `#include <dpumesh/dmesh.h>` and link `-ldpumesh`
(previously `#include <thrift/transport/dpm.h>` against `libthrift`). This repo is the
standalone extraction — no Thrift dependency.
