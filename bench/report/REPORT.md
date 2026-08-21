# DPUmesh Evaluation

DPUmesh exposes three application adapters — the native API, the LD_PRELOAD
socket shim, and gRPC — and all three reach a backend the same way: registered
Host TX memory, DPA SG-DMA into DPU staging, the Linkerd outbound stack running
inside the DPU, and a DMA write into the destination Pod's RX memory. Nothing
here compares a meshed path against an unmeshed one. Every number below was
produced with Linkerd in the path, choosing the endpoint.

The two protocol treatments the deployment assigns are not benchmark knobs.
Native and preload carry byte streams, so their Services are opaque
(`DPUMESH_L7_OPAQUE_SVC`): policy, discovery, and the endpoint balancer run, and
the balancer's choice is made once per connection. gRPC is HTTP/2
(`DPUMESH_L7_SVC`): the same stack runs, plus per-request routing through the
HTTP layer. A byte stream has no request boundary for the proxy to route on, so
the opaque treatment is the most Linkerd a native or preload Service can use.

## The path is real

Both treatments were read out of the proxy's own metrics while load was running.

Opaque, under native load, from one of the eight worker runtimes:

| Metric | Value |
|---|---|
| `tcp_open_total{authority="echo-dpumesh…", dst_pod="echo-dpumesh-…lbpg7"}` | 6 |
| `tcp_open_total{authority="echo-dpumesh…", dst_pod="echo-dpumesh-13-…xlxfr"}` | 7 |
| `tcp_open_total{authority="echo-dpumesh…", dst_pod="echo-dpumesh-14-…vcnwv"}` | 1 |
| `outbound_tcp_protocol_connections_total{protocol="detect"}` | 20 |

The three rows are three distinct backend Pods of one Service, each resolved
through service discovery — the balancer picked among them, it did not forward
to a fixed address.

Protocol-aware, under gRPC load, from the same worker:

| Metric | Value |
|---|---|
| `request_total{direction="outbound", authority="echo-grpc-dpumesh…", dst_pod="echo-grpc-dpumesh-…lg75l"}` | 491,094 |
| `outbound_http_route_request_duration_seconds_count` | 490,915 |

Half a million HTTP requests traversed the outbound route layer on one worker
alone. Sessions themselves spread across all eight worker runtimes (25–28
sessions each over the campaign), which is the port-affinity sharding doing its
job.

## Policy

Nothing above this line was decided by a policy. The namespace held no `Server`,
no `AuthorizationPolicy` and no route, so every connection was admitted by the
default the proxy is configured with: the enforcement point ran, but nothing had
ever asked it to refuse. These are the arms that did.

The subject is the native Service's three backends on the port they serve. Each
arm attaches a resource, offers one point with connection churn — a verdict is
taken once per connection and remembered on it, so a run that opens one
connection tests one verdict — and reads both the client's result and the DPU's
own verdict counters.

| Arm | Attached | Requests completed | Verdicts |
|---|---|---:|---|
| P0 | nothing | 41,955 | 211 admitted |
| P1 | a `Server` and no authorization | 0 | 1 denied |
| P2 | + `AuthorizationPolicy` naming the caller's identity | 44,582 | 224 admitted |
| P3 | the same, naming a different identity | 0 | 1 denied |
| P4 | + `AuthorizationPolicy` naming the caller's address | 44,100 | 221 admitted |
| P5 | the same, naming a different network | 0 | 1 denied |
| P6 | nothing | 42,046 | 211 admitted |

The `Server` stays attached from P1 to P5; a `Server` with no authorization
denies everything, which is what makes P1 the deny arm and what each later arm
is authorizing against.

P3 and P5 are what make P2 and P4 mean anything. A policy that admits whatever
is offered would pass P2 and P4 on its own; changing one field of the same
resource to name a caller that is not this one refuses every connection, so the
identity string and the client address are compared rather than carried.

Neither input comes from the Pod. The identity is built from the service account
and namespace in the Pod's node-agent-signed assertion, and the address is the
cluster address that assertion binds — which matters because an authorization's
`networks` clause is matched before its identity clause and an empty match
denies, so a synthetic source address would make every realistic policy refuse
every connection.

