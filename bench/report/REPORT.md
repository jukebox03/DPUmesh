# DPUmesh Evaluation

> **Current gRPC build note (2026-09-02).** The cross-adapter tables below are
> retained receipts for their 2026-08-25 build. Commit `36d095d` subsequently
> moved DOCA ownership into a per-Pod broker. Its unoptimized baseline is
> [`grpc-broker-baseline-20260901/`](data/grpc-broker-baseline-20260901/FINAL.md),
> and the same-build batching/CPU diagnosis plus retained optimization is
> [`grpc-batching-20260901/`](data/grpc-batching-20260901/FINAL.md). The follow-up
> [`grpc-l7-perf-20260901/`](data/grpc-l7-perf-20260901/FINAL.md) fixes the
> embedded build's missing Linkerd jemalloc and release LTO, and profiles the final
> DPU hot path. The independent professor-facing repetition in
> [`grpc-professor-20260902/`](data/grpc-professor-20260902/ANALYSIS.md) is the
> current receipt; its `FINAL.md` is the short professor-facing summary
> (correctness gates, max RPS and 10k RPS latency against per-Pod Linkerd). Capacity carries two definitions there: delivery-clean
> 64 B 90k / 1 KiB 75k / 8 KiB 29.75k RPC/s, and p99 ≤ 5 ms 80k / 70k / 20k.
> Its central finding is that DPU worker CPU follows the number of requests in
> flight (0.69 core per open request, 692 ARM µs/RPC at 500 RPS against 66–87 at
> the knee); per-thread PMU counters trace this to a per-event fixed cost
> (467k instructions, 4.9k cache misses and 747 µs per 64 B RPC at 100 RPS on
> one worker against 173k / 1.7k / 154 µs at the knee), not a spin, a timer or
> the Host coalescer. The single-request closed-loop p50 floor is 0.94 ms and
> the 100 RPS open-loop p50 is 1.6 ms. Against the same application DPUmesh is
> 0.39× direct-TCP and, per configured proxy core, level with a 1-core Linkerd
> sidecar (13.3k vs 12.5k/s at 64 B). A=4/6/8/12 workers give 40k/70k/80k/130k.
> The four pre-registered experiments that settle the open questions are in its
> `EXPERIMENT.md`. These receipts do not replace the old native/preload
> comparison arms.

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

Every arm below is judged twice — by what the client completed, and by what the
DPU's own counters say the enforcement point decided — because traffic that
stops without a matching verdict is not a policy result, and traffic that flows
without one is not a routing result. A stage whose client returns no reply at
all is recorded as `nodata` and fails: a missing measurement must never read as
a refusal, or a stopped instrument would pass every arm that expects traffic to
stop.

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

The subject is the native Service's three backends on the port they serve. Each
arm attaches a resource, offers one point with connection churn — a verdict is
taken once per destination a stream reaches and remembered on the connection, so
a run that opens one connection tests one verdict — and reads both the client's
result and the DPU's own verdict counters.

| Arm | Attached | Requests completed | Verdicts |
|---|---|---:|---|
| P0 | nothing | 41,955 | 211 admitted |
| P1 | a `Server` and no authorization | 0 | 1 denied |
| P2 | + `AuthorizationPolicy` naming the caller's identity | 44,582 | 224 admitted |
| P3 | the same, naming a different identity | 0 | 1 denied |
| P4 | + `AuthorizationPolicy` naming the caller's address | 44,100 | 221 admitted |
| P5 | the same, naming a different network | 0 | 1 denied |
| P6 | nothing | 42,046 | 211 admitted |

P0 and P6 are not "no enforcement": the namespace held no `Server`, no
`AuthorizationPolicy` and no route, so every connection was admitted by the
default the proxy is configured with — the enforcement point ran, and nothing
had asked it to refuse. The `Server` stays attached from P1 to P5; a `Server`
with no authorization denies everything, which is what makes P1 the deny arm and
what each later arm is authorizing against.

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

**A verdict is never taken against this proxy's own default.** An inbound policy
watch is held per destination workload and port for as long as that Pod is
registered, so nothing evicts it; a watch the policy controller has not answered
yet reports *no verdict* rather than the default's answer, and the destination
Service's protection class decides instead; and each worker starts a Pod's watch
when that Pod registers, which puts the answer in place before the Pod's first
caller arrives. The negative control is the deny arm re-offered after the Service
has been silent for longer than a store's idle-eviction window:

