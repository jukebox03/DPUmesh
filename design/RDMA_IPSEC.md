# Per-Pod-pair RDMA with inline IPsec

Inter-node data travels over RoCEv2 RC SEND/RECV and the NIC applies IPsec ESP.
Distinct Pod pairs never share a data key. Node authentication uses the DPU's
Ed25519 key together with the signed topology. Traffic inside a node stays
plaintext; the inter-node segment is protected by hardware IPsec instead of a
TLS record layer.

## Unit of isolation

The logical unit is a Pod-pair association. Each direction carries one security
association, and every worker lane serving that Pod pair shares it. A transmit
SA owns a single sequence-number and IV counter across all lanes that reference
it, so an SA with the same key and salt is never duplicated. When a lane needs
its own key, the lane identifier enters the key-derivation context and produces
a separate SA.

The application QP on the host is a byte stream and the RC QP the DPU builds is
a different object. Several application streams share one RC QP only within the
same Pod pair and lane.

## Components

```
Pod → host shared TX memory → DPA DMA / Arm worker → Pod-pair RC QP lane
   → SF representor / TX SA ──ESP──▶ RX SA (authenticate, replay)
   → permitted-QPN check → peer RC QP
node crypto manager A ◀──TCP / node TLS 1.3──▶ node crypto manager B
```

Three processes take part. The **worker** (`dpu_worker`, `dpu_proxy`) is the data
path and owns the QP and channel of each Pod-pair lane. The **manager**
(`peer_manager`), one per node in its own thread, drives the node-pair control
session and the association lifetime. The **crypto owner**
(`dpumesh_crypto_owner`) is a separate process that alone holds the NIC's DOCA
Flow IPsec state and installs or removes SAs and rules only on the manager's
request. Neither workload code nor the manager touches the device.

## Control session and key agreement

Each node pair keeps one TLS 1.3 control connection per direction over TCP
(`peer_control`, `peer_tls`). Session resumption is disabled so every connection
performs a fresh key agreement. HELLO exchanges cluster identifier, node name,
boot nonce and worker count, and handshake completion is distinct from pinning
the topology key. Before the pin there is no exporter output and no SA or QP
allocation. An inbound connection does not know its peer in advance, so the
authenticated key is resolved to a node name through the topology
(`dmesh_topology_node_by_key`) and then pinned.

Keys come from the OpenSSL exporter (`SSL_export_keying_material`, label
`EXPORTER-DPUmesh-IPsec-v1`). The context is a canonical encoding of protocol
version, cluster, both endpoints (node, public key, boot nonce, Pod UID,
registration incarnation), session, association, suite, epoch, direction and
receive SPI. Both sides must encode the same bytes to derive the same key. The
suite is AES-128-GCM with a 4-byte salt, a 16-byte ICV and ESP transport mode.
The receiving manager reserves the SPI in the 32-bit space and checks it against
current and retiring SAs. Key material is held only until the hardware install
consumes it and is wiped on completion.

Control frames and negotiation bodies (`peer_security_wire`) are serialized with
explicit endianness rather than copied from C structures. The messages are
HELLO, PROPOSE, ACCEPT, LANE_PARAMS, RX_READY, TX_READY, LANE_READY, ACTIVATE,
QUIESCE, DRAINED, COMMIT, REVOKE and ERROR.

## Automatic negotiation and install order

When a worker opens a Pod-pair stream the manager performs the following
automatically.

1. **PROPOSE.** The proposer sends the association identifier, its receive SPI
   and a lane mapping sized to the minimum of the two worker counts.
2. **AUTHORIZE and PREPARE.** The accepter queries its worker for the local
   registration snapshot and port policy without allocating a QP, rechecks
   policy, then creates INIT QPs and posts receive queues.
3. **ACCEPT and LANE_PARAMS.** Each side returns its own SPI, registration and
   lane halves (QPN, PSN, GID, MTU), and both record the association in the
   registry (`peer_association`).
