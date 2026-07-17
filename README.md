# DPUmesh

A service-mesh **data plane** on NVIDIA DOCA (Comch + DMA). The transport runs on the
**BlueField DPU/DPA**, not the host CPU — your application keeps its full host core (no
in-host sidecar tax). See **[design/API.md](design/API.md)** (user-facing) and
**[design/CORE.md](design/CORE.md)** (internals) for the transport whitepapers,
**[design/NAMING.md](design/NAMING.md)** for the name/identity control plane, and
**[bench/RESULT.md](bench/RESULT.md)** for the measurement log.

This repository is the standalone extraction of the DPUmesh transport: the sources have
**no Thrift dependency** (they were previously compiled into `libthrift`; here they build
into their own `libdpumesh.so`).

## Two surfaces

DPUmesh offers **exactly two** public APIs, and neither is built on the other — both sit
directly on the internal core:

| | | for |
|---|---|---|
| **Native API** — `<dpumesh/dmesh.h>` | RDMA-verbs-shaped (`dmesh_create_cq` / `dmesh_create_qp` / `dmesh_alloc` / `dmesh_post_send` / `dmesh_poll_cq`). **Zero-copy send AND receive.** | new code |
| **POSIX socket ABI** — `libdmesh_preload.so` | `LD_PRELOAD`; you never call it, it impersonates libc. It owns the copies `read(2)`/`write(2)` mandate — which is why the native path pays none. | **unmodified** binaries |

## Layout

```
include/dpumesh/    public headers — #include <dpumesh/dmesh.h>
    dmesh.h             the native API (the one you call — dmesh_*)
    dmesh_common.h      wire ABI + config shared by host and DPU
src/                host transport library sources
    dmesh_core.{h,c}    INTERNAL core: transport + connection lifecycle (not installed)
    dmesh_resolve.c     name/identity resolver — the file-backed registry both surfaces resolve through
    dmesh_api.c         the native API impl — the verbs-shaped data path (→ libdpumesh.so)
    dmesh_preload.c     LD_PRELOAD socket shim (→ libdmesh_preload.so)
doca/               DPU-side control plane (ARM) + DPA kernel — built with meson
    *.c *.h             comch / dma / worker / proxy / L7 hook
    device/dpa_kernel.c the DPA (EU) data-plane kernel — built by dpacc
    meson.build         DPU build; build_dpacc.sh drives the DPA compile
bench/              benchmark + validators + bench.sh (deploy+run) + k8s/ manifest + Dockerfiles
design/             API.md (user-facing) + CORE.md (internals) + NAMING.md (name/identity control plane)
Makefile            host build (libdpumesh.so + bench binaries)
```

`src/dmesh_core.h` is **internal and deliberately not installed** — nothing outside this repo
may include it. The shim is a *sibling façade*, not a client of the native API, so it compiles
against the core (`-Isrc`) for the QP internals and the internal lifecycle it needs.

## Build

**Host** (this machine — libdpumesh + benchmark binaries):

```sh
make            # build/lib/libdpumesh.so, build/lib/libdmesh_preload.so, build/bin/*
make clean
```
Requires DOCA on the host (`pkg-config` finds `doca-common`/`doca-comch`/`doca-dpa`) and
`go` for the TCP baseline binaries.

> The bench binaries take the library as an **order-only** prerequisite, so changing only
> `libdpumesh.so` does not relink them — a stale binary then links against a deleted symbol
> and the build still looks green. `rm build/bin/<x>` (or `rm -rf build`) to force.

**DPU** (BlueField ARM — the control plane + DPA kernel): built with meson/ninja on the
DPU. `bench/bench.sh deploy` does this over ssh (`meson setup build && ninja`); `build_dpacc.sh`
cross-compiles `device/dpa_kernel.c` for BF3 via `dpacc`.

## Deploy & test

`bench/bench.sh` reads `.env` (copy `.env.example` → `.env` and fill in the DPU ssh target,
sudo passwords, and PCI addresses). It is the single entry point — deploy, benchmark, and
validators (see **[bench/BENCH.md](bench/BENCH.md)**). `deploy` is the **only** bring-up path:
it starts the DPU and every pod together, in order.

```sh
./bench/bench.sh deploy                 # build host+DPU, images, (re)start DPU, launch pods
DPUMESH_INGEST_SHARDS=2 DPUMESH_ARM_EGRESS_THREADS=2 \
./bench/bench.sh deploy                 # measured DPU thread config (≈2× small-RPC rate; CORE.md §4.1)
./bench/bench.sh latency|bandwidth|rate|all [dpumesh|tcp|both]  # RPC benchmark vs TCP+Envoy baseline
./bench/bench.sh loopback <N> <SIZE> [<ZC>]             # self-routing validator
./bench/bench.sh verbs   <N> <SIZE> <ZC> <WIN> <PIPE>   # concurrency-depth validator (window × pipeline)
./bench/bench.sh stream  <N> <SIZE> [<SVC_LIST>] [<FPW>]# byte-stream / L7-proxy frame validator
./bench/bench.sh preload <N> <SIZE> <CONNS>             # LD_PRELOAD shim (vanilla TCP apps)
./bench/bench.sh status | logs | cleanup
```

The DPU checkout location defaults to `~/DPUmesh` on `$DPU_HOST`; override with `DPU_PROJ`.

## Include-path note

Consumers include the transport as `#include <dpumesh/dmesh.h>` and link `-ldpumesh`
(previously `#include <thrift/transport/dpm.h>` against `libthrift`). Update any external
caller (e.g. a socialNetwork integration) accordingly, or add `-Iinclude` and keep the
`<dpumesh/...>` form.