### The verdict used to lapse when a Service fell quiet

The first pass of these arms found a hole, and it is worth stating because the
shape of it is not obvious from either the code or the traffic.

An inbound policy watch is held per destination workload and port, and the store
that caches watches evicts one that has gone 90 s without a connection. An
evicted watch is respawned holding the proxy's *configured* default, and its
value is replaced only when the policy controller answers — asynchronously,
after the connection that respawned it has already been decided. Each of the
eight ARM workers holds its own store, so the window was one connection per
worker, and it reopened every time a Service went quiet.

| Arm, before the fix | Result |
|---|---|
| P1, watch warm from the previous arm | 0 admitted, refused on the first connection |
| P1b, after 100 s of silence | **8 admitted**, then refused |

Nothing about the traffic showed it. The run failed either way; only the verdict
counter said whether it failed for the right reason.

Three changes close it, and none of them is a timeout:

- the adapter **holds** each destination's watch for as long as that Pod is
  registered, so nothing evicts it and no respawn can return it to the default;
- a watch the controller has not answered yet reports **no verdict** rather than
  the default's answer, so the destination Service's protection class decides —
  the same answer a port no policy names gets;
- each worker starts a Pod's watch when that Pod registers, which puts the
  answer in place before the Pod's first caller arrives, and drops it when the
  registration ends, which is what bounds a set that no longer expires.

After the fix the same arm refuses the first connection, and the campaign now
fails the stage if it admits anything at all:

| Arm, after the fix | Result |
|---|---|
| P1b, after 100 s of silence | 0 admitted, refused on the first connection |

The deploy's own smoke gate is the second reading: every connection it makes now
carries a resolved verdict, where before the fix the first connection to each
destination reported no policy.

## Routing

A route can only apply where there are requests to route, so the subject is the
protocol-aware gRPC Service. The route's parent is the Service the DPU dials —
the ClusterIP its outbound policy discovery asks about — and its match is the
gRPC method path the benchmark calls.

| Arm | Attached | Requests completed | What the proxy reports |
|---|---|---:|---|
| R0 | nothing | 11,770 | `route_kind="default"` |
| R1 | `HTTPRoute` matching the method | 11,738 | 11,838 requests under `route_kind="HTTPRoute"` |
| R2 | the same route, matching a path nothing calls | 0, and 35,752 failed | no route matched |
| R3 | the same route, `backendRefs` in another Service | 0 | 81 `dmesh_backend_target_mismatches_total` |
| R4 | nothing | 11,845 | `route_kind="default"` |

R1 shows the route is consulted; R2 shows it decides. A rule that cannot match
is the whole difference between every request being served and none of them
being served, which is not something a decorative route layer could produce.

R3 is a limit rather than a defect. Linkerd may replace a Service's ClusterIP
with one of that Service's endpoints, but it may not move a DMA session to a
different Service: the session's backend channel was published against one
Service key and the signed generation places the selected address in another, so
the connector refuses it and counts it. Nothing falls through to kernel TCP.
Traffic splitting across Services — the ordinary use of `backendRefs` weights —
is therefore not available here; splitting *within* a Service is what the
balancer already does.

## Load balancing

The client stamps the serving Pod's id into every reply, so the distribution is
observable from outside the DPU. The two protocol treatments balance at
different grains, and that is a consequence of how their stacks are built rather
than a setting: an opaque session shares one workload stack whose balancer holds
every ready endpoint, while a protocol-aware session gets a session-local stack
whose balancer can only reach the endpoint that session's backend channel
serves.

Opaque, three backends, one connection per client thread, three repetitions of
each:

| Client threads | Backends reached, per repetition |
|---|---|
| 1 | 1, 1, 1 |
| 2 | 2, 2, 1 |
| 4 | 1, 2, 3 |
| 6 | 3, 3, 3 |

One connection can only land on one endpoint, so a single thread reaching one
backend is what connection-grained balancing means; across repetitions the
backend it reaches changes. Six connections reach all three in every repetition.
The four-thread repetition that put all four on one backend is ordinary p2c at
that sample size, and it is the reason a single repetition is not evidence about
a balancer.