4. **INSTALL_RX, RX_READY, INSTALL_TX, TX_READY.** The manager has the crypto
   owner install the directional SAs. The receive SA authenticates, drops
   replays and checks the local QPNs permitted for that SA; the transmit SA
   encrypts and forwards. There is no plaintext miss path.
5. **ENABLE_QPS.** Only after both sides have receive and transmit protection in
   place does the worker move its QPs to RTR and RTS.
6. **GRANT.** The worker accepts application SEND only on lanes past ACTIVE and
   the peer activation barrier, and rechecks authority and generation
   immediately before activation.

The manager splits the requests emitted by `peer_association_next` into crypto
actions sent to the owner and worker actions sent through a mailbox, collects
every lane completion and completes once. Accepting a command is not completion.
An unknown outcome quarantines the association; completion is never inferred
from a timeout.

## Worker transport layer

`peer_pair_transport` gives each worker an independent transport layer and a
manual verbs factory (`peer_wire_verbs`, implemented by `peer_wire_rdma`).
Between the control owner and the worker sit two 64-entry single-producer
single-consumer mailboxes and an eventfd, with the roles split as `OPEN`,
`AUTHORIZE`, `PREPARE`, `ENABLE`, `GRANT`. QPs are created in INIT without a
connection manager and receive queues are posted there; RTR and RTS wait for the
protection barrier on both sides. The worker's PD and CQ, its lease, CQ
generation, 16-entry send queue, 32-entry receive queue and 73728-byte slots are
reused. Memory is retained when a QP or MR destroy fails.

`peer_pair_wire` translates each node-channel frame into the canonical lane
frame: a 64-byte `DMSD` header and a body of up to 64 KiB. Association, lane and stream generations are validated, and
handle reuse is covered by a separate stream generation. On would-block the
already encoded bytes are kept as they are and no new generation is issued. A
send completion returns only the send slot; source custody returns with
STREAM_ACK after the destination DMA and REV_DONE.

## Hardware graph in the crypto owner

The owner opens the uplink PF and the worker's SF representor in DOCA Flow
switch mode and directs the root miss to the kernel target, which keeps the OVS
and TC path intact. Traffic it does not own is left alone. The graph on the
switch port is:

```
ingress root  SF, local→peer, UDP  → tx_select: dst peer + dest QPN
                                     → meta{encrypt,sa} → egress root
              PF, peer→local, ESP  → decrypt: dst local + SPI
                                     → decrypt(sa) → decap → egress root
              PF, peer→local, UDP  → rx_plain: UDP/4791 → DROP, else kernel
              miss                 → kernel (OVS/TC)
egress root   meta{encrypt}, local→peer → encrypt: ESP encap + encrypt(sa) → PF
              meta{decrypt}, peer→local → rx_gate: meta sa + dest QPN → SF,
                                          miss DROP
```

The transmit `destQP` is the peer QPN and the receive `destQP` is the local QPN.
The metadata field, two path bits plus a 20-bit SA identifier, needs an explicit
mask to become part of the key so that per-SA entries stay distinct.
Classification entries for a peer IP pair are resident for the owner's lifetime;
only SA and per-lane entries follow the association lifetime. Plaintext RoCE and
unmatched QPNs are dropped, so without an SA traffic cannot reach its
destination.

The manager and the owner speak over a UNIX stream socket (`peer_crypto_ipc`).
Requests are explicitly endian-encoded frames carrying 20 bytes of key material
that both sides wipe immediately after handoff, and the socket is mode 0600. If
the owner disconnects, in-flight requests complete with an unknown outcome and
the adapter turns unhealthy, which makes the manager revoke every open
association. A shared IPsec SA identifier is never reused within an owner's
lifetime, because resetting a shared resource the device still holds wedges port
stop. Two DOCA builds are supported: one configures the SA pool per port with
`doca_flow_port_cfg_set_nr_resources` and
`doca_flow_port_shared_resource_set_cfg`, the other globally with
`doca_flow_shared_resource_set_cfg`.

## Rekey and revocation

