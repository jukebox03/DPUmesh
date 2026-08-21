# Linkerd on DPUmesh

The DPU binary always links the Rust adapter in `rust/` and the pinned
Linkerd proxy fork in `port/linkerd2-proxy/`. There is no reference/null
consumer and no runtime bypass for a Service assigned to Linkerd.

## Data path

```text
Pod native/preload/gRPC API
  -> registered Host TX memory
  -> DPA SG-DMA into DPU staging
  -> Linkerd outbound policy, discovery and protocol stack
  -> exact local Pod or remote Pod UID selected by Linkerd
  -> local SG-DMA, or authenticated RDMA peer channel
  -> destination Host RX memory
```

`DPUMESH_L7_OPAQUE_SVC` enables the opaque Linkerd stack.
`DPUMESH_L7_SVC` enables protocol-aware HTTP/1, HTTP/2 and gRPC handling.
All names are Kubernetes `namespace/name` Service keys. The deployment assigns
every data Service: native and preload protocols are opaque, while gRPC is
protocol-aware HTTP/2. The ordered L4 machinery remains the substrate used by
opaque Linkerd sessions and the peer channel; it is not exposed as an alternate
deployment.

Protocol-aware frontend sessions get a session-local outbound stack so HTTP
connection pools cannot cross DMA session boundaries. Opaque sessions share a
workload stack and carry their `SessionToken` through the endpoint balancer
and connector. The backend registry consumes the exact session key and records
the exact local Pod or remote Pod UID selected by Linkerd.

Endpoint workload TLS is disabled for DMA sessions. Node-local bytes are
isolated by registered DMA mappings; node-to-node confidentiality and peer
authentication belong to the RDMA peer channel. Applying Linkerd's ordinary
sidecar TLS and transport header over `DmeshIo` would send proxy ciphertext
to a Pod because the destination DPU intentionally performs only the stock
inbound policy verdict, not a second byte-stream proxy.

## Runtime and ownership

Each of the eight ARM data workers hosts a Tokio `current_thread` runtime and
one persistent driver. The eight Linkerd runtimes form worker-local shards of
the DPU proxy: connection state, policy state, backend registry, DMA callbacks,
and reply routing remain on the worker selected by port affinity.

| Owner | State |
|---|---|
| DPUmesh | DOCA/DPA resources, rings, DMA staging, peer channels, conntrack and egress arena |
| Linkerd adapter | `DmeshIo` pairs, session registry, Tokio driver and Linkerd stacks |
| Shared ABI | [`include/dmesh_l7.h`](include/dmesh_l7.h) |

Connection handles include a worker-local incarnation above the encoded
`(pod, port)` key. Late Rust callbacks therefore cannot attach to a new
connection that reused the same Host port.

EOF is directional. `l7_conn_eof` ends one input half, Linkerd drains all
earlier output, and `dmesh_l7_tx_fin` publishes the corresponding ordered
transport FIN. A normal session is retired only after both output FINs are
accepted. If a stack endpoint disappears before that handshake,
`dmesh_l7_session_failed` makes the C datapath abort both remaining halves and
release their custody; it cannot be mistaken for a graceful half-close.

## Build and deploy

```sh
./bench/bench.sh linkerdbuild
./bench/bench.sh build
./bench/bench.sh deploy
```

The deployment requires:

- Linkerd destination, policy and identity services and their authenticated
  management-link relay;
- DPU identity material and trust anchors;
- signed membership, topology, workload-scope and Service-target feeds;
- `DPUMESH_L7_FAIL_CLOSED=1`.

The Service-target feed presents real ClusterIPs and ready endpoint addresses
to Linkerd. A selected local endpoint resolves to its current registered Pod;
a selected remote endpoint resolves to its topology Pod UID and opens the
corresponding peer stream. Cross-Service, stale and unresolved selections fail
closed and never fall through to kernel TCP.

See [`design/DATA.md`](../design/DATA.md) for byte ownership and
[`design/CONTROL.md`](../design/CONTROL.md) for identity, policy and peer
channel rules.

## Submodule

```sh
git submodule update --init linkerd/port/linkerd2-proxy
```