The endpoint set is followed rather than fixed at start-up. Scaling one backend
to zero and offering the same six-connection load leaves the balancer holding
two ready endpoints, and only those two serve; restoring the backend brings it
back to three. No request failed in either direction.

| Endpoint set | `outbound_tcp_balancer_endpoints{ready}` | Backends that served, union of three repetitions |
|---|---:|---|
| one of three withdrawn | 2 | 2 of 2 |
| restored | 3 | 3 of 3 |

The protocol-aware Service balances per session, not per request. With two
backends and one client channel every one of 25,104 requests went to a single
Pod, and the session's balancer reported one ready endpoint. The same load over
four channels reached both Pods, and each session still saw one ready endpoint
of its own:

| gRPC client channels | Ready endpoints in a session | Requests served, by Pod |
|---|---:|---|
| 1 | 1 | 25,104 / 0 |
| 4 | 1 | 90,801 / 29,974 |

Discovery resolves both endpoints — the second is present and stays `pending`.
What the balancer cannot do is dial it: the session owns exactly one backend
channel, a second take is refused and counted in
`dmesh_backend_take_errors_total`, and nothing is dialled over TCP instead.
Spread across backends therefore comes from the number of client channels, and
an application that opens one channel per destination Service will use one
backend of it however many replicas it has.

This is also how to read the tables below: the native Service has three backends
and the preload and gRPC Services have one, so part of native's throughput is
server-side parallelism its balancer hands it.

## Round-trip latency, one outstanding request

One client core, one server core per Pod, three repetitions, median.

| Request | native p50 | preload p50 | gRPC p50 | native p99 | preload p99 | gRPC p99 |
|---|---:|---:|---:|---:|---:|---:|
| 64 B | 337 | **148** | 1,253 | 687 | **568** | 1,525 |
| 1 KiB | 337 | **151** | 1,284 | 695 | **569** | 1,547 |
| 8 KiB | 1,003 | **907** | 1,258 | 1,268 | **1,105** | 1,578 |

Two facts sit in this table. The preload shim answers a small request in half
the time the native API does — about 190 µs of fixed cost separates them, and it
is fixed: at 8 KiB, where transfer dominates, the gap falls to 96 µs. And gRPC's
latency barely moves across a 128× change in frame size (1,253 → 1,258 µs),
because what it spends is per-message protocol work, not per-byte transfer.

## Throughput on one client core

Concurrency 32 on a single connection, one client core.

| Request | native req/s | preload req/s | gRPC req/s |
|---|---:|---:|---:|
| 64 B | 34,619 | **76,586** | 2,194 |
| 1 KiB | 45,042 | **87,073** | 2,107 |
| 8 KiB | 49,898 | **65,366** | 1,879 |

Neither L4 arm is CPU-bound here — the client core sits at 0.07 (native) and
0.14 (preload) of a core. The ceiling is the closed loop's own arithmetic: 32
outstanding requests divided by the round-trip time. The latency gap above is
therefore the whole throughput gap, and gRPC's 2 K/s is the same arithmetic
applied to a 15 ms queueing latency at 32 concurrent streams on one channel.

## Scaling client threads

Six client cores and six server cores, 1 KiB, one connection (gRPC: one
channel) per thread.

| Threads | native req/s | preload req/s | gRPC req/s |
|---|---:|---:|---:|
| 1 | 38,799 | 27,458 | 2,216 |
| 2 | 86,113 | 57,073 | 5,312 |
| 4 | 192,361 | 151,310 | 12,334 |
| 6 | **303,217** | 273,745 | 19,439 |

The ordering from the single-connection table inverts. Native scales 7.8× over
six threads and passes preload, because the per-request cost that decides a
multi-core ceiling is lower for native even though its single-request latency is
higher. gRPC scales 8.8× — its curve is not flat, it is simply an order of
magnitude below.

