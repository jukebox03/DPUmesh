# The rapids4 self-hosted runner

Hosted GitHub runners have no DOCA SDK, no BlueField, and no cluster. Everything
that needs one of those runs on rapids4 through a self-hosted runner. This file
is the registration contract, so a reboot or a reinstall does not have to be
reconstructed from memory.

## Label contract

Every job that must land on rapids4 declares:

```yaml
runs-on: [self-hosted, linux, x64, rapids4]
```

`self-hosted`, `linux` and `x64` are applied automatically. **`rapids4` is not** —
it has to be given at registration. Without it those jobs queue forever, and if a
second self-hosted runner is ever added they land on the wrong machine.

## Registration

Get a token from the repository: Settings → Actions → Runners → New self-hosted
runner. It is valid for one hour and for one registration.

```sh
mkdir -p ~/actions-runner && cd ~/actions-runner
curl -o runner.tar.gz -L \
  https://github.com/actions/runner/releases/download/v2.336.0/actions-runner-linux-x64-2.336.0.tar.gz
tar xzf runner.tar.gz

./config.sh --url https://github.com/jukebox03/DPUmesh \
            --token <REGISTRATION_TOKEN> \
            --name rapids4 --labels rapids4 --unattended

sudo ./svc.sh install "$(whoami)"    # run as the invoking user, never as root
sudo ./svc.sh start
```

Registering as a service is what makes the runner come back after a reboot. Run
it as the ordinary account: the jobs build in a workspace, talk to the cluster
with that account's kubeconfig, and never need root.

Confirm it took, before trusting any later green light:

```sh
sudo ./svc.sh status
```

then run the **rapids4-env** workflow from the Actions tab. It fails on purpose
if the runner is root, if DOCA does not resolve, or if a tool the build and
bench jobs call is missing.

## What the environment must provide

The runner service does not read `~/.bashrc`; only what the service environment
already holds is visible to a job. On rapids4 everything needed resolves without
help — `doca-common`, `doca-comch` and `doca-dpa` are in the default pkg-config
path, and `gcc`, `make`, `python3`, `kubectl` and `nc` are in `/usr/bin`.

If that ever stops being true, the fix is `~/actions-runner/.env`, which the
runner reads at start:

```sh
echo 'PKG_CONFIG_PATH=/opt/mellanox/doca/lib/x86_64-linux-gnu/pkgconfig' >> ~/actions-runner/.env
sudo ./svc.sh stop && sudo ./svc.sh start
```

Note that `/usr/bin/cargo` is the distribution's, which is older than the
toolchain `linkerd/rust` pins. Nothing on this runner builds Rust today; the DPU
build that does uses `$HOME/.cargo/bin/cargo` over ssh.

## Rules these jobs follow

- **No `pull_request` trigger.** This repository is public, and a pull request
  from a fork carries its own workflow code. Nothing on this machine may be
  driven by a stranger's branch. The rapids4 jobs trigger on `workflow_dispatch`
  and on `push` to `main` only. Keep "Require approval for all outside
  collaborators" set under Settings → Actions → General.
- **One job at a time.** Every rapids4 workflow shares
  `concurrency: group: rapids4` with `cancel-in-progress: false`. A build beside
  a measurement invalidates the measurement, and a cancelled build can leave a
  half-written tree.
- **The workspace is not disposable.** `_work/` survives between jobs. What keeps
  a stale object from turning a broken build green is `actions/checkout`, which
  cleans the tree on every run. If a job passes or fails inexplicably, clear
  `~/actions-runner/_work/DPUmesh` before believing it.
- **No deploy from CI.** The bench jobs measure what is already running.
  `bench/bench.sh deploy` rebuilds the DPU and restarts every pod; it stays a
  deliberate act at a terminal.
- **The DPU build stays out.** `doca/meson.build` is compiled on the BlueField
  over ssh, which would put `DPU_PASS` into the runner environment.

## The jobs

| workflow | trigger | what only this machine can do |
|---|---|---|
| `rapids4-env` | manual | reports the runner environment and gates on DOCA |
| `contracts-rapids4` | push to `main`, manual | `make` + all 27 contracts, vs 13 on a hosted runner |
| `rapids4-health` | nightly, manual | proves the deployed campaign still answers |

## What runs unattended, and what does not

Nothing on this machine measures performance on a schedule. What is deployed on
rapids4 is a variable of the research, not a constant: the DPU's topology and
the campaign change with every experiment, and a number sampled nightly against
whatever happened to be up is not a series, it is a scatter of unrelated points.
Performance campaigns are run by hand, from `bench/suite/`, by someone who knows
which configuration they meant to measure.

The one scheduled job is **rapids4-health**, at 04:00 Seoul time. It asks a
different question:

1. Is a campaign deployed at all? An empty namespace between experiments is a
   normal state, so it is reported and the run passes.
2. If one is deployed, what is it? `ci/dpu-state.sh` reads the DPU's own startup
   line for N/K/A, the L7 layer and the load balancer, and fails if that line
   cannot be found at all — a DPU that is down or has lost its log.
3. Does the path still answer? One request, one connection, three seconds,
   against whichever client is deployed. The reply is printed and thrown away.
   It is there to prove the path is alive, not to say how fast it is.

Each run appends one JSON object to `data/health.jsonl` on the `gh-pages`
branch and regenerates the page there, so the history survives the 90-day limit
on workflow logs. A red run in the morning means a campaign is wedged, not that
something got slower.

The page has two halves. The strip is whether the node answered. The table is
**what was deployed** — N/K/A, the L7 layer, how many clients — with a gold rule
on any run where the DPU's topology changed from the run before it. On a machine
whose configuration is a variable of the research, that table is the more useful
half: it is a log of the states this node has been in, and it is the thing you
want when a hand-run campaign produces a number you cannot explain.

No latency or throughput is recorded, on purpose. The smoke request proves the
path carries bytes; its timing is one sample against whatever happened to be
deployed, and keeping it would rebuild the performance series this replaced.

Both halves run locally, which is the point of keeping them out of the YAML:

```sh
ci/health-check.sh > record.json     # exit 1 when the campaign is not usable
ci/health-page.py data/health.jsonl index.html
```

Set Settings → Pages → Source to the `gh-pages` branch after the first run.

## Setting the machine up

CI never deploys. `bench/bench.sh deploy` rebuilds the DPU and restarts every
Pod, which is not something a scheduled job may decide to do.

The topology the report's numbers were taken under:

```sh
DPUMESH_DPA_THREADS=32 DPUMESH_RINGS_PER_POD=8 DPUMESH_ARM_WORKERS=8 \
BENCH_NUMA_POLICY=local BENCH_DEPLOY_SCOPE=grpc bash bench/bench.sh deploy
BENCH_NUMA_POLICY=local bash bench/bench.sh pin grpc
```

Passing none of those leaves the DPU with **one** ARM data worker, which is a
different machine and a different set of numbers. Confirm what took by reading
the DPU's own line, which is where `ci/dpu-state.sh` reads it from too:

```sh
ci/dpu-state.sh
bench/bench.sh dpulog 200 | grep 'DPU PROXY MODE ON'
```

`BENCH_DEPLOY_SCOPE` picks the campaign and the two do not overlap: `grpc`
starts only the L7 paths so no other backend can enter the DPU registry while it
runs, and `l4`/`all` start the byte-stream pods instead. Add `BENCH_LINKERD=1`
for the linkerd sidecar columns, and `L7_BACKEND=linkerd` with
`DPUMESH_L7_SVC=<ns>/<service>` to put linkerd2-proxy inside the DPU rather than
beside the Pod.
