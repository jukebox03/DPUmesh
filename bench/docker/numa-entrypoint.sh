#!/bin/sh
set -eu

# Apply the benchmark NUMA policy before the target process allocates or
# registers DMA memory. Preload targets defer LD_PRELOAD until after numactl so
# the shim does not intercept numactl's own sockets.
node="${BENCH_NUMA_NODE:-}"
target_preload="${NUMA_TARGET_LD_PRELOAD:-}"

if [ -z "$node" ]; then
    if [ -n "$target_preload" ]; then
        exec env LD_PRELOAD="$target_preload" "$@"
    fi
    exec "$@"
fi

case "$node" in
    *[!0-9]*)
        echo "invalid BENCH_NUMA_NODE: $node" >&2
        exit 2
        ;;
esac

if [ -n "$target_preload" ]; then
    exec /usr/bin/numactl --cpunodebind="$node" --membind="$node" \
        env LD_PRELOAD="$target_preload" "$@"
fi

exec /usr/bin/numactl --cpunodebind="$node" --membind="$node" "$@"
