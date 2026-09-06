65536-capacity-clean-{1,2,3}.json.contaminated: lean2 64 KiB capacity samples taken
20:35-20:37 KST while host-side cargo test/check builds for lean3 ran on rapids4,
the load-generator host. Throughput dropped to 11.7-12.1K RPC/s with lower
CPU/RPC; excluded from the A/B and re-measured as reps 4-6.
64-matched-clean-1.json.died: the lean2 runtime exited silently at the start of
this run (dpu-log-died.txt); the bench reply was empty.