| Arm | Result |
|---|---|
| P1b, the deny arm after 100 s of silence | 0 admitted, refused on the first connection |

The campaign fails that stage if it admits anything at all, and the deploy's own
smoke gate is the second reading: every connection it makes carries a resolved
verdict.

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
| R4 | nothing | 11,845 | `route_kind="default"` |

R1 shows the route is consulted; R2 shows it decides. A rule that cannot match
is the whole difference between every request being served and none of them
being served, which is not something a decorative route layer could produce.

A route may also send requests into a *different* Service, which is the ordinary
canary shape:

| Arm | What it decided |
|---|---|
| X1 | 6,908 requests served by the other Service, `target_mismatches` flat |
| X2 | a `Server` on that Service refused all of them — 513 denials, nothing completed |
| X2b | withdrawing it restored service |
| X3 | weighted `backendRefs` split 18,386 / 18,587 across the two Services, `fail=0` |

X2 is the security half, and it is the reason the destination's own Service is
what grades a stream: the caller's Service carries no policy at all, so a stream
refused at X2 was graded by the Pod that receives the bytes rather than by the
Service the client addressed. The stage cannot use an admitted-count gate — the
caller still legitimately reaches its own Service's backend — so what it asserts
is that the destination's denial counter moved, and every refusal the DPU logged
during the stage named the same destination Pod.

What still guards the dial is liveness and node placement, not Service identity:
an endpoint the balancer selects is resolved through the signed generation to a
live Pod, a remote Pod UID, or a distinct decline. Nothing falls through to
kernel TCP.

## Load balancing

The client stamps the serving Pod's id into every reply, so the distribution is
observable from outside the DPU. The two protocol treatments balance at
different grains, and that is a consequence of how their stacks are built rather
than a setting.

**Opaque sessions balance per connection.** An opaque session shares one workload
stack whose balancer holds every ready endpoint, and one connection can only land
on one endpoint. Three backends, one connection per client thread, three
repetitions of each:

| Client threads | Backends reached, per repetition |
|---|---|
| 1 | 1, 1, 1 |
| 2 | 2, 1, 2 |
| 4 | 3, 3, 3 |
| 6 | 3, 3, 3 |

Across repetitions the backend a single thread reaches may change. The number
reached is not a fairness gate: Linkerd's p2c/EWMA balancer may legitimately
prefer a strict subset. Each arm instead requires successful traffic, no more
backends than connections, and the expected ready-endpoint count from every
active DPU worker that reports one. A missing or mixed worker reading fails.

The endpoint set is followed rather than fixed at start-up. Scaling one backend
to zero and offering the same six-connection load changes the DPU reading from
three ready endpoints to two; restoring it returns the reading to three. No
request failed in either direction. The union is reported as characterization,
not as a promise that p2c must exercise every ready endpoint.

| Endpoint set | `outbound_tcp_balancer_endpoints{ready}` | Backends that served, union of three repetitions |
|---|---:|---|
| one of three withdrawn | 2 | 2 of 2 |
| restored | 3 | 3 of 3 |

**Protocol-aware sessions balance per request.** A backend is selected for each
request and reached through its own backend channel, so spread comes from the
route rather than from how many channels a client opened. Two backends of one
Service, requests attributed by the destination Pod the DPU's own
`request_total` names:

| Client channels | Backend A | Backend B | `fail` | `take_errors` |
|---:|---:|---:|---:|---:|
| 1 | 7,843 | 9,719 | 0 | 0 |
| 4 | 48,288 | 53,189 | 0 | 0 |

One channel reaching both backends is the result. Scaling back to one replica is
also judged: the DPU reported one ready endpoint, that Pod served 16,398
requests, and `fail=0`, `take_errors=0`. This restoration arm used to be written
without a verdict and could not fail the campaign; the accepted 21/21 receipt is
[`policy-route-lb-20260824-judge-v3/stages.csv`](data/policy-route-lb-20260824-judge-v3/stages.csv).

## Linkerd surfaces