Adding cores does not by itself help a single thread: native at one thread is
38,799 req/s across six cores against 45,042 on one, and preload drops from
87,073 to 27,458. Spreading one thread over six cores costs locality and buys
nothing.

## What a request costs

Host CPU is each Pod's cgroup `usage_usec` — scheduler accounting, not the
100 Hz tick, which undercounts a core that runs in bursts. DPU CPU is the summed
tick delta of the data path's threads. The window opens after the load is in
steady state and closes before it ends. For native the server figure sums all
three backends, since the balancer chooses which one serves.

At each arm's own saturation point (concurrency 32, one thread):

| API | req/s | Host cores | ARM cores | Host µs/req | ARM µs/req |
|---|---:|---:|---:|---:|---:|
| native | 48,952 | 0.139 | 0.534 | **2.91** | 11.43 |
| preload | 89,224 | 0.330 | 1.029 | 3.80 | 11.53 |
| gRPC | 2,065 | 0.153 | 1.059 | 74.18 | 512.62 |

Native and preload cost the DPU the *same* per request — 11.43 against 11.53 µs
of ARM — which is what should happen: below the adapter they are the same
opaque session on the same data path. They differ on the Host, where native's
submission path is 23% cheaper per request. gRPC costs 45× the ARM time of
either.

Because those three rows sit at three different rates, and per-request cost
falls as load rises, they are also measured at one offered rate every arm can
serve:

| API | 8,000/s host µs/req | 8,000/s ARM µs/req | 15,000/s host µs/req | 15,000/s ARM µs/req |
|---|---:|---:|---:|---:|
| native | **37.65** | 104.08 | **27.36** | 95.87 |
| preload | 55.14 | **100.40** | 40.72 | 98.21 |
| gRPC | 133.45 | 495.99 | 101.46 | 400.77 |

The relative picture holds at a matched rate: native is 1.46–1.49× cheaper
than preload on the Host, the two are indistinguishable on the ARM, and gRPC
costs 2.4–3.7× the Host and 4.1–4.9× the ARM of either. The absolute numbers are
much larger than the saturation table because 8–15 K/s is a lightly loaded
regime for this rig — the fixed per-second work of eight worker runtimes is
divided among few requests. Comparing a per-request cost across the two tables
measures the load level, not the API.

## Why gRPC costs what it does

The proxy builds a session-local outbound stack for every protocol-aware
session and a workload-shared one for every opaque session
(`dmesh_session_stack_cache_hits_total` climbed while
`dmesh_session_stack_builds_total` stayed flat during the opaque arms). HTTP
parsing turns a byte stream into requests, and those requests carry H1/H2 pools
and reconnect caches that must not cross a DMA session boundary, so that stack
cannot be shared.

The larger term is per-request. An opaque session does policy, discovery, and
balancer dispatch once per connection and then moves bytes; a protocol-aware
session does route matching and balancer dispatch once per *request*, which is
why its per-request cost does not amortise as offered load rises the way a byte
proxy's does. The proxy pays that dispatch on every request even though the
answer cannot vary: the session owns one backend channel, so the endpoint the
balancer may reach is fixed for the life of the session.

## Deployment constraints

Everything above measures a path that works. What follows is what a deployment
has to accept in order to get it. None of it is a tuning knob.

**A Pod is meshed by editing its spec.** There is no injection. Linkerd enters a
workload through one namespace annotation and a mutating webhook; this tree has
no webhook, and its controller publishes the signed topology and nothing else.
Every meshed Pod carries, by hand: `privileged: true`, a `/dev/infiniband`
hostPath, a hostPath mount for `libdpumesh.so`, `DPUMESH_PCI_ADDR` and
`DPUMESH_SERVICE`, and `config.linkerd.io/skip-inbound-ports` on the data port.

That last annotation is load-bearing, not cosmetic. Removing it from the three
live backends — an annotation edit on running Pods, no restart — made the
destination controller advertise those endpoints as meshed, and the next point
returned `rcnt=0 fail=1` with `Linkerd session ended before both output FINs` in
the DPU log. Restoring the annotation restored the path on the following point.
A backend has to look unmeshed to Linkerd's destination controller, because the
DPU *is* the proxy and there is no second one waiting behind the endpoint
address.

