**Restoration and final verification complete**

The production TX code and the existing binary were not modified. Every file
this work added lives in this diagnosis directory.

- 114 samples in total: 72 clean, 12 profile, 24 trace, 6 PMU. Every retained
  reply is OK with fail, drops, overflow and worker_fail all 0.
- All 24 histograms report dropped = 0; all 12 profiles report 0 lost samples.
- In every "after" sample, publication retries were 0 and accepted bytes matched
  arena copy bytes.
- At the end of the final L7 measurement, all eight workers reported 0 residual
  sessions, tasks, registrations, DMA, queue entries, stalls, ACKs and FINs. No
  claim is made in place of an arena peak or a full free-list scan.
- The `dtx_20260906` trace instance and its uprobe event group were confirmed
  removed.
- Both native Deployments' Pod templates and the Service spec match the snapshot
  taken before the work exactly.
- After restoration the DPU PID 3331009 executable is SHA256
  `2200fc9207e01b7e07e3aa0d45f26d55fef6f9aee07fb4a61cdfd3169389422e`, with
  `DPUMESH_L7_SVC` and `DPUMESH_L7_OPAQUE_SVC` both empty, i.e. the L4 state.
- Both native workloads are Ready and the native smoke reports fail, drops,
  overflow and worker_fail all 0. The `pending=64` in the native reply is the
  measurement cutoff value and is not read as a residual-session gauge.
- The control relays this work started (28086/28087/28088) were stopped. The
  28089 controller relay, which predates the work, was left running.
- The diagnosis Python files and shell scripts were syntax-checked. Because no
  production code changed, the full unit and build suite was not re-run; what
  was verified is the instrumentation, deployment, restoration and the data
  invariants.

Evidence: [validation.json](validation.json),
[restored smoke](raw/restored-native-smoke.txt),
[restored Pod state](raw/restored-native-pods.txt),
[final hash/L4/probe check](raw/restored-final-check.txt),
[original workloads](raw/initial-workloads.json),
[restored workloads](raw/restored-workloads.json),
[relay shutdown record](raw/temporary-relay-cleanup.json).

The perf originals are losslessly compressed as `.data.gz`, and the large
metrics, profile and symbol texts are kept as `.txt.gz`, which the aggregators
read directly. SHA256 before and after compression confirms identity;
[perf artifact manifest](raw/perf-artifacts.json) and
[text artifact manifest](raw/text-artifacts.json) carry the sizes and original
hashes. CPU profiles and traces were never mixed into the performance acceptance
samples.
