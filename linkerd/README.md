# linkerd on DPUmesh

The DPU-side L7 layer. linkerd2-proxy runs on the BlueField ARM, linked into the
DPUmesh DPU binary, supplying the policy and protocol handling the transport does
not perform on its own.

This is not an integration under `integrations/`. Those link into a foreign
process and carry their own build; this links into the DPUmesh DPU binary and is
consumed by `doca/meson.build`. It is not independently buildable.

`design/L7.md` is the design. `CONTRACT.md` is what the two sides owe each other.

## Layout

```text
linkerd/
  README.md      this file
  CONTRACT.md    interface contract: wire ABI, adapter API, build integration
  include/       dmesh_l7.h — the contract, compiled into the DPU binary
  shim/          l7_null.c — the reference consumer
  rust/          dmesh-l7 — the staticlib carrying linkerd2-proxy
  port/          submodule: youngmin-kaist/DPUMesh
    DPUMesh/       the port's own C datapath — reference for ABI convergence
    linkerd2-proxy/  nested submodule, branch `dpumesh`
      linkerd/doca/  the junction: io.rs, driver.rs, api.rs, shim.c
  bench/         six-configuration campaign: Envoy x2, linkerd x2, TCP, DPUmesh
                 (host sidecars). See bench/report/REPORT_LINKERD.md
```

## Modes

Which services the layer handles, and how far it is involved in each, is set at
startup:

```sh
DPUMESH_L7_DECISION_SVC=<service ids>   # policy only; payload stays on the L4 path
DPUMESH_L7_OPAQUE_SVC=<service ids>     # payload traverses the layer as a stream
DPUMESH_L7_SVC=<service ids>            # payload traverses it as framed messages
```

A service named by no list is forwarded by the data plane alone; a service named
twice is rejected at startup. With every list empty the layer is not attached and
the egress arena is not allocated. [`design/L7.md`](../design/L7.md) defines what
each mode contributes and what it costs.

## Consumers

The consumer is chosen at build time; `doca/` is identical either way.

```sh
bash bench/bench.sh deploy                     # reference consumer
L7_BACKEND=linkerd bash bench/bench.sh deploy  # linkerd2-proxy
```

`shim/l7_null.c` implements the contract with no protocol of its own: opaque
streams pass through, framed streams are reassembled and routed per message, and
`decision` queries are answered. `DPUMESH_L7_FRAMED_RR=message` moves its
balancer per message instead of once per connection.

`rust/` builds `libdmesh_l7.a` from linkerd2-proxy against the port's crates.
Building it needs the toolchain the port pins and a checkout whose path the
manifest resolves:

```sh
rustup toolchain install 1.90.0
cargo build --release            # in linkerd/rust
```

`L7_BACKEND=linkerd` also starts the port's mock destination, identity and policy
binaries, which is what lets the proxy obtain a certificate without the DPU
holding cluster credentials.

## Division of ownership

| Layer | Owner |
|---|---|
| Host library, preload shim | DPUmesh |
| Host↔DPU wire ABI: control messages, forward ring, reverse ring, credits | DPUmesh (normative) |
| DPA execution units, ARM workers, SG-DMA, custody, egress arena | DPUmesh |
| Adapter contract (`dmesh_l7.h`) | Shared |
| HTTP termination, policy, load balancing, identity | linkerd port |

DPUmesh moves bytes. linkerd decides where they go.

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

`DPUMesh@785cb1a`, `linkerd2-proxy@4f926826`.

Present: `DmeshIo` as an `AsyncRead`/`AsyncWrite`/`Peek`/`PeerAddr` endpoint, the
acceptor that feeds it through the outbound stack, a connector that reaches
DMA-provided backends, and the mock control-plane binaries.

Absent: inbound proxying, more than one worker, runtime-sized connection slots,
staging flow control, a reusable backend channel registry, and the query
interface `decision` mode uses. `CONTRACT.md` §10 states which of these the
contract requires and which one bounds the integration today.