**The application must use one of the three adapters.** The preload shim is the
one that takes unmodified binaries, and it takes them properly: it occupies the
application's file descriptor number with a kernel `socketpair` end, so `epoll`,
`poll` and `select` see ordinary readiness and an event-loop application needs
no change. What it cannot do is attach to a binary `LD_PRELOAD` does not reach —
a static link, or a runtime that issues syscalls without going through libc,
which is every Go program. A large part of the Kubernetes ecosystem is Go, and
for those workloads the native API or the gRPC adapter means a source change.

**There is no endpoint mTLS.** Every outbound request the proxy counted carries
`tls="no_identity"` with `no_tls_reason="not_provided_by_service_discovery"`:
discovery offers no identity for these endpoints, and a DMA-session endpoint
answers `ConditionalClientTls::None(Disabled)` in both the TCP and the HTTP
stacks, so no handshake would be attempted even if it did. This is the design's
own trade — node-local bytes are isolated by the registered DMA mapping and
node-to-node confidentiality belongs to the authenticated RDMA peer channel,
which is why identity is established at registration rather than per connection.
It is still a trade: an audit requirement that reads "the proxy encrypts every
hop" is not met by this deployment, and the same annotation that keeps the data
path working is what makes discovery withhold the endpoint identity.

**Thirty-two Pods per node.** `MAX_PODS` is the Pod-state table's capacity, and
the live cap the DPU enforces is `MAX_DPA_RINGS x N / K` — eight rings per
execution unit, eight units, two rings per Pod. On this deployment the two meet
at 32, so raising the constant alone buys nothing: past it, a Pod needs either
fewer rings each or a larger per-unit ring array, and the ring array is DPA
device code. The wire format itself reaches 127.

**Routing and balancing stay inside one Service.** A session's backend channel
is published against one Service key, so an `HTTPRoute` may reorder, filter or
reject requests within its parent Service but may not send them to another one,
and a request that names a foreign Service is refused rather than dialled over
TCP. Within a Service the choice is per session: a session owns exactly one
backend channel, so spread across backends comes from the number of client
connections or channels, not from per-request dispatch.

## Method and limits

- Deploy: `BENCH_DEPLOY_SCOPE=all bench/bench.sh deploy` at `1518aae`,
  N/K/A/L = 32/8/8/8, all eight ARM workers hosting a Linkerd runtime,
  fail-closed L7 policy, node-agent-relayed control plane.
- Campaign: `bench/suite/api_l7_compare.sh` (phases A/B/C, 99 points, 3
  repetitions) and `bench/suite/api_l7_cost.sh` (closed and open-loop CPU, 3
  repetitions each). Raw data in `data/api-l7-20260821/`.
- Policy, routing and balancing: `bench/suite/policy_route.sh`, with the
  resources it attaches in `bench/k8s/policy/`. Every arm is judged twice — by
  whether the client's requests completed and by the DPU's own counters —
  because traffic that stops without a matching verdict is not a policy result.
- Raw data in `data/policy-route-20260821/` for the first pass, which is where
  the lapsing verdict was found, and `data/policy-route-20260821-fixed/` for the
  build these tables report. The balancing arms were not re-run against the fix;
  it does not touch the data path.
- Every one of the 99 points carried `fail=0 drops=0 overflow=0 worker_fail=0
  reorder=0`. No point was discarded.
- There is no host-sidecar control. The deployment builds one arrangement — the
  DPU-hosted mesh — so these numbers rank the three APIs against each other, not
  the mesh against a sidecar.
- The three Services do not hold equal backend counts (3 / 1 / 1). Native's
  throughput includes the server-side parallelism its balancer provides.
- The policy arms attach resources to one Service and read process-wide verdict
  counters, so they show that enforcement happens and what decides it. They do
  not measure what enforcement costs; no arm was run with and without a policy
  at a matched rate.
- Run-to-run spread on this rig is several percent; differences smaller than
  that are not differences.
- Latency figures are the client's own histogram; the DPU is not instrumented
  per request.
