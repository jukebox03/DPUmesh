#!/usr/bin/env bash
# Six-path gRPC L7 campaign: the four paths of the existing gRPC evaluation plus
# two linkerd columns, measured in one session so the linkerd numbers and the
# numbers they are compared against come from the same machine state.
#
#   grpc-envoy-permissive   Envoy tcp_proxy sidecars, plaintext
#   grpc-envoy-strict       Envoy tcp_proxy sidecars, mTLS on the inter-pod leg
#   grpc-linkerd            linkerd2-proxy sidecars, HTTP/2 detected and proxied
#   grpc-linkerd-opaque     linkerd2-proxy sidecars, benchmark port opaque
#   grpc-tcp                no mesh
#   grpc-dpumesh            gRPC over the DPUmesh EventEngine adapter
#
# The measurement is the one REPORT_GRPC.md defines, unchanged: a constant-rate
# open loop for what a given load costs, and a fixed in-flight window for what a
# path sustains, the second run once at one core per endpoint and once at six.
# This script only adds paths to it; it does not introduce a method.
#
# Stages run in order and each is separately invocable, because a campaign this
# long is worth resuming rather than restarting.
#
#   setsid nohup linkerd/bench/suite/linkerd_campaign.sh all \
#       >/tmp/linkerd-campaign.log 2>&1 </dev/null &
set -euo pipefail

PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
[ -f "$PROJ_ROOT/.env" ] && { set -a; source "$PROJ_ROOT/.env"; set +a; }
: "${HOST_PASS:?.env missing HOST_PASS}" "${DPU_HOST:?.env missing DPU_HOST}"

BENCH="$PROJ_ROOT/bench/bench.sh"
COLLECTOR="$PROJ_ROOT/bench/suite/l4_proxy_data.sh"
CLOSED="$PROJ_ROOT/bench/suite/grpc_closed_sweep.sh"
DISTILL="$PROJ_ROOT/bench/suite/distill.py"
PLOT_GRPC="$PROJ_ROOT/integrations/grpc/bench/suite/plot_grpc.py"
PLOT_SLO="$PROJ_ROOT/bench/suite/plot_slo.py"
LINKERD="${LINKERD_BIN:-$HOME/.linkerd2/bin/linkerd}"

NS="${NS:-test-bench}"
CTRL_PORT="${CTRL_PORT:-9092}"
DATA_ROOT="$PROJ_ROOT/linkerd/bench/report/data"
# A campaign is one unit of work, not one calendar day. It runs for hours and
# its stages resume each other, so the output directory is resolved once and
# remembered: deriving it from the clock on every stage splits a single campaign
# across midnight and throws away the capacity boundaries already discovered.
# Pass OUT to start a new one, or point it at an existing directory to continue.
CAMPAIGN_POINTER="$DATA_ROOT/.campaign"
if [ -n "${OUT:-}" ]; then
  :
elif [ -s "$CAMPAIGN_POINTER" ] && [ -d "$(<"$CAMPAIGN_POINTER")" ]; then
  OUT="$(<"$CAMPAIGN_POINTER")"
else
  STAMP="${STAMP:-$(date +%Y%m%d)}"
  OUT="$DATA_ROOT/linkerd-$STAMP"
fi
mkdir -p "$DATA_ROOT"
printf '%s\n' "$OUT" >"$CAMPAIGN_POINTER"
FIGS="${FIGS:-$PROJ_ROOT/linkerd/bench/report/figures}"
CONFIGS="${CONFIGS:-grpc-envoy-permissive grpc-envoy-strict grpc-linkerd grpc-linkerd-opaque grpc-tcp grpc-dpumesh}"
LINKERD_CONFIGS="${LINKERD_CONFIGS:-grpc-linkerd grpc-linkerd-opaque}"
# The DPU topology the gRPC evaluation is defined at. The collector re-checks it
# against the live DPU log and refuses the run if it differs.
DPU_DPA_THREADS="${DPUMESH_DPA_THREADS:-32}"
DPU_RINGS_PER_POD="${DPUMESH_RINGS_PER_POD:-8}"
DPU_ARM_WORKERS="${DPUMESH_ARM_WORKERS:-8}"
# Repetition count of the published gRPC campaign. Raise for a tighter median.
REPS="${REPS:-1}"

mkdir -p "$OUT"
LOG="$OUT/campaign.log"
log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$LOG" >&2; }
die() { log "FATAL: $*"; exit 1; }

# ---------------------------------------------------------------- stage: deploy
stage_deploy() {
  log "=== deploy: gRPC scope with the linkerd pods admitted ==="
  [ -x "$LINKERD" ] || die "linkerd CLI not found at $LINKERD (set LINKERD_BIN)"
  "$LINKERD" check >"$OUT/linkerd-check-preinstall.txt" 2>&1 ||
    die "linkerd control plane is not healthy; see $OUT/linkerd-check-preinstall.txt"

  env DPUMESH_DPA_THREADS="$DPU_DPA_THREADS" \
      DPUMESH_RINGS_PER_POD="$DPU_RINGS_PER_POD" \
      DPUMESH_ARM_WORKERS="$DPU_ARM_WORKERS" \
      DPUMESH_PROXY_L7_SVC= DPUMESH_LOG_LEVEL=40 \
      BENCH_NUMA_POLICY=local BENCH_DEPLOY_SCOPE=grpc BENCH_LINKERD=1 \
      "$BENCH" deploy 2>&1 | tee -a "$LOG"

  BENCH_NUMA_POLICY=local "$BENCH" pin grpc 2>&1 | tee -a "$LOG"
  log "deploy complete"
}

