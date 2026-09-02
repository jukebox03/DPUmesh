#!/usr/bin/env bash
# Resolve the geometry of the DPU deployment that validation fixtures join.
# Output is: "K A" (rings per Pod, effective ARM data workers).

resolve_deployed_geometry() {
    local root="${1:?repository root required}"
    local throughput="${DPUMESH_THROUGHPUT_WORKERS:-}"
    local rings="${DPUMESH_RINGS_PER_POD:-}"
    local workers="${DPUMESH_ARM_WORKERS:-}"
    local banner geometry rc

    if [ -n "$throughput" ]; then
        case "$throughput" in
            4|6|8|12) printf '%s %s\n' "$throughput" "$throughput"; return 0 ;;
            *) echo "DPUMESH_THROUGHPUT_WORKERS must be 4, 6, 8 or 12" >&2
               return 2 ;;
        esac
    fi

    # Fully specified expert geometry remains available for density runs. Match
    # the DPU's normalization so the number of runtime admin ports is effective
    # A, rather than an impossible requested value.
    if [ -n "$rings" ] && [ -n "$workers" ]; then
        [[ "$rings" =~ ^[0-9]+$ ]] && [ "$rings" -ge 1 ] && [ "$rings" -le 16 ] || {
            echo "DPUMESH_RINGS_PER_POD must be in 1..16" >&2; return 2; }
        [[ "$workers" =~ ^[0-9]+$ ]] && [ "$workers" -ge 1 ] || {
            echo "DPUMESH_ARM_WORKERS must be positive" >&2; return 2; }
        [ "$workers" -le 16 ] || workers=16
        while [ "$workers" -gt 1 ] &&
              { [ "$workers" -gt "$rings" ] || [ $((rings % workers)) -ne 0 ]; }; do
            workers=$((workers - 1))
        done
        printf '%s %s\n' "$rings" "$workers"
        return 0
    fi

    # Half a geometry cannot be completed with a default: a stale K=8 beside a
    # 12-worker deployment would make the fixture inspect only part of it. Say
    # so, then take the effective values the running DPU printed at startup.
    if [ -n "$rings$workers" ]; then
        echo "only one of DPUMESH_RINGS_PER_POD=${rings:-unset} and DPUMESH_ARM_WORKERS=${workers:-unset} is set; ignoring it and reading effective K/A from the running DPU (set both, or DPUMESH_THROUGHPUT_WORKERS, to skip the DPU read)" >&2
    fi
    rc=0
    banner=$(cd "$root" && timeout 120 bench/bench.sh dpubanner 2>/dev/null) || rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "cannot read the DPU log (bench.sh dpubanner rc=$rc); is the DPU reachable?" >&2
        return 1
    fi
    geometry=$(printf '%s' "$banner" |
        sed -n 's/.*N\/K\/A=[0-9]*\/\([0-9]*\)\/\([0-9]*\).*/\1 \2/p' | tail -1)
    [ -n "$geometry" ] || {
        echo "no 'DPU PROXY MODE ON' banner in the DPU log; is the DPU up?" >&2
        return 1
    }
    printf '%s\n' "$geometry"
}
