# CI

Checks live here as scripts and Makefile targets; the workflows in
`.github/workflows/` only decide when to run them. Anything CI fails on can be
reproduced at a terminal with the same command.

## Workflows

Every push, on a hosted x86 runner. Under a minute.

| | what it protects |
|---|---|
| `contracts-hostfree` | the contracts that need no DOCA and no BlueField (`make test-hostfree`) |
| `headers-standalone` | the four public headers compile alone as C99 and C++17, and pull in no DOCA |
| `abi-guard` | a changed public header carries either an `ABI_MAJOR` bump or an explicit `ABI-Impact:` line |
| `docs-links` | every relative link in active Markdown outside immutable measurement reports resolves |

On a path change only.

| | what it protects | where |
|---|---|---|
| `contracts-arm64` | the same contracts plus the crate's unit tests | aarch64 hosted |
| `rust-build` | `dmesh-l7` builds, tests, and passes clippy | hosted |
| `rust-fmt` | `linkerd/rust/src/lib.rs` formatting | hosted |
| `contracts-rapids4` | **every contract** (`make test`) | rapids4, needs DOCA |

On a schedule.

| | when | what it asks |
|---|---|---|
| `rapids4-health` | daily, 04:00 Seoul | does the deployed campaign still answer, and what is deployed |
| `submodule-watch` | daily, 06:00 Seoul | is the linkerd2-proxy pin still reachable upstream |
| `docs-links` | weekly | links again, against a moving filesystem |

Manual only: `rapids4-env`, which reports the self-hosted runner's environment
and fails if it is root, if DOCA does not resolve, or if a tool the other jobs
call is missing.

## Scripts

| | |
|---|---|
| `check-abi-bump.sh` | the ABI decision, called by `abi-guard` |
| `check-doc-links.sh` | active-document relative links, called by `docs-links` |
| `check-submodule-pin.sh` | submodule pin reachability, called by `submodule-watch` |
| `dpu-state.sh` | what the DPU and the deployed campaign currently are |
| `health-check.sh` | does the campaign answer; prints one JSON record |
| `health-page.py` | the accumulated records as one self-contained page |

## The two Makefile targets

```
make test-hostfree     the contracts that need no DOCA and no BlueField
make test              every contract
```

The difference between them is the definition of "needs the DOCA SDK". Hosted CI
runs the first; only rapids4 can run the second. `test-hostfree` refuses to run
under `-DNDEBUG`, because these tests are built on `assert()` and would all pass
silently without it.

## What runs unattended

`rapids4-health`, and it does not measure performance. What is deployed on
rapids4 is a variable of the research — the DPU's topology and the campaign
change with every experiment — so a number sampled on a schedule against
whatever happened to be up is not a series. Whether the node still answers is.

Each run appends one JSON record to `data/health.jsonl` on the `gh-pages` branch
and regenerates the page there. The record holds the result, the load average,
the deployed clients, and the DPU's topology; the page marks every run whose
topology differs from the last run that stated one, which makes it a log of the
states this node has been in. A red run means a campaign is wedged, not that
something got slower.

The request it sends is real load, and the clients' control servers are serial
accept loops, so it refuses to probe a machine that is working: over
`HEALTH_MAX_LOAD` (default 3.0), or when the client does not answer a `PING`, the
run records `busy` and stops there. `busy` is not a fault — but a client that is
wedged rather than occupied is silent in exactly the same way, and what separates
them is that a wedged one is still silent the next night. A run of consecutive
`busy` records is the signal to go and look.

Performance campaigns are run by hand with `bench/bench.sh` by someone who
chose the configuration. `ci/dpu-state.sh` reports the live DPU geometry and
deployed Pod-set identity recorded with that run.

## The rapids4 runner

Hosted runners have no DOCA SDK, no BlueField, and no cluster. Everything that
needs one of those runs on rapids4 through a self-hosted runner, and every such
job declares:

```yaml
runs-on: [self-hosted, linux, x64, rapids4]
```

`self-hosted`, `linux` and `x64` come from the runner itself. **`rapids4` does
not** — it is a label given at registration, and a job that omits it from the
list queues forever instead of failing.

Constraints on anything added to those jobs:

- **No `pull_request` trigger.** This repository is public, and a pull request
  from a fork carries its own workflow code. The rapids4 jobs trigger on
  `workflow_dispatch`, on `push` to `main`, or on a schedule — never on
  `pull_request`.
- **One job at a time.** Every rapids4 workflow shares
  `concurrency: group: rapids4` with `cancel-in-progress: false`. A build beside
  a measurement invalidates the measurement.
- **The workspace is not disposable.** `_work/` survives between jobs; what
  keeps a stale object from turning a broken build green is `actions/checkout`,
  which cleans the tree every run. If a job passes or fails inexplicably, clear
  `~/actions-runner/_work/DPUmesh` before believing it.
- **No deploy from CI.** `bench/native_deploy.sh all` rebuilds the DPU, host
  runtime, controller and workloads; it stays a deliberate terminal operation.
- **The DPU build stays out.** `doca/meson.build` is compiled on the BlueField
  over ssh, which would put `DPU_PASS` into the runner environment.
- **The runner service does not read `~/.bashrc`.** A job sees only what the
  service environment holds; the place to add to it is `~/actions-runner/.env`.

## Hardware deployment profile

The self-hosted node uses the repository's one-node deployment profile:

```sh
./bench/native_deploy.sh all
./bench/native_deploy.sh status
ci/dpu-state.sh
```

The profile is `N/K/A/L=32/8/8/8`, with two allocatable native channels and
empty L7 Service lists. `ci/dpu-state.sh` reads the DPU's startup banner and
records effective geometry, L7 mode, load-balancer mode, deployed Pod count and
client CPU affinity. `ci/health-check.sh` uses `bench.sh ping` and one bounded
native `point`; it does not deploy, restart or modify the system.