# ---------------------------------------------------------------- stage: verify
# A linkerd column is only worth reporting if the proxy is actually in the path
# and doing what the column claims: L7 for the detected path, byte forwarding for
# the opaque one. Both are read off the proxy's own counters after a short run,
# not assumed from the annotation.
pod_ip() {
  kubectl get pod -n "$NS" -l "app=$1" \
    -o go-template='{{range .items}}{{if not .metadata.deletionTimestamp}}{{if eq .status.phase "Running"}}{{.status.podIP}}{{"\n"}}{{end}}{{end}}{{end}}' \
    2>/dev/null | sed -n '1p'
}
pod_of() {
  kubectl get pod -n "$NS" -l "app=$1" \
    -o go-template='{{range .items}}{{if not .metadata.deletionTimestamp}}{{if eq .status.phase "Running"}}{{.metadata.name}}{{"\n"}}{{end}}{{end}}{{end}}' \
    2>/dev/null | sed -n '1p'
}
client_app_of() {
  case "$1" in
    grpc-envoy-permissive) echo bench-grpc-envoy ;;
    grpc-envoy-strict)     echo bench-grpc-envoy-strict ;;
    grpc-tcp)              echo bench-grpc-tcp ;;
    grpc-dpumesh)          echo bench-grpc-dpumesh ;;
    grpc-linkerd)          echo bench-grpc-linkerd ;;
    grpc-linkerd-opaque)   echo bench-grpc-linkerd-opaque ;;
  esac
}

stage_verify() {
  log "=== verify: injection, mTLS identity, and the L7/opaque split ==="
  local app pod n_inject=0
  for app in bench-grpc-linkerd echo-grpc-linkerd \
             bench-grpc-linkerd-opaque echo-grpc-linkerd-opaque; do
    pod=$(pod_of "$app"); [ -n "$pod" ] || die "$app has no Running pod"
    kubectl get pod -n "$NS" "$pod" -o json |
      jq -e '[.status.containerStatuses[]?, .status.initContainerStatuses[]?][]
             | select(.name=="linkerd-proxy") | .ready' >/dev/null ||
      die "$app pod $pod has no ready linkerd-proxy container"
    n_inject=$((n_inject + 1))
  done
  log "linkerd-proxy present and ready in $n_inject/4 benchmark pods"

  "$LINKERD" check --proxy -n "$NS" >"$OUT/linkerd-check-proxy.txt" 2>&1 ||
    log "WARN: linkerd check --proxy reported findings; see $OUT/linkerd-check-proxy.txt"

  # One short run per linkerd column, then read the client proxy's counters.
  local config capp ip
  : >"$OUT/linkerd-datapath.txt"
  for config in $LINKERD_CONFIGS; do
    capp=$(client_app_of "$config")
    ip=$(pod_ip "$capp"); [ -n "$ip" ] || die "$capp has no pod IP"
    log "$config: 5 s warm run to populate proxy counters"
    printf 'RUN 48 48 8 5 200 8\n' | timeout 120s nc -N "$ip" "$CTRL_PORT" |
      tee -a "$OUT/linkerd-datapath.txt" >/dev/null || die "$config warm run failed"

    local metrics http detect opaq
    metrics=$("$LINKERD" diagnostics proxy-metrics -n "$NS" "po/$(pod_of "$capp")" 2>/dev/null || true)
    printf '%s\n' "$metrics" >"$OUT/proxy-metrics-$config.txt"
    # The proxy labels each outbound connection with the protocol it settled on
    # and emits outbound_http_* only for the streams it parsed as HTTP.
    http=$(awk '/^outbound_http_route_backend_requests_total|^outbound_http_balancer_/ {n++} END{print n+0}' <<<"$metrics")
    detect=$(awk '/^outbound_tcp_protocol_connections_total.*protocol="detect"/ {n++} END{print n+0}' <<<"$metrics")
    opaq=$(awk '/^outbound_tcp_protocol_connections_total.*protocol="opaq"/ {n++} END{print n+0}' <<<"$metrics")
    log "$config: outbound HTTP series=$http, protocol=detect series=$detect, protocol=opaq series=$opaq"
    case "$config" in
      grpc-linkerd)
        [ "$http" -gt 0 ] && [ "$detect" -gt 0 ] ||
          die "grpc-linkerd carried no HTTP series: the proxy did not detect HTTP/2" ;;
      grpc-linkerd-opaque)
        [ "$http" -eq 0 ] && [ "$opaq" -gt 0 ] ||
          die "grpc-linkerd-opaque carried HTTP series: the port is not opaque" ;;
    esac

    # mTLS is visible where it is enforced: the receiving proxy records the
    # mesh identity it authenticated the client as.
    local sapp speer
    sapp="echo${capp#bench}"
    speer=$("$LINKERD" diagnostics proxy-metrics -n "$NS" "po/$(pod_of "$sapp")" 2>/dev/null |
      awk -F'client_id="' '/^inbound_tcp_transport_header_connections_total/ && NF>1 \
        {split($2,a,"\""); print a[1]}' | sed -n '1p')
    [ -n "$speer" ] ||
      die "$config: $sapp recorded no authenticated client identity; the leg is not mTLS"
    log "$config: inter-pod leg authenticated as $speer"
  done
  log "data-path verification complete"
}

