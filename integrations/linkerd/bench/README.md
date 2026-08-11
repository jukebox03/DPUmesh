# linkerd Benchmark

The linkerd columns of the gRPC L7 evaluation. They use the gRPC workloads and
the collectors of [`integrations/grpc/bench`](../../grpc/bench/) unchanged; what
lives here is the pod topology that puts a linkerd2-proxy sidecar in the path
and the campaign driver that runs all six L7 paths as one session.

This is a measurement of stock linkerd against stock Envoy and against DPUmesh.
It is not the linkerd-on-DPUmesh integration; that is [`../PLAN.md`](../PLAN.md),
and its M4 milestone is what this dataset becomes the baseline for.

## 1. Layout

```text
k8s/pods.yaml                  four injected pods: two linkerd columns
suite/linkerd_campaign.sh      six-path campaign driver, stage by stage
report/                        measured evaluation and per-point data
```

## 2. The two columns

linkerd has no plaintext mode between meshed pods, so both columns are mutually
authenticated and encrypted. What separates them is what the proxy does with the
bytes:

| Column | Port | Proxy behaviour |
|---|---|---|
| `grpc-linkerd` | detected | parses HTTP/2, load balances per request, re-encodes |
| `grpc-linkerd-opaque` | `config.linkerd.io/opaque-ports` | forwards bytes, balances per connection |

The opaque column is the one that lines up with the Envoy `tcp_proxy` sidecars,
which also forward bytes; the detected column is what a linkerd deployment
actually does with gRPC. Reporting both separates the cost of the mesh's
transport from the cost of its L7 work.

The client's control port is excluded from inbound redirection
(`config.linkerd.io/skip-inbound-ports`). It carries the collector's `RUN`
command, not benchmark traffic, so proxying it would add protocol detection and
sidecar CPU to a channel that is not under test.

## 3. Running it

```sh
integrations/linkerd/bench/suite/linkerd_campaign.sh all
```

Stages are separately invocable, because a campaign of this length is worth
resuming rather than restarting:

| Stage | What it does |
|---|---|
| `deploy` | gRPC-scope deployment with `BENCH_LINKERD=1`, then the one-core pinning |
| `verify` | proves the proxy is in the path and that the two columns differ as claimed |
| `open` | constant-rate open loop over all six paths |
| `closed1` | fixed in-flight window, one core per endpoint |
| `closed6` | fixed in-flight window, six cores per endpoint, one path at a time |
| `figures` | distills the run and renders the host-CPU, capacity and latency-budget figures |

Environment: `OUT` `FIGS` `CONFIGS` `REPS` `STAMP` `LINKERD_BIN`.

The prerequisite is a healthy linkerd control plane in the cluster; the driver
refuses to deploy without one. `BENCH_LINKERD` defaults to 0, so a deployment
that does not ask for linkerd is byte-identical to what it was before.

## 4. What `verify` establishes

A column is only worth reporting if the proxy is in the path and doing what the
column's name claims. Both are read off the proxy's own counters after a short
run rather than assumed from the annotation:

- `outbound_tcp_protocol_connections_total{protocol="detect"}` and the
  `outbound_http_*` families for the L7 column;
- `protocol="opaq"` and no `outbound_http_*` for the opaque column;
- `inbound_tcp_transport_header_connections_total{client_id=...}` on the
  receiving proxy, which is the identity it authenticated the caller as — the
  evidence that the inter-pod leg is mTLS.

The stage fails the campaign if any of these disagree with the configuration.

## 5. Placement

An injected proxy is not built from this repository, so it does not start under
the `numactl` wrapper the benchmark images use. `bench.sh pin` moves its pages
onto the benchmark node after pinning it; without that the proxy would read its
own memory across the interconnect and the linkerd columns would carry a penalty
no other path pays.

The proxy shares its application's core, exactly as the Envoy sidecar does: the
one-core budget is per pod, so a meshed path pays for its proxy out of the same
core its application runs on.

## 6. Measurement rules

Those of [the gRPC benchmark](../../grpc/bench/README.md#7-measurement-rules),
unchanged. The evaluation is in [report/REPORT_LINKERD.md](report/REPORT_LINKERD.md).
