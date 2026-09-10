> Historical measurement record. Deployment and registration descriptions below apply to the recorded revision, not the current implementation. Current placement and protocol: [CONTROL](../../../../design/CONTROL.md) (updated 2026-09-10). Measurements are unchanged.

# Restoration

- The running DPU PID 3533612 carries the same pre-campaign direct binary,
  SHA256 `2200fc9207e01b7e07e3aa0d45f26d55fef6f9aee07fb4a61cdfd3169389422e`.
- `/home/jukebox/DPUmesh/doca/build/dpumesh_dpu` was reverted to direct as well.
  The lean2/lean3 builds are gone from the DPU with the scratch cleanup below.
- The DPU runs the L4 profile: `DPUMESH_L7_SVC=''`, `DPUMESH_L7_OPAQUE_SVC=''`,
  N/K/A = 32/8/8.
- Both native Deployments and the `echo-dpumesh-native` Service were reapplied
  from their pre-campaign specs; both Pods are Ready with 0 restarts. The native
  64 B smoke ran 163,605 RPC/s with 0 failures.
- The 28086/28087/28088 control relays this campaign started were stopped and
  their ports are free. The 28089 controller relay was left running.
- Before every L7 arm shut down, the arena read 1,024/1,024 free with 0 live
  chunks.

DPU scratch cleanup (after the user approved it): `/tmp/park-arm-20260906/`
(1.9 GB) and `/tmp/tx-fixed-overhead-20260906/` (93 MB) were deleted, so no
lean/lean2/lean3 binary remains on the DPU. The earlier campaigns'
`/tmp/arena-tx-batching-perf-20260906/` (direct/batch binaries) and
`/tmp/direct-tx-diagnosis-20260906/` are untouched. The DPU root filesystem is
92% used (9.9 GB free), and the DPU kubelet's ephemeral-storage eviction log
continues independently of this campaign.