Twenty stages, each reading the enforcement point's own counter as well as the
client, because in most of them the client cannot tell an enforced policy from a
policy that was never built.

| Surface | Result |
|---|---|
| `HTTPRoute` request timeouts | enforced — the route's `REQUEST_TIMEOUT` counter moved |
| method matching | POST serves, GET matches nothing |
| header matching | `content-type: application/grpc` serves, `application/json` matches nothing |
| `GRPCRoute` | the called method serves, another method matches nothing |
| inbound `Server` | refuses by default, and refuses an unnamed identity |
| failure accrual | healthy endpoint 6,765, ejected endpoint 3 |
| HTTP/1.1 | 6,815 requests through the protocol-aware path, `fail=0` |
| retries | enforced — 136 failures reach the client without the policy, 0 with it, on 87 retries that all succeeded |
| route-level authorization | enforced — the route's caller served 6,470 requests, another identity refused with 272 denials |

Three of these measure something other than their subject unless the arm is
built carefully, and the shape of each mistake generalises:

**A timeout is not measured by what the client completes.** The client's latency
includes a DMA round trip on both sides that the route timeout does not cover,
so a 1 ms timeout against a 1.26 ms client-observed p50 catches single-digit
requests. The route's own counter is the reading.

**A breaker is not measured against an outage.** It ejects an endpoint so that
traffic lands on the endpoint beside it. An all-failing backend leaves it nowhere
to eject to, and so does a failing backend a route chose by weight — the weight
already sent those requests there. The failing endpoint has to be inside the
balancer under test, which is why it joins the Service through the Service
selector.

**A policy is not carried by every kind of route.** A gRPC failure is an HTTP 200
carrying `grpc-status`, so a gRPC retry condition lives only on a `GRPCRoute`;
annotated on an `HTTPRoute` it is dropped, and the limit beside it still builds a
retry policy with no condition, which the proxy attaches to every request and
which can never fire. The inbound half has the same shape: a `Server` whose
`proxyProtocol` is gRPC carries gRPC routes, so an authorization written against
a `policy.linkerd.io` `HTTPRoute` parented to it never appears in the port's
policy at all and the port keeps its deny-by-default.

## Automatic injection

Nine stages. An annotated Pod carries every piece of the patch — control-plane
label, `skip-inbound-ports`, library and attestation mounts,
`DPUMESH_RINGS_PER_POD`, Service identity, preload shim, node affinity — and its
traffic takes a DPU inbound verdict. The same Deployment without the annotation carries none of
it and serves 429,101 requests over kernel TCP.

That second half is what makes the feature safe to turn on: a Pod nobody
annotated carries no shim and is a working Pod. The last two stages are what
make the annotation mean something: with the webhook scaled to zero the
namespace refuses Pod creation — the API server's refusal names
`inject.dpumesh.io` — and admits the same Pod again once the webhook answers.
A Pod born unpatched would keep working over kernel TCP with no identity and no
policy, so creation waits instead (`failurePolicy: Fail`).

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

Per-request backend selection adds nothing measurable to these prices. With a
50/50 weighted route alternating every gRPC session's requests across two
Services, the ARM reads 489–496 µs/request at the same 8,000/s against 481–487
with one backend — inside this rig's run-to-run spread — and the DPU attributes
the split at exactly 50.0/50.0. A closed loop at concurrency 32 completes 9%
less on the alternating arm at +10% µs/request; that is the route hop's latency
converted into throughput by a closed loop, not a data-path cost. The registry
that hands sessions their backend channels is a lifecycle cost only: an
instrumented build counted three lock acquisitions per session, none per
request, and zero contended takes across ~550 K requests and 3,081 session
builds (`data/api-l7-selcost-20260825-222604/`,
`data/backend-lock-20260825/`).

## What a session costs

Building a Linkerd session is the largest single cost above the transport, and
sharing one stack per workload is what removes most of it. The arm is two full
deploys of the same arrangement differing only in whether per-workload stack
sharing was compiled in (that build's `DPUMESH_SHARED_STACKS` switch; sharing
has since become unconditional), driven by a closed loop that reconnects after
every N completions:

