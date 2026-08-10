#!/bin/bash
# Record where one configuration's endpoint cores go for one load point.
#
# Samples both endpoint cores with call stacks in the middle of a constant-rate
# open-loop run. Over the same window it records the physical busy time of each
# core, the cgroup CPU, per-thread CPU and context switches of every container,
# and the DPU ARM cores the run consumes.
#
# Sampling is core-wide, one sample per fixed number of unhalted cycles. Call
# stacks come from DWARF; symbols for processes inside containers resolve
# through perf's build-id cache. Classification is core_layers.py.
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
info() { echo -e "${GREEN}[core]${NC} $*"; }
warn() { echo -e "${YELLOW}[core]${NC} $*" >&2; }
err()  { echo -e "${RED}[core]${NC} $*" >&2; }
step() { echo -e "${BLUE}[core]${NC} $*"; }
die()  { err "$*"; exit 1; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck disable=SC1091
[ -f "$ROOT/.env" ] && { set -a; . "$ROOT/.env"; set +a; }
NS="${NS:-test-bench}"
CTRL_PORT="${CTRL_PORT:-9092}"
: "${HOST_PASS:?.env must define HOST_PASS}"

CONFIG=""
BODY=48                 # payload bytes; the wire frame adds a 16 B header
RATE=400000
THREADS="${THREADS:-8}" # persistent connections, one per connection-affine DPU shard
WARMUP="${WARMUP:-200}"
DUR=26
SETTLE=6
WINDOW=10
PERIOD=2000000          # unhalted cycles between samples
REP=1
OUT=""

usage() {
  cat <<EOF
Usage: $0 --config CFG --out DIR [options]

  --config CFG    dpumesh-native | dpumesh-preload | envoy-permissive |
                  envoy-strict | grpc-dpumesh | grpc-tcp |
                  grpc-envoy-permissive | grpc-envoy-strict
  --out DIR       write perf.data, unwound stacks, per-core, per-thread, DPU deltas
  --body B        payload bytes per direction (default $BODY; frame = B + 16)
  --rate R        offered requests/s, constant arrivals (default $RATE)
  --threads N     persistent connections (default $THREADS)
  --dur S         load duration (default $DUR)
  --settle S      delay before sampling starts (default $SETTLE)
  --window S      sampling window (default $WINDOW)
  --period C      cycles between samples (default $PERIOD)
  --rep N         repetition index recorded in meta.txt (default $REP)
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --config) CONFIG="$2"; shift 2 ;;
    --out)    OUT="$2"; shift 2 ;;
    --body)   BODY="$2"; shift 2 ;;
    --rate)   RATE="$2"; shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    --dur)    DUR="$2"; shift 2 ;;
    --settle) SETTLE="$2"; shift 2 ;;
    --window) WINDOW="$2"; shift 2 ;;
    --period) PERIOD="$2"; shift 2 ;;
    --rep)    REP="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done
[ -n "$CONFIG" ] || { usage >&2; exit 2; }
[ -n "$OUT" ] || { usage >&2; exit 2; }

### ------------------------------------------------------------ topology
case "$CONFIG" in
  dpumesh-native)        CLIENT_APP=bench-dpumesh;            SERVER_APP=echo-dpumesh ;;
  dpumesh-preload)       CLIENT_APP=preload-bench;            SERVER_APP=preload-echo ;;
  envoy-permissive)      CLIENT_APP=bench-tcp;                SERVER_APP=echo-tcp ;;
  envoy-strict)          CLIENT_APP=bench-tcp-strict;         SERVER_APP=echo-tcp-strict ;;
  grpc-dpumesh)          CLIENT_APP=bench-grpc-dpumesh;       SERVER_APP=echo-grpc-dpumesh ;;
  grpc-tcp)              CLIENT_APP=bench-grpc-tcp;           SERVER_APP=echo-grpc-tcp ;;
  grpc-envoy-permissive) CLIENT_APP=bench-grpc-envoy;         SERVER_APP=echo-grpc-envoy ;;
  grpc-envoy-strict)     CLIENT_APP=bench-grpc-envoy-strict;  SERVER_APP=echo-grpc-envoy-strict ;;
  *) die "unknown config: $CONFIG" ;;
esac

SUDO() { echo "$HOST_PASS" | sudo -S "$@"; }
dpu_sudo() {
  [ -n "${DPU_HOST:-}" ] || return 1
  ssh -o ConnectTimeout=8 "$DPU_HOST" \
    "echo '${DPU_PASS:-}' | sudo -S bash -c '$1'" 2>/dev/null |
    sed 's/^\[sudo\][^:]*: *//'
}

