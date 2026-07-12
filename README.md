# DPUmesh

A service-mesh **data plane** on NVIDIA DOCA (Comch + DMA). The transport runs on the
**BlueField DPU/DPA**, not the host CPU — your application keeps its full host core (no
in-host sidecar tax). See **[design/API.md](design/API.md)** for the user-facing whitepaper,
**[design/CORE.md](design/CORE.md)** for the internal-implementation whitepaper, and
**[bench/scale_log.md](bench/scale_log.md)** for the measurement log.

This repository is the standalone extraction of the DPUmesh transport: the sources have
**no Thrift dependency** (they were previously compiled into `libthrift`; here they build
into their own `libdpumesh.so`).

## Layout

```
include/dpumesh/    public headers — #include <dpumesh/dmesh.h>
    dmesh.h             socket/epoll-style façade (the API you call — dmesh_*)
    dmesh_core.h        C core header (raw engine under the façade — dpumesh_*)
    dmesh_common.h      wire ABI + config shared by host and DPU
src/                host transport library sources
    dmesh.c             the socket/epoll façade impl (→ libdpumesh.so)
    dmesh_core.c        the host-side transport core engine (→ libdpumesh.so)
    dmesh_preload.c     LD_PRELOAD socket shim (→ libdmesh_preload.so)
doca/               DPU-side control plane (ARM) + DPA kernel — built with meson
    *.c *.h             comch / dma / worker / proxy / L7 hook
    device/dpa_kernel.c the DPA (EU) data-plane kernel — built by dpacc
    meson.build         DPU build; build_dpacc.sh drives the DPA compile
bench/              benchmark + validators + bench.sh (deploy+run) + k8s/ manifest + Dockerfiles
design/             API.md (user-facing whitepaper) + CORE.md (internals whitepaper)
Makefile            host build (libdpumesh.so + bench binaries)
```

## Build

**Host** (this machine — libdpumesh + benchmark binaries):

```sh
make            # build/lib/libdpumesh.so, build/lib/libdmesh_preload.so, build/bin/*
make clean
```
Requires DOCA on the host (`pkg-config` finds `doca-common`/`doca-comch`/`doca-dpa`) and
`go` for the TCP baseline binaries.

**DPU** (BlueField ARM — the control plane + DPA kernel): built with meson/ninja on the
DPU. `bench/bench.sh deploy` does this over ssh (`meson setup build && ninja`); `build_dpacc.sh`
cross-compiles `device/dpa_kernel.c` for BF3 via `dpacc`.

## Deploy & test

`bench/bench.sh` reads `.env` (copy `.env.example` → `.env` and fill in the DPU ssh target,
sudo passwords, and PCI addresses). It is the single entry point — deploy, benchmark, and
validators (see **[bench/BENCH.md](bench/BENCH.md)**).

```sh
./bench/bench.sh deploy                 # build host+DPU, images, (re)start DPU, launch pods
./bench/bench.sh latency|bandwidth|rate|all [dpumesh|tcp|both]  # RPC benchmark vs TCP+Envoy baseline
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
