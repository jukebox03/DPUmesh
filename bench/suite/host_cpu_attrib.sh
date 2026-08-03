#!/bin/bash
# Attribute host core time to the operations that spend it.
#
# Answers "which thread, doing what, consumed the endpoint core" for one
# benchmark process during a window. Works for every configuration: the native
# and POSIX clients, the gRPC pair, and an Envoy sidecar.
#
# Three views, from cheapest to most invasive:
#   per-thread   delta user/system time and context switches from /proc
#   per-syscall  sampled /proc/<tid>/syscall and wchan, so time is attributed to
#                an operation without a profiler
#   per-symbol   optional perf record, when perf can see the process
#
# Sampling is deliberately /proc-based: perf collects nothing inside the
# benchmark containers on this host, and the questions that matter — which
# thread is the funnel, and whether it is running or parked — do not need it.
#
# Nothing here drives load. Start the run separately, then point this at it.
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
info() { echo -e "${GREEN}[cpu]${NC} $*"; }
warn() { echo -e "${YELLOW}[cpu]${NC} $*" >&2; }
err()  { echo -e "${RED}[cpu]${NC} $*" >&2; }
step() { echo -e "${BLUE}[cpu]${NC} $*"; }

DURATION=10
INTERVAL_MS=20
OUT=""
PID=""
APP=""
COMM=""
WANT_PERF=0
NS="${NS:-test-bench}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck disable=SC1091
[ -f "$ROOT/.env" ] && { set -a; . "$ROOT/.env"; set +a; }

usage() {
  cat <<EOF
Usage: $0 (--pid PID | --app K8S_APP | --comm NAME) [options]

  --pid PID          attribute this process
  --app APP          resolve the container PID of a k8s app label in \$NS
  --comm NAME        resolve a host process by command name (e.g. bench_grpc)
  --duration S       sampling window, default $DURATION
  --interval MS      sample period, default $INTERVAL_MS
  --out DIR          write CSVs here as well as printing
  --perf             also record symbols with perf (skipped if it sees nothing)

Examples:
  $0 --app bench-grpc-dpumesh --duration 10
  CONTAINER=sidecar1 $0 --app bench-grpc-envoy --duration 10
  $0 --comm bench_grpc --duration 10 --perf --out /tmp/attrib
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --pid) PID="$2"; shift 2 ;;
    --app) APP="$2"; shift 2 ;;
    --comm) COMM="$2"; shift 2 ;;
    --duration) DURATION="$2"; shift 2 ;;
    --interval) INTERVAL_MS="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --perf) WANT_PERF=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) err "unknown argument: $1"; usage >&2; exit 2 ;;
  esac
done

### ------------------------------------------------------------ target
resolve_pid() {
  if [ -n "$PID" ]; then return; fi
  if [ -n "$COMM" ]; then
    PID=$(ps -eo pid,comm --no-headers | awk -v c="$COMM" '$2==c{print $1; exit}')
    [ -n "$PID" ] || { err "no process named $COMM"; exit 1; }
    return
  fi
  if [ -n "$APP" ]; then
    command -v kubectl >/dev/null || { err "kubectl not found"; exit 1; }
    command -v jq >/dev/null || { err "jq not found"; exit 1; }
    : "${HOST_PASS:?--app needs HOST_PASS from the repository-root .env}"
    local pod cid
    pod=$(kubectl get pod -n "$NS" -l "app=$APP" \
      -o go-template='{{range .items}}{{if not .metadata.deletionTimestamp}}{{if eq .status.phase "Running"}}{{.metadata.name}}{{"\n"}}{{end}}{{end}}{{end}}' \
      2>/dev/null | sed -n '1p')
    [ -n "$pod" ] || { err "no running pod for app=$APP in $NS"; exit 1; }
    cid=$(kubectl get pod -n "$NS" "$pod" -o json |
      jq -r --arg c "${CONTAINER:-$APP}" \
        '.status.containerStatuses[] | select(.name==$c) | .containerID' | sed -n '1p')
    cid="${cid#*://}"
    [ -n "$cid" ] && [ "$cid" != null ] ||
      { err "container ${CONTAINER:-$APP} not found in $pod"; exit 1; }
    PID=$(printf '%s\n' "$HOST_PASS" |
      sudo -S crictl inspect "$cid" 2>/dev/null | jq -r '.info.pid')
    [ -n "$PID" ] && [ "$PID" != null ] ||
      { err "could not resolve a host PID for $pod"; exit 1; }
    info "app=$APP pod=$pod container=${CONTAINER:-$APP} -> pid=$PID"
    return
  fi
  err "one of --pid, --app or --comm is required"; usage >&2; exit 2
}
resolve_pid
[ -d "/proc/$PID" ] || { err "pid $PID is gone"; exit 1; }
[ -z "$OUT" ] || mkdir -p "$OUT"

