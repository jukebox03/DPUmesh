# DPUmesh

A service-mesh **data plane** on NVIDIA DOCA (Comch + DMA). The transport runs on the
**BlueField DPU/DPA**, not the host CPU — your application keeps its full host core (no
in-host sidecar tax). See **[api.md](api.md)** for the full user-facing whitepaper and
**[bench/scale_log.md](bench/scale_log.md)** for the measurement log.

This repository is the standalone extraction of the DPUmesh transport: the sources have
**no Thrift dependency** (they were previously compiled into `libthrift`; here they build
into their own `libdpumesh.so`).

## Layout

```
include/dpumesh/    public headers — #include <dpumesh/dpm.h>
    dpm.h               socket/epoll-style façade (the API you call)
    dpumesh.h           C core header
    dpumesh_common.h    wire ABI + config shared by host and DPU
src/                host transport library sources
    dpumesh_doca.c      the host-side transport (→ libdpumesh.so)
    dmesh_preload.c     LD_PRELOAD socket shim (→ libdmesh_preload.so)
doca/               DPU-side control plane (ARM) + DPA kernel — built with meson
    *.c *.h             comch / dma / worker / proxy / L7 hook
    device/dpa_kernel.c the DPA (EU) data-plane kernel — built by dpacc
    meson.build         DPU build; build_dpacc.sh drives the DPA compile
bench/              benchmarks + validators (dmesh + vanilla-TCP baselines) + Dockerfiles
Makefile            host build (libdpumesh.so + bench binaries)
test-bench.sh       deploy + benchmark harness (k8s pods, DPU over ssh)
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
DPU. `test-bench.sh` does this over ssh (`meson setup build && ninja`); `build_dpacc.sh`
cross-compiles `device/dpa_kernel.c` for BF3 via `dpacc`.

## Deploy & test

`test-bench.sh` reads `.env` (copy `.env.example` → `.env` and fill in the DPU ssh target,
sudo passwords, and PCI addresses).

```sh
./test-bench.sh deploy                 # build host+DPU, images, (re)start DPU, launch pods
./test-bench.sh dpumesh <RPS> <DUR> <SIZE> [<CONNS>]   # RPC benchmark over DPUmesh
./test-bench.sh tcp     <RPS> <DUR> <SIZE> [<CONNS>]   # TCP+Envoy baseline
./test-bench.sh stream  <N> <SIZE> [<SVC_LIST>] [<FPW>]# byte-stream / L7-proxy frame validator
./test-bench.sh preload <N> <SIZE> <CONNS>             # LD_PRELOAD shim (vanilla TCP apps)
./test-bench.sh status | logs | cleanup
```

The DPU checkout location defaults to `~/DPUmesh` on `$DPU_HOST`; override with `DPU_PROJ`.

## Include-path note

Consumers include the transport as `#include <dpumesh/dpm.h>` and link `-ldpumesh`
(previously `#include <thrift/transport/dpm.h>` against `libthrift`). Update any external
caller (e.g. a socialNetwork integration) accordingly, or add `-Iinclude` and keep the
`<dpumesh/...>` form.
