#!/bin/sh
set -eu

proxy_source=${1:-doca/dpu_proxy.c}
control_source=${2:-doca/comch_server.c}

dma_error_callback=$(
    awk '
        /^static void px_dma_err_cb\(/ { in_callback = 1 }
        in_callback { print }
        in_callback && /^}/ { exit }
    ' "$proxy_source"
)

if [ -z "$dma_error_callback" ]; then
    echo "dma_fault_scope_test: px_dma_err_cb not found" >&2
    exit 1
fi

case "$dma_error_callback" in
    *dma_ready*)
        echo "dma_fault_scope_test: shared DMA error callback must not unpublish a pod" >&2
        exit 1
        ;;
esac

case "$dma_error_callback" in
    *"eng->dma_stalled = 1"*) ;;
    *)
        echo "dma_fault_scope_test: IO fault no longer latches engine recovery" >&2
        exit 1
        ;;
esac

cleanup_path=$(
    awk '
        /^pod_begin_cleanup\(/ { in_cleanup = 1 }
        in_cleanup { print }
        in_cleanup && /^}/ { exit }
    ' "$control_source"
)

case "$cleanup_path" in
    *dma_ready*"0"*) ;;
    *)
        echo "dma_fault_scope_test: authoritative pod cleanup must unpublish DMA readiness" >&2
        exit 1
        ;;
esac

engine_pump=$(
    awk '
        /^static int px_engine_pump\(/ { in_function = 1 }
        in_function { print }
        in_function && /^}/ { exit }
    ' "$proxy_source"
)

# The lane loop publishes reverse entries: only for ready pods (the dead branch
# continues first) and never while a data retry is active.
case "$engine_pump" in
    *pod_data_ready*retry_batches*px_rev_kick_lane*) ;;
    *)
        echo "dma_fault_scope_test: reverse publication must respect retry isolation and pod readiness" >&2
        exit 1
        ;;
esac

case "$engine_pump" in
    *px_rev_drop_dead*) ;;
    *)
        echo "dma_fault_scope_test: pod cleanup must discard dead-host reverse state" >&2
        exit 1
        ;;
esac

echo "dma_fault_scope_test: PASS"