HZ=$(getconf CLK_TCK)
CMD=$(tr -d '\0' < "/proc/$PID/comm")
CORES=$(awk '/Cpus_allowed_list/{print $2}' "/proc/$PID/status")
info "pid=$PID comm=$CMD cores=$CORES window=${DURATION}s interval=${INTERVAL_MS}ms"

### ------------------------------------------------------------ syscall names
# x86_64 numbers that appear on these data paths. Anything else prints its
# number, which is enough to look up.
syscall_name() {
  case "$1" in
    0) echo read ;;      1) echo write ;;      3) echo close ;;
    7) echo poll ;;      9) echo mmap ;;      16) echo ioctl ;;
    23) echo select ;;  35) echo nanosleep ;; 44) echo sendto ;;
    45) echo recvfrom ;; 46) echo sendmsg ;;  47) echo recvmsg ;;
    202) echo futex ;;  228) echo clock_gettime ;;
    230) echo clock_nanosleep ;; 232) echo epoll_wait ;;
    233) echo epoll_ctl ;; 271) echo ppoll ;;
    281) echo epoll_pwait ;; 291) echo epoll_create1 ;;
    318) echo getrandom ;; 425) echo io_uring_setup ;;
    441) echo epoll_pwait2 ;;
    -1) echo "(running)" ;;
    "") echo "(unknown)" ;;
    *) echo "syscall_$1" ;;
  esac
}

### ------------------------------------------------------------ snapshots
# tid,comm,utime,stime,vcsw,nvcsw
snapshot_threads() {
  local out="$1" t tid comm stat rest ut st v nv
  : >"$out"
  for t in /proc/"$PID"/task/*; do
    tid=${t##*/}
    [ -r "$t/stat" ] || continue
    stat=$(cat "$t/stat" 2>/dev/null) || continue
    comm=$(tr -d '\0' < "$t/comm" 2>/dev/null || echo "?")
    rest=${stat#*") "}
    # After the state field: utime is 12th, stime 13th.
    set -- $rest
    ut=${12}; st=${13}
    v=$(awk '/^voluntary_ctxt_switches/{print $2}' "$t/status" 2>/dev/null || echo 0)
    nv=$(awk '/^nonvoluntary_ctxt_switches/{print $2}' "$t/status" 2>/dev/null || echo 0)
    printf '%s,%s,%s,%s,%s,%s\n' "$tid" "$comm" "${ut:-0}" "${st:-0}" \
      "${v:-0}" "${nv:-0}" >>"$out"
  done
}

# Per-core busy fields from /proc/stat for the cores this process may run on.
snapshot_cores() {
  local out="$1"
  awk -v list="$CORES" '
    BEGIN {
      n = split(list, parts, ",")
      for (i = 1; i <= n; i++) {
        if (parts[i] ~ /-/) { split(parts[i], r, "-"); for (c = r[1]; c <= r[2]; c++) want["cpu" c] = 1 }
        else want["cpu" parts[i]] = 1
      }
    }
    $1 in want { print $1 "," $2 "," $3 "," $4 "," $5 "," $6 "," $7 "," $8 }
  ' /proc/stat >"$out"
}

### ------------------------------------------------------------ sampling
# Samples every live thread's current syscall and wchan. A thread blocked in
# epoll_wait/futex is parked; one reported as running is on the core.
sample_loop() {
  local out="$1" deadline t tid sc wc n
  : >"$out"
  deadline=$(awk -v d="$DURATION" 'BEGIN{print systime() + d}')
  while [ "$(date +%s)" -lt "${deadline%.*}" ]; do
    for t in /proc/"$PID"/task/*; do
      tid=${t##*/}
      [ -r "$t/syscall" ] || continue
      sc=$(awk '{print $1}' "$t/syscall" 2>/dev/null) || continue
      wc=$(cat "$t/wchan" 2>/dev/null || echo "-")
      [ -n "$wc" ] || wc="-"
      printf '%s,%s,%s\n' "$tid" "${sc:-}" "$wc" >>"$out"
    done
    sleep "$(awk -v ms="$INTERVAL_MS" 'BEGIN{printf "%.3f", ms/1000}')"
  done
}

### ------------------------------------------------------------ run
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

step "=== sampling $CMD for ${DURATION}s ==="
snapshot_threads "$TMP/th0"
snapshot_cores  "$TMP/co0"

PERF_DATA=""
if [ "$WANT_PERF" = 1 ]; then
  if command -v perf >/dev/null 2>&1; then
    PERF_DATA="$TMP/perf.data"
    perf record -q -g --call-graph=fp -F 997 -p "$PID" \
      -o "$PERF_DATA" -- sleep "$DURATION" >/dev/null 2>&1 &
    PERF_BG=$!
  else
    warn "perf not found; skipping symbol view"
  fi
fi

sample_loop "$TMP/samples"

[ -z "${PERF_BG:-}" ] || wait "$PERF_BG" 2>/dev/null || true
snapshot_threads "$TMP/th1"
snapshot_cores  "$TMP/co1"

