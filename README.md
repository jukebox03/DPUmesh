# DPUmesh

A service-mesh **data plane** on NVIDIA DOCA (Comch + DMA). The transport — connection setup,
framing, routing, and DMA — runs on the **BlueField DPU/DPA**, so the application runs no
in-process sidecar. The app addresses a **service by name**, never a pod: its connection is a
submission queue that **ends at the DPU**, which re-originates the connections to the backends
and maps every reply back. Envoy's shape, with the transport offloaded to the DPU.

> **Measured status (be honest with yourself).** At single-pod / L4 / 1KB on BlueField, ranked by
> throughput: **direct kernel TCP ≫ TCP+Envoy > DPUmesh** (conc32: 0.93 / 0.41 / 0.20 Mrps). The
> Envoy sidecar tax is real (~2.3×), but DPUmesh lands **below** the taxed sidecar path it aims to
> replace, at ~2× the latency. Its *host* CPU/request beats TCP+Envoy's — but that cost is
> **relocated onto 3.3–4.6 DPU-ARM cores**, so counting host+DPU it is **~7× TCP's total CPU per
> request at every DPU config**, and **amortizing one DPU across many pods does not help** (the DPU
> cost scales with traffic — it is not a fixed overhead that divides across pods). DPUmesh edges
> ahead only under heavy app-work (~15–23% over TCP+Envoy). Full, reproducible data:
> **[bench/report/REPORT.md](bench/report/REPORT.md)**. The one untested lever that could change
> this is **L7** (a much heavier sidecar); intra-node L4 is DPUmesh's worst case.

Whitepapers: **[design/API.md](design/API.md)** (what you call) ·
**[design/CORE.md](design/CORE.md)** (how it works) ·
**[design/NAMING.md](design/NAMING.md)** (name/identity control plane).

## The layering rule

```
L4 — the transport   host <-> DPU. A byte stream: no message boundaries, no routing keys,
                     nothing L7 on the wire. Exactly what TCP gives you.
L7 — the DPU's codec, chosen per service in the proxy config (never by the app):
       codec (_FRAME_SVC/_L7_SVC)  knows message boundaries  ->  LB picks PER MESSAGE
       none  (passthrough, default) bytes only, no boundaries ->  conn pinned for life
```

Pinning is what you get when nothing can see where messages end — not a setting. To
load-balance a long-lived connection, give the service a codec. Routing policy lives in the
DPU's config; the app only names a service. (Envoy parity: `tcp_proxy` vs `http_connection_manager`.)

## Two APIs

Two public surfaces; neither built on the other — both sit directly on the internal core.

| | for |
|---|---|
| **Native** — `<dpumesh/dmesh.h>`: verbs-shaped (`dmesh_create_cq`/`create_qp`/`alloc`/`post_send`/`poll_cq`), zero-copy both ways, 16 calls | new code |
| **POSIX ABI** — `libdmesh_preload.so`: `LD_PRELOAD` impersonates libc; owns the copies `read`/`write` mandate (which the native path avoids) | **unmodified** binaries |

## Layout

```
include/dpumesh/    public headers (#include <dpumesh/dmesh.h>)
src/                host library: dmesh_core.{h,c} (internal core) · dmesh_api.c (native)
                    · dmesh_preload.c (LD_PRELOAD shim) · dmesh_resolve.c (name registry)
doca/               DPU-side control plane (ARM) + DPA kernel (device/dpa_kernel.c) — meson
bench/              benchmark, evaluation suite, deploy (bench/README.md is the guide)
design/             API.md · CORE.md · NAMING.md
Makefile            host build → libdpumesh.so, libdmesh_preload.so, bench binaries
```

`src/dmesh_core.h` is **internal, not installed** — the shim is a sibling façade (compiles
against `-Isrc` for QP internals), not a client of the native API.

## Build & deploy

```sh
make                                    # libdpumesh.so + libdmesh_preload.so + bench binaries
```

Needs DOCA on the host (`pkg-config` finds `doca-common`/`doca-comch`/`doca-dpa`). The library
carries a versioned SONAME (`libdpumesh.so.1`) so a stale out-of-tree binary fails at the loader
rather than SIGSEGV'ing on a moved struct. The **DPU** side builds with meson/ninja on the DPU;
`bench/bench.sh deploy` does it over ssh.

`bench/bench.sh` reads `.env` (ssh target, sudo passwords, PCI addresses) and is the single entry
point; `deploy` is the **only** bring-up path (starts the DPU and every pod together, in order).

```sh
./bench/bench.sh deploy                                     # build host+DPU, images, DPU, pods
DPUMESH_INGEST_SHARDS=4 DPUMESH_ARM_EGRESS_THREADS=4 ./bench/bench.sh deploy   # (4,4) DPU config
DPUMESH_PROXY_L7_SVC=11 ./bench/bench.sh deploy             # L7 codec → per-message LB
./bench/bench.sh latency|bandwidth|rate|point ...          # quick runs
./bench/bench.sh loopback|verbs|stream|preload ...         # feature validators
```

Full harness guide and the staged evaluation: **[bench/README.md](bench/README.md)**.
