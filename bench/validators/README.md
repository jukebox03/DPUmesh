# DPUmesh transport validators

Correctness validators for DPUmesh transport **features** that the RPC benchmark
(`../bench_dpumesh.c` / `../echo_dpumesh.c`) does not exercise. They are separate from
the benchmark and are driven by `../bench.sh` (`loopback` / `verbs` / `stream` / `preload`).

| file                 | binary             | validates                                              |
|----------------------|--------------------|--------------------------------------------------------|
| `loopback_dpumesh.c` | `loopback_dpumesh` | self-routing (one pod is client+server of its own svc) + oriented-tuple demux, one request at a time |
| `verbs_dpumesh.c`    | `verbs_dpumesh`    | the same self-routing proof **at depth** — `WINDOW` concurrent QPs × `PIPELINE` outstanding each, so it drives the completion path where loopback drives it one-at-a-time. It is what caught the ready-list lost-edge race |
| `stream_dpumesh.c`   | `stream_dpumesh`   | the DPU L7 frame-proxy engine (`DPUMESH_PROXY=frame`: length-prefix parser, per-dst SG-DMA egress) |
| `preload_runner.c`   | `preload_runner`   | LD_PRELOAD transparency: vanilla TCP apps over DPUmesh via `libdmesh_preload.so` (pod/image: `preload-dpumesh`) |
| `tcp_client.c`, `tcp_echo.c` | (POSIX)    | the vanilla TCP apps the preload validator runs unmodified under the shim |

Container defs for these live beside the sources here
(`loopback_dpumesh.Dockerfile`, `verbs_dpumesh.Dockerfile`,
`stream_dpumesh.Dockerfile`, `preload_dpumesh.Dockerfile`).

All validator pods are brought up by `bench.sh deploy` (`start_pods`); none self-starts
on demand — starting a pod against an already-running DPU is not a supported flow.

**Build wiring:** these sources live in `bench/validators/`; the root `Makefile`
paths point here (see `../BENCH.md` §5).