pod_ip_of() {
  kubectl get pod -n "$NS" -l "app=$1" \
    -o go-template='{{range .items}}{{if not .metadata.deletionTimestamp}}{{if eq .status.phase "Running"}}{{.status.podIP}}{{"\n"}}{{end}}{{end}}{{end}}' \
    2>/dev/null | sed -n 1p
}

# Echoes "container_name pid" for every container of an app label.
containers_of() {
  local app="$1" sandbox cid name pid
  sandbox=$(SUDO crictl pods --label "app=$app" -q 2>/dev/null | sed -n 1p)
  [ -n "$sandbox" ] || return 0
  for cid in $(SUDO crictl ps --pod "$sandbox" -q 2>/dev/null); do
    name=$(SUDO crictl inspect "$cid" 2>/dev/null | jq -r '.status.metadata.name')
    pid=$(SUDO crictl inspect "$cid" 2>/dev/null | jq -r '.info.pid')
    [ -n "$pid" ] && [ "$pid" != null ] && printf '%s %s\n' "$name" "$pid"
  done
}

CLIENT_IP=$(pod_ip_of "$CLIENT_APP")
[ -n "$CLIENT_IP" ] || die "no running pod for app=$CLIENT_APP in $NS"

mapfile -t CLIENT_CT < <(containers_of "$CLIENT_APP")
mapfile -t SERVER_CT < <(containers_of "$SERVER_APP")
[ "${#CLIENT_CT[@]}" -gt 0 ] || die "no containers for $CLIENT_APP"
[ "${#SERVER_CT[@]}" -gt 0 ] || die "no containers for $SERVER_APP"

ALL_PIDS=()
for row in "${CLIENT_CT[@]}" "${SERVER_CT[@]}"; do ALL_PIDS+=("${row##* }"); done

CG_PATH=()
for pid in "${ALL_PIDS[@]}"; do
  cg=$(SUDO cat "/proc/$pid/cgroup" 2>/dev/null | sed -n '1s/^[^:]*:[^:]*://p')
  CG_PATH+=("${cg:+/sys/fs/cgroup$cg}")
done

core_of_pid() { taskset -pc "$1" 2>/dev/null | awk '{print $NF}'; }
CLIENT_CORE=$(core_of_pid "${CLIENT_CT[0]##* }")
SERVER_CORE=$(core_of_pid "${SERVER_CT[0]##* }")
[ -n "$CLIENT_CORE" ] && [ -n "$SERVER_CORE" ] || die "could not read core affinity"
[ "$CLIENT_CORE" != "$SERVER_CORE" ] || die "endpoints share core $CLIENT_CORE"
case "$CLIENT_CORE$SERVER_CORE" in *,*) die "endpoint holds more than one core" ;; esac
CORES="$CLIENT_CORE,$SERVER_CORE"

mkdir -p "$OUT"
info "config=$CONFIG rep=$REP client=$CLIENT_APP(core $CLIENT_CORE) server=$SERVER_APP(core $SERVER_CORE)"

### ------------------------------------------------------------ symbols
step "registering build-ids"
BID_LOG="$OUT/buildids.txt"
: >"$BID_LOG"
for pid in "${ALL_PIDS[@]}"; do
  SUDO bash -c '
    pid=$1
    awk "\$6 ~ /^\// {print \$6}" "/proc/$pid/maps" | sort -u |
    while read -r path; do
      host="/proc/$pid/root$path"
      [ -f "$host" ] || continue
      if perf buildid-cache --add "$host" >/dev/null 2>&1; then
        echo "$pid $path added"
      else
        echo "$pid $path FAILED"
      fi
    done
  ' _ "$pid" >>"$BID_LOG" 2>/dev/null || true
done
info "$(grep -c ' added$' "$BID_LOG" || true) mappings registered"

### ------------------------------------------------------------ snapshots
snapshot_cores() {
  awk -v a="cpu$CLIENT_CORE" -v b="cpu$SERVER_CORE" \
    '$1 == a || $1 == b { print }' /proc/stat >"$1"
}

# pid,usage_usec from each container's cgroup.
snapshot_cgroup() {
  local out="$1" i
  : >"$out"
  for i in "${!ALL_PIDS[@]}"; do
    [ -n "${CG_PATH[$i]}" ] || continue
    printf '%s,%s\n' "${ALL_PIDS[$i]}" \
      "$(awk '/^usage_usec/{print $2}' "${CG_PATH[$i]}/cpu.stat" 2>/dev/null)" >>"$out"
  done
}