### ------------------------------------------------------------ per-thread
step "=== per-thread CPU and scheduling ==="
awk -F, -v hz="$HZ" -v dur="$DURATION" '
  NR==FNR { u0[$1]=$3; s0[$1]=$4; v0[$1]=$5; n0[$1]=$6; next }
  {
    tid=$1
    if (!(tid in u0)) next            # thread appeared mid-window
    u=($3-u0[tid])/hz; s=($4-s0[tid])/hz
    cpu=(u+s)/dur
    printf "%s,%s,%.4f,%.4f,%.4f,%d,%d\n", tid, $2, cpu, u/dur, s/dur,
           $5-v0[tid], $6-n0[tid]
  }
' "$TMP/th0" "$TMP/th1" | sort -t, -k3 -rn >"$TMP/threads.csv"

printf '  %-8s %-16s %8s %8s %8s %12s %12s\n' \
  TID COMM CORES USER SYS VOL_CSW INVOL_CSW
awk -F, '{ printf "  %-8s %-16s %8.3f %8.3f %8.3f %12d %12d\n", $1,$2,$3,$4,$5,$6,$7 }' \
  "$TMP/threads.csv"
awk -F, '{c+=$3; v+=$6; n+=$7} END {
  printf "  %-8s %-16s %8.3f %8s %8s %12d %12d\n", "TOTAL","",c,"","",v,n }' \
  "$TMP/threads.csv"

### ------------------------------------------------------------ per-syscall
step "=== where each thread's samples were (top 4 per thread) ==="
printf '  %-8s %-14s %7s  %s\n' TID SITE SHARE "(syscall / wchan)"
awk -F, '
  { key=$1 SUBSEP $2 SUBSEP $3; cnt[key]++; tot[$1]++ }
  END { for (k in cnt) { split(k, p, SUBSEP); printf "%s,%s,%s,%d,%d\n", p[1], p[2], p[3], cnt[k], tot[p[1]] } }
' "$TMP/samples" | sort -t, -k1,1n -k4,4rn >"$TMP/sites.csv"

while IFS=, read -r tid sc wc cnt tot; do
  prev=${seen_tid:-}
  if [ "$tid" != "$prev" ]; then rank=0; seen_tid=$tid; fi
  rank=$((rank + 1))
  [ "$rank" -le 4 ] || continue
  printf '  %-8s %-14s %6.1f%%  %s\n' "$tid" "$(syscall_name "$sc")" \
    "$(awk -v c="$cnt" -v t="$tot" 'BEGIN{printf "%.1f", t? c*100/t : 0}')" "$wc"
done <"$TMP/sites.csv"

### ------------------------------------------------------------ per-core
step "=== physical core busy over the window (includes softirq) ==="
printf '  %-8s %8s %8s %8s %8s %8s\n' CORE BUSY USER SYS IRQ SOFTIRQ
awk -F, '
  NR==FNR { for (i=2; i<=8; i++) a[$1,i]=$i; next }
  {
    tot=0; for (i=2; i<=8; i++) { d[i]=$i-a[$1,i]; tot+=d[i] }
    if (tot <= 0) next
    idle=d[5]+d[6]
    printf "  %-8s %8.3f %8.3f %8.3f %8.3f %8.3f\n", $1,
           (tot-idle)/tot, d[2]/tot, d[4]/tot, d[7]/tot, d[8]/tot
  }
' "$TMP/co0" "$TMP/co1"

### ------------------------------------------------------------ symbols
if [ -n "$PERF_DATA" ] && [ -s "$PERF_DATA" ]; then
  step "=== top symbols ==="
  if ! perf report -i "$PERF_DATA" --stdio --sort=symbol 2>/dev/null |
       grep -vE '^#|^$' | head -20; then
    warn "perf collected no samples for pid $PID"
  fi
elif [ "$WANT_PERF" = 1 ]; then
  warn "perf produced no data; the /proc views above are the attribution"
fi

### ------------------------------------------------------------ keep
if [ -n "$OUT" ]; then
  cp "$TMP/threads.csv" "$OUT/threads.csv"
  cp "$TMP/sites.csv"   "$OUT/sites.csv"
  cp "$TMP/samples"     "$OUT/samples.csv"
  [ -z "$PERF_DATA" ] || [ ! -s "$PERF_DATA" ] || cp "$PERF_DATA" "$OUT/perf.data"
  info "wrote $OUT/{threads,sites,samples}.csv"
fi

cat <<'EOF'

Reading this:
  A thread whose samples sit in epoll_wait/ppoll/futex is parked, not working.
  One thread near 1.000 cores with the rest idle is a funnel, and adding cores
  will not move it. High INVOL_CSW with low CORES is contention for the core,
  not work. Compare BUSY against the summed per-thread CORES: the difference is
  softirq and interrupt time the process does not account for.
EOF