# ---------------------------------------------------------------- stage: open
stage_open() {
  log "=== open loop: constant rate, 8 channels, ${REPS} rep/point, 6 paths ==="
  # BENCH_LINKERD reaches the collector's own recovery redeploy, which would
  # otherwise rebuild the stack without the pods the campaign is measuring.
  env CONFIGS="$CONFIGS" REPS="$REPS" BENCH_LINKERD=1 \
    "$COLLECTOR" --no-deploy --no-perf --out "$OUT/open" 2>&1 | tee -a "$LOG"
  log "open loop complete: $OUT/open"
}

# ---------------------------------------------------------------- stage: closed1
stage_closed1() {
  log "=== fixed window, one core per endpoint ==="
  BENCH_NUMA_POLICY=local "$BENCH" pin grpc 2>&1 | tee -a "$LOG"
  env CONFIGS="$CONFIGS" REPS="$REPS" \
    "$CLOSED" --out "$OUT/closed-1core" 2>&1 | tee -a "$LOG"
  cp "$OUT/closed-1core/points.csv" "$OUT/closed_1core.csv"
  log "fixed window at 1+1 complete: $OUT/closed_1core.csv"
}

# ---------------------------------------------------------------- stage: closed6
# Six cores per endpoint, one path at a time: the measured path owns twelve
# cores and every other gRPC pod is confined to the six outside them, so each
# path meets the same allocation and no idle pod shares a measured core.
stage_closed6() {
  log "=== fixed window, six cores per endpoint, one path at a time ==="
  local config first=1
  : >"$OUT/closed_6core.csv"
  for config in $CONFIGS; do
    log "6+6: repinning for $config"
    env BENCH_NUMA_POLICY=local BENCH_CAP_CONFIG="$config" \
      "$BENCH" pin grpcl7cap 2>&1 | tee -a "$LOG"
    env CONFIGS="$config" REPS="$REPS" \
      "$CLOSED" --out "$OUT/closed-6core/$config" --configs "$config" 2>&1 | tee -a "$LOG"
    local src="$OUT/closed-6core/$config/points.csv"
    [ -s "$src" ] || { log "WARN: $config produced no 6-core points"; continue; }
    if [ "$first" = 1 ]; then cat "$src" >>"$OUT/closed_6core.csv"; first=0
    else tail -n +2 "$src" >>"$OUT/closed_6core.csv"; fi
  done
  log "fixed window at 6+6 complete: $OUT/closed_6core.csv"
}

# ---------------------------------------------------------------- stage: figures
stage_figures() {
  log "=== distilling and rendering ==="
  mkdir -p "$FIGS"
  python3 "$DISTILL" "$OUT/open" "$OUT/measurements.csv" 2>&1 | tee -a "$LOG"
  python3 "$PLOT_GRPC" "$OUT/measurements.csv" "$FIGS" 2>&1 | tee "$OUT/summary_open.txt"
  python3 "$PLOT_SLO" "$FIGS" slo_linkerd_open "$OUT/open" 2>&1 | tee "$OUT/summary_slo_open.txt"
  [ -s "$OUT/closed_1core.csv" ] &&
    python3 "$PLOT_SLO" "$FIGS" slo_linkerd_closed "$OUT/closed_1core.csv" 2>&1 |
      tee "$OUT/summary_slo_closed.txt"
  log "figures in $FIGS, per-point data in $OUT"
}

# ---------------------------------------------------------------- dispatch
case "${1:-all}" in
  deploy)   stage_deploy ;;
  verify)   stage_verify ;;
  open)     stage_open ;;
  closed1)  stage_closed1 ;;
  closed6)  stage_closed6 ;;
  figures)  stage_figures ;;
  all)      stage_deploy; stage_verify; stage_open; stage_closed1; stage_closed6
            stage_figures ;;
  *) cat <<EOF
usage: $0 [deploy|verify|open|closed1|closed6|figures|all]

  deploy   gRPC-scope deployment with the linkerd pods, then the 1-core pinning
  verify   proves the proxy is in the path and that L7/opaque differ as claimed
  open     constant-rate open loop over all six paths
  closed1  fixed in-flight window, one core per endpoint
  closed6  fixed in-flight window, six cores per endpoint, one path at a time
  figures  distill, host-CPU and capacity figures, latency-budget curves

env: OUT FIGS CONFIGS REPS STAMP LINKERD_BIN
EOF
     exit 2 ;;
esac
