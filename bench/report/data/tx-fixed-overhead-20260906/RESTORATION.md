# Restoration

- The running DPU PID 3456922 carries the same pre-campaign direct binary,
  SHA256 `2200fc9207e01b7e07e3aa0d45f26d55fef6f9aee07fb4a61cdfd3169389422e`
  (`raw/initial-binary.txt`, `raw/restored-binary.txt`).
- `/home/jukebox/DPUmesh/doca/build/dpumesh_dpu` was reverted to direct as well.
  The lean build survives only as `/tmp/tx-fixed-overhead-20260906/lean`
  (SHA256 `1577c3cc…`).
- The DPU runs the L4 profile: `DPUMESH_L7_SVC=''`, `DPUMESH_L7_OPAQUE_SVC=''`,
  N/K/A = 32/8/8.
- Both native Deployments' pre-campaign Pod templates and replica counts and the
  `echo-dpumesh-native` Service spec were reapplied unchanged; both Pods are
  Ready with 0 restarts (`raw/restored-workloads.json`).
- Native 64 B smoke: 169,523 RPC/s with fail, drop, overflow, worker_fail and
  reorder all 0.
- The 28086/28087/28088 control relays this campaign started were stopped and
  their ports are free; the pre-existing 28089 controller relay was left running.
- The `txfo_20260906` trace instance and its uprobes were removed after each
  trace window.
- Before every L7 arm shut down, the arena read 1,024/1,024 free with 0 live
  chunks, confirmed seven times (`raw/quiescence-*-arena.txt`).

DPU scratch cleanup (after the user approved it, together with the park-arm
campaign): `/tmp/tx-fixed-overhead-20260906/` was deleted, so no lean binary
remains on the DPU. The DPU's copy of
`bench/report/data/tx-fixed-overhead-20260906/arena_offsets.c` remains.
