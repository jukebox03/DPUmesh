#!/bin/bash
# Compare the native, preload, and gRPC APIs over the same embedded-Linkerd
# data path. Every arm traverses the DPU Linkerd outbound stack: native and
# preload as opaque sessions, gRPC as a protocol-aware HTTP/2 stack.
#
# Phase A runs every arm on the shared one-core-per-Pod pinning, so the arms
# hold equal core budgets. Phase B repins the client and server of one arm to
# six cores each and scales client threads. Phase C reads the per-backend
# request distribution the client reports, which is how endpoint selection is
# observed from outside the DPU.
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH="$SUITE_DIR/../bench.sh"
OUT_DIR="${OUT_DIR:-$SUITE_DIR/../report/data/api-l7-$(date +%Y%m%d-%H%M%S)}"
REPS="${REPS:-3}"
DUR="${DUR:-10}"
WARM="${WARM:-2000}"
SIZES="${SIZES:-64 1024 8192}"
THREAD_SET="${THREAD_SET:-1 2 4 6}"
CONC="${CONC:-32}"
ARMS="${ARMS:-dpumesh preload grpc-dpumesh}"
SETTLE="${SETTLE:-2}"

mkdir -p "$OUT_DIR"
CSV="$OUT_DIR/points.csv"
LOG="$OUT_DIR/raw.log"
[ -f "$CSV" ] || echo "phase,arm,rep,req,reply,conc,threads,mrps,gbps,p50,p95,p99,p999,avg,rcnt,fail,drops,overflow,worker_fail,reorder,dist" >"$CSV"

field() { sed -n "s/.*[[:space:]]$2=\([^ ]*\).*/\1/p" <<<"$1"; }

# point <phase> <arm> <rep> <req> <conc> <threads>
point() {
    local phase="$1" arm="$2" rep="$3" req="$4" conc="$5" thr="$6" r
    r=$(bash "$BENCH" point "$arm" "$req" 8 "$conc" "$DUR" "$WARM" "$thr" 2>/dev/null |
        sed 's/\x1b\[[0-9;]*m//g' | grep -E '^(OK|ERR)' | tail -1)
    if [[ "$r" != OK* ]]; then
        echo "RETRY $phase $arm rep$rep req=$req conc=$conc thr=$thr -> $r" >>"$LOG"
        sleep 3
        r=$(bash "$BENCH" point "$arm" "$req" 8 "$conc" "$DUR" "$WARM" "$thr" 2>/dev/null |
            sed 's/\x1b\[[0-9;]*m//g' | grep -E '^(OK|ERR)' | tail -1)
    fi
    echo "$phase $arm rep$rep req=$req conc=$conc thr=$thr :: $r" >>"$LOG"
    [[ "$r" == OK* ]] || { printf '    %-13s req=%-6s conc=%-3s thr=%s  FAILED: %s\n' "$arm" "$req" "$conc" "$thr" "$r"; return 0; }
    printf '%s,%s,%s,%s,8,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$phase" "$arm" "$rep" "$req" "$conc" "$thr" \
        "$(field "$r" mrps)" "$(field "$r" gbps)" "$(field "$r" p50)" \
        "$(field "$r" p95)" "$(field "$r" p99)" "$(field "$r" p999)" \
        "$(field "$r" avg)" "$(field "$r" rcnt)" "$(field "$r" fail)" \
        "$(field "$r" drops)" "$(field "$r" overflow)" "$(field "$r" worker_fail)" \
        "$(field "$r" reorder)" "$(field "$r" dist)" >>"$CSV"
    printf '    %-13s req=%-6s conc=%-3s thr=%s  Mrps=%-10s p50=%-8s p99=%-8s fail=%s\n' \
        "$arm" "$req" "$conc" "$thr" "$(field "$r" mrps)" "$(field "$r" p50)" \
        "$(field "$r" p99)" "$(field "$r" fail)"
    sleep "$SETTLE"
}

snapshot_l7() { # <tag>
    { echo "=== l7 sessions $1 ==="; bash "$BENCH" l7metrics 2>&1 | sed 's/\x1b\[[0-9;]*m//g'; } \
        >>"$OUT_DIR/l7_metrics.txt"
}

phase_a() {
    echo "== Phase A: shared one-core pinning (fair), latency and throughput =="
    bash "$BENCH" pin fair >>"$LOG" 2>&1
    snapshot_l7 "phaseA-before"
    local rep sz arm
    for rep in $(seq 1 "$REPS"); do
        echo "  -- rep $rep --"
        for sz in $SIZES; do
            for arm in $ARMS; do point A-lat "$arm" "$rep" "$sz" 1 1; done
        done
        for sz in $SIZES; do
            for arm in $ARMS; do point A-tput "$arm" "$rep" "$sz" "$CONC" 1; done
        done
    done
    snapshot_l7 "phaseA-after"
}

phase_b() {
    echo "== Phase B: six client + six server cores per arm, client-thread scaling =="
    local arm profile rep thr
    for arm in $ARMS; do
        case "$arm" in
            dpumesh)      profile=native ;;
            preload)      profile=preload ;;
            grpc-dpumesh) profile=grpc ;;
        esac
        echo "  -- $arm (pin profile $profile) --"
        bash "$BENCH" pin "$profile" >>"$LOG" 2>&1
        sleep 3
        for rep in $(seq 1 "$REPS"); do
            for thr in $THREAD_SET; do point B-scale "$arm" "$rep" 1024 "$CONC" "$thr"; done
        done
        snapshot_l7 "phaseB-$arm"
    done
    bash "$BENCH" pin fair >>"$LOG" 2>&1
}

phase_c() {
    echo "== Phase C: endpoint distribution across the native Service's three backends =="
    bash "$BENCH" pin native >>"$LOG" 2>&1
    sleep 3
    local rep thr
    for rep in $(seq 1 "$REPS"); do
        for thr in 1 3 6; do point C-lb dpumesh "$rep" 1024 "$CONC" "$thr"; done
    done
    snapshot_l7 "phaseC"
    bash "$BENCH" pin fair >>"$LOG" 2>&1
}

echo "output -> $OUT_DIR"
for want in "$@"; do
    case "$want" in
        A) phase_a ;;
        B) phase_b ;;
        C) phase_c ;;
        *) echo "usage: $0 [A] [B] [C]" >&2; exit 1 ;;
    esac
done
[ $# -gt 0 ] || { phase_a; phase_b; phase_c; }
echo "done -> $CSV"
