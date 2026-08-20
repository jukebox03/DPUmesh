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
| `bench-run` | manual | one ad-hoc operating point, uploaded as an artifact |
| `bench-publish` | manual | the frozen set in [bench-frozen.txt](bench-frozen.txt), appended to the `gh-pages` history |

`bench-publish` writes to the `gh-pages` branch and nowhere else. To see the page,
set Settings → Pages → Source to the `gh-pages` branch after the first run. It also
runs itself nightly at 04:00 Seoul time, and a nightly run skips itself when the
1-minute load average is above `BENCH_LOAD_LIMIT` (3.0) — someone else is working.
A run started by hand never skips: then the load is the operator's own choice.

## What a measurement has to say about itself

`ci/bench-frozen.txt` fixes what the client asks for. It cannot fix what the DPU
is, and the DPU is what moves the number: one ARM worker or four, the L7 backend
loaded or not, which core each process sits on, what else is deployed in the
namespace. `ci/bench-config.sh` reads all of that before the first point and
stamps every row with a `config_id`.

Nothing is published without it. The page draws no line between two points whose
`config_id` differs — it breaks the line and marks the run — because a slope
across a redeploy is not a trend, it is two machines side by side. Points from
before the fingerprint existed carry none and are not plotted at all.

To read a `config_id` back to a machine, use the Configurations table at the
bottom of the page.
