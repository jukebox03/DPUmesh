> Historical measurement record. Deployment and registration descriptions below apply to the recorded revision, not the current implementation. Current placement and protocol: [CONTROL](../../../../design/CONTROL.md) (updated 2026-09-10). Measurements are unchanged.

# Restoration

- Both native Deployments were restored to their exact pre-campaign Pod
  templates and replica counts.
- The `echo-dpumesh-native` Service spec and port 9092 were restored exactly.
- Both native Pods are Ready with 0 restarts.
- The running DPU PID 3383809 carries the same pre-campaign binary, SHA256
  `2200fc9207e01b7e07e3aa0d45f26d55fef6f9aee07fb4a61cdfd3169389422e`.
- The DPU runs the L4 profile: `DPUMESH_L7_SVC=''`, `DPUMESH_L7_OPAQUE_SVC=''`,
  N/K/A = 32/8/8.
- Native 64 B smoke: 180,780 RPC/s with fail, drop, overflow, worker_fail and
  reorder all 0.
- The 28086/28087/28088 relays this campaign started were stopped; the
  pre-existing 28089 controller relay was left running.
- The `atxb_20260906` trace instance and its events were removed.
- Before every L7 arm shut down, the arena read 1,024/1,024 free with 0 live
  chunks.

Evidence is in `raw/restored-*`, `raw/quiescence-*` and `raw/relay-process.json`.
