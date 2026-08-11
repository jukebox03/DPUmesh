# linkerd on DPUmesh

The DPU-side L7 layer. linkerd2-proxy runs on the BlueField ARM and is linked
into the DPUmesh DPU binary, supplying the policy and protocol handling that the
transport does not perform on its own.

This directory is not an integration under `integrations/`. Integrations link
into a foreign process and carry their own build; this code links into the
DPUmesh DPU binary and is consumed by `doca/meson.build`. It is not
independently buildable.

## Structure

There is one data plane: arriving payload lands in pod staging and is forwarded
by scatter-gather DMA to its destination, with no copy on the ARM. What varies
per service is how far the L7 layer is involved.

| Mode | What the L7 layer contributes | Bytes traverse linkerd | ARM copies |
|---|---|---|---|
| `decision` | authorization, discovery, endpoint choice, identity, telemetry | no — only the question | 0 |
| `opaque` | the above, plus mTLS | yes | 2 |
| `l7` | the above, plus HTTP routing, retries, timeouts | yes | 2 |

`decision` is the default. In this mode the L7 layer is consulted once when a
connection is established and the payload never enters it, so policy costs
nothing per byte. Services that require encryption use `opaque`; services that
require request-level control use `l7`.

When the L7 layer is unavailable, the data plane falls open to its own
round-robin and per-connection pinning. This is an availability fallback, not a
selectable mode, and it runs without policy.

## Division of ownership

| Layer | Owner |
|---|---|
| Host library, preload shim | DPUmesh |
| Host↔DPU wire ABI: control messages, forward ring, reverse ring, credits | DPUmesh (normative) |
| DPA execution units, ARM workers, SG-DMA, custody | DPUmesh |
| Adapter contract (`dmesh_l7.h`) | Shared |
| HTTP/2 termination, policy, load balancing, identity | linkerd port |

DPUmesh moves bytes. linkerd decides where they go.

## Layout

```
linkerd/
  README.md      this file
  PLAN.md        implementation design and working notes
  CONTRACT.md    interface contract: wire ABI and adapter API
  port/          submodule: youngmin-kaist/DPUMesh
    DPUMesh/       the port's own C datapath — reference for ABI convergence
    linkerd2-proxy/  nested submodule, branch `dpumesh`
      linkerd/doca/  the seam: shim.c, driver.rs, io.rs
  bench/         six-configuration campaign: Envoy x2, linkerd x2, TCP, DPUmesh
                 (host sidecars). See bench/report/REPORT_LINKERD.md
```

Implementation code is not present yet. `CONTRACT.md` defines what it must
satisfy; `PLAN.md` records where it lands.

## Submodule

The port is developed in a separate repository. Both levels update together.

```sh
git submodule update --init --recursive linkerd/port     # first clone
git submodule update --remote --recursive linkerd/port   # pull upstream work
git add linkerd/port && git commit                       # pin
```

The nested submodule's recorded URL uses SSH. Without access, rewrite it
locally; `.gitmodules` stays untouched:

```sh
git -C linkerd/port config url."https://github.com/".insteadOf "git@github.com:"
git -C linkerd/port config submodule.linkerd2-proxy.url \
    https://github.com/youngmin-kaist/linkerd2-proxy.git
```

## Port status

`DPUMesh@785bc1a`, `linkerd2-proxy@4f926826`.

Present: `DmeshIo` (an `AsyncRead`/`AsyncWrite`/`Peek`/`PeerAddr` endpoint over
DMA buffers), an async driver binding DOCA progress-engine notification
descriptors to the tokio reactor, an acceptor that feeds DMA connections through
the outbound stack, and a connector that reaches DMA-provided backends.

Absent: inbound proxying, more than one worker, runtime-sized connection slots,
staging flow control, a reusable backend channel registry, and the query
interface that `decision` mode requires. `CONTRACT.md` states which of these the
contract requires.
