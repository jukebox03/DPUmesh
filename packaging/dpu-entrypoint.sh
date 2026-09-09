#!/bin/sh
set -eu
: "${DPUMESH_CLUSTER_ID:?}" "${DPUMESH_NODE_NAME:?served host node is required}"
: "${DPUMESH_DPU_PCI:?}" "${DPUMESH_DPU_REPRESENTOR:?}"
ulimit -l unlimited
exec /usr/local/bin/dpumesh_dpu -p "$DPUMESH_DPU_PCI" -r "$DPUMESH_DPU_REPRESENTOR" -l "${DPUMESH_LOG_LEVEL:-40}"
