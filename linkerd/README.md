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
It is an aarch64 archive linked into the DPU binary, so it is built on the DPU:
`rust-toolchain.toml` pins 1.90.0 and `Cargo.lock` fixes the dependency graph,
and both builds use `--locked`.

`L7_BACKEND=linkerd bash bench/bench.sh deploy` does the whole of it, and
nothing here has to be prepared by hand:

```text
1  rsync linkerd/rust/ and linkerd/port/linkerd2-proxy/ to ~/l7build on the DPU
2  cargo build --release --locked                     -> libdmesh_l7.a
   cargo build --release --locked -p linkerd-app-integration
       --bin mock-identity --bin mock-destination --bin mock-policy
   install the three binaries into ~/l7build/mock/
3  preflight: staticlib readable, three mocks executable, fixtures readable
4  meson -Dl7_backend=linkerd -Dl7_lib_path=~/l7build/rust/target/release/libdmesh_l7.a
5  start the mock control plane, then dpumesh_dpu, then the pods
6  one connection through the layer, as a validation
```

`bash bench/bench.sh linkerdbuild` runs steps 1–3 alone. `L7_BACKEND=null`
performs none of them: no Rust build, no mocks.

The mock destination, identity and policy binaries are the port's own, and are
what lets the proxy obtain a certificate without the DPU holding cluster
credentials. The DPU layout is one tree:

```text
~/l7build/
  rust/                    linkerd/rust, with Cargo.lock and rust-toolchain.toml
  port/linkerd2-proxy/     the port; sibling of rust/, which its manifest resolves
  mock/                    mock-identity, mock-destination, mock-policy
```

Fixtures — the trust anchors and the identity token — are read from
`~/l7build/port/linkerd2-proxy/linkerd/app/integration/src/data`.

Service and pod identifiers reach the proxy as addresses under `10.96.0.0/16`
and `10.97.0.0/16`, outside the loopback range an outbound proxy refuses to
dial. `LINKERD_BACKEND_ADDR` must be the address the layer publishes for the
service under test, `10.96.0.<service id>:9092`. `LINKERD_ADMIN_ADDR` (default
`127.0.0.1:4191`) serves the proxy's metrics: protocol detection and per-route
request counts.

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
contract requires and which one bounds the integration today; §12 is what a run
will actually do, with the decline codes.

`CONTRACT.md` §9 defines the `own-datapath` feature that separates the
standalone binary's build from the embedded one. That split lives in the
checkout under `port/` and is not committed upstream, so a clean recursive clone
does not yet reproduce it. What it resolves to is read off the DPU:

```text
cd ~/l7build/rust               cargo tree -e features   ->  dmesh-doca, no own-datapath
cd ~/l7build/port/linkerd2-proxy cargo tree -p linkerd2-proxy -e features
                                                         ->  dmesh-doca/own-datapath
```

Checking the standalone side needs two things this integration does not: the
port's own C datapath beside the proxy, and the toolchain flag linkerd2-proxy
builds itself with.

```sh
rsync -a linkerd/port/DPUMesh/ $DPU:~/DPUMesh/   # build.rs resolves ../../../DPUMesh
ssh $DPU 'cd ~/DPUMesh && meson setup build'     # writes build/device/dpa_kernel.a
ssh $DPU 'cd ~/l7build/port/linkerd2-proxy && \
          RUSTFLAGS="--cfg tokio_unstable" cargo +1.90.0 check -p linkerd2-proxy'
```

Without `tokio_unstable` the check stops in `kubert-prometheus-tokio`, which is
the proxy's own requirement and not something the split introduces.