| Reconnect period | OFF µs/req | ON µs/req | OFF reconnects/15 s | ON reconnects/15 s | OFF p99 µs | ON p99 µs |
|---|---:|---:|---:|---:|---:|---:|
| never | 43.3 | 42.6 | 0 | 0 | 1,476 | 1,460 |
| 4000 | 44.2 | 42.4 | 32 | 34 | 1,585 | 1,549 |
| 1000 | 47.3 | 42.4 | 104 | 138 | 1,739 | 1,538 |
| 250 | 58.7 | 44.2 | 256 | 527 | 33,795 | 1,578 |
| 60 | 101.8 | 50.9 | 436 | 1,929 | 40,175 | 1,666 |

From each arm's own churn slope against its own steady point, one session costs
**3.9 ms of ARM time unshared and 0.4–0.5 ms shared, a 87% reduction**. Under
heavy churn the closed loop completes 4.4× more sessions and 4.3× more requests,
and the 30–40 ms p99 spikes of per-session stack building disappear.

The steady point does not move, which is the expected result: the per-request
data path does not know the stacks are shared. Holding live sessions fixed at a
window of 128 and varying threads gives 2.98 / 4.95 / 7.06 / 13.69 µs per request
unshared against 2.86 / 4.87 / 6.98 / 13.52 shared — identical within noise, and
every row at two threads or more is concurrent same-service sessions served
byte-exact through **one** shared stack.

The proxy's own counters are the proof that the sharing is what changed:

| | unshared | shared |
|---|---:|---:|
| `sessions_opened` | 419 | 1,308 |
| `session_stack_builds` | 419 | **1** |
| `stack_cache_hits / misses` | 0 / 0 | **1,307 / 1** |
| `backend_session_mismatches` | **0** | **0** |

`mismatches=0` on the unshared arm is the plumbing invariant: the session every
connection carried equalled the one its stack was built for, across all 419
sessions, before sharing was ever enabled.

The synchronous half of one build is 148.4 µs — 8.7 µs to clone the outbound
template, 107.7 µs of layers, 32.0 µs of service construction. The rest of the
0.4–0.5 ms is lazy discovery and policy work, task execution and teardown, and
the surrounding DPUmesh lifecycle, none of which is instrumented yet.

## Why gRPC costs what it does

The proxy builds a session-local outbound stack for every protocol-aware session
and a workload-shared one for every opaque session. HTTP parsing turns a byte
stream into requests, and those requests carry H1/H2 pools and reconnect caches
that must not cross a DMA session boundary, so that stack cannot be shared.

The larger term is per-request. An opaque session does policy, discovery and
balancer dispatch once per connection and then moves bytes; a protocol-aware
session does route matching and balancer dispatch once per *request*, which is
why its per-request cost does not amortise as offered load rises the way a byte
proxy's does. That dispatch is what buys per-request backend selection, and it is
paid on every request.

## Deployment constraints

Everything above measures a path that works. What follows is what a deployment
has to accept in order to get it. None of it is a tuning knob.

**A Pod is meshed by one annotation, and the webhook writes the rest.**
`dpumesh.io/inject: "enabled"` on the Namespace or the Pod template, with the
Pod's Service in a `dpumesh-service` label, and an admission webhook adds the
transport-library and node-agent mounts, `DPUMESH_RINGS_PER_POD` and
`DPUMESH_SERVICE`, the preload shim, node affinity, and the two Linkerd
markers. The Pod stays unprivileged and mounts no device — the DOCA objects
live in the per-Pod broker (design/CONTROL.md §2-1.9). It refuses the Pod
when no node carries `dpumesh.io/dpu=true`, because a Pod holding part of the
patch fails later and elsewhere.

The second Linkerd marker — `config.linkerd.io/skip-inbound-ports` on the data
port — is load-bearing, not cosmetic. Removing it from the three live backends,
an annotation edit on running Pods with no restart, made the destination
controller advertise those endpoints as meshed, and the next point returned
`rcnt=0 fail=1` with `Linkerd session ended before both output FINs` in the DPU
log. Restoring it restored the path on the following point. A backend has to look
unmeshed to Linkerd's destination controller, because the DPU *is* the proxy and
there is no second one waiting behind the endpoint address.

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
node-to-node confidentiality belongs to the authenticated peer channel, which is
why identity is established at registration rather than per connection. It is
still a trade: an audit requirement that reads "the proxy encrypts every hop" is
not met by this deployment, and the same annotation that keeps the data path
working is what makes discovery withhold the endpoint identity.