A rekey keeps the connection and pauses transmission briefly. The new receive SA
and its permitted QPNs are installed on both sides; after the new RX_READY,
admission stops so that outstanding SEND, DMA and STREAM_ACK drain; once both
sides report DRAINED and the new transmit SA is installed, the epoch commits.
The previous epoch survives a bounded overlap and is removed once the old flows
and QPs are done. A further rekey is refused while a retiring epoch is still
present.

Revocation runs as: block admission, block hardware transmit and receive
(BLOCK), close the worker QPs and fence DMA (DESTROY), remove flows and SAs
(REMOVE_ALL), release the slot (FORGET). A local Pod unregister reaches the
manager through a hook and closes the matching association. Memory is not
reassigned to another Pod until DMA already posted has finished. On owner
shutdown the switch port stop reclaims the graph, and a teardown watchdog turns
a stalled stop into a fail-closed exit. Flows used by live associations are
already gone at that point, so no plaintext path survives.

## Authority freshness

A topology version is not an expiry and redelivering the same document is not a
refresh. Per association the manager keeps three leases, authority, policy and
control, as monotonic deadlines and applies the earliest expiry. The control
lease is refreshed by a challenge/response heartbeat over TLS, never by
connection state alone. The worker also checks the local deadline immediately
before a new SEND and before delivering a received payload. The freshness
sources behind the three leases, a signed security feed and a policy watch, are
placeholder values, and replacing them is outstanding work.

## Configuration

`DPUMESH_PEER_TRANSPORT=rdma` selects the RDMA carrier and
`DPUMESH_PEER_SECURITY=ipsec-pod-pair` turns on Pod-pair protection. The worker transport device comes from
`DPUMESH_PEER_RDMA_DEVICE`, `_GID_INDEX` and `_MTU`; the control TCP endpoint
from `DPUMESH_PEER_BIND` and `_PORT`; the crypto owner from
`DPUMESH_PEER_CRYPTO=unix:PATH`; the lease window from `DPUMESH_PEER_LEASE_MS`;
and time-based rekey from `DPUMESH_PEER_REKEY_S`, where 0 disables it. Without a
crypto owner the runtime refuses to start rather than falling back to plaintext.
Run the owner as:

```
dpumesh_crypto_owner --socket /run/dpumesh/crypto.sock --pci 0000:03:00.0 \
                     --sf-iface en3f0pf0sf0 [--sa-limit N] [--ovs-guard BRIDGE]
```

The owner must be up before the runtime, and shutdown order is runtime then
owner. `--ovs-guard` leaves a plaintext UDP/4791 DROP rule on the SF so that
plaintext stays blocked even when the owner is absent. Meson builds the owner
only when `doca-flow`, `doca-dpdk-bridge` and `libdpdk` are present, and the
runtime itself does not link DPDK.

## Validation

Without a device, `make test-peer-security` covers the exporter, the codecs,
control, association, pair transport and the manager. Within it
`peer_manager_test` places two nodes in one process and drives negotiation,
GRANT, data, rekey and revocation over fake verbs and fake hardware.
`peer_crypto_ipc_test` covers the frame codec together with owner loss,
reconnection and backpressure.

Hardware validation lives in `inline-crypto/coexist/`, with results in
[OWNER_RESULTS.md](../inline-crypto/coexist/OWNER_RESULTS.md). It runs on two
DPUs carrying different DOCA versions. The owner passes dynamic SA and rule
install, rekey and removal, and `peer_e2e_probe` has two nodes negotiate
automatically over real TCP and exchange encrypted RDMA on the lanes whose SAs
the owner installed. Performance is compared on the same path by swapping the
owner for a stub adapter. Inline IPsec adds roughly 6 to 7 us per round trip and
costs 5 to 13% of throughput, which at 64 KiB is about 1.36 GB/s encrypted
against about 1.44 GB/s plaintext.

Outstanding validation: simultaneous isolation of two Pod pairs, rejection of
replay and plaintext injection, lossless live rekey, signed security feed and
policy watch freshness, and the full host data path through real Kubernetes Pod
registration and the broker.