# pid,tid,comm,utime,stime,voluntary_csw,nonvoluntary_csw
snapshot_threads() {
  local out="$1" pid t tid comm stat rest sw
  : >"$out"
  for pid in "${ALL_PIDS[@]}"; do
    for t in /proc/"$pid"/task/*; do
      tid=${t##*/}
      stat=$(cat "$t/stat" 2>/dev/null) || continue
      comm=$(tr -d '\0' <"$t/comm" 2>/dev/null || echo '?')
      sw=$(awk '/^voluntary_ctxt_switches/{v=$2}
                /^nonvoluntary_ctxt_switches/{n=$2}
                END{printf "%d,%d", v+0, n+0}' "$t/status" 2>/dev/null) || sw="0,0"
      rest=${stat#*") "}
      # shellcheck disable=SC2086
      set -- $rest
      printf '%s,%s,%s,%s,%s,%s\n' "$pid" "$tid" "$comm" "${12}" "${13}" "$sw" >>"$out"
    done
  done
}

# "HZ n" then one "T tid comm ticks" line per DPU data-path thread.
snapshot_dpu() {
  dpu_sudo 'pid=$(pgrep -x dpumesh_dpu | head -1); [ -z "$pid" ] && exit 1;
    echo "HZ $(getconf CLK_TCK)";
    for task in /proc/$pid/task/*; do tid=${task##*/}; comm=$(<"$task/comm");
      stat=$(<"$task/stat"); rest=${stat#*") "}; set -- $rest;
      echo "T $tid $comm $((${12}+${13}))"; done' >"$1" 2>/dev/null || : >"$1"
}

### ------------------------------------------------------------ run
LOADLINE="OPEN $BODY $BODY $THREADS $DUR $WARMUP $RATE const"
step "load: $LOADLINE -> $CLIENT_APP@$CLIENT_IP:$CTRL_PORT"
( printf '%s\n' "$LOADLINE" | timeout "$((DUR + 120))" nc -N "$CLIENT_IP" "$CTRL_PORT" \
    >"$OUT/run.txt" 2>&1 ) &
LOAD_PID=$!

sleep "$SETTLE"
kill -0 "$LOAD_PID" 2>/dev/null || die "load ended early: $(cat "$OUT/run.txt")"

HZ=$(getconf CLK_TCK)
snapshot_dpu     "$OUT/dpu0.txt";     DWALL0=$(date +%s.%N)
snapshot_threads "$OUT/threads0.csv"; TWALL0=$(date +%s.%N)
snapshot_cgroup  "$OUT/cg0.csv";      CWALL0=$(date +%s.%N)
snapshot_cores   "$OUT/cores0.txt";   WALL0=$(date +%s.%N)

step "perf record -c $PERIOD --call-graph dwarf -C $CORES for ${WINDOW}s"
SUDO perf record -e cycles -c "$PERIOD" -g --call-graph dwarf,8192 -C "$CORES" \
  -o "$OUT/perf.data" -- sleep "$WINDOW" >"$OUT/perf.log" 2>&1

snapshot_cores   "$OUT/cores1.txt";   WALL1=$(date +%s.%N)
snapshot_cgroup  "$OUT/cg1.csv";      CWALL1=$(date +%s.%N)
snapshot_threads "$OUT/threads1.csv"; TWALL1=$(date +%s.%N)
snapshot_dpu     "$OUT/dpu1.txt";     DWALL1=$(date +%s.%N)

wait "$LOAD_PID" 2>/dev/null || true
RESULT=$(sed -n 1p "$OUT/run.txt")
[ "${RESULT:0:2}" = OK ] || warn "run did not report OK: $RESULT"

### ------------------------------------------------------------ unwind
step "unwinding stacks"
SUDO bash -c "perf script --no-inline -i '$OUT/perf.data' >'$OUT/perf.script' 2>'$OUT/script.log'" || true
SUDO chown "$(id -u):$(id -g)" "$OUT/perf.data" "$OUT/perf.script" "$OUT/script.log"
[ -s "$OUT/perf.script" ] || die "perf script produced nothing; see $OUT/script.log"

### ------------------------------------------------------------ derived
WALL=$(awk -v a="$WALL0" -v b="$WALL1" 'BEGIN{printf "%.4f", b-a}')
TWALL=$(awk -v a="$TWALL0" -v b="$TWALL1" 'BEGIN{printf "%.4f", b-a}')
CWALL=$(awk -v a="$CWALL0" -v b="$CWALL1" 'BEGIN{printf "%.4f", b-a}')
DWALL=$(awk -v a="$DWALL0" -v b="$DWALL1" 'BEGIN{printf "%.4f", b-a}')

{
  echo "core,busy,user,system,irq,softirq,idle"
  awk -v hz="$HZ" -v wall="$WALL" '
    NR==FNR { for (i=2; i<=11; i++) a[$1,i]=$i; next }
    {
      tot=0; for (i=2; i<=11; i++) { d[i]=$i-a[$1,i]; tot+=d[i] }
      idle=d[5]+d[6]
      printf "%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", $1,
        (tot-idle)/hz/wall, d[2]/hz/wall, d[4]/hz/wall, d[7]/hz/wall, d[8]/hz/wall, idle/hz/wall
    }
  ' "$OUT/cores0.txt" "$OUT/cores1.txt"
} >"$OUT/core_busy.csv"

{
  echo "pid,tid,comm,cores,user,system,csw_per_s"
  awk -F, -v hz="$HZ" -v wall="$TWALL" '
    NR==FNR { u[$1","$2]=$4; s[$1","$2]=$5; c[$1","$2]=$6+$7; next }
    ($1","$2) in u {
      du=($4-u[$1","$2])/hz/wall; ds=($5-s[$1","$2])/hz/wall
      csw=($6+$7-c[$1","$2])/wall
      if (du + ds > 0.0005 || csw > 100)
        printf "%s,%s,%s,%.4f,%.4f,%.4f,%.0f\n", $1, $2, $3, du+ds, du, ds, csw
    }
  ' "$OUT/threads0.csv" "$OUT/threads1.csv" | sort -t, -k4 -rn
} >"$OUT/thread_cpu.csv"

{
  echo "pid,role,cores"
  awk -F, -v wall="$CWALL" -v cc="${CLIENT_CT[*]}" -v sc="${SERVER_CT[*]}" '
    BEGIN { n=split(cc, a, " "); for (i=2; i<=n; i+=2) role[a[i]]="client"
            n=split(sc, b, " "); for (i=2; i<=n; i+=2) role[b[i]]="server" }
    NR==FNR { u[$1]=$2; next }
    ($1 in u) { printf "%s,%s,%.5f\n", $1, role[$1], ($2-u[$1])/1e6/wall }
  ' "$OUT/cg0.csv" "$OUT/cg1.csv"
} >"$OUT/cgroup_cpu.csv"

{
  echo "tid,comm,cores"
  awk -v wall="$DWALL" '
    $1=="HZ" { hz=$2; next }
    NR==FNR { if ($1=="T") a[$2]=$4; next }
    $1=="T" && ($2 in a) { printf "%s,%s,%.4f\n", $2, $3, ($4-a[$2])/hz/wall }
  ' "$OUT/dpu0.txt" "$OUT/dpu1.txt" | sort -t, -k3 -rn
} >"$OUT/dpu_cpu.csv"
DPU_CORES=$(awk -F, 'NR>1 {s+=$3} END {printf "%.4f", s+0}' "$OUT/dpu_cpu.csv")

{
  echo "config=$CONFIG rep=$REP"
  echo "client_app=$CLIENT_APP client_core=$CLIENT_CORE"
  echo "server_app=$SERVER_APP server_core=$SERVER_CORE"
  for row in "${CLIENT_CT[@]}"; do echo "client_container=${row%% *} pid=${row##* }"; done
  for row in "${SERVER_CT[@]}"; do echo "server_container=${row%% *} pid=${row##* }"; done
  echo "body=$BODY frame=$((BODY + 16)) rate=$RATE threads=$THREADS arrival=const"
  echo "dur=$DUR settle=$SETTLE window=$WINDOW wall=$WALL cwall=$CWALL period=$PERIOD hz=$HZ"
  echo "dpu_cores=$DPU_CORES"
  echo "result=$RESULT"
} >"$OUT/meta.txt"

info "samples: $(grep -oE '[0-9]+ samples' "$OUT/perf.log" | sed -n 1p), DPU ARM ${DPU_CORES} core"
sed -n '2,$p' "$OUT/core_busy.csv" | while IFS=, read -r c busy user sys irq soft _; do
  printf '  %-6s busy %s (user %s sys %s irq %s softirq %s)\n' "$c" "$busy" "$user" "$sys" "$irq" "$soft"
done
info "-> $OUT"