**Pod density is the K dial.** A node serves the smaller of `MAX_PODS` — 127,
the `int8` wire ceiling — and `MAX_DPA_RINGS × N / K`. This BlueField reports
32 execution units, so eight rings per Pod seat 64 Pods and two seat 127; what
a lower K trades away is per-Pod parallelism, including the ARM data workers it
caps at A ≤ K. A `K = 2` deployment has been run at 48 Pods of one Service
(`data/scale-20260825-172249/stages.csv`, 7 of 7): all 48 Ready in 26 s — under
the fail-closed shim Ready is itself the registration proof — zero table
refusals, 1.41 M requests without a failure, and a drain in steps of eight with
no DPU error line. 127 itself has not been run, and near the ceiling ARM DRAM
binds first: each live Pod holds 64 MB of DPU staging.

**Cross-node destinations are refused in these measurements.** Every campaign
in this report intentionally used one node with the peer carrier unset. The
TLS/TCP/RDMA peer implementation exists, but no second DPU was available to
produce a cross-node receipt, so a Service whose only replicas are elsewhere
had no route in the measured deployments.

## Method and limits

- Deploy: `BENCH_DEPLOY_SCOPE=all bench/bench.sh deploy`, N/K/A/L = 32/8/8/8,
  all eight ARM workers hosting a Linkerd runtime, fail-closed L7 policy,
  node-agent-relayed control plane.
- Cost and latency campaign: `bench/suite/api_l7_compare.sh` (phases A/B/C, 99
  points, 3 repetitions) and `bench/suite/api_l7_cost.sh` (closed and open-loop
  CPU, 3 repetitions each) at `1518aae`. Raw data in `data/api-l7-20260821/`.
- Session cost: `bench/suite/l7_session_cost.sh --reps 2 --dur 15` over two
  deploys differing only in that build's stack-sharing switch
  (`DPUMESH_SHARED_STACKS`, since removed — sharing is unconditional). Raw data
  in `data/l7-shared-off-20260821/` and `data/l7-shared-on-20260821/`.
- Selection cost: `bench/suite/api_l7_cost.sh` closed and open-8K, 3
  repetitions, single-backend and 50/50-weighted arms at `9d450b5`. Raw data in
  `data/api-l7-selcost-20260825-222604/`; the registry-lock reading in
  `data/backend-lock-20260825/`.
- Function campaigns: `bench/suite/policy_route.sh` (policy, route, cross,
  fanout, surfaces, lb) with the resources it attaches in `bench/k8s/policy/`,
  and `bench/suite/inject.sh`. Policy through surfaces decided 45 of 45 in
  `data/policy-route-20260824-095824/stages.csv`; the corrected balancing scope
  decided 21 of 21 in `data/policy-route-lb-20260824-judge-v3/stages.csv`; and
  injection decided 9 of 9 in `data/inject-20260825-170137/stages.csv`. The LB
  rows in the earlier combined artifact carried no verdict and are not evidence.
- Every one of the 99 performance points carried `fail=0 drops=0 overflow=0
  worker_fail=0 reorder=0`, and every session-cost point carried `fail=0 drops=0
  reorder=0 worker_fail=0`. No point was discarded.
- There is no host-sidecar control. The deployment builds one arrangement — the
  DPU-hosted mesh — so these numbers rank the three APIs against each other, not
  the mesh against a sidecar.
- The three Services do not hold equal backend counts (3 / 1 / 1). Native's
  throughput includes the server-side parallelism its balancer provides.
- The function arms show that enforcement happens and what decides it. They do
  not measure what enforcement costs; no arm was run with and without a policy
  at a matched rate, and the request counts are the volume a ten-second stage
  happened to carry, not a capacity.
- Run-to-run spread on this rig is several percent; differences smaller than
  that are not differences.
- Latency figures are the client's own histogram; the DPU is not instrumented
  per request.
- One reported column needs care rather than repair: `route_hits` can go negative
  across a stage that tears a route down, because the proxy drops a route's
  metric label set with the route. It gates nothing.
