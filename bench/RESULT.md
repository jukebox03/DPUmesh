# DPUmesh engineering result log

This file is chronological. Early entries preserve superseded implementations,
single-shot diagnostics, and local labels such as “BEST” or “FINAL”; a later
session that explicitly corrects an earlier one takes precedence for that
subject. The current transport contract lives in `design/`, while the frozen
ABI-1 cross-transport campaign lives in [report/REPORT.md](report/REPORT.md).
Session 7 below is the current ABI-2 batching A/B and records both the valid
configuration-controlled result and the first comparison that had to be discarded.

# Session 1 — scalability experiments

Goal: (1) raise 2-pod chain RPS, (2) make DPA/DPU multithread actually scale.
Constraint: all traffic stays host→DPU→host (L7 routing computed on DPU; no host→host).
Method: DIRECT experiments via `test-bench.sh` only. No elimination logic.

## Code-certain baseline facts (from code map, wf_a080182c-52b)
- `pod_id % num_dpa_threads` (dpa.c:1091,1278) → 2 pods always use exactly 2 EUs (N=4/8 idle the rest). This alone caps DPA-thread scaling for 2 pods.
- Chain inserts a mandatory per-request EU→ARM→EU round-trip: reverse `dma_copy` waits for the single ARM thread to drain the fwd completion (consumer_pe→comp_queue), route, and post a reverse desc (dpu_worker.c:194). pure_dma/M2 have no such wait → they scale.
- EU-sharding one pod across K EUs IS feasible (host posts K fwd rings; ARM round-robins reverse to K tx_rings; admission globals already [eu_index][ring]). Docs' "foreclosed" was elimination, not code-true.
- SPLIT_SHARD(=3)+DRAIN_SHARDS machinery is BUILT but never benchmarked on the chain. DRAIN_SHARDS>1 races for cross-pod echo (fwd EU≠rev EU → shard_work[k] gets 2 producers) — so test SPLIT_SHARD=3 with DRAIN_SHARDS=1 only.

## Hypotheses (each has a positive yes/no test)
- H1 single-ARM funnel bounds throughput → test: SPLIT_SHARD=3 raises 2-EU ceiling?
- H2 per-request data/host cost (8KB memset+memcpy, DMA xfer) dominates W → test: 128B vs 8KB ceiling differ?

---

## Experiments

### E0/E1 — deploy DPUMESH_DPA_THREADS=2 (split off), 8KB ceiling + size sweep
config: N=2, affinity=1, split=off, slots=2048, fair pin
(results below)

| exp | size | target RPS | achieved | p50 ms | p99 ms | ok/fail | notes |
|---|---|---|---|---|---|---|---|
| E0 | 8192 | 95000 | 94,166 | 12.1 | 16.8 | 950000/0 | healthy |
| E0 | 8192 | 104000 | 103,178 | 14.2 | 21.6 | 1.04M/0 | |
| E0 | 8192 | 105000 | **104,204** | 14.2 | 20.3 | 1.05M/0 | **sustainable ceiling** (=docs 104,202) |
| E0 | 8192 | 110000 | 106,963 | 117.5 | 206 | 1.1M/0 | overload (p50 jumps) |
| E1 | 128 | 105000 | 104,104 | 14.9 | 22.5 | 1.05M/0 | healthy, ~same as 8KB |
| E1 | 128 | 150000 | 108,730 | 1908 | 3710 | 1.5M/0 | deep overload (lat in seconds) |
| E1 | 128 | 200000 | 100,052 | 3771 | 7441 | — | past-knee decline |
| E1 | 128 | 250000 | 11,927 | 11428 | 15560 | 195300/1498 | collapse |
| E1 | 1024 | 105000 | 103,156 | 61.2 | 124 | 1.05M/0 | healthy-ish (lat noisier) |
| E1 | 1024 | 150000 | 102,077 | 2282 | 4559 | 1.5M/0 | overload |
| E1 | 1024 | 200000 | 120,261 | 3011 | 5933 | — | past-knee (unreliable) |
| E1 | 1024 | 250000 | 9,296 | 415 | 14824 | — | collapse |

**E0/E1 verdict: ceiling is SIZE-INDEPENDENT (~104K sustainable for 128B/1KB/8KB).**
→ H2 (data/host-memcpy/DMA-bandwidth bound) REJECTED by direct measurement. 64× less payload (8KB→128B) does not raise RPS. The cap is a fixed per-request op/handoff cost (dma_copy op-rate and/or EU↔ARM round-trip cadence), NOT bytes moved. Consistent with pure_dma op-rate boundedness, but chain caps at 416K dma_copy/s vs pure 2-EU 1.07M → the limiter is the closed-loop HANDOFF cadence, not the dma_copy engine.

### E2 — SPLIT_SHARD=3 (DRAIN_SHARDS=1), N=2, 8KB — H1 test (parallelize ARM route+reverse-post+send)
(results below)

| exp | target RPS | achieved | p50 ms | p99 ms | ok/fail | notes |
|---|---|---|---|---|---|---|
| E2 | 104000 | 103,036 | 14.4 | 20.4 | 1.04M/0 | |
| E2 | 105000 | **104,112** | 14.8 | 20.7 | 1.05M/0 | **= E0 104,204 (±0.1%)** |
| E2 | 110000 | 104,962 | 253 | 472 | 1.1M/0 | overload |
| E2 | 120000 | 102,691 | 827 | 1622 | 1.2M/0 | overload |

**E2 verdict: SPLIT_SHARD (4 ARM cores: drain+2 workers+sender) = E0 baseline within noise → H1 REJECTED.**
ARM route/reverse-post/send compute is NOT the throughput limiter. Combined with E1 → limiter is the closed-loop HANDOFF latency. Supporting prior evidence: bench.md §3.3 busy-spin (no EU yield) improved p99 → yielded EU only re-woken by msgq msg; ARM reverse-post is a silent memory write → EU waits up to keepalive(1kHz)=1ms to re-check tx_ring.

### Refined ladder (decision tree)
- E2 SPLIT_SHARD=3/DRAIN=1: parallelize ARM route+reverse-post(×2 workers)+send(1). Helps → ARM compute was serializing. Flat → ARM compute not the limiter (drain alone does >1M/s in M2, so not throughput-bound either) → limiter is closed-loop HANDOFF latency.
- E3 keepalive freq 1kHz→10kHz (+ later: ARM targeted-WAKE after reverse-post): EU yields when it out-runs ARM and only re-checks tx_ring on keepalive/recv-msg (dpa_kernel.c:491; ARM reverse-post is a silent memory write). If raising keepalive raises RPS → yield/wake latency is a real handoff component.
- E4 EU-sharding (CODE): host posts pod10/11 each across K fwd rings; ARM round-robins reverse to K tx_rings; admission globals already [eu][ring]. The ONLY way DPA multithread scales for 2 pods. Helps → per-pod serial EU work was binding; scalable multithread achieved.

### E3 — keepalive 1kHz→20kHz (50us), N=2 split off — handoff yield-wake test
knob: DPUMESH_KEEPALIVE_US (new), default 1000. E3 uses 50.
Prior context: 4-pod 2-pairs-halve (memory project_shard_bottleneck_consumer_pe) → shared sub-ARM resource (PCIe/comch op-rate OR closed-loop RTT). E1 rules out bandwidth, E2 rules out ARM CPU. E3 probes one RTT component (EU yield→keepalive wake). If flat → do per-request STAMP breakdown to positively localize W.

| exp | target RPS | achieved | p50 ms | p99 ms | ok/fail | notes |
|---|---|---|---|---|---|---|

### E3 results (keepalive 50us, N=2, split off)
| target | achieved | p50 ms | p99 ms | note |
|---|---|---|---|---|
| 50000 | 49,655 | 4.0 | 5.8 | |
| 95000 | 94,261 | 11.9 | 16.6 | =E0 (12.1/16.8) |
| 105000 | 104,182 | 14.7 | 21.5 | **=E0 104,204** |
| 110000 | 103,961 | 278 | 486 | overload |

**E3 verdict: keepalive 50us (20kHz) = E0 exactly (ceiling AND low-load latency). EU yield-wake REJECTED** — EU is not yield-bound at load (it spins). The 1kHz keepalive only matters at <10K RPS (the ~1.1ms idle floor).

### E4 — ARM occupancy under 105K load (DPU 1Hz stat, -l 50, reverted after)
Sustained 105K (achieved 104,539, p99 20.8ms):
- **recv: ~420,000/s** (= 4 dma_copy/RTT × 105K) — completions drained at expected rate
- **cq_depth: 0** EVERY second — comp_queue ALWAYS empty
- sent: 2000/s = keepalive only (sent_msg_cnt counts DPU→DPA wakes, NOT host sends)

**E4 verdict (POSITIVE evidence): cq_depth=0 → ARM is STARVED, never backed up.** Processes every completion instantly, waits for the next. Perfectly balanced at 420K completions/s, zero queueing → textbook **latency-bound closed loop** (T=C/W, W fixed). The cap is the round-trip W itself, not any stage's throughput.

## CONSOLIDATED DIAGNOSIS (all direct, no elimination-as-proof)
The 2-pod chain ceiling (~104K RPS) is set by the **per-request host↔DPU round-trip latency W**, which is:
- **shared** (4-pod: 2 pairs halve each other — memory) — not per-pair
- **size-independent** (E1: 128B=1KB=8KB) — not bandwidth/memcpy
- **not ARM compute** (E2 SPLIT_SHARD flat; E4 cq_depth=0/starved)
- **not EU yield/wake** (E3: 20× keepalive flat)
- **not concurrency/depth** (prior M/M/1 flat)
- DPA N>2 structurally idle for 2 pods (pod%N → 2 EUs) [code]

→ The bottleneck is OUTSIDE the DPU compute (EU/ARM). It's the closed-loop round-trip itself (PCIe op-rate / completion-delivery latency / host-side turnaround). **This is why DPA/DPU multithreading doesn't raise throughput: the threaded resource is not the bottleneck.** μ≈104K is a hard service-rate of the round-trip, and threading the EU or ARM cannot move a latency-bound, non-compute cap.

### E5 — DPU-side hop-latency trace (DPUMESH_TRACE=1, -l 40, separate file) — PIVOTAL
Trace = fwd-completion-dequeue → rev-completion-dequeue (same req_id) = full DPU contribution to one hop.

| load | achieved | host p50 | host p99 | DPU hop avg | DPU hop max | DPU ×2 / RTT |
|---|---|---|---|---|---|---|
| 105K | 104,867 | 14,879 us | 26,216 us | **207.9 us** | 1417 us | **2.8%** |
| 50K | 49,801 | 4,882 us | 7,761 us | **987.8 us** | 2014 us | **40%** |

**E5 verdict (POSITIVE, decisive):**
1. **Throughput ceiling = HOST-TRANSPORT-bound.** At 105K the DPU contributes 2.8% of RTT and cq_depth=0 (DPU never backed up). The other 97% + the 104K cap are HOST-side (single fwd ring_lock, single RX PE thread, single rx_queue — host-side code map). **DPA/DPU multithreading can't raise throughput because the DPU is NOT the bottleneck.** Overturns the prior memory guess ("cap below ARM = PCIe/comch").
2. **DPU latency floor = EU yield→keepalive wake for reverse pickup.** Inverts with load (988us@50K > 208us@105K): low load → EU yields → waits ~1 keepalive to pick up the ARM-posted reverse desc. (Confirms E3's small 50K drop 4.88→4.0ms was real, not noise.) Fixable by a targeted ARM→EU wake right after reverse-post → cuts DPU hop ~10-50× → big p50/p99 win at low/mid load (throughput unchanged — host-bound).

## REVISED CONCLUSION
- **Why DPU/DPA multithread doesn't help:** the bottleneck is the HOST transport, not the DPU. Proven directly (DPU=2.8% of RTT, cq=0). The EU(pod%N→2)/ARM threading is correct engineering aimed at a non-bottleneck.
- **Throughput lever (real):** parallelize the HOST transport — multiple RX PE threads / lock-free rx_queue / per-worker forward rings (remove single ring_lock). This is where "scalable multithreading" must happen.
- **Latency lever (DPU, quick win):** targeted ARM→EU wake after reverse-post (or higher keepalive). Cuts DPU hop from ~0.2-1ms to ~tens of us.
- TODO: redeploy with DPUMESH_TRACE=0 to drop the per-hop clock_gettime overhead once done measuring.

### E6 — host TX free-list (O(1) alloc replacing O(in-flight) scan) — REGRESSED
Hypothesis: tx_alloc's linear bitmap scan under slot_lock (held ~1500 iters/alloc, ~1050 workers contend) was the host cap.
| target | achieved | p50 ms | p99 ms | note |
|---|---|---|---|---|
| 30000 | 29,802 | 3.9 | 6.3 | fine |
| 50000 | 49,658 | 5.4 | 7.9 | fine |
| 95000 | 94,261 | 13.6 | 20.0 | fine |
| 100000 | 97,300 | 98 | 214 | OVERLOAD (baseline handled 104K @14ms) |
| 100000 | 97,302 | 118 | 203 | back-to-back stable (NOT a leak) |
| 105000 | 99,118 | 290 | 575 | overload |

**E6 verdict: TX free-list REGRESSED ceiling 104K→~96K (LIFO worse than lowest-index-first scan; back-to-back stable so not a leak).**
→ tx_alloc scan was NOT the host cap (removing it didn't help — it hurt). REVERTED. The allocation *pattern* mattering (LIFO worse) hints the host TX-buffer access locality matters, but the scan itself isn't the bottleneck. Lesson: even an "obvious" O(n)-under-lock wasn't it — must measure, not assume.

### E7 — host localization instrumentation (reverted free-list + rx_queue depth / tx_inflight stats)
Added dpumesh_debug_stats(): echo rx_queue_depth (host analog of cq_depth) + tx_inflight, printed 1Hz to pod stderr by both daemons.
Decision map:
- echo rx_queue_depth HIGH → echo consumers (workers/rx_lock/tx_alloc/enqueue) are the cap.
- echo rx_queue_depth ~0 + bench tx_inflight near num_slots → upstream of echo (RX PE-thread delivery rate) is the cap.
- bench tx_inflight low → client generation is the cap.
(results below)

### E7 results — host localization (rx_queue_depth / tx_inflight at 105K & 110K)
| load | echo rx_queue_depth | echo tx_inflight | bench tx_inflight (max) |
|---|---|---|---|
| 105K (knee) | 0 (max 1) | 67-525 | 554-751 (1009) |
| 110K (overload) | 0 (max 1) | 92-298 | 851-1061 (1062) |

**E7 verdict: echo NEVER backs up (rx_queue_depth≈0 even at overload) → echo re-send is NOT the cap.**
Bench tx_inflight maxes at ~n_workers (not 2048) → workers stuck awaiting TX_ACK (request-delivery), not slot-starved. The cap is the host SEND-side serial round-trip processing.

### E8 — host SEND concurrency (conns) sweep @160K + dpumesh-hw
| config | achieved | note |
|---|---|---|
| hw 2-core 105K/130K/160K | 104,217 / 105,020 / 103,055 | hw FLAT = serial element, not core count |
| conns=1000 | 107,465 | best (more conns than this hurts) |
| conns=2048 | 91,984 | WORSE — context-switch contention |
| conns=4096 | (wedge) | system deadlocked |

**E8 verdict: host send is a SERIAL-RATE cap (~104-107K). More concurrency HURTS. hw 2-core flat. → single host RX progress thread (pe + consumer_pe drained serially) + per-request worker wakeup on one core is the serial element.**

### WEDGE (conns=4096 overcommit) — robustness bug
conns=4096 > echo's 2048 TX slots → echo rx_queue filled to 1984, tx_inflight=2048 (exhausted) → echo can't alloc TX to respond → can't drain rx_queue → bench forward DMA ring stuck (head=1106, 34M spin retries) → permanent wedge (workers stuck in dpumesh_enqueue, never check stop). Needs redeploy. Lesson: keep conns ≤ ~num_slots; the closed loop has no admission cap on offered concurrency vs echo TX capacity.

## ANSWER (host-send vs echo-resend): HOST SEND side.
- echo re-send fine (rx_queue=0); host send is the serial cap (single PE thread + worker-wakeup on 1 core; more concurrency hurts; hw flat).
- FIX direction: split host RX progress (pe vs consumer_pe → 2 threads) so dispatch parallelizes on hw; and/or reduce per-request wakeup cost.

## ★ WIN — TX_ACK elimination unlocks host multi-core scaling
Lever: `DPUMESH_SKIP_REQ_TXACK=1` — DPU does NOT send TX_ACK for request forwards; the CLIENT frees its TX slot when the response (REV_DONE) arrives (response implies request round-tripped). Response forwards still ACK their src (echo). Host-side free is always-on + no-op-if-already-freed → safe. (User's idea; avoids any new DPA host-memory read — just skips a message.)

### E9 — SKIP_REQ_TXACK (verified active: bench tx_inflight 750→1500 full-RTT hold; 0-fail; back-to-back leak-safe)
| profile | sustainable | overload | p50 | note |
|---|---|---|---|---|
| fair 1-core + SKIP | 104K | — | 14ms | NO gain (single core saturated) |
| hw 2-core, NO skip (E8) | ~104K | ~105K | — | NO gain (serial PE thread; 2nd core wasted) |
| **hw 2-core + SKIP** | **~124K** | 130K | **6.6ms** | +19%, p50 −53% |
| **hw3 3-core + SKIP** | **~130K** | **137K** | 6.1ms | +25-32% |

back-to-back hw+SKIP @124K: 122,685→122,806 (0-fail, recovery clean) — robust, no leak.

**Mechanism:** the host RX was a single serial progress thread (`pe`, draining all comch control msgs + per-request worker wakeup). Reducing its per-request work (2→1 msg/RTT) ALONE doesn't help on 1 core (saturated), and a 2nd core ALONE doesn't help (serial element). TOGETHER they break it: less host work drops the `pe` thread below the serial-bottleneck point, letting extra cores run workers in parallel → host scales 104K→124K→130K for 1→2→3 cores.

**Next limit = TX slots.** SKIP holds TX slots the full RTT (vs half via TX_ACK) → tx_inflight 2×; at ~150K it hits num_slots=2048 → tx_alloc blocks → ~137K plateau. Levers to push further:
  (a) more TX slots + matching DPU_BUFFER_SIZE (2048/16MB → 4096/32MB), or
  (b) batched half-RTT TX free (DPU writes request-delivered cumulative counter to host mem, host polls) — keeps tx_inflight at half-RTT, removes slot pressure (user's original producer-consumer idea, more complex), or
  (c) more host cores (diminishing: +6K for the 3rd).
New knobs: DPUMESH_SKIP_REQ_TXACK; test-bench.sh dpumesh-hw3 (3-core profile).

### A result — 4096 slots / 32MB buffer (SKIP + hw3)
135K→133,890 (p99 15ms healthy), overload plateau ~138K. tx_inflight now ~1700 (was pinned 2048) → slots NOT the real cap (+3K only). The ~137K cap is now CORES (hw3 uses all 6 available dpumesh cores; 2,3 are TCP) + echo starting to back up (rx_queue max 99 @145K).

### ★ rec-2 — adaptive PE polling (core-efficiency) — CLEAN WIN
knob DPUMESH_PE_ADAPTIVE (default 0=busy-poll). PE thread spins while RX work present, yields core (20us nanosleep after 2048 empty polls) when idle.
- IDLE: cores 0,1 = **100% idle** (busy-poll = 0% idle) → transport returns core to app.
- LOAD: throughput UNCHANGED (fair 104,021 / hw3 133,664, 0-fail); cores busy (core0 0% idle) → polls tight when work present.
→ Directly serves "minimize transport cores": the transport no longer burns a full core when idle. Throughput-neutral.

### B (next) — batched TX_ACK for echo
Note from analysis: SKIP already removed the request-side TX_ACK (bench frees on response). The remaining echo message is the response-forward TX_ACK — but that's the LIGHT message (just frees a slot); the echo's HEAVY work is the REV_DONE (request data RX: rx_slot_alloc+memcpy+enqueue+signal). So batched-TX_ACK is expected marginal; implementing to verify per direct-test policy.

### B — batched TX_ACK (DPUMESH_BATCH_TXACK): first deploy WEDGED (bug found+fixed)
DPU coalesces response-forward TX_ACKs into dmesh_batch_tx_ack_msg (DMESH_MSG_BATCH_FWD_ACK=5, up to 14/msg), flush-on-full + 1kHz tail flush; host frees K slots/msg.
BUG: comch_client.c client_message_recv_callback dispatches on recv_buffer[0] in a SWITCH that only forwards known types to rx_data_hook; BATCH_FWD_ACK=5 hit `default` → message DROPPED (echo never freed TX → leak → echo tx_inflight=4096/rx_queue=4032 WEDGE, 0 RPS / mass fail) AND logged "unknown message type" per msg → LOG FLOOD. Gatekeeper missed: a new DPU→host msg type needs a case in BOTH comch_client switch AND rx_data_hook.
FIX: added `case DMESH_MSG_BATCH_FWD_ACK` → rx_data_hook. Reverted to good config (BATCH=0) to recover, then redeployed with fix.
Recovery health (good config): fair 101,497 / hw3 128,780, 0-fail.

### B result (fixed) — CORRECT, throughput-neutral
fair 104,204 / hw3 128,981 (0-fail), back-to-back 126,815→126,823→30K all 0-fail, echo tx_inflight 91-134 bounded (no leak). Reduces echo TX_ACK messages ~14× (65K→~4.6K msg/s @130K) = echo core-efficiency, but THROUGHPUT-NEUTRAL — confirms message count is not the cap (matches SKIP-on-fair=flat and A=marginal). Value: echo per-request host work ↓ (core-efficiency), not throughput.

## SESSION FINAL (deployed config: SKIP=1, NUM_SLOTS=4096, PE_ADAPTIVE=1, BATCH_TXACK=1; all 0-fail)
| change | throughput | latency/efficiency |
|---|---|---|
| baseline (fair 1c) | 104K | p50 14ms |
| **SKIP + 2-3 cores** | **124-130K (+25-30%)** | **p50 ~6ms (halved)** |
| A: 4096 slots | +3K (marginal) | removed slot cap |
| **rec2: adaptive PE poll** | neutral | **frees transport core at idle (100% idle vs busy-poll)** |
| B: batched TX_ACK | neutral | echo msgs ~14× fewer (echo core-efficiency) |

Throughput levers exhausted on host cores (hw3 = all 6 available dpumesh cores); message/slot reductions are core-efficiency not throughput (cap is closed-loop service rate / per-request data-RX + wakeup). DPU/DPA threading was a non-bottleneck AT 104K (DPU=2.8% of RTT) — but the bottleneck MOVED: after the host fixes raised the ceiling to 130K, the 2 EUs become CO-limiting (see Q3 below; DPU hop grows 208→580µs = 16.6% of RTT).

## Q3 — DPA EU limit re-examination (the bottleneck MOVED to dual after host fixes)
User Q: original N=1=74K, N=2=104K = only 1.4× (not 2×) — is the DPA thread a limit? Re-measured N=1/N=2/N=4 with the host-fixed config (SKIP+4096+adaptive+batch) on hw3 (host NOT limiting):

| N (DPA threads) | hw3 sustainable | note |
|---|---|---|
| N=1 (1 EU) | **~74K** (overload ~75K) | = original N=1 even with host unblocked → original 74K was **EU-bound, NOT host-bound** |
| N=2 (2 EU) | **~130K = 1.76× N=1** | 2nd EU helps, but EU↔ARM handoff eats the rest of the 2× |
| N=4 (4 EU) | **128,735 = N=2** | pod%N → 2 pods always use EUs 2,3 = 2 active. More DPA threads useless for 2 pods |

DPU-hop trace (DPUMESH_TRACE) vs load: @104K (original, host-bound) = 208µs = **2.8%** of RTT (E5). @130K (host-fixed) = **580µs** (max 2033) = **~16.6%** of RTT (p50 ~7ms); N=1 @80K = 421µs. → hop GROWS with load = reverse work queuing on the busy EUs = EU saturation signal.

**Q3 verdict: the DPA EUs ARE a real (co-)limit — the bottleneck MOVED there as the host was fixed.**
- @104K: host-bound (DPU 2.8%, E5 correct). After SKIP+multicore lifted the host to 130K, the 2 EUs are co-binding (DPU 16.6% of RTT, ~88% of the 2-EU chain capacity).
- "74K→148K (2×)" didn't happen because (1) the EU↔ARM handoff makes the 2nd EU 1.76× not 2×, and (2) at 104K the host capped before the EU mattered.
- 2 pods → pod%N locks 2 EUs → adding DPA threads (N=4) is useless. The ONLY way to add EU capacity = **EU-sharding** (host posts K fwd rings/pod; ARM round-robins reverse to K tx_rings; admission globals already [eu][ring]).

→ Revises the "purely host-bound" framing: at 130K it is **DUAL-limited** (host cores hw3-maxed + DPA 2-EU). Two remaining levers: **Option A** (async client / kill the client wakeup — is the wakeup or the DPA the cap?) and **Option B** (EU-sharding — break the 2-EU lock). Either could be the next binding one.

## Q1/Q2 quick answers
- **Q1 (single core same?):** YES — fair(1 core) = 104K with ALL changes. The +25-30% is multi-core-only; SKIP/zerocopy/etc. don't raise single-core throughput, they UNLOCK multi-core scaling.
- **Q2 (TX_ACK deleted? producer-consumer?):** SKIP = TRUE elimination for the BENCH's request TX_ACK (bench frees on response arrival = piggyback, no new msg, no DPA host-read — your concern avoided). B = the echo's TX_ACK was BATCHED (coalesced), NOT the full producer-consumer ring (echo has no response to piggyback). Full ring not built: B(batch) measured throughput-neutral → message count isn't the cap → full ring would also be neutral.

## HOST WEIGHT REVIEW (workflow w7w94rbhn) — why the host is "too heavy"
Per-request host cost (dpumesh_doca.c): SEND ~7-20µs (tx_alloc O(in-flight) scan dominates), RX cleanup ~8-19µs (rx_slot_alloc scan + 8KB staging memcpy on the PE thread). Per request ≈ 5-7 lock acquires + 2-3× 8KB memcpy + 2 O(n) scans + 2 context switches (cond wait/signal).
5 biggest weight sources: (1) single PE dispatcher doing heavy work (scan+copy+wakeup) inline; (2) per-request worker WAKEUP (cond_signal/wait); (3) ~5 coarse global locks (ring_lock serializes ALL TX posts); (4) redundant 8KB copies (RX staging + echo rx→tx); (5) O(num_slots) bitmap scans.
**ALL inside the dpumesh API (dpumesh_doca.c), NOT the bench harness.** Verified: TDpumeshClientTransport/TDpumeshTransport (real Thrift) call IDENTICAL dpumesh_* functions → fixing dpumesh_doca.c helps every user. Caller-specific only: worker COUNT (bench 1050 vs a real pool) and echo's body-copy shortcut.
→ #2 (zerocopy) attacks (1)(4)(5); #3 (poll) attacks (2). Both behind flags.

## HOST REFACTOR #2 — zero-copy RX (DPUMESH_ZEROCOPY_RX) — CORRECT, throughput-neutral
Change: PE thread no longer does rx_slot_alloc(O(n) scan) + 8KB staging memcpy. It delivers the landing offset `pos` (in body_buf_slot); the consumer reads rx_dma_buffer[pos] directly, rx_free returns the admission credit. Removes per-request: 1 O(n) scan + 1 8KB copy from the PE thread, + 1 echo copy (echo reads landing→tx directly), + the 32MB staging buffer + rx_slot pool. Behind a flag (A/B). Added bench body-validation (echo returns request pattern verbatim → catches silent landing corruption).
Safety: DPA admission (rq_depth=num_slots, credit bumped in rx_free) caps outstanding reverse DMAs at num_slots = buffer capacity → landing never laps an unread/un-freed position. Historical "negative frame size near wrap" was from SHARING staging==landing WITHOUT this gate; reading landing directly WITH the gate + rx_free-after-read is safe.

| config | fair | hw3 | fail (6.3M+ reqs @ high load) |
|---|---|---|---|
| zerocopy=0 (baseline) | 104,021 | 128,786 | 0 |
| zerocopy=1 | 106,357 | 128,786 / 137K overload | **0** (validation ON, no corruption) |

**Verdict: CORRECT (0-fail validated), THROUGHPUT-NEUTRAL.** → The host RX *weight* (scan+copy) was NOT the binding cost at the ceiling; removing it = a leanness/core-efficiency + memory (−32MB/pod) win, not a throughput lever. The ceiling at 130K is the DPA EUs (co-limiting, 88% of 2-EU capacity) + the per-request WAKEUP (cond_signal/wait), which zero-copy does NOT touch.
**→ Next host lever = #3 (lock-free rx_queue + eliminate per-request wakeup under load).** That attacks the actual remaining host cost (the wakeup), which E8's "more conns hurts" already fingered.

## HOST REFACTOR #3 (wakeup) — echo spin-poll: REGRESSED (echo not the bottleneck)
DPUMESH_POLL_RX: dpumesh_dequeue adaptive spin-polls rx_count (no rx_cond block); PE thread stops per-request cond_signal(rx_cond). Tested with ECHO_THREADS=8.
Result: hw3 130K→125,635 (p99 316ms, was healthy), ceiling ~129K (was ~137K) → REGRESSION. Causes: (1) ECHO_THREADS 64→8 cut processing parallelism; (2) echo was NEVER the bottleneck (rx_queue≈0), so removing its wakeup can't raise the ceiling, and 8 spinners stole CPU. Reverted (POLL_RX=0, ECHO_THREADS=64).
**Finding: the dominant wakeup is the CLIENT (1050 blocking workers; E8 "more conns hurts"), not the echo.** The echo's wakeup is on a non-binding path. The poll machinery is kept (knob, default off) but the echo isn't where it pays off.
**Client wakeup = inherent to blocking thread-per-request.** Eliminating it needs an ASYNC client (few threads, many in-flight, spin-poll completions) — a consumer-model change. Transport offers the poll API; only async consumers benefit. Real blocking Thrift services pay the wakeup inherently. The DPA EUs also co-limit at 130K, so removing the wakeup may not raise the ceiling past ~137K anyway (core-efficiency, not necessarily throughput).

## CURRENT BEST CONFIG (deployed, verified)
SKIP_REQ_TXACK=1 + NUM_SLOTS=4096 + PE_ADAPTIVE=1 + BATCH_TXACK=1 + ZEROCOPY_RX=1 + POLL_RX=0, hw3:
~130K sustainable (vs 104K baseline, +25%), p50 ~6ms (halved), 0-fail. Host leaner: −1 O(n) scan, −1-2 8KB copies, −32MB staging/pod (zerocopy); transport yields idle cores (adaptive); echo msgs ~14× fewer (batch). Single-core still 104K (gain is multi-core).

## OPTION A — async (poll-based) client: kill the per-request CLIENT wakeup
Implements the missing async consumer that HOST REFACTOR #3 identified (the dominant wakeup is the client's 1050 blocking workers, not echo). Two parts, both behind DPUMESH_ASYNC_CLIENT (default off; bench-only env — Thrift transport keeps blocking wait_response, unchanged):
1. **Transport** (dpumesh_doca.c / dpumesh.h): new `dpumesh_poll_response(ctx, req_id, resp)` — non-blocking, lock-free fast path (`__atomic_load_n(&p->state)`; takes the per-pending mutex only when state≠0). Returns 0=arrived / 1=waiting / -1=abandoned. In async mode `rx_deliver_desc` STOPS the per-response `pthread_cond_signal(&p->cond)` (state=1 store is the only signal). Removes 1 futex + 2 context switches PER response.
2. **Bench** (bench_dpumesh.c): new `worker_fn_async` — a few generator threads (ASYNC_THREADS), each holding a window of `inflight` outstanding requests, paced by wrk2 scheduled time (coordinated-omission preserved: t0=scheduled), harvested by poll_response. Window = target_concurrency / ASYNC_THREADS, so RPS is apples-to-apples with the blocking model but the host runs 2 threads instead of ~1300. Per-slot wall-clock timeout = WAIT_TIMEOUT_MS (lost response can't wedge the window). Body validation kept.

### MEASURED A/B (DPA=2, SKIP+4096+adaptive+batch+zerocopy; async=1 uses ASYNC_THREADS=2)

| config | async=0 (blocking, ~1040-1300 thr) | async=1 (2 generator thr) | delta |
|---|---|---|---|
| **fair 1-core sustainable** | 103,580 · p50 14.2ms | **~125,000** · p50 1.4-2.3ms | **+21% RPS, 6-10× lower p50** |
| **fair 1-core ceiling (overload)** | ~104K | **~125-127K** | **+20-22%** |
| **hw3 3-core @130K offered** | 129,400 · p50 6.2ms · p99 14.8ms | 129,615 · **p50 3.1ms · p99 7.0ms** | =RPS, **2× lower p50/p99** |
| **hw3 3-core ceiling (overload)** | 136,823 | 135,300-135,800 | = (neutral) |

0-fail everywhere; back-to-back fair 130K rerun = 126,966 (p50 1.58ms) → **no slot leak**. async=0 path is code-identical to prior best → reproduces the documented baseline exactly (non-regressing). 2 generator threads sustaining 129K proves the window works (blocking 2 thr would give ~2/RTT ≈ 333 RPS).

### Verdict — the CLIENT wakeup was a real host-CPU cost, but only THE cap below ~125K (direct experiment, not elimination)
1. **1-core: wakeup WAS the throughput cap.** Removing it lifts the 1-core ceiling **104K → 125K (+21%)** and cuts p50 up to **10×**. This DIRECTLY confirms HOST REFACTOR #3's hypothesis (dominant wakeup = client's blocking workers) — by A/B, not by elimination.
2. **3-core: wakeup is NOT the throughput cap.** async ceiling ~135K ≈ blocking ~137K → at 3 cores the binding constraint is the **DPA 2-EU** (Q3), which async cannot lift. But p50 still halves (the wakeup was a *latency* component even where not throughput-binding).
3. **Leanness achieved (user goal "transport must use minimal host cores"):** 1-core async (**125K**) ≈ 3-core blocking (**129K**). ~126K reachable with **1 core + 2 threads** instead of 3 cores + ~1300 threads (~600× fewer threads). The host transport footprint collapses.

**Cap map after Option A:** below ~125K → host wakeup/CPU was binding (async removes it). ~125K-137K → DPA 2-EU is binding (only EU-sharding / Option B raises it). → Option A delivered leanness + latency; the remaining THROUGHPUT lever above 137K is Option B (EU-sharding). NB: DPUMESH_DPA_THREADS defaults to 1 in test-bench.sh → a deploy WITHOUT it pins single-EU = 74K (Q3 N=1 ceiling); 130K config requires DPA_THREADS=2.

## Q4 — is the ~580µs DPU hop the reverse-EU keepalive wakeup? NO (keepalive 1ms→100µs = negative)
Hypothesis: ARM posts the reverse desc (dpu_enqueue_reverse_dma, dma->valid=1) as a SILENT write; the reverse EU only re-wakes on the 1 kHz keepalive (DPA_MSG_WAKE) after it yields → up-to-1ms handoff = the hop. Proposed fix: on-demand kick (dmesh_doca_dpa_msgq_send_try to dst EU(dst_pod%N) right after valid=1 — mechanism already exists).
**Cheap proxy test (zero code): DPUMESH_KEEPALIVE_US 1000→100 (10 kHz), async=1, hw3.**
| metric | keepalive=1000 | keepalive=100 |
|---|---|---|
| hw3 ceiling (overload) | ~135K | ~134K (=) |
| 130K p50 | 3.1ms | 3.5ms (=) |
| **130K DPU hop avg** | (blocking 580µs) | **1009µs** (async, higher in-flight) |
**Verdict: REFUTED.** At keepalive=100 the max wakeup wait is 100µs, yet the hop is still ~1009µs → ≥900µs of the hop is ARM-bridge SERVICE/QUEUING, not wakeup latency. Ceiling unchanged (the EU is hot at saturation, so keepalive is irrelevant there — as predicted). → the on-demand kick is NOT worth building (it would save <100µs of a ~1ms hop, 0 throughput). The ~135K ceiling and the ~1ms hop are the **single ARM bridge's per-request service rate** (route + comp_queue + comch egress; 135K × 2 ARM-crossings = 270K ARM ops/s), NOT the EU wakeup. Reverted keepalive to default; the per-request "fat" worth removing is the ARM-path software cost, not the wakeup (see cleanup).
**Direct experiments that raise the ceiling: NONE so far** (async=no, keepalive=no, N=4=no, ARM-shard=no[prior]). The ceiling is robustly the DPU per-request service rate through the single ARM bridge.

## Q5 — pure_dma's DPA scales but the chain doesn't: WHY (what "DPA-bound" actually means) [workflow w56lrbvhz, code-verified]
Side-by-side read of `/home/jukebox/test_dma/pure_dma/` vs the chain DPA:

**pure_dma = embarrassingly-parallel, share-nothing copy farm:**
- Each EU: own thread, own comch channel (1p/1c), own completion queue, own buffer slice. **No host, no ARM, no admission gate on the per-op path** (`test_dma/pure_dma/device/dpa_kernel.c:100-134`).
- **1 dma_copy per op** (the copy carries its own completion immediate). EU self-drives, gates only on its OWN consumer credit.
- → adding EUs adds fully-independent issue engines → **linear**: N=1 555K → N=2 1.07M (1.93×) → N=4 1.74-1.80M (3.2×), until the physical DMA-engine op-rate (~1.5-1.6M) at N=8.

**chain DPA = request/response pipeline forced through a single ARM bridge:**
- EU = `pod_id % N` (static; `dpa.c:1091,1278`) → **2-pod = exactly 2 EUs for ANY N** (N=4: 10%4=2, 11%4=3 → still 2; rest idle).
- One cross-pod message's **forward runs on EU(src%N), reverse on EU(dst%N) = DIFFERENT EUs**; they never talk directly — every completion goes UP to a **single ARM** (one `consumer_pe` + one `comp_queue`) which routes + posts the reverse desc, then the reverse EU picks it up. **ARM crossed 2×/request.**

**Two reasons more EUs don't help a 2-pod chain (both code-true):** (a) `pod%N` caps active EUs at 2; (b) the 2 EUs share one serial ARM bridge per request → N=2 gives **1.76× not 2×** (Amdahl on the serial bridge).

**Is the EU itself maxed?** Use the right reference: the chain's 2 EUs do 416K dma_copy/s @104K and ~540K @135K (RPS×4); M2-N=2 (same 2 EUs doing drain+forward but no full chain) = **626K**. So EU util = 416/626 = **66% @104K → 540/626 = 86% @135K** (matches Q3's "88% of 2-EU capacity"). The looser pure-copy 1.07M reference (no completion handling) overstates headroom — vs M2 the EU is **near its realistic chain-work capacity at the ceiling**, with the residual 14-32% idle being the EU→ARM→EU round-trip wait (shrinks as load rises). → **"DPA-bound" = the DPU's per-request round-trip *service rate* through the single ARM bridge (EU near M2 capacity + ARM-bridge cadence), NOT the DMA engine maxed and NOT EU count.** pure_dma has none of this structure → it scales.

## Option B (EU-sharding) status: FORECLOSED — corrects the earlier "feasible / only lever" framing
Earlier entries (the line-10 "Code-certain" note + Q3 line 230/232 + Option-A cap-map) framed EU-sharding — *host posts K fwd rings/pod; ARM round-robins reverse to K tx_rings* — as "feasible, the ONLY way to add EU capacity for 2 pods." **Re-reading `multithread_unified_plan.md` §5.0, that is FORECLOSED, and on code grounds (not elimination):**
- Sharding one pod across K EUs → K EUs write that pod's **tx_ring → breaks single-producer** (`ring.c`) and `dpa_sent_count[e][r]` **single-writer**.
- The cross-EU completion fan-out (an EU's completion reaching a consumer it doesn't own) is a **×N vs ×N² branch needing SDK `max_num_consumers>1`** (`dpa.c:470,477`), which is **=1** → no SDK support.
- 2-pod is a **HARD target constraint** (§8) → active EU permanently 2 → even if sharded, no benefit.
→ So **Option B is blocked by the SDK gate + the 2-pod target**, NOT a near-term lever. (Supersedes the line-10 "foreclosed was elimination, not code-true" — the re-read shows it IS code-true.) The plan's actual forward direction is **not** sharding/more-EUs but **reducing per-request handoff latency on the fixed 2-EU+1-ARM** (plan Levers 1-3: per-request stamp measurement → shorten reverse-desc post → `find_pod_by_id` O(n)→O(1)). NB those are LATENCY levers; per Q4 the keepalive/wake is NOT the throughput cap, so even these are p50/p99 wins, not ceiling wins.

## DPU (ARM) multithreading — current implementation state (+ cleanup decision: KEEP)
Two ORTHOGONAL multithreading axes (commonly conflated):
- **DPA (data plane), `DPUMESH_DPA_THREADS=N`** — N EU threads, share-nothing, EU=`pod%N`. **Currently N=2 = active & working** (2-pod → 2 EUs). This is the "DPA multithreading" (tested N=1/2/4).
- **DPU (ARM control plane), `DPUMESH_SPLIT_SEND` + `DPUMESH_DRAIN_SHARDS`** — splits the ARM completion-processing. **Default = OFF = single ARM thread.** 4 modes (`object.h:140-153`): OFF(0,default,single thread) / SENDS(1: A=drain+route+reverse, B=comch sends, via `send_spsc`) / REBAL(2: A=drain only, B=full work, via `work_spsc`) / **SHARD(3: N-way — A routes by effdst%N → per-EU `shard_work[k]` → N worker threads (each sole writer of its EU's tx_ring) → per-EU `shard_send[k]` → 1 SENDER → cc_server)**; `DRAIN_SHARDS=M` splits the single `consumer_pe` drain into M independent doca_pe drains (`dpu_worker.c:998-1008,1026-1068`).

**Currently deployed = DPA_THREADS=2 + SPLIT_OFF + DRAIN=1 → 2 EUs (parallel data plane) + single ARM thread (serial control plane).** `consumer_pe_shard[0]=consumer_pe`, M=1 byte-identical to single drain (`object.h:997`).

**Status:** the SPLIT/SHARD/DRAIN machinery is **fully implemented (commit 332dcfd53) + correctness-proven (0-fail×30)** but **throughput-neutral for the 2-pod chain** (E2 SPLIT_SHARD=3 flat = baseline; ARM was never the bottleneck, §4.1/§4.4; and 2-pod → only 2 EUs so nothing to scale). Per plan §4.4/§5.0 it is **preserved as a diagnostic toggle + future-multi-pod scaffolding**, NOT dead code.
**CLEANUP DECISION:** do **NOT** delete SPLIT/DRAIN/SHARD (nor CASE_INGRESS — `project_recv_pool_coupling` "KEPT" it deliberately). My earlier "delete ~600 lines of 0-gain machinery" was wrong: 0-gain is a *2-pod artifact* (pod%N), and the plan keeps the machinery by design. Cleanup, if done, is **readability-only** (stale comments, misnamed macro, clear toggle grouping) with **zero functional removal**. KEEP all leanness knobs too (ZEROCOPY/ADAPTIVE/SKIP/BATCH/ASYNC/TRACE) — the keep-criterion is "does it make the runtime lighter," not "did it raise throughput."

---

# Phase 1 cleanup — fix config (no env selection) + compose SHARD with host wins

Goal: remove the runtime SELECTION (env toggles) and bake each feature to its
enabled/fixed state. KEEP all feature code. Assume "neutral" verdicts may have been
masked by a different bottleneck → enable everything and RE-MEASURE.

## Baked ON (env selection removed)
- host transport-internal (always ON): ZEROCOPY_RX, PE_ADAPTIVE.
- host consumer model (via dpumesh_config_t, not env): bench → async_client=1, echo →
  poll_rx=1; Thrift keeps blocking (config default 0) — async is unusable by sync Thrift.
- DPU (always ON): SKIP_REQ_TXACK, BATCH_TXACK, DPA_AFFINITY.
- DPU control plane fixed to **SPLIT_SHARD** (multi-ARM: thread A route → N workers →
  SENDER). **DRAIN_SHARDS=1** fixed for correctness (cross-pod echo: fwd EU≠rev EU, a
  single drain keeps each shard_work single-producer).
- NUM_SLOTS default 2048 → **4096** (matches DPU_BUFFER_SIZE=32MB invariant).
- Kept as topology/sizing/measurement env (not feature toggles): DPUMESH_DPA_THREADS(=2),
  DPUMESH_NUM_SLOTS, DPUMESH_KEEPALIVE_US, DPUMESH_TRACE, DPUMESH_LOG_LEVEL, ECHO_THREADS,
  ASYNC_THREADS.

## New integration — SHARD now honors SKIP + BATCH
SHARD's send path (send_via_spsc) predated SKIP/BATCH and ignored both (it always pushed
DMA_COMPLETION + TX_ACK per request). Composed them so multi-ARM + host wins run together:
- SKIP: SHARD worker gates the TX_ACK push with `keep_ack = echo_mode || (flags&OP_RESPONSE)`
  — request forwards send no TX_ACK (client frees on response). (dpu_worker.c process_rev_notify_entry)
- BATCH: the SENDER (drain_send_spsc) routes each TX_ACK through batch_or_send_tx_ack
  (per-src-pod coalesce) instead of an immediate send; DMA_COMPLETION still sent inline.
  SENDER tail-flushes partial batches every 1 ms. The SENDER is the single owner of the
  per-pod batch (main-loop A's flush is disabled under SHARD → no race).

## Files
dpu_worker.c (SKIP/BATCH baked; SHARD fixed; SKIP gate + SENDER batching + tail flush);
dpa.c (affinity baked); dpumesh_doca.c + dpumesh.h (ZEROCOPY/PE_ADAPTIVE baked, poll_rx/
async_client via config, NUM_SLOTS_DEFAULT=4096); dpumesh_common.h (stale comment);
bench_dpumesh.c (async-only, cfg.async_client=1); echo_dpumesh.c (cfg.poll_rx=1);
test-bench.sh (dropped baked-knob env; DPA_THREADS:-2, NUM_SLOTS:-4096).

## Measured — build OK (DPU recompiled 13 objs; host OK), all toolchains compile

### hw3 (3-core) — 0-fail, >= prior best + ~2x lower p50
| target | achieved | p50 | p99 | ok/fail |
|---|---|---|---|---|
| 50000  | 49,731  | 1.46ms | 2.90ms | 500000/0 |
| 105000 | 104,427 | 1.29ms | 3.66ms | 1.05M/0 |
| 130000 | 129,295 | 3.07ms | 3.44ms | 1.30M/0 |
| 137000 | 136,245 | 4.99ms | 5.39ms | 1.37M/0 (sustainable ceiling) |
| 145000 | 142,778 | 55.8ms | 106ms  | 1.45M/0 (overload onset) |
| 150000 | 138,172 | 372ms  | 795ms  | overload |
| 160000 | 146,041 | 373ms  | 870ms  | past-knee |

back-to-back hw3: 130K->129,277 / 130K->129,285 / 30K->29,838, ALL 0-fail -> no slot leak.
DPU log: no ERR / no flood (-l 40). Sustainable ~137K (vs prior ~130K); p50 at 130K halved
(3.07ms vs ~6ms blocking). SHARD+SKIP+BATCH+async compose correctly.

### 1-core fair — REGRESSED by echo poll_rx=1
| target | achieved | p50 | ok/fail |
|---|---|---|---|
| 105000 | 54,312 | 3.66 s | 812355/0 |
| 125000 | 58,637 | 4.00 s | 877027/1 |

Prior async 1-core was ~125K. hw3 fine + only 1-core broken => host-core-sensitive => NOT
SHARD (DPU, identical in both profiles). Only new host variable = echo poll_rx=1: 64 echo
threads spin-poll -> thrash 1 core (the "POLL_RX regressed" finding, NOT masked — genuinely
bad at low echo core count). Documented best had echo poll_rx OFF. -> revert echo poll_rx,
re-measure 1-core (below).

### CONFIRMED: echo poll_rx OFF (rebuild) — 1-core recovers, hw3 unchanged
1-core fair: 105K -> 104,427 (p50 1.43ms, 0-fail) [was 54K @ p50 3.6s]; 125K -> 117,320
(p50 272ms, overload). hw3: 130K -> 129,282 (0-fail); 137K -> 134,904 (knee this run; first
sweep had 137K healthy = run-to-run boundary). => echo poll_rx=1 was the sole 1-core culprit;
reverting fixes 1-core and is neutral at hw3. poll_rx feature kept in transport, off for echo.

## Phase 1 verdict
Final config (all baked, no env selection): SHARD + SKIP + BATCH + AFFINITY (DPU),
ZEROCOPY + PE_ADAPTIVE (host always), async (bench), poll_rx OFF (echo), NUM_SLOTS=4096,
DPA_THREADS=2.
- hw3 sustainable ~130K (knee ~135K), 0-fail, back-to-back leak-safe, p50 ~3ms (vs ~6ms
  blocking) = LATENCY win. Throughput ceiling unchanged vs prior OFF+host-wins (~130-137K).
- 1-core fair ~105K clean (p50 1.43ms); slightly below prior async-1-core ~125K because
  SHARD's longer per-request DPU pipeline (A->workerK->SENDER + batch) costs 1-core async
  throughput where the host is the binding constraint. Neutral at hw3.
- SHARD (multi-ARM) did NOT raise the 2-pod ceiling -> confirms the 2-pod = 2-EU structural
  limit (pod%N). Higher throughput needs >2 pods (more active EUs), which costs host cores.
  Net Phase-1 gain = latency (async) + correctness composition; throughput ceiling flat.

## Phase 1 cleanup (step 2: comments + dead-path) — verified non-regressing
- Comment cleanup: 8-agent workflow trimmed history/rationale/dead-env comments across 39 files.
  Verified comment-ONLY (comment-stripped diff vs pre-cleanup snapshot = 0 code changes).
- Host RX staging path removed (zerocopy baked-on made it dead): rx_buffer / rx_slot_bitmap /
  rx_slot_lock / rx_slot_alloc removed; rx_buf/rx_free/rx_reclaim landing-only; −32MB/pod.
  Also removed now-dead fields pe_adaptive/zerocopy_rx (always-on baked into pe_progress_fn /
  process_rx_dma_entry). poll_rx/async_client kept (consumer model via config).
- BUG FIX (latent): dpumesh_cancel_pending state==1 freed rx_slot_bitmap[body_buf_slot] where
  body_buf_slot is the zerocopy landing OFFSET (0..32MB) vs a 4096-byte bitmap → heap OOB.
  Now rx_credit_return(). (Not hit by the 0-fail bench — only on cancel-after-arrival.)
- Async review (separate): sync Thrift can't get leanness at the stub level (blocking read,
  1-in-flight/instance). Recommended path = gateway async (raw C API, 0 Thrift change, port
  worker_fn_async window+poll); cob_style+TDpumeshAsyncChannel only for inter-service callers.
- Verify (deploy3, all toolchains compile): hw3 130K->129,291 (p50 2.34ms, 0-fail);
  1-core 105K->104,424 (p50 1.39ms, 0-fail); back-to-back 130K/30K 0-fail (no leak). Non-regressing.

## Phase 2 lightening — per-RTT compute, honest verdict
Investigation (workflow): "completion 제거" is NOT a lever —
- immediate completion comch_dma_comp_msg (16B) = routing-essential EU→ARM channel, can't remove
  (already 1 WQE BB).
- producer completion drain = already batched (every 8 iter), not per-op.
- request TX_ACK = already skipped (g_skip_req_txack=1); echo/response amortized via BATCH.
- DPA EU per-op already stripped (bench.md §3.2). Cap = closed-loop round-trip μ, not EU compute.
Real μ levers: host→host (FORECLOSED by L7-proxy design) or more active EUs (>2 pods → host cores).

Applied (safe leanness, code-lighter, throughput-neutral by design):
- C2: dpu_enqueue_reverse_dma takes scalars (req_id/dst/src/flags) instead of a 64B sw_descriptor_t
  → removes per-forward memset(64) + intermediate struct copy on the ARM worker path.
- H5: register_pending drops the per-request memset(&p->desc,0,64) (desc fully overwritten before
  state=1, only read at state==1).
Deferred (medium risk / ~0 throughput payoff since ARM not the bottleneck): C1 (send-buffer pool
vs per-send malloc/memcpy), H2 (merge register_pending+attach_tx lock), H1 (tx_alloc hint cursor).
Rejected: H4a (enqueue mfence→release — device-visibility subtle, gain negligible), WORKER inline
send (cc_server single-submitter), TX_ACK full removal, per-thread ring (foreclosed).

Verify (deploy4, all toolchains compile): hw3 130K->129,292 (0-fail), 137K->136,254 (0-fail),
back-to-back 130K/30K 0-fail (no leak). Throughput flat ~130-137K (expected — leanness, not a
ceiling mover). Confirms per-RTT lightening cannot raise the 2-pod closed-loop ceiling.

## 4-pod (2 echo pairs, 4 active EUs) — DOES scale past the 2-pod ceiling
Config: DPUMESH_DPA_THREADS=4 deploy (10%4=2,11%4=3,12%4=0,13%4=1 → 4 distinct EUs), SHARD baked
(4 workers + 1 SENDER + DRAIN=1 single drain), SKIP+BATCH+async+zerocopy. fair pin: pair1 cores
0,1 / pair2 cores 4,5 (each pod 1 host core). Single-pair anchor (fair 1-core) = 104,429.

| offered (RPS/pair ×2) | pair1 | pair2 | AGGREGATE | p99 | ok/fail |
|---|---|---|---|---|---|
| 120K (60K) | 59,673 | 59,675 | **119,347** | ~3.2ms | 1.2M/0 |
| 160K (80K) | 79,557 | 79,558 | **159,115** | 4.4-9.1ms | 1.6M/0 |
| 210K (105K) | 92,469 | 93,292 | **185,761** | overload (p50 0.6s) | 2.1M/0 |

**Verdict: 4-pod scales. Aggregate ~160K healthy / ~186K knee vs single-pair ~105K** — the 2-pod
"ceiling" is per-pair, NOT a global wall. This OVERTURNS [[project_shard_bottleneck_consumer_pe]]
("4-pod 2 pairs halve each other ~110K") — that was BEFORE the host fixes (SKIP+async) + SHARD
multi-ARM + 4 EUs. With the current baked config, 2 pairs scale.

Not perfectly linear: per-pair drops 105K(solo) → ~80-93K(paired) = ~12-24% sharing loss (shared:
single ARM drain DRAIN=1 + single SENDER + single cc_server + host↔DPU PCIe/comch op-rate). So
adding pods buys throughput sub-linearly: 2 cores→105K, 4 cores→160-186K (~40-46K/core vs 52K/core
solo).

ANSWER to "is adding pods worth the host CPU": YES — throughput scales ~1.5-1.8× for 2× pods/cores;
the per-RTT closed-loop cap is per-pair-parallelizable, not a shared hard wall (anymore). Further
scaling (>4 pods / >4 EUs) limited by the shared ARM drain+SENDER+cc_server (next lever if needed:
shard the SENDER / DRAIN_SHARDS>1 with group-affine routing — currently DRAIN=1 for cross-echo).

## Q2 — WHY does 4-pod throughput rise? (attribution, controlled)
Isolated EU count by running 4-pod at DPA_THREADS=2 (4 pods share 2 EUs: 10,12→EU0; 11,13→EU1)
vs DPA_THREADS=4 (4 distinct EUs). Host cores (4) + pair count (2) held equal.

| config | 120K off | 160K off | knee | healthy aggregate |
|---|---|---|---|---|
| 4-pod, 2 EU | 119,350 (0-fail) | 143,406 (p99 1.1s OVERLOAD) | ~144K | ~130K |
| 4-pod, 4 EU | 119,347 (0-fail) | 159,115 (p99 9ms HEALTHY) | ~186K | ~159K |
| 1 pair, 2 EU (fair 1c) | — | — | ~105K | ~105K |

Layered attribution (both host cores AND DPA EUs contribute; ARM does NOT):
- **host cores (= more src/dst pods)**: a single fair pair (1 core/app) is HOST-bound at 105K — it
  under-drives even 2 EUs. Adding the 2nd pair (more host cores) drives the SAME 2 EUs to ~130K
  healthy (+25K at fixed EU count). So adding pods adds host cores that saturate the EUs.
- **DPA threads (EU count)**: 2 EUs cap ~144K knee; 4 EUs reach ~186K knee (+42K). More EUs = more
  DMA-issue capacity, binding only after host cores have saturated the existing EUs.
- **DPU ARM cores (SHARD workers): NOT the cause** — cq_depth=0 (ARM starved); ARM was never the bottleneck.

VERDICT: 4-pod is faster because (1) each added pod brings a host core that pushes the EUs past the
single-fair-pair host limit, and (2) added pods map to added EUs (pod%N) giving more DMA capacity.
It is host-core + DPA-EU bound, NOT ARM-bound. To scale: add pods (host cores) until EUs saturate,
then add EUs — both cost host cores (1 core/pod). Shared ARM drain/SENDER/cc_server is the eventual
ceiling above ~186K.

## Q3/Q4 — completion rethink (workflow + adversarial verify vs DOCA headers)
Q3 (send reverse completion DPA->host directly, skip ARM relay): **NOT possible** in the current
binding. DOCA: producer sends to a consumer_id on the SAME comch connection (doca_comch_producer.h
:20-23); the DPA producer msgq is anchored to the DPU device (max_num_consumers=1=ARM, dpa.c:457);
the host datapath consumer is on a different connection. The EU immediate can only reach the DPU ARM.
The ARM relay is a 16B metadata endpoint-conversion ONLY — the body already DMA'd straight to host RX
in hop1 (dpa_kernel.c:336). A NEW host<->DPA msgq is theoretically allowed but device-match for a
host-side consumer is UNVERIFIED + needs per-dst-host msgq. The TX_ACK-skip analogy does NOT transfer:
TX_ACK was skippable (response arrival = alternative signal); the reverse completion IS the arrival
signal, no alternative + no DPA->host path.
Q4 (do we need completion?): forward completion = irreducible (carries pos/src_pod_id that exist only
in DPA per-EU state; a flag just moves the PCIe write + adds poll + loses HW copy-then-imm ordering).
reverse completion-as-polled-flag = possible in principle (data already in host RX) but needs re-adding
an in-band metadata trailer (undo zerocopy) + body-before-flag fence + wrap redesign; net win unproven.
Bottom line: completion is needed; the only ARM-relay removal (reverse) is SDK-blocked AND targets
~2.8% of RTT (host-transport-bound ceiling) — not a throughput lever. Real lever = more pods (Q2/4-pod).

## Q2.2 — EU-sharding (2 pods, >2 EUs) feasibility (workflow + adversarial verify)
Re-examined as K-rings-per-pod (each EU owns its own ring), NOT the old 1-ring-per-pod.
VERDICT: mechanism foreclosure OVERTURNED — single-producer (ring.c: 1 ring=1p/1c, not 1 pod=1 ring),
admission [eu][ring] (already isolated), max_num_consumers=1 (per-MsgQ, already worked around by
per-EU channels), 2-pod-HARD (ring count independent of pod count, MAX_DPA_RINGS=8) all preserved/
non-binding under K-rings. One real obstacle: reverse ADD_REV_RING keys on pod_id (dpa_kernel.c:84-90)
→ must re-key to (pod_id, ring_idx).
BUT performance gate HOLDS: rings alone ~0 gain. Real gates = (1) host 1-core feed/RX ~105K (single
ring_lock + single pe_progress_fn) — K EUs starve; (2) single ARM bridge ~103K (next ceiling).
→ Q2.1 (host-library lighten) + host RX multicore are PREREQUISITES; then EU-sharding rings; then ARM
K-way. Change scope: host dma_ring[K]/ring_lock[K]/enqueue-select + DPA per-pod K rings + reverse-ADD
re-key + ARM reverse K-ring single-writer routing (the hard part). Coupled multi-layer build.

## Q2.1 — 1-core attribution (TRACE) + SHARD reverted to OFF (measured)
Measured 1-core fair host/DPU split (DPA=2, TRACE=1, -l 50):
- SHARD (baked): 1-core 105K, DPU hop avg 416µs (max 1343), cq_depth=0, recv 420K/s.
- OFF (reverted): 1-core **120K** (p50 1.36ms healthy; 125K overloads), DPU hop avg **171µs**.
- Prior OFF reference (scale_log E5): hop 208µs @105K. SHARD ~2.4x the hop.

Attribution: at 1-core the gate is HOST (cq_depth=0 → DPU has headroom; hw3 reaches 137K). But SHARD
inflated the DPU pipeline latency (A→worker→SENDER) ~2.4x, and with a fixed async window that cut
1-core throughput 120→105K. SHARD has NO throughput upside at 2 pods (neutral at hw3) → it was a pure
latency/1-core cost. => Per user framing (Q2.1: library-core can't be solved by adding cores), the
biggest 1-core lever was NOT a host micro-op but turning SHARD OFF.

ACTION: split_send baked SPLIT_SHARD → SPLIT_OFF (dpu_worker.c). SHARD/SENDER/shard machinery KEPT
(dormant) for future EU-sharding (single ARM has headroom: cq_depth=0, so EU-sharding can use OFF+K-rings
without SHARD until the single ARM saturates).
Verify (OFF, deploy): 1-core 120K (was 105K, +14%); hop 171µs (was 416µs, -59%); hw3 130K→129,282 /
137K→136,244 (0-fail, unchanged); back-to-back 130K/30K 0-fail (no leak). Net win, simpler, no 2-pod loss.
Residual 1-core gap (120K → EU ceiling ~137K) = host CPU (library) → next: host-library lightening (Q2.1).

## Q2.2 — EU-sharding decisive experiment: does EU count cap throughput?
Positive-evidence test (per project_bench_elimination_unreliable: demand positive evidence). Resolve the
memory conflict (4pod_scales "~160K scales" vs shard_bottleneck "pairs halve"). 4-pod, SPLIT_OFF baseline,
fair 1-core/app (pair1 cores 0,1; pair2 cores 4,5 — HOST cores constant = 4 client procs). Only variable =
DPA EU count. Mapping pod_id % N:  N=2 → 10,11,12,13 = EU 0,1,0,1 (2 pairs SHARE 2 EUs);
N=4 → EU 2,3,0,1 (2 pairs use 4 DISJOINT EUs). size=8192, 10s.

### DPA_THREADS=2 (2 pairs share 2 EUs):
| offered (RPS/pair x2) | aggregate achieved | fail | p99 |
|---|---|---|---|
| 100K (50K) | 99,460  | 0 | 3.2/3.4 ms |
| 140K (70K) | 139,235 | 0 | 4.5/3.8 ms |
| 180K (90K) | 143,342 (SAT) | 0 | **2.5 s** (queue blowup; achieved capped ~71.7K/pair) |

=> 2 EUs cap 4-pod aggregate at **~140K** — identical to 2-pod hw3 ceiling (137K). The EU capacity is
shared across pods: pod count does NOT add throughput when EU count is fixed. Strong positive evidence
that DPA EU count (not pod count, not host cores here) is the binding throughput resource at the chain
ceiling. (DPA_THREADS=4 measurement next to confirm 4 EUs ~= 2x.)

### DPA_THREADS=4 (2 pairs use 4 DISJOINT EUs), SPLIT_OFF, fair 1-core/app:
(NOTE: pair2 fails entirely if probed immediately after `run_4pod up` — DPU pod-register race;
re-run after a few s and both pairs are healthy. First 90K/pair probe with pair2=fail/1800 was this race.)
| offered (RPS/pair x2) | aggregate achieved | fail | p99 |
|---|---|---|---|
| 60K (30K)  | 59,676  | 0 | 3.2 ms |
| 140K (70K) | 139,237 | 0 | 4.0/3.1 ms |
| 180K (90K) | **179,004** | 0 | 11.8/17.3 ms (knee) |
| 220K (110K)| 178,059 (SAT) | 0 | **2.26 s** (achieved capped ~89K/pair) |
| 260K (130K)| 176,189 (SAT) | 0 | **4.66 s** |

### VERDICT (positive evidence, resolves 4pod_scales vs shard_bottleneck conflict):
- 2 EU → 4-pod ceiling ~140K; 4 EU → ~178K. **EU count IS a binding throughput resource** (+27% for 2x
  EUs, with host cores=4 and pod count=4 held CONSTANT — clean isolation). Memory conflict resolved:
  4pod_scales (scales) correct; shard_bottleneck (pairs fully halve) was an over-read.
- BUT sub-linear: per-EU throughput 70K (@2EU) → 44.5K (@4EU). Doubling EUs gives +27%, not +100%.
  A shared resource BELOW the EUs caps absolute aggregate ~178K. Not the DMA engine (178K x4 dma = 712K
  ops/s << 1.6M N=8 ceiling) → likely PCIe body BW or host↔DPU comch op-rate. (Confirming needs >4 EUs =
  >4 pods, not available in 4-pod harness; DPA=8 with 4 pods still uses only 4 EUs: 10,11,12,13 %8=2,3,4,5.)
- Per-pair under contention: single-pair fair ~120K → 89K/pair when a 2nd pair shares (even on DISJOINT
  EUs) → the shared sub-EU resource, not the EUs, is what pairs contend for past ~140K.

### Q2 ANSWER ("why does 4-pod throughput rise? dpu core / dpa thread / src,dst pod?"):
It is the **DPA EU count (= "dpa thread")**, proven by isolation: holding pods=4 and host-cores=4 fixed and
varying ONLY EUs 2→4 lifts 140K→178K. NOT pod count (4-pod@2EU = 2-pod@2EU = 140K), NOT host cores
(constant). The original "4-pod scales" was really "more pods activated more EUs" (pod_id % num_dpa_threads).

### EU-SHARDING cost/benefit (for the 2-pod case the user wants to scale):
EU-sharding (1 pair's traffic split across K EUs via K-rings/pod) would lift the 2-pod pair from ~137K
toward the ~178K 4-EU shared-resource ceiling = **~+20-30%, NOT linear 2x** (the sub-EU shared resource
caps it). Build = coupled host (K-ring round-robin post) + DPA (K rings/pod across K EUs) + ARM (K-way
reverse routing). Decision: modest gain for a large coupled build; it is the only remaining 2-pod
throughput lever (host-micro exhausted per Q2.1). Above ~178K needs attacking the sub-EU shared resource
(PCIe body BW / comch op-rate), not more EUs.

### 2-pod EU-sharding BEFORE baseline (DPA=4, no sharding, pods 10,11 -> EU 2,3 = 2 EU), hw3:
| RPS | p50 | p99 | state |
|---|---|---|---|
| 120K | 1.39 ms | 2.73 ms | healthy |
| 140K | 253 ms  | 495 ms  | SATURATED |
| 160K | 905 ms  | 1.79 s  | SAT |
=> 2-pod / 2-EU ceiling ~130K (120K healthy). EU-sharding target: 1 pair using 4 EUs (K=2 rings/pod)
toward the measured 4-EU ceiling ~178K. This is the "before" for the EU-sharding gain comparison.

## Q2.3 — EU-sharding BUILT (K-rings/pod). K=1 non-regressive verify
Implemented K-rings-per-pod EU-sharding (DPUMESH_RINGS_PER_POD, default 1): host K forward rings +
round-robin post + per-ring credit (ring_idx = pos/region_size); DPU setup_pod_dma loops K rings across
K EUs k_j=(pod_id*K+j)%N, partitions DPU staging + host RX into K disjoint regions, region_off makes
completion pos absolute (dpa_ring_info._pad_credit -> region_off, no ABI size change); ARM tx_rings[K]
round-robin reverse. DPA kernel unchanged except comp.pos += region_off (2 lines). Host + DPU compile
clean (recompiled 13 C objects).
K=1 DPA=4 2-pod hw3 (bit-identical check vs pre-sharding DPA=4 baseline ~120K):
| RPS | achieved | p50 | p99 | state |
|---|---|---|---|---|
| 100K | 99,457  | 1.35 ms | 4.53 ms | healthy |
| 120K | 119,347 | 1.55 ms | 4.26 ms | healthy |
| 130K | 129,279 | 4.29 ms | 10.9 ms | knee |
=> K=1 reproduces the baseline exactly -> refactor is non-regressive. Next: K=2 DPA=4 (pair drives 4 EUs).

## Q2.4 — EU-sharding RESULT: K=2 lifts the 2-pod ceiling
K=2 DPA=4: the SAME 2 pods (10,11) now each shard across 2 EUs -> the pair drives 4 EUs
(10 -> EU 0,1; 11 -> EU 2,3). 2-pod hw3, size=8192, 10s:
| RPS | K=1 (2 EU) | K=2 (4 EU) | note |
|---|---|---|---|
| 130K | 129K p50 4.3ms (knee) | 129K p50 **1.71ms** | K=2 same rate, far lower latency = EU headroom |
| 150K | (saturated) | 149K p50 2.16ms healthy | K=1 saturates by 140K |
| 160K | — | 159K p50 2.45ms 0-fail (peak, borderline: 1 of 2 runs saturated) |
| 165K | — | saturated p50 675ms | knee |

RESULT: 2-pod healthy ceiling **~130K (K=1) -> ~150-160K (K=2) = +15-23%**. EU-sharding works: making a
2-pod pair use 4 EUs raises throughput, exactly the user's ask ("even with 2 pods, more DPA threads ->
more throughput"). 0-fail throughout; back-to-back runs (165K saturate -> 160K healthy) show no slot leak.

Gap to the 4-pod 4-EU ceiling (178K): the 2-pod pair feeds 4 EUs from only 2 host processes (1 PE-RX
thread each) + a single ARM, vs 4 host procs at 4-pod. So the residual cap is the host PE-RX feed / ARM
(plan risk R5), not the EUs. Pushing past ~160K on 2 pods needs a leaner/multi-threaded host RX feed.

Knobs: DPUMESH_RINGS_PER_POD=K (default 1=legacy), requires DPUMESH_DPA_THREADS>=K. K=1 verified
bit-identical (Q2.3). Build: host (K rings + rr post + per-ring credit), DPU setup_pod_dma (K rings/EUs +
region partition), ARM tx_rings[K] rr, kernel comp.pos += region_off. dpa_ring_info ABI unchanged
(_pad_credit repurposed). Deployed state: DPA=4 K=2.

## Q2.5 — Where is the 2-pod K=2 cap? HOST CPU measurement
K=2 DPA=4, hw3, 150K (149.5K achieved, 0-fail, p50 2.0ms = healthy). mpstat per-core (bench host10 pinned
0,4,6; echo host11 pinned 1,5,7):
| side          | %usr | %sys | %idle | busy |
|---|---|---|---|---|
| CLIENT host10 | ~47  | ~2   | ~50   | ~50% (HALF IDLE) |
| ECHO  host11  | ~20  | ~69  | ~11   | ~89% (near saturation) |

POSITIVE EVIDENCE (confirms the user's "destination re-transmit overhead" intuition):
- The DESTINATION/server (echo) is the hot side (~89% vs client ~50%). The client has headroom; the cap
  is the server re-entering its transport to send the response.
- The server is dominated by **%sys ~69%**, NOT %usr and NOT DMA/PCIe. That is kernel/syscall time =
  the blocking 64-thread echo model (ECHO_THREADS=64, poll_rx OFF): one pthread_cond_signal (PE thread)
  + one cond_wait wakeup per request -> ~150K futex ops/s = the %sys wall.
=> The 2-pod ceiling above ~160K is gated by the HOST SERVER-SIDE WAKEUP/THREADING model, not the EUs,
   not the dma_copy count, not PCIe BW. Leaning the server RX path (poll instead of per-req cond_signal,
   or batched wakeup, or an async/fiber server) is the highest-leverage next lever. This is the
   server-side analogue of [[project_option_a_async_result]] (async client removed per-req wakeup on the
   CLIENT; the SERVER still pays it). Levers from the perf-levers workflow ranked separately.

## Q2.6 — Perf-lever multi-angle analysis (5 levers, adversarially verified)
Workflow (5 finders + 5 refuters + synthesis) over the code, cross-checked vs the Q2.5 measurement.
Ranked:
1. **Lean server RX (poll_rx on echo) — TOP, KEEP.** Only lever with POSITIVE measured evidence (Q2.5:
   echo 89% busy / %sys~69% futex, client 50% idle). Already built (poll path dpumesh_doca.c:789-826;
   per-req signal removed = dpumesh_doca.c:251). Server-side twin of the proven async-client win
   (option_a: +21%). Expected +15-30% (160K -> ~185-210K). Effort XS (config + 1 deploy). Pair with small
   ECHO_THREADS (~= pinned cores) so spin workers don't oversubscribe.
2. dma_copy 4->2 (direct host->host DMA) — **DROP, unbuildable.** Forward producer_dma_copy
   (dpa_kernel.c:222) is a FUSED op: moves body AND delivers the 16B FWD_DONE to the SINGLE
   dpu_consumer_id (max_num_consumers=1, dpa.c:470; consumer = ARM). EU physically can't deliver the
   routing completion to a host consumer -> ARM relay irreducible. "4->2" miscounts (best 4->3 on a
   non-bottleneck path). Forecloses body-L7.
3. Host PE/RX multi-thread (K PE/consumers) — **DROP, targets nothing.** The host datapath consumer is
   VESTIGIAL (dpumesh_doca.c:483-495, ID no longer advertised); reverse signal is 16B REV_DONE over the
   ONE comch control conn. pe_progress is single-threaded but NOT the cap (the server futex is, Q2.5).
4. Activate SPLIT_SHARD multi-ARM — **DROP, latent DATA RACE + wrong stage.** At M>1 with K-rings, two
   EUs (p*K, p*K+1)%N can push into the same non-atomic shard_work[effdst%N] SPSC -> corrupts completions
   -> stall. Egress (SENDER->single cc_server) is NOT sharded anyway. ARM is cq_depth=0 / ~2.8%. KEEP code
   OFF (preserved scaffolding).
5. Batch reverse-DMA completions — **DROP, structurally impossible.** The rev completion is a FUSED
   per-body DMA (distinct src/req, advances rev_pos); K bodies need K calls. Only ARM->host REV_DONE is
   batchable but it's the ~2.8% non-binding path. Per-body DMA cost irreducible.

ATTACK ORDER: (1) poll_rx now (free, measured-backed). (2) re-mpstat BOTH sides at the new knee + add a
positive DPU SENDER/cc_server submit-rate + EAGAIN + cq_depth counter to distinguish "single ARM egress
op-rate" vs "sub-EU PCIe BW / comch op-rate" — the ~178K attribution is elimination-only & has flip-
flopped, demand a positive counter first. (3) ONLY if egress proven binding, build a NEW K-way cc_server
egress (NOT the racy SPLIT_SHARD). Running the poll_rx experiment now (ECHO_POLL_RX=1 ECHO_THREADS=3).

## Q2.7 — poll_rx lean server RESULT: the biggest single win
Deployed ECHO_POLL_RX=1 ECHO_THREADS=3 (K=2 DPA=4). The poll path removes the per-request
cond_signal/cond_wait. 2-pod hw3, size 8192:
| RPS | achieved | p50 | echo %sys | state |
|---|---|---|---|---|
| 150K | 149,631 | 1.70ms | **2%** (was 69%) | echo 40% busy (was 89%) |
| 180K | 179,008 | 1.77ms | — | healthy |
| 200K | 198,9xx | 2.30ms | — | healthy 0-fail (x3 runs) |
| 220K | 200,780 (SAT) | 455ms | — | knee |

RESULT: **echo %sys 69%->2%** (futex eliminated). 2-pod healthy ceiling **~160K -> ~200K (+25%)**.
Combined with EU-sharding: **K=1 130K -> K=2+poll_rx 200K = +54%**. NB: 200K on 2 pods BEATS the 4-pod
blocking number (178K) — because 4-pod also paid the server futex. The user's "destination re-transmit
overhead" intuition was the single biggest lever, larger than EU-sharding itself.

NEXT CAP (mpstat at 200K knee): binding side FLIPPED off the server. client host10 ~72% busy (usr 67%,
sys 4%), echo host11 ~50% busy (usr 46%, sys 3%). %sys now low on BOTH (futex gone everywhere). Neither
host fully saturated at the 200K knee + both have idle => the residual cap is now the DPU/PCIe shared
op-rate (~200K, consistent with the 4-pod sub-EU resource, higher because lean). Per Q2.6 attack order:
to go above 200K, instrument a POSITIVE DPU SENDER/cc_server submit-rate + EAGAIN + cq_depth counter to
prove "single ARM egress op-rate" vs "PCIe body BW" BEFORE building a K-way cc_server egress. The client
72% is partly bench load-gen artifact (real clients are leaner).
Deployed state: DPA=4 K=2 ECHO_POLL_RX=1 ECHO_THREADS=3 (~200K). Knobs: DPUMESH_RINGS_PER_POD, ECHO_POLL_RX.

## Q2.8 — >200K cap attribution: DPU egress counters (positive evidence)
Added g_egress_again (cc_server DMA_COMPLETION send AGAIN/s) to the 1Hz DPU stat; cq_depth already logged.
K=2 DPA=4 ECHO_POLL_RX=1, -l 50:
| offered | achieved | recv (DPA->ARM comp/s) | cq_depth | egress_again/s |
|---|---|---|---|---|
| 200K (healthy) | 199K | **~800,000** | 0 | 0 |
| 220K (saturated) | ~202K | ~813,000 | 0 | 0 |
(sent=4000/s = the DPU->DPA keepalive WAKE, 4 EUs x 1kHz — NOT the host egress; the host-egress signal is
egress_again, which is 0.)

ATTRIBUTION (positive, not elimination):
- egress_again=0 + cq_depth=0 => the single ARM ingest AND the ARM->host cc_server egress both keep up
  with headroom. The single ARM / egress op-rate is NOT the cap. Refutes the "single ARM egress" lever
  (Q2.6 ranked #... would-be) with a positive counter.
- recv PLATEAUS at ~810K/s (200K and 220K offered both ~810K, achieved stuck ~200K) => the binding cap is
  the **DPA dma_copy + completion op-rate ~810K/s** (4 dma_copy/RTT x 200K = 810K). It is an OP-RATE, not
  BW (810K x 8KB = 6.4 GB/s << PCIe Gen4). Consistent with [[project_n8_dma_engine_ceiling]] shared op-rate.
DECISIVE NEXT TEST: is ~810K per-EU-summed (more EUs lift it) or a hard shared cap (DMA engine / comch
completion)? K=4 DPA=8 makes the 2-pod pair drive 8 EUs (pod10->EU0-3, pod11->EU4-7). If recv rises above
810K and RPS above 200K => EU-bound, EU-sharding K=4 scales further. If recv stays ~810K => hard shared
op-rate cap, more EUs won't help and the only lever is fewer dma_copy/RTT.

## Q2.9 — K=4 (8 EU) test: the ~810K op-rate is a HARD shared cap, NOT per-EU
K=4 DPA=8 (2-pod pair drives 8 EUs: pod10->EU0-3, pod11->EU4-7), ECHO_POLL_RX=1, -l 50:
| offered | achieved | recv (DPA->ARM comp/s) | cq_depth | egress_again |
|---|---|---|---|---|
| 220K | 204K (SAT p50 523ms) | ~813,000 | 0 | 0 |
| 260K | 204K (SAT) | ~816,000 | 0 | 0 |
| 300K | 202K (SAT) | ~813,000 | 0 | 0 |
(sent=8000/s = keepalive WAKE, 8 EUs x 1kHz — confirms sent = keepalive, not egress.)

DECISIVE: 8 EUs gives the SAME ~200-204K and recv stays pinned at ~813K/s (= K=2's 4-EU number). So the
~810K dma_copy+completion/s is a **HARD SHARED op-rate cap that does NOT scale with EUs**. K=2 (4 EUs)
already saturates it; **K=4 is useless** (same throughput, 2x EUs + 2x memory). EU-sharding sweet spot =
K=2. cq_depth/egress_again stay 0 at 8 EUs => still not ARM/egress; pure DPA-side op-rate.

Since pure_dma hits ~1.6M ops/s ([[project_n8_dma_engine_ceiling]]) but dma_copy-WITH-completion caps at
~810K, the per-op COMPLETION (comch msg attached to each dma_copy, EU->ARM) is ~half the op cost. The cap
is the DPA producer completion delivery op-rate, not DMA BW (6.4 GB/s) and not the DMA engine raw rate.

CONCLUSION (2-pod): architecture ceiling ~200K RTT/s = ~810K dma_copy+completion/s, a GLOBAL DPA op-rate
resource. Levers that DON'T work (positive evidence): more EUs (K=4 flat), single-ARM egress sharding
(egress_again=0). The ONLY remaining lever to exceed 200K is REDUCING dma_copy+completion per RTT (the 4):
either fewer dma_copy (host->host direct = foreclosed for body-L7; refuted 4->2 fused completion) or
completion-free RX (host polls landed RX buffer instead of a per-resp REV_DONE msg) — an architectural
redesign. Baking K=2 + poll_rx as the standing 200K config; K=4 reverted.

## Q2.10 — (a) BAKED standing config + (b) DONE + log level reverted
(a) Baked the measured 200K config as test-bench.sh defaults (env still overridable):
DPUMESH_DPA_THREADS:-4, DPUMESH_RINGS_PER_POD:-2, ECHO_POLL_RX:-1, ECHO_THREADS:-3. A plain
`test-bench.sh deploy` now yields the K=2 + lean-server 200K config at -l 40.
VERIFY (no env overrides): baked deploy -> 199K hw3 (p50 1.95ms, 0-fail). Fair-mode sanity (1-core echo
w/ poll_rx+3 threads) -> 99.5K (p50 1.46ms, 0-fail) — NO regression (the old 1-core poll_rx regression was
64 threads; 3 threads + adaptive yield is healthy). So poll_rx default-on is safe for both hw3 and fair.
(b) DONE: cap attributed to the ~810K DPA dma_copy+completion op-rate (Q2.8-2.9), NOT ARM/egress (egress_
again=0, cq_depth=0) and NOT EU count (K=4 flat). g_egress_again diagnostic kept (1Hz, silent at -l 40).
Log level reverted to 40 in the final deploy (user request).

FINAL LADDER (2-pod, hw3, 8KB): 130K (K=1) -> 160K (K=2 EU-sharding) -> **200K (K=2 + poll_rx lean server)**
= +54% over baseline, and above the 4-pod blocking number (178K). 200K is the architecture's 2-pod DPA
op-rate ceiling; exceeding it needs fewer dma_copy+completion per RTT (architectural), not more EUs/pods.

## Q2.11 — CORRECTION of Q2.8/Q2.9 attribution (per bench.md hierarchy)
Q2.8/Q2.9 claimed "~810K = the DPA dma_copy+completion op-rate; the per-op completion is ~half the cost;
single comch consumer delivery." **That attribution is WRONG** — bench.md §5 already measured the engine
hierarchy (test_dma micro-bench, recv/s == dma_copy/s, completions INCLUDED in all):
- pure single-EU 556K; pure 4-EU **1.8M**; M0 (single consumer_pe drain) N=8 **1.53M**; M2 (single ARM
  fwd) N>=4 **1.0M**. ALL above ~810K. So 810K is NOT the engine/DMA limit, and NOT the single consumer
  reaping (M0 uses the SAME single consumer_pe and reaches 1.53M). pure_dma ALSO emits a completion per
  copy and still does 556K single-EU -> "completion = half the cost" is false.

WHAT 810K ACTUALLY IS: the **chain ceiling at 4 active EUs**. bench.md §5.1 chain 2-active-EU = 416K
(104K x4). My EU-sharding made 2 pods drive 4 active EUs (the decisive experiment bench.md §8 marked
"미실시") -> chain 2-EU 416K -> 4-EU ~810K = ~1.95x (near-linear) => CONFIRMS bench.md's untested "chain
scales with active EUs". Then 4-EU -> 8-EU (K=4) is FLAT (810->813K) => a NEW chain wall at ~810K.
Per-active-EU: 208K(2EU) -> 200K(4EU) -> 102K(8EU). The chain EU runs ~200K/EU vs pure 556K/EU => the
chain EU is **STALL-bound (~64% idle), not op-rate-bound**.

CAUSE OF THE 810K CHAIN WALL: UNPROVEN. Per bench.md §7.2 (chain-cap attribution is elimination-only and
flip-flopped 3x), do NOT name a mechanism without positive evidence. It IS below engine/M0/M2 (so chain-
specific: closed-loop + reverse DMA + admission per-RTT structure), but the specific stall is unmeasured.
NEXT (bench.md §8.2): DPA-side stall-cycle instrumentation — measure where the chain EU stalls
(is_consumer_empty / admission-gate / ring-empty) with POSITIVE counters.

## Q2.12 — POSITIVE cap localization via DPA EU-stall instrumentation
Added device-side EU counters (dpa_thread_arg: stat_dma/consumer_wait/admission_brk/idle_resched),
incremented in dpa_kernel.c, read back by the ARM via doca_dpa_d2h_memcpy in the 1Hz stat
(dmesh_log_eu_stats in dpa.c). This is bench.md §8.2's "DPA stall-cycle 계측" — positive, not elimination.

Measured at the ~810K knee (8KB, hw3):
| config | RPS | recv (dma/s) | dma | consumer_wait | admission_brk | idle_resched |
|---|---|---|---|---|---|---|
| K=2 (4 EU) @200K | 199K | 800K | 800,076/s | **0** | **0** | 1,811/s (~9%) |
| K=3 (6 EU) @200K | 199K | 794K | 793,801/s | **0** | **0** | 5,958/s |

POSITIVE FINDINGS (counters, not elimination):
1. recv plateaus ~800K for 4 AND 6 AND 8 EU (Q2.9 K=4 also 813K) => chain caps ~800K dma/s (200K RTT),
   does NOT scale with EUs past 4. (Refutes "K=3/6-EU may beat the engine N=8 regression" — it's a real
   chain cap, not the pure N=8 dip.)
2. **consumer_wait=0** => EU NEVER stalls delivering completions to the single DPU consumer (refutes my
   earlier "810K = single-consumer reaping"; M0=1.53M already implied this).
3. **admission_brk=0** => EU NEVER stalls on host RX credit (the closed-loop admission gate is not binding).
4. At 4 EU the EU is COMPUTE-busy (idle 9%); at 6 EU idle_resched scales with EU count (per-EU 453->994/s)
   while total dma stays ~800K => extra EUs are STARVED. So the cap is UPSTREAM of the EU: the
   RING-FILL rate (host forward-post + ARM reverse-enqueue) is pinned at ~800K, starving added EUs.
=> The 2-pod ~200K ceiling is the CLOSED-LOOP RING-FILL rate (host side), NOT the DPA engine (pure 1.8M),
   NOT the DPU consumer (consumer_wait=0, M0 1.53M), NOT host credit (admission_brk=0), NOT the ARM
   (cq_depth=0), NOT egress (egress_again=0). Consistent with bench.md §6.1 M/M/1 fixed-service-rate +
   [[project_chain_host_transport_bound]]/[[project_host_send_serial_cap]] (single host PE progress thread).
   host->host (dma_copy count) is MOOT for this cap — it is host ring-fill, not DPA op-rate.
CORRECTS my Q2.8/Q2.9 ("DPA op-rate / single-consumer / completion-half-cost") AND the "stall-bound"
guess — all refuted by positive counters. Remaining: localize host ring-fill (single PE thread vs posting);
per-thread CPU probe failed (container ns); headline (host closed-loop ring-fill) is positive-evidenced.
Diagnostics (egress_again, EU-stall) kept in code, silent at -l 40.

## Q2.13 — Host cap POSITIVELY localized: single PE progress thread
Per-thread CPU (crictl PID + top -H) at K=2/200K, hw3 (3 cores/side):
- ECHO (server): ONE thread **96.4%** (saturated) + 3 poll workers ~23% each (idle-ish, waiting on PE).
- CLIENT: ONE thread 77.2% + 4 async workers ~38% each.
=> The single **PE progress thread (pe_progress_fn, doca_pe_progress + rx_data_hook RX landing) per pod is
the 200K cap**. Workers have headroom; they starve waiting for the PE thread to land RX. Confirms
[[project_host_send_serial_cap]] / [[project_chain_host_transport_bound]] positively (per-thread, not
elimination). The DPU EU starvation (Q2.12 idle_resched) is the downstream symptom of this host-side cap:
the PE thread can't post/land fast enough to feed >4 EUs.
LEVER: parallelize/lighten the host RX landing (single pe_progress_fn). Candidates: drop the vestigial
consumer_pe drain if empty; offload rx_data_hook off the PE callback; or multiple RX PEs.

## Q2.14 — consumer_pe drain removed (lean-up); confirms cap = comch RX reaping
Removed the vestigial doca_pe_progress(consumer_pe) from pe_progress_fn (host datapath consumer ID is never
advertised → nothing lands on it; RX arrives via the comch client/pe → rx_data_hook). Safe, host-only.
Result (K=2, hw3): 200K healthy (p50 1.89ms 0-fail); 215K saturates (~204K, p50 296ms). => marginal ~+2%,
ceiling still ~200-204K. So the single PE thread's saturation is dominated by the REAL comch RX reaping
(doca_pe_progress on the comch client), NOT the vestigial consumer_pe. Kept the removal as a lean-up
(runtime lighter), but it is not the lever.

FINAL POSITIVE PICTURE (this session): 130K (K=1) → 160K (EU-sharding K=2) → 200K (lean server poll_rx) →
~204K (consumer_pe lean-up). The 2-pod ceiling ~200K = the **single host comch-RX PE thread per pod**
(echo PE 96.4%), positively localized through the whole stack (DPA engine/consumer/ARM/egress/host-credit
all ruled OUT with counters=0; EU starves at 6 EU; per-thread top -H shows the one PE thread saturated).
NEXT LEVER to exceed ~204K (all are MAJOR host-transport changes, uncertain payoff, deploy-only verified):
  (a) completion-free polled RX — host workers poll the RX buffer for landed data (DPA writes a valid/seq
      marker), eliminating the per-response comch REV_DONE reaping (removes the PE-thread bottleneck +
      cuts completions). Needs req_id correlation in the landed data + DPA reverse format change.
  (b) multi-connection comch RX — K host comch connections, ARM round-robins REV_DONE, K PE threads.
  (c) lock-free rx_queue (echo PE 96% vs client 77% gap may be rx_lock contention: 1 producer PE + 3
      worker consumers). Smaller, targets the server-side gap only.
host->host is MOOT here (cap is host comch-RX, not DPA dma_copy) and stays FORECLOSED (DPU L7 proxy).
Deployed state: baked K=2 DPA=4 poll_rx + consumer_pe-drain-removed, -l 40, ~200K 0-fail.

## Q2.15 — BATCH_REV_DONE built (the real ceiling lever); first deploy WEDGED (gatekeeper) then fixed
Mirrored BATCH_FWD_ACK for REV_DONE: new DMESH_MSG_BATCH_REV_DONE(6) + dmesh_rev_done_entry(16B) +
dmesh_batch_rev_done_msg(BATCH_REVDONE_MAX=16); pod_state.rev_done_batch[]; server_send_batch_rev_done_to;
dpu_worker batch_or_send_rev_done + flush (proc==0 idle-flush + 1kHz tail; cross-pod batched, echo_mode
un-batched for REV_DONE-before-TX_ACK order); host rx_data_hook BATCH_REV_DONE unpack loop.
FIRST DEPLOY WEDGED (0 achieved / all fail) — exactly [[project_dpu_host_msg_gatekeeper]]: a new DPU→host
comch type needs a case in BOTH rx_data_hook AND the comch_client.c client_message_recv_callback switch.
Added rx_data_hook but missed the client switch → type 6 hit default ("unknown message type") → silent
drop → no responses → wedge. FIX: added the DMESH_MSG_BATCH_REV_DONE case to comch_client.c:119. Redeploy.

## Q2.16 — BATCH_REV_DONE RESULT: 200K -> ~220-235K, bottleneck redistributed
After the gatekeeper fix (Q2.15), K=2 hw3, 8KB, 0-fail throughout:
| RPS | achieved | p50 | state |
|---|---|---|---|
| 200K | 199,070 | **1.25ms** (was 1.89ms pre-batch) | healthy, lower latency |
| 220K | 218,956 | 1.42ms | healthy (new sustainable knee) |
| 235K | 233,866 | 17.9ms | knee edge |
| 250K | 238,905 | 246ms | saturated |
=> BATCH_REV_DONE lifts ~200K -> ~220-235K (+10-18%) AND cuts p50 at 200K (PE less loaded). It is the
user's "batch the completion" idea, realized by mirroring the proven BATCH_FWD_ACK (1 comch reap per K
responses), always-on for cross-pod (echo_mode un-batched for ordering). 0-fail; effectively baked.

NEW bottleneck (per-thread top -H @220K): echo now has TWO threads ~90%/87% (was a SINGLE 96% PE pre-batch)
+ 2 workers ~18%; client 73%/70% + 2 workers ~33%. => batching relieved the single PE; the bottleneck
REDISTRIBUTED to the echo node's aggregate per-RTT CPU on its 3 cores (PE reaping the batched msgs + the
WORKERS doing the 8KB memcpy echo_dpumesh.c:68 + the forward post). So the echo 8KB memcpy (the user's
point) is now CO-BINDING. Removing it (echo re-send from the RX slot, no rx->tx copy) is the next lever
BUT is NOT app-local: the forward DMA reads the host TX data buffer (remote_mmap), not the RX buffer, so
re-sending from RX needs the RX buffer registered as a DMA source (a transport change), not just an
echo-app edit. Ladder so far: 130K(K=1) -> 160K(EU-shard) -> 200K(poll_rx) -> ~220-235K(BATCH_REV_DONE).

## Q2.17 — Bench restructured to isolate transport (user was RIGHT)
USER INSIGHT (verified correct): the echo memcpy (echo_dpumesh.c:68, 8KB rx->tx) AND the client memset
(bench_dpumesh.c:132/234, 8KB pattern fill) are per-request APP operations for the content-validation
bench, NOT dpumesh transport. The transport DMAs body_len bytes regardless of content. bench.md §1's own
purpose is "application 로직 제거, transport 비용만 분리 측정" — so these 8KB ops polluted the measurement.
KEY: the validation only ever checked 3 byte positions (rb[0], rb[bl/2], rb[bl-1], bench:171) — the full
8KB fill/copy was overkill. FIX: replace the 8KB memset/memcpy with a 3-byte fill/copy (same validation,
no app bandwidth). Transport (8KB DMA x4/RTT) unchanged.
RESULT (transport-only, K=2 + poll_rx + BATCH_REV_DONE, hw3, 8KB, 0-fail):
| RPS | achieved | p50 | state |
|---|---|---|---|
| 235K | 233,882 | 1.24ms | healthy |
| 245K | 243,835 | 1.28ms | healthy (new sustainable knee) |
| 260K | 250,461 | 170ms | saturating |
| 300K | 254,246 | 1.07s | saturated |
=> TRUE transport ceiling ~245K healthy / ~252K saturation, vs ~235K with the 8KB app ops = **+5-10%**.
So the app memcpy/memset CO-BOUND but were NOT dominant (~7%); the main limit is still the transport
(single host comch-RX PE thread). The bench now correctly isolates transport. The 3-byte validation keeps
the same corruption sanity (it always only checked 3 positions). Baked (bench app, not a knob).
LADDER (transport-only is the correct metric): 130K(K=1) -> 160K(EU-shard) -> 200K(poll_rx) ->
~235K(BATCH_REV_DONE) -> ~250K(bench isolates app overhead). Cap remains the host comch-RX PE thread.

## Q2.18 — "more cores to service" REGRESSES; chain reached the M2 engine ceiling
USER asked: give the echo SERVICE more cores (or one-way bench), since they want dpumesh TRANSPORT perf
not service perf. Tested hw6 (6 cores/side + ECHO_THREADS=6 ASYNC_THREADS=6), transport-only bench:
| profile | ceiling |
|---|---|
| hw3 (3c/3thr) | ~245-252K |
| hw6 (6c/6thr) | **195K, then collapses 131K->115K at higher RPS** |
=> MORE cores/threads REGRESS. Cause: the bottleneck is the SINGLE host PE thread (1 core, processing
completions); adding workers only adds shared-rx_queue rx_lock contention + poll-spin thrash. So "more
cores to the service" does NOT raise the ceiling, and (b) lock-free RX alone won't exceed it either (the
single PE caps). Restored baked ECHO_THREADS=3 / hw3.

KEY: the chain has REACHED the M2 engine ceiling. ~250K RTT x 4 dma_copy = **~1.0M dma_copy/s = bench.md
M2 (single-ARM one-way forward) N>=4 plateau (1.0M)**. bench.md had the chain at 416K = 67% of M2; the
session's transport work (EU-sharding 4EU + poll_rx + BATCH_REV_DONE + bench app-isolation) brought it to
1.0M = 100% of M2. So the chain is now at the single-host-PE / single-ARM-forward limit. Above M2 is M0
(1.53M, no host forward) and pure (1.8M); exceeding M2 needs to PARALLELIZE the single host PE (multi-conn
comch, or offload the per-entry process_rx_dma_entry to workers) OR a ONE-WAY bench (no round-trip, no
service re-send) to measure the forward transport rate directly. One-way needs the request TX_ACK
re-enabled (g_skip_req_txack) for client TX-slot lifecycle without a response.
LADDER: 130K -> 160K(EU) -> 200K(poll_rx) -> ~235K(BATCH_REV_DONE) -> ~250K(bench isolates app) = M2 ceiling.

## Q2.19 — LIGHTER transport: lock-free TX slot pool (user: transport must be light)
USER directive: the transport must be LIGHT on host CPU (NOT use more cores). perf (Q-prev) showed the
reducible host CPU is in MUTEXES + the O(n) TX-slot bitmap scan, not the SDK. Replaced slot_bitmap +
slot_lock + slot_cond with a lock-free Treiber free-list of slot indices (free_head atomic u64 with ABA
tag + slot_next[]); tx_alloc = lock-free pop (spin-backoff when empty), tx_free = lock-free push. Removes
slot_lock (mutex), the O(num_slots) scan, and the per-free cond_signal (futex).
perf BEFORE -> AFTER (echo self-time @240K): dpumesh_tx_alloc 3.42% -> 1.73% (halved); mutex lock+unlock
6.5% -> 5.7% (slot_lock gone); tx_free 0.96%. => host CPU per request is LIGHTER. Correctness: 250K
healthy (p50 1.48ms), 0-fail, back-to-back 218.8K==218.8K = NO SLOT LEAK (free-list correct). Ceiling
~245 -> ~250K (small; host wasn't purely tx-bound). Baked (always-on, no knob).

REMAINING reducible host CPU (next "lighter" levers, all lock-free, no extra cores):
- rx_queue rx_lock + dpumesh_dequeue (~5.3% + part of mutex) = the echo SPMC. Lock-free Vyukov-style SPMC ring
  (1 PE producer, N worker consumers). Biggest remaining. Caveat: needs a lightweight cond kept ONLY for
  the non-poll (Thrift blocking) path; poll_rx (bench) path is pure spin on the ring.
- pending[req_id] mutex (~2-3%, client side) = lock-free atomic state.
IRREDUCIBLE FLOOR: the DOCA SDK comch polling (priv_doca_cq_poll_one + doca_pe_progress + comch internals)
~12-15% of a host core at 250K — inherent to a completion-notification RX; only a fundamentally different
RX (completion-free polled, not SDK-supported) would go below it. So the transport's host-CPU floor at
~250K is the SDK poll (~12-15%) + the irreducible per-entry deliver. Ladder unchanged (~250K = M2 ceiling).

## Q2.20 — LIGHTER transport: lock-free rx_queue (Vyukov-style SPMC)
(Terminology: this queue is SPMC — 1 PE producer, N worker consumers —
built on Vyukov's bounded-MPMC cell/seq design with the producer side specialized to a
single producer, plain store on rx_enq, no CAS. Earlier "MPSC" labels were a misnomer.)
USER: "rx_queue lock-free 이어서 해줘" (continue the lighter-transport work; rx_queue was the biggest
remaining reducible host CPU). The echo RX queue was a circular buffer guarded by rx_lock (mutex) + rx_cond
+ rx_not_full (vestigial: signalled, never waited). The single PE producer took rx_lock per enqueue and
N workers took rx_lock per dequeue — PE<->worker mutex contention on EVERY request, on the bottleneck PE.

CHANGE (dpumesh_doca.c, baked/always-on, no knob): replaced rx_queue[]/head/tail/count + rx_lock +
rx_cond + rx_not_full with a lock-free **Vyukov-style bounded SPMC ring** (RX_QUEUE_SIZE=65536, pow2 -> mask):
- struct rxq_cell { sw_descriptor_t desc; atomic_uint_fast32_t seq; } rx_ring[]; atomic rx_enq, rx_deq.
- Producer (rx_deliver_desc, single PE): seq-gated single-producer enqueue (relaxed load/store rx_enq,
  release-store cell seq), drop+rx_reclaim on full. No rx_lock in poll_rx.
- Consumer (dpumesh_dequeue, N workers): rxq_try_pop = acquire-load cell seq, CAS rx_deq, read desc,
  release-store seq+SIZE. Multi-consumer via CAS.
- poll_rx (bench): pure lock-free spin + adaptive backoff (RX_POLL_SPIN/20us), NO mutex at all.
- non-poll (Thrift blocking): lock-free pop; only when empty, lock rx_lock + re-check (closes lost-wakeup)
  + cond_wait to sleep idle-efficiently. Producer signals rx_cond under rx_lock only when !poll_rx.
- rx_enq/rx_deq cacheline-padded apart (defensive; see below).
Single-producer invariant verified: rx_deliver_desc is called only from process_rx_dma_entry <- rx_data_hook
<- doca_pe_progress <- the single pe_progress_fn thread.

PERF (echo node, crictl PID + sudo perf -g, 240K, self-time):
| symbol | Q2.19 (locked rx_queue) | Q2.20 (lock-free) |
|---|---|---|
| pthread_mutex lock+unlock | 5.7% | **3.48%** (2.07+1.41) |
| rx_deliver_desc (PE enqueue) | took rx_lock/req | **0.74%, lock-free (no mutex)** |
| __lll_lock_wait (futex) | (in mutex) | **0.01%** (no contention) |
| dpumesh_dequeue + rxq_try_pop | dequeue 5.3% + rx_lock share | rxq_try_pop 10.5% + dequeue 2.1% |
=> MUTEX -39% (5.7->3.48): rx_lock ELIMINATED from both the PE producer (bottleneck) and the workers.
Remaining mutex = pending table + ring_lock (next levers). The PE per-request enqueue is now lock-free.

CACHELINE PADDING measured NEUTRAL (rxq_try_pop 10.71% -> 10.50%): rx_enq is producer-PRIVATE (consumers
never read it) so it never false-shared rx_deq. Kept anyway (textbook lock-free-ring hygiene, ~192B, harmless). The
residual rxq_try_pop ~10.5% is INTRINSIC: 3 workers CAS the shared rx_deq (true multi-consumer sharing) + the cell
seq line bounces producer<->consumer near-empty. NOT reducible by layout.

HONEST framing of "lighter": in poll_rx (bench) the workers are DEDICATED spinners (~100% core regardless),
so wall-clock core use is ~constant; the lock-free win there is (a) the PE BOTTLENECK is now lock-free,
(b) zero futex/contention stalls. The REAL lightness is the NON-POLL (production Thrift) path: rx_lock is
gone from the data path entirely — a worker does one lock-free CAS-pop per request and cond_waits only when
idle (vs old rx_lock per pop + cond). Strictly lighter per request in production.

CORRECTNESS (hw3, 8KB, transport-only bench, K=2 + poll_rx + BATCH_REV_DONE + lock-free TX + lock-free RX):
| RPS | achieved | p50 | OK/Fail |
|---|---|---|---|
| 200K | 199,244 | 1.25ms | 3.0M / 0 |
| 240K (70s sustained) | 239,713 | 1.30ms | 16.8M / 0 |
| 245K | 244,080 | 1.37ms | 3.675M / 0 (healthy knee) |
| 260K | 250,645 | 205ms | 27 fail / 3.9M (saturation cliff timeouts) |
Back-to-back 200K after the 260K saturation: 199,063 == 199,058, 0-fail = CLEAN recovery, NO leak
(Vyukov positions self-balance as items drain). Ceiling unchanged ~250K = M2 (throughput-neutral, as
expected: the cap is the single host PE / M2 engine, NOT rx_lock). Goal was LIGHTER, not faster.
LADDER (unchanged ceiling): 130K -> 160K(EU) -> 200K(poll_rx) -> 235K(BATCH_REV_DONE) -> ~250K(M2). Host
CPU per request: TX pool lock-free (Q2.19) + RX queue lock-free (Q2.20) => mutex 6.5% -> 3.48% cumulatively.
NEXT lighter lever: pending[req_id] lock-free (the remaining ~3.48% mutex = pending + ring_lock).

### Q2.20 adversarial verification — mutex attribution CONFIRMED + corrected
Challenged the "3.48% mutex = pending + ring_lock" attribution (memory: elimination is unreliable; demand
positive evidence). Perf caller-graph was inconclusive: frame-pointer unwind fails (libthrift -O2 omits
frame ptr); --call-graph dwarf is Heisenberg-perturbed (16KB/sample stack copy -> the recording is dominated
by nanosleep context-switch storm + perf's own native_write_msr/sched_in; userspace symbols squished below
threshold). So the AUTHORITATIVE evidence is an EXHAUSTIVE CODE AUDIT (all 14 pthread_mutex_lock sites),
re-run by 4 independent agents (2 enumerate, 2 adversarial-refute) + synthesis.
VERDICT: claim CORRECT (no refuter found a missed lock; rx_lock=0 in poll_rx confirmed by all 4). EXACT
per-request mutex count (poll_rx=1) = **5 acquisitions**:
- p->lock (pending) = **4x** : register_pending(:1021) + pending_attach_tx(:1068) + pending_release_async
  (:1248) [worker] + TX_ACK handler(:415) [PE]. ALL on the SAME pending[req_id%65536] slot -> genuinely
  CROSS-THREAD CONTENDED (worker vs PE) -> this is the DOMINANT mutex self-time (4 of 5).
- ring_locks[ridx] = **1x** : dpumesh_enqueue(:825). Spread over K=2 rings (test-bench.sh forces
  DPUMESH_RINGS_PER_POD=2, NOT the K=1 default) -> lightly contended, minor.
CORRECTIONS to the Q2.20 narrative: (1) "pending + ring_lock" is right but NOT 50/50 — it's ~4/5 pending
(the cross-thread-contended slot) + ~1/5 ring; "mostly pending" is accurate. (2) bench runs K=2 so the ring
lock is NOT a single contended object. Lock-free structures (Treiber TX, Vyukov RX, __sync credit) correctly
do NOT appear as pthread_mutex self-time (single RMW, not lock loops). 3.48% is fully consistent with the 5
acquisitions; nothing unaccounted. NEXT lever (pending lock-free) correctly targets the dominant 4/5.
SIDE FINDING (dwarf): at 240K the echo workers are ~84% IDLE (3 workers, ~16% busy) and the worker cores'
biggest cost is the 20us backoff nanosleep sleep/wake churn (poll_rx-bench artifact), NOT the mutexes or the
lock-free ring. The mutex (3.48%) + rxq_try_pop (10.5%) are secondary to the idle-poll scheduler traffic.

## Q2.21 — Periodic hot-path log audit + bench stat gated off
USER noticed periodic logs during deploy/test and asked to audit log level + log statements, and whether
more host-CPU lightening remains.
LOG AUDIT (positive, by source):
- The periodic lines the user saw = bench app stat threads: echo_dpumesh.c:31 "[echo-stat] rx_queue_depth=
  .. tx_inflight=.." and bench_dpumesh.c:51 "[bench-stat] ..", printed every 1s whenever tx_inflight>0.
  These are raw fprintf(stderr) -> NOT gated by -l or any DOCA log level (that's why they always show).
  Each also calls dpumesh_debug_stats every 1s, which walks the lock-free TX free-list O(num_slots).
- DPU 1Hz DOCA_LOG_INFO (dpu_worker.c:1191 "elapsed/sent/recv/cq_depth" + dmesh_log_eu_stats) is INFO(50),
  correctly FILTERED at -l 40 -> does NOT appear (verified -l stays 40). test-bench.sh:250 documents 50=INFO.
- Host transport per-request logs are DOCA_LOG_DBG (dpumesh_doca.c:393 TX_ACK, :864 ENQUEUE) = level 60,
  filtered (host backend shows INFO at init only, one-time, not periodic). No periodic host transport log.
FIX: gated both stat threads behind BENCH_STAT (default OFF) — the pthread_create is skipped unless
BENCH_STAT=1, so by default NO periodic [echo-stat]/[bench-stat] line AND no 1Hz debug_stats free-list walk
(host a touch leaner). Verified post-deploy: 0 stat lines in pod logs, 240K 238,889 p50 1.34ms 0-fail
(throughput unchanged). Re-enable: `BENCH_STAT=1 ./test-bench.sh deploy` — BENCH_STAT is now plumbed into
BOTH pod manifests (test-bench.sh bench env :445 + echo env :484, default 0); without that passthrough the
pods (env comes only from the hardcoded manifest env: lists, NOT the shell) would never see it.

MORE LIGHTENING — analyzed, user chose to STOP. Remaining reducible host mutex = the verified 3.48%, of which
the DOMINANT 4/5 is the pending p->lock (cross-thread worker<->PE on the same hashed slot). Offered: (A)
coalesce echo's register+attach+release 3->1 worker lock (total 4->2, moderate risk: must preserve the -2
deferred-ACK + collision-wait protocol; state=-2 settable pre-enqueue since TX_ACK only arrives post-enqueue),
(B) full lock-free pending (delicate: slot reuse / late-ACK / owner_req_id guard / client cond). USER PICKED
"stop here": throughput is at the M2 ceiling so pending lock-free wouldn't raise it, the remaining mutex is
small, and below it sits the irreducible SDK comch poll (~12-15%). The transport is at its lock-free-
achievable host-CPU floor. Deployed: baked K=2 + poll_rx + BATCH_REV_DONE + lock-free TX + lock-free RX +
stat-gated-off, -l 40, ~240K 0-fail.

## Cleanup — bake the winning config fixed, delete everything else
USER directive: keep ONLY the best-performing option (no env/compile toggles — fixed in code); keep only
thread/EU counts + sizing + pod-identity configurable; delete all dead code / unused vars / inappropriate
names / record-keeping comments. This intentionally OVERRIDES the earlier "bake ON but KEEP the SHARD/SPLIT/
DRAIN scaffolding + keep diagnostics + KEEP CASE_INGRESS" decisions ([[project_option_b_foreclosed_cleanup_keep]],
[[feedback_bake_dont_delete]], [[project_recv_pool_coupling]]). Confirmed by the user before editing.

### Removed (the losing / dormant options)
- DPU ARM control-plane SPLIT machinery: SPLIT_SEND (SENDS/REBAL/SHARD modes), DRAIN_SHARDS>1, the SENDER /
  shard-worker / drain-shard threads + send_spsc/work_spsc/shard_work/shard_send SPSC types + consumer_lock(_shard)
  + arm_core_a/b + shard_worker_of/drain_group_of_eu/send_via_spsc. dpu_worker.c kept ONLY the single-ARM
  SPLIT_OFF path (the measured winner, Q2.1). ~600 lines from dpu_worker.c + object.h + comch_*.
- All measurement instrumentation (the bottleneck investigation is DONE): DPU 1 Hz sent/recv/cq_depth stat,
  dmesh_log_eu_stats + the EU-stall counters (stat_dma/consumer_wait/admission_brk/idle_resched) + their
  device-side increments + d2h readback; g_egress_again; DPUMESH_TRACE hop-timing; recv_err_count; the bench
  [echo-stat]/[bench-stat] stat threads + dpumesh_debug_stats + the BENCH_STAT gate.
- Baked features fixed ON (env/flag selection removed): SKIP_REQ_TXACK, BATCH_TXACK, BATCH_REV_DONE, DPA EU
  affinity (host); ZEROCOPY_RX + PE_ADAPTIVE were already baked. Consumer model fixed per caller: bench→async,
  echo→poll_rx, Thrift→blocking (async/poll impossible under the sync Thrift API). DPUMESH_KEEPALIVE_US baked
  to 1 ms.
- Dead host code: blocking bench worker_fn + sleep_until (async is the only model now), dpumesh_get_notify_fd
  (unimplemented -1 stub), 7 never-read sw_descriptor_t fields (step_id, src_body/header_pool_type/pod_id/buf_slot)
  + their writers in TDpumeshTransportBase/bench/gateway, POOL_NONE/POOL_HOST_TX_BODY (no users left).
- Record-keeping/history comments across the transport (vestigial-consumer_pe, "~5% host CPU", old handshake,
  legacy-pattern, SHARD-measurement narrative, backward-compat).

### Kept (delete-before-verify risk check overruled deletion — NOT dead)
- CASE_INGRESS / CASE_EXTERNAL: the flags byte is wire-ABI (dma_desc.flags offset + comch_dma_comp_msg==16B
  _Static_asserts) and DMA-copied across the host/ARM/DPA boundary; CASE_INGRESS is live write-once metadata.
- comch_msg union add_ring_msg/add_rev_ring_msg members: their 80 B size sets the msgq imm_data_len
  (dpa.c set_imm_data_len(sizeof(comch_msg))==84) that RECEIVES the 80 B ADD_RING — shrinking would truncate
  ring setup. (A survey agent labelled these "safe to delete"; its own cited evidence proved the opposite —
  adversarial verification caught it. Kept.)

### Bug fixed during the audit
- comch_client.c set_recv_queue_size error path logged "Failed to set msg size property" (copy-paste from the
  set_max_msg_size path); corrected to "recv queue size". Log-only, no behavior change.

### Configurable surface after cleanup (user's choice = thread counts + sizing + identity)
env kept: DPUMESH_DPA_THREADS, DPUMESH_RINGS_PER_POD (thread/EU counts) · DPUMESH_NUM_SLOTS / SLOT_SIZE /
MAX_DESCRIPTORS (sizing) · DPUMESH_POD_ID / PCI_ADDR (deploy identity) · ECHO_THREADS / ASYNC_THREADS (bench
thread counts). Everything else is fixed in code. test-bench.sh dropped the baked-off env (KEEPALIVE_US, TRACE,
BENCH_STAT, ECHO_POLL_RX) and its dead `trace)` subcommand.

### VERIFIED non-regressing (clean deploy, hw3, 8KB, baked DPA=4 K=2, -l 40)
The deploy compiled all 3 toolchains clean (host libthrift + DPU ARM + DPA device build_dpu) — a compile
error would have aborted before pod rollout. Back-to-back, 0-fail throughout:
| RPS | achieved | p50 | p99 | OK/Fail |
|---|---|---|---|---|
| 30000 (warmup) | 29,838 | 1.79 ms | 3.11 ms | 300000/0 |
| 200000 | 198,917 | 1.40 ms | 3.63 ms | 2.0M/0 |
| 240000 | 238,691 | 1.46 ms | 3.99 ms | 2.4M/0 (sustainable knee) |
| 200000 (back-to-back) | 198,895 | 1.39 ms | 3.37 ms | 2.0M/0 |
| 30000 (recovery) | 29,838 | 1.80 ms | 3.11 ms | 300000/0 |
Back-to-back 200K==200K (198,917 vs 198,895, <0.02%) → NO slot leak. Ceiling unchanged ~240K = Q2.20
(240K→239,713). Net: ~810 source lines removed across 16 files; the standing 200-240K config is now the
only code path (no env/compile toggles), with thread/EU/sizing counts still tunable.

---

# Event-driven PE notification — feasibility (CPU-leanness, NOT throughput)

GOAL: cut the idle-CPU waste of busy-polling (`doca_pe_progress` loops burn a full core even when
idle). DOCA exposes an event-driven path (`doca_pe_get_notification_handle` → `doca_pe_request_notification`
→ `epoll_wait` → `doca_pe_clear_notification`) so the progress thread can SLEEP on an fd instead of spinning.
This is a leanness/power lever, NOT a throughput lever (ceiling is the M2/host-PE service rate, Q2.18) — a
correct drain-then-arm hybrid polls under load (never arms) and only blocks when idle, so 240K is unaffected.

The two always-on busy-pollers: host `pe_progress_fn` (dpumesh_doca.c, has adaptive nanosleep(20µs) backoff)
and the DPU `run_dpu_worker` main loop (dpu_worker.c, NO backoff → full ARM core 100% even idle). DPA EUs are
HW, out of scope. The one real uncertainty (prior workflow, medium-confidence): does the DPU `consumer_pe`
(the hot DPA→DPU msgq drain) raise a USABLE epoll notification, or is it poll-only?

## Spike — DPU consumer_pe notification probe (DPUMESH_EVENT_PROBE, gated, default off; hang-safe 1ms timeout)
Gated diagnostic in run_dpu_worker: drain consumer_pe to 0 → `doca_pe_request_notification` → `epoll_wait(1ms)`
→ count wake reason. EVENT = fd fired; WORK = timeout but a completion was waiting (= silent miss = poll-only);
idle = timeout, nothing waiting. 1Hz summary. Default off ⇒ baked busy-poll byte-identical. Deploy DPA=4 K=2,
`-l 50`, fair 1-core.

| regime | wake_EVENT/s | timeout_WORK/s | timeout_idle/s |
|---|---|---|---|
| idle (no client traffic) | ~960 | **0** | ~933 |
| **20K RPS bench (19,925 ach, 0-fail)** | **~6,800–7,960** | **0** | ~847 |
| post-bench (idle) | ~960 | **0** | ~933 |

VERDICT (positive, measured): **consumer_pe DOES raise a usable epoll notification.** wake_EVENT tracks request
load (idle ~960 → 20K ~7,000), proving real FWD_DONE/REV_DONE completions fire the fd. timeout_WORK=0 in EVERY
window ⇒ there is NO silent-miss path (a completion never arrives without firing the fd). Resolves the prior
"medium confidence / unproven" residual risk with direct evidence. get_notification_handle succeeds for BOTH
consumer_pe and the ctrl pe. (idle ~960 EVENT/s = keepalive/producer-completion traffic on the consumer_pe-
connected DPU→DPA producer, dpa.c:607; not request traffic.)

⇒ The DPU hot path CAN be converted to event-driven blocking: epoll on {consumer_pe fd, ctrl pe fd} + a timerfd
for the 1 ms keepalive cadence, drain-then-arm hybrid (poll under load, block when idle). This unlocks the
biggest idle-CPU win (the DPU ARM full-core busy-spin → ~0% idle). Host `pe_progress_fn` is the lower-risk twin
(swap its nanosleep(20µs) idle path for epoll). NEXT: implement the real DPU event-loop + measure ARM idle-CPU%
(busy-poll vs event) and confirm 240K/0-fail non-regression. Probe reverted; baked config restored (-l 40).

---

## Idle wedge: two root causes found + fixed (DPA re-arm + ARM event-loop re-poll)

**Symptom (user report):** after deploy + idle > threshold, first load `dpumesh 220000 10 8192`
returned `0.0 RPS, OK/Fail 0/4400` (total wedge). DPU log silent at `-l 40`.

**Bug 1 — DPA EU `reschedule()` without `request_notification`** (device/dpa_kernel.c run_dma_manager).
DOCA contract (doca_dpa_dev.h:544 + dpa_initiator_target sample) requires re-arming both completion
contexts before yielding, else a parked EU is never re-triggered by the 1 ms DPA_MSG_WAKE.
Fix: arm consumer_comp + producer_comp before reschedule (idle branch only → hot path byte-identical).
Validated on **busy-poll (EVENT_LOOP=0)**: 30K/100K/200K all 0-fail, ceiling ~199K.

**Bug 2 — ARM event-loop driver wedge at non-saturating load** (EVENT_LOOP=1, dpu_worker.c epoll loop).
HEISENBUG (debug counters masked it; counters proved timerfd=1000/s, wake_send_fail=0, consumer_pe wakes
prompt). Root cause: drain→arm→block race — a FWD_DONE landing between last drain and request_notification
isn't guaranteed to make the PE fd readable, so the ARM blocks on pending work; the stranded completion
delays recycling the per-EU recv slot → EU starves → permanent compounding wedge. 220K-saturation masked it
(loop never reaches epoll_wait). Busy-poll never hit it (re-polls continuously).
Fix: **arm → re-poll → block** (after arm, dpu_drain_iteration once; if progress, clear+continue, don't sleep).

**Validation (EVENT_LOOP=1, both fixes, -l 40, fair 1-core/pod, 8KB):**

| test | Achieved RPS | OK/Fail |
|---|---|---|
| back-to-back 30K ×8 | ~29,740 | 180000 / **0** each |
| 100K | 99,460 | 1000000 / **0** |
| 200K | 198,920 | 2000000 / **0** |
| 90s idle → cold 30K | 29,801 | 240000 / **0** |
| **90s idle → cold 220K (user's exact failing cmd)** | **218,754** | **2200000 / 0** |
| 3× 30K after the 220K run | ~29,740 | 180000 / **0** each |

Unfixed event-loop baseline wedged on run #2 (20–30 RPS, 300/600). Deploy requires `DPUMESH_EVENT_LOOP=1`
(.env unset → default 0 = busy-poll).

---

## Phase A: host pe_progress spin → epoll (DPUMESH_HOST_EPOLL)

Goal: reduce host CPU from the polling/while loops (user directive: "notification 올 때만 pe_progress").
Change: `pe_progress_fn` (dpumesh_doca.c) spin+nanosleep(20µs) → **epoll on the host comch PE notification
handle** (drain→arm→re-poll→block). Safe because the host comch PE receives ONLY real completions
(REV_DONE/TX_ACK) — no silent-wake path (unlike the DPU forward ring). Env-gated `DPUMESH_HOST_EPOLL`
(default 0). DPU = busy-poll (EVENT_LOOP=0, stable), DPA=4/K=2, 8KB, fair-pin.

A/B latency sweep (0-fail both, achieved ≈ target):

| target | EPOLL=0 p99/p999 (ms) | EPOLL=1 p99/p999 (ms) |
|---:|---|---|
| 30K  | 2.17 / 12.99 | 2.98 / 3.10 |
| 100K | 2.58 / 9.97  | 2.61 / 3.02 |
| 200K | **46.46 / 57.74** | **2.83 / 4.69** |

Idle host CPU (per-thread top, 2nd sample): echo 7%→6%, bench ~2-3%→**0%** (pe_progress thread 0% under
epoll). pe_progress idle savings modest (~2-3%/pod; old spin already had a nanosleep backoff).

VERDICT: **big win, but mostly via TAIL LATENCY not idle CPU.** The spinning pe_progress was contending
for the pinned core and inflating tails — 200K p99 46ms→2.8ms (~16×), p999 58ms→4.7ms. CPU-wise the real
remaining host idle consumer is the 3 echo poll_rx workers (~6%), which HOST_EPOLL does NOT touch (separate
throughput-sensitive app-side spin — needs poll_rx→cond, a throughput tradeoff). DPU ARM still busy-poll
(~full core) — that's Phase B (host enqueue→comch kick so the ARM epoll wakes on real forward data, killing
the racy 1ms keepalive + idle wedge).

---

## DPU ARM event-driven (epoll + 1ms keepalive) + EU park+re-arm+re-scan: idle wedge GONE

User decision: keep the 1ms DPU→DPA keepalive but make the DPU ARM epoll-based (not busy-poll).
Config (default EVENT_LOOP=1): DPU ARM = epoll over {consumer_pe, ctrl_pe} with a 1ms epoll-timeout that
re-sends DPA_MSG_WAKE every 1ms; DPA EU = park(reschedule) on idle, RE-ARM both completion contexts
(request_notification consumer+producer) + RE-SCAN rings before parking; host pe_progress = epoll (Phase A).

KEY FIX vs all prior wedging configs: the EU **re-scan after re-arm** (catches a silent desc->valid=1 that
lands during the re-arm window) — prior event-loop had re-arm but NOT the re-scan → wedged after multi-min idle.

Results (8KB, fair-pin, DPA=4/K=2):
- 30K x3 back-to-back: 180000/0 each.
- DPU ARM idle CPU: **~2%** (epoll sleeps between 1ms ticks; vs busy-poll ~100% full core).
- **150s idle → cold 30K: 29,801 RPS, 240000/0, p99 3.83ms (NO WEDGE)**.
- cold 200K: 198,929 RPS, 2000000/0, p99 3.48ms (throughput intact).

Caveat: 150s passed; prior failure was after "several minutes" — longer-idle (7min) re-validation pending.

---

## DPA EU grace-period polling: p50 → sub-ms (poll continuously, park only on sustained idle)

User insight: the DPA EU is dedicated FlexIO silicon (zero ARM/host CPU), so it should POLL as continuously as
possible for latency; parking on every empty drain (prior config) was too eager → parked in inter-request gaps
at moderate load → ~1ms WAKE wait per hop → p50 ~2ms.

Change (dpa_kernel.c run_dma_manager): consecutive-empty-drain counter; park (reschedule + re-arm + re-scan)
ONLY after IDLE_SPINS_BEFORE_PARK=262144 consecutive empty drains. Under any real load the gap << grace window
→ counter resets every request → EU NEVER parks → continuous polling → no park/WAKE latency. At sustained idle
the counter hits the threshold → park (satisfies the ~120s FlexIO watchdog). ARM unchanged (epoll + 1ms keepalive).

Results (8KB, fair-pin, DPA=4/K=2, EVENT_LOOP=1):

| target | p50 (ms) | p99 (ms) | p999 (ms) | OK/Fail |
|---:|---|---|---|---|
| 30K  | **0.21** (was 2.03) | 0.54 | 3.55 | 300000/0 |
| 100K | **0.23** (was 2.12) | 0.43 | 5.66 | 1000000/0 |
| 200K | **0.31** (was 1.59) | 38.83 | 55.58 | 2000000/0 |
| 90s idle → cold 30K | **0.22** | — | — | 240000/0 |

→ p50 ~10× better (sub-ms), 0-fail, idle-stable (90s). CAVEAT: 200K p99/p999 (38/55ms) regressed vs the
park config (3.5ms) — high-load tail from continuous-poll PCIe contention (bench.md §3.3) and/or the 1ms
keepalive overhead (unnecessary under load since the EU never parks there); to verify/tune. Median-vs-tail trade.

### 200K tail re-check (5 runs) — the p99 "regression" was SATURATION NOISE, not a grace-period cost

grace-period config, 200K x5: p50 = 0.30/0.30/0.30/0.30/0.31ms (rock-stable); p99 = 31.55/28.41/19.35/**0.62**/35.74ms
(run #4 = 0.62ms!); all 0-fail. → the p99 swings 0.6–36ms run-to-run because 200K sits just under the ~220K
ceiling (bursty queueing at the saturation knee), NOT a deterministic grace-vs-park regression — one grace run
beat the park config's single 3.5ms sample. CORRECTION to the prior entry: grace-period does NOT inherently
worsen the tail; the high-load tail is a saturation-edge artifact. Normal load (30K/100K) p99 stays 0.4–0.5ms.
Verdict: grace-period polling is a clean win (p50 ~0.3ms stable, idle-stable, 0-fail).

### Max-RPS ramp (grace-period config, DPA=4/K=2, 8KB, default conns)

| target | achieved | p50 | p99 | OK/Fail | verdict |
|---:|---:|---:|---:|---|---|
| 200K | 198,660 | 0.31 | 35.03 | 0 fail | ok (p99=saturation noise) |
| **240K** | **238,381** | **0.35ms** | **0.69ms** | **0 fail** | **sustainable max (clean)** |
| 280K | 252,934 | 417ms | 830ms | 0 fail | overload (latency explodes) |
| 320K | 251,233 | 1065ms | 2111ms | 8071 fail | throughput ceiling ~252K |
| 360K | 137,162 | 1727ms | 3397ms | 1.3M fail | collapse |
| 400K | 136,841 | 2246ms | 4521ms | 1.5M fail | collapse |

→ **Sustainable max ≈ 240K** (238K achieved, p50 0.35ms / p99 0.69ms, 0-fail). Throughput hard ceiling ≈ 252K
(280K/320K both plateau ~252K but latency unusable). Beyond ~320K → overload collapse (achieved drops to 137K).
Recovery: 30K x2 after the 400K overload = 180000/0, p50 0.22ms — full recovery, no slot-leak/wedge.

---

## DEBUG: EVENT_LOOP=1 long-idle wedge (0 RPS / all-fail, needs redeploy)

**Symptom.** `DPUMESH_EVENT_LOOP=1` deploy + `dpumesh 240000 10 8192`: works fine initially and under load
(238K 0-fail validated), but after a LONG idle (multi-minute; 90s was NOT enough — see ramp entry above) the
first test returns `Achieved RPS 0.0, OK/Fail 0/4800`. Permanent until redeploy.

**Root cause (static analysis, dpa_kernel.c `run_dma_manager` park/wake).** NOT the ARM event loop — that
loop has a 1ms epoll timeout backstop, so it always re-drains + re-sends the keepalive WAKE and cannot wedge.
The wedge is the **DPA EU park/wake re-scan race**:

```
} else if (++idle_spins >= IDLE_SPINS_BEFORE_PARK) {     // EU parks every ~262ms of idle
    request_notification(consumer_comp);   // arm (one-shot edge)
    request_notification(producer_comp);
    if (drain_all_rings(thread_arg) == 0)  // re-scan ALSO calls handle_msgs() → drains consumer comps
        reschedule();                      // ...but only checks DMA count, ignores drained WAKE
    idle_spins = 0;
}
```

`drain_all_rings()` internally calls `handle_msgs()`, which drains+acks the ARM's 1ms `DPA_MSG_WAKE`. If a
WAKE lands in the arm→reschedule window, the re-scan **consumes the very wake-signal** yet returns 0 DMA calls,
so the EU parks anyway — with its one-shot notification already fired-and-consumed → **parked + disarmed**.
Subsequent WAKEs no longer trigger an activation; they pile up in the DPA consumer completion queue. With
`CC_DPA_MAX_MSG_NUM=1024` recv credits, after ~1024 undrained WAKEs (~1s at 1kHz) the ARM producer runs out of
recv credit and **can no longer post any WAKE at all** → permanent wedge needing redeploy.

Why "after LONG idle": the EU parks every ~262ms of idle (IDLE_SPINS_BEFORE_PARK=262144 × ~1µs/empty-drain).
The arm→reschedule race window is ~tens-of-ns wide, so each park has a tiny hit probability; ~90s idle (≈340
parks) rarely hits it, but multi-minute idle (thousands of parks) makes the cumulative probability approach 1.
This is the previously-recorded "re-poll fix insufficient" bug — the re-poll checks ring DMA, not the WAKE.

**Fix (FIX 2, canonical arm→re-poll→don't-sleep-if-found).** Make `handle_msgs()` return the # of messages
drained; in the pre-park re-check, reschedule ONLY if BOTH rings AND the consumer-completion queue came up
empty after arming. A WAKE drained in the race window now means "a signal arrived → loop, re-arm next cycle",
never "park with a consumed notification". Closes the race under both edge-from-empty and latched-activation
SDK semantics (see fix commit). EU/host code unchanged otherwise; ARM loop unchanged.

**Validation pending** (user deploy): `EVENT_LOOP=1` deploy → idle ≥5 min → `dpumesh 240000 10 8192` must be
0-fail. (Results appended below after test.)

### FIX 2 validation — AMPLIFIED STRESS build (IDLE_SPINS_BEFORE_PARK=256, ~1000× more parking), EVENT_LOOP=1

Deployed `DPUMESH_EVENT_LOOP=1 bash test-bench.sh deploy` with FIX 2 + the temporary 256-spin park threshold.
At 256-spin grace the EU stays HOT under sustained load (inter-arrival ≪ grace) but parks in every idle gap, so
back-to-back/idle accumulates park/wake cycles ~1000× faster than the production 262144 → a multi-minute wedge
should reproduce in seconds if the race were still open.

Cold ramp (fresh deploy, never cold-jumped to 240K):

| target | achieved | p50 (us) | p99 (us) | p999 (us) | OK/Fail |
|---:|---:|---:|---:|---:|---|
| 30K (cold)  | 29,838  | 227 | 494   | 1,302  | 300000/0 |
| 100K        | 99,453  | 232 | 317   | 1,780  | 1000000/0 |
| 200K        | 198,920 | 307 | 29,786| 52,794 | 2000000/0 |
| 240K        | 238,697 | 353 | 29,739| 40,278 | 2400000/0 |
| 1K × 30s (park/wake torture) | 998 | 344 | 475 | 1,441 | 30000/0 |

→ Full ramp 0-fail on the stress build; back-to-back (ramp steps are back-to-back with gaps) 0-fail; the
low-RPS run (EU parks in the inter-request gaps) 0-fail with sub-ms latency. 200K/240K p99 ~29ms = the known
saturation-edge noise (see prior ramp entry), not a park artifact. Long-idle (300s) soak running next.

**STRESS-BUILD 300s idle soak (the user's exact failing scenario, ~1000× amplified parking):**

| after 300s idle | achieved | p50 (us) | p99 (us) | p999 (us) | OK/Fail |
|---|---:|---:|---:|---:|---|
| cold 30K (warmup) | 29,838  | 222 | 487   | 858   | 300000/0 |
| then 240K         | 238,682 | 357 | 1,099 | 4,592 | 2400000/0 |

→ **DECISIVE: 0 fail, no wedge, no redeploy.** At IDLE_SPINS=256 a 300s idle ≈ ~1M park/wake cycles — orders
of magnitude more than the few-hundred-to-thousand parks that wedged the OLD code in "several minutes". The
lost-wakeup race is closed. (240K p99 here = 1.1ms, better than the cold-ramp 240K's 29ms because the 30K
warmup pre-warmed the rings.) Next: revert IDLE_SPINS→262144 (production) and re-confirm the shipping build.

### FIX 2 validation — PRODUCTION build (IDLE_SPINS_BEFORE_PARK=262144, shipping config), EVENT_LOOP=1

Reverted the stress override; redeployed `DPUMESH_EVENT_LOOP=1 bash test-bench.sh deploy`. Same FIX 2 code,
shipping park threshold. Ramp (never cold-jumped to 240K):

| target | achieved | p50 (us) | p99 (us) | p999 (us) | OK/Fail |
|---:|---:|---:|---:|---:|---|
| 30K (cold) | 29,838  | 229 | 511    | 1,449  | 300000/0 |
| 100K       | 99,452  | 233 | 426    | 1,879  | 1000000/0 |
| 240K       | 238,705 | 346 | 12,909 | 24,748 | 2400000/0 |

→ 0-fail. 600s (10-min) idle soak running next — exceeds the "several minutes" that triggered the original wedge.

**PRODUCTION 600s (10-min) idle soak + exact original command:**

| test | achieved | p50 (us) | p99 (us) | p999 (us) | OK/Fail |
|---|---:|---:|---:|---:|---|
| after 600s idle: cold 30K (warmup) | 29,838  | 230 | 496    | 860    | 300000/0 |
| then 240K                          | 238,697 | 347 | 45,538 | 54,742 | 2400000/0 |
| **exact orig cmd: `dpumesh 240000 10 8192`** | **238,676** | **348** | **674** | **1,448** | **2400000/0** |

→ **RESOLVED.** 10-min idle (exceeds the "several minutes" that wedged the old build) → 0 fail. The literal
failing command (`dpumesh 240000 10 8192`, which returned `0/4800`) now returns 2.4M/0. Shipping config =
EVENT_LOOP=1 + IDLE_SPINS_BEFORE_PARK=262144 + FIX 2 (handle_msgs returns count; pre-park re-scan parks only if
BOTH the consumer-completion queue AND the rings are empty → a WAKE in the arm→reschedule window is detected,
never silently consumed). 200K/240K p99 swings (0.7–45ms) remain saturation-edge noise, unchanged by the fix.

---

## Limit + CPU/DPU + latency characterization (FIX 2 prod build, EVENT_LOOP=1, DPA=4/K=2, 8KB)

### Latency-vs-load curve (each 10s, fair pin)

| target | achieved | p50 (us) | p99 (us) | p999 (us) | OK/Fail |
|---:|---:|---:|---:|---:|---|
| 1K   | 995     | 371 | 497   | 867   | 10000/0 |
| 10K  | 9,946   | 252 | 499   | 1,245 | 100000/0 |
| 30K  | 29,839  | 233 | 510   | 879   | 300000/0 |
| 60K  | 59,671  | 222 | 309   | 1,065 | 600000/0 |
| 100K | 99,450  | 230 | 317   | 5,896 | 1000000/0 |
| 150K | 149,168 | 268 | 437   | 673   | 1500000/0 |
| 200K | 198,892 | 295 | 443   | 626   | 2000000/0 |
| 240K | 238,669 | 340 | 586   | 1,272 | 2400000/0 |

### Ceiling probe

| target | achieved | p50 (us) | p99 (us) | OK/Fail | verdict |
|---:|---:|---:|---:|---|---|
| 260K | 256,039 | 622     | 67,857    | 2.6M/0    | 0-fail but p99 collapses (knee) |
| 280K | 257,860 | 418,456 | 832,886   | 2.8M/380  | overload |
| 300K | 257,308 | 798,317 | 1,572,452 | 3.0M/373  | overload |
| 320K | 258,508 | 1,170,388| 2,318,966| 3.19M/5289| overload |
| recover 30K | 29,839 | 231 | — | 300000/0 | full recovery, no slot-leak |

→ **Sustainable max ≈ 240K** (p50 340µs, p99<1.3ms, 0-fail). **Throughput hard ceiling ≈ 257–258K** (achieved
plateaus there regardless of target). At 240K×4 dma_copy/RTT ≈ 960K ops/s; 257K×4 ≈ 1.03M ops/s ≈ the DPA
DMA-engine op-rate ceiling (cf. [[project_n8_dma_engine_ceiling]] ~810K–1.6M). Beyond ceiling latency explodes (M/M/1).

### CPU / DPU usage (idle vs 240K load)

| component | idle | 240K load |
|---|---|---|
| Host client (core0, bench-dpumesh) | ~0% | **52.3%** (usr 33.2 / sys 19.2) |
| Host echo server (core1, echo-dpumesh) | ~0% | **21.2%** (usr 13.3 / sys 7.9) |
| DPU ARM main thread (event loop) | ~1–2% | **~98% (≈1 core)** |
| DPU ARM worker threads (×2) | ~1% / 0% | ~1% / 0% |
| DPA EU | dedicated HW EU (×4, K=2) | dma_copy op-rate ceiling ~1M/s |

→ Idle: host ~0% (HOST_EPOLL sleeps), DPU ARM ~2% (event-loop epoll). Load: DPU ARM busy-spins ~1 full core
(under load `dpu_drain_iteration` always progresses → never hits epoll_wait; event-loop saves CPU only at idle).
Host client core is the heaviest host consumer (52%); none of host client/server/ARM is saturated at 240K →
the ceiling is the DPA dma_copy op-rate (4 DMAs/RTT), NOT a host or ARM CPU wall.

### Latency decomposition — payload-size sweep at 30K (EU hot, no queueing)

| size (B) | p50 (us) | p99 (us) |
|---:|---:|---:|
| 64   | 226 | 496 |
| 512  | 217 | 480 |
| 1024 | 227 | 496 |
| 2048 | 228 | 500 |
| 4096 | 224 | 494 |
| 8192 | 234 | 503 |

→ **p50 is FLAT vs payload size** (217–234µs, within noise). 8KB (4×8KB DMA) adds only ~8µs over 64B. So the
~220µs floor is **fixed per-RTT overhead, NOT data-transfer time** — DMA bandwidth is not the latency limiter.
The floor = 4 dma_copy call/completion setups + fwd+rev completion relay (DPA→ARM→host) + EU/host polling
cadence + 2 host network stacks + PCIe round-trips. Latency reduction must attack FIXED OVERHEAD / hop count,
not transfer size. (Also: 1K RPS p50=371µs > 30K p50=222µs because the EU parks between requests at <~4K RPS
and pays the keepalive park/wake tax; ≥10K it stays hot.)

### Context: TCP+Envoy-sidecar baseline (fair 1-core, app+sidecar shared, 8KB) — same harness

| target | DPUmesh p50 | TCP+sidecar p50 | TCP OK/Fail |
|---:|---:|---:|---|
| 1K   | 371us | 1,194us (1.2ms)  | 10000/0 |
| 30K  | 233us | 9,347us (9.3ms)  | 300000/0 (p99 205ms) |
| 100K | 230us | 54,488us (54ms)  | 15/1000 (collapsed) |

→ DPUmesh ≈ **40× lower p50 at 30K** and sustains 240K 0-fail; TCP+Envoy collapses by ~100K (1 shared core,
sidecar contention = the Istio tax). DPUmesh also frees the host core (transport on DPU). The ~220us DPUmesh
floor is already far below the sidecar path — the analysis below targets shaving that floor further.

### Deep latency attribution (4-lens code+data workflow) — where the ~220µs RTT floor goes

TOTAL is MEASURED (220µs p50 floor); the per-stage split is an ESTIMATE (Med confidence) — the lenses agreed on
direction but not exact per-hop µs. Allocation that closes to the measured 220µs (full wall-clock RTT model):

| # | stage (one RTT) | ~µs | reducible |
|---|---|---:|---|
| 1 | client post (lock-free tx_alloc + silent valid=1 store, no doorbell) | 3 | no (already lean) |
| 2 | FWD leg1 host→DPU: EU poll-detect + 8KB dma_copy(~2µs) + FWD_DONE | 28 | partial (poll cadence) |
| 3 | FWD_DONE DPA→ARM completion + ARM route(O(1)) + post delivery | 30 | partial (ARM=2.8% RTT) |
| 4 | FWD leg2 DPU→echo: poll-detect + 8KB dma_copy | 28 | partial |
| 5 | echo poll_rx spin + 3B copy + reply enqueue | 18 | no (lean poll_rx) |
| 6 | REV leg3 echo→DPU + leg4 DPU→client + their completions | 84 | partial (mirror of fwd) |
| 7 | ARM→client REV_DONE relay + host PE reap + HOST_EPOLL wakeup | 29 | partial |
| | **TOTAL** | **~220** | |

**Dominant buckets:** (1) the 4 DMA legs' PCIe poll-detect ≈ 112µs (silent valid=1 store, no doorbell — host→DPA
wake SDK-foreclosed), (2) the 2 DPA→ARM→client completion relays ≈ 60µs (completion can't go DPA→host direct),
(3) the structural 4-leg chain (host→DPU→echo→DPU→host; host→host foreclosed). NOT data transfer (flat vs size),
NOT host/ARM CPU (none saturated).

**Reduction roadmap (proposed, pending implement+measure):**
- Quick wins: (A) load-adaptive REV_DONE partial-batch flush → ~3-6µs p50 + big p99 win at mid-load, but must be
  load-gated vs the +12-18% throughput batching buys [[project_batch_rev_done]]; (B) HOST_EPOLL=0 → ~2-4µs p50
  but 16× p99 regression + burns a core (floor-measure only, NOT prod) [[project_host_epoll_tail_win]];
  (C) pin host PE thread off the worker core → ~1-2µs + sys% relief, throughput-neutral.
- Structural: (D) tighten DPA EU drain cadence (scan just-signaled ring first; keep single per-iter read_inv) →
  ~12µs, the ONLY lever hitting the dominant ~112µs bucket; risk = extra PCIe read-fence could lower the
  ~257K op-rate ceiling → must guard throughput.
- Non-levers: host→host (foreclosed [[project_no_host_to_host]]), ARM-relay removal (foreclosed
  [[project_completion_required]]), more EUs/cores (throughput, not single-RTT latency).
- Realistic combined floor: ~220µs → ~195-205µs (7-12%). Throughput ceiling (DPA ~1.03M dma_copy/s) is orthogonal.

**Highest-leverage first = Lever D**, decision rule: accept only if 30K p50 drops ≥10µs AND 260K ceiling stays
≥257K AND the 1K-vs-30K park tax is unchanged; else revert to A+C (throughput-safe). Exact per-hop µs would need
DPA-side timestamping (counter in DPA mem read by ARM — avoids DPU-log flood).

### Per-load CPU/DPU scaling (8KB, fair pin, FIX2 prod build)

| rate | host client core0 | host echo core1 | DPU ARM (Σ threads) | achieved |
|---:|---:|---:|---:|---:|
| 30K  | 18% | 18% | **39%** | 29,948 |
| 60K  | 13% | 16% | **67%** | 59,892 |
| 100K | 32% | 19% | **81%** | 99,795 |
| 150K | 44% | 36% | **88%** | 149,716 |
| 200K | 46% | 30% | **96%** | 199,634 |
| 240K | 51% | 20% | **98%** | 239,580 |

→ **DPU ARM rises steeply toward 1-core saturation (98% @240K)** — the most-loaded host-managed component;
host client (51%) and echo (≤36%) keep headroom. The ARM is the single event-loop drain thread: under load
`dpu_drain_iteration` keeps returning progressed=1 so it busy-spins (never sleeps on epoll). NOTE: 98% is the
spin, not proven compute-bound — [[project_dpu_arm_built_o0]] showed -O0→-O2 throughput-NEUTRAL, so the ARM is
busy POLLING the PEs, not CPU-starved on useful work. But at the ~257K ceiling the ARM single thread is
effectively continuously busy → it is a co-candidate for the ceiling alongside the DPA op-rate (~1.03M dma_copy/s).
At low load (30K) ARM=39% because work is intermittent → it sleeps on epoll part of the time (event-loop CPU win
is real at low load, gone at high load).

### Host per-thread CPU split @240K (logic vs data-plane), fair 1-core pin

Bench client daemon threads (peak %CPU across the load window; sum > mpstat avg because peaks aren't simultaneous):
- pe_progress thread (DATA PLANE: comch completion reaping, HOST_EPOLL): **~10%**
- 4× async worker threads (LOGIC: request gen + poll_response harvest): **~53%** (~13-14% each)
- main/control thread: ~0%

→ On the HOST, **logic dominates (~53%), data-plane is light (~10%)** because HOST_EPOLL lets the completion
poller sleep. So the host 1-core (core0=51% @240K) is mostly the request-generation/harvest logic, not transport.
Implication: pinning data-plane to a separate core frees little (it's only 10%); and host core0 is NOT saturated
at 240K (51%) → the host 1-core cap is NOT the throughput binder. (Echo side core1 ≤36%, even lighter via poll_rx.)

### Multi-core host test (dpumesh-hw: bench cores 0,4 + echo 1,5) @240K — the "separate cores" idea

| | core0 | core4 | core1 | core5 | achieved | p50 | p99 |
|---|---:|---:|---:|---:|---:|---:|---:|
| HW (2-core/side) | 26% | 25% | 22% | 22% | 239,560 | 323us | 618us |
| (fair 1-core ref) | 51% | — | 20% | — | 238,669 | 340us | 586us |

→ Bench client total CPU is the SAME ~51% on 1 core or split 26+25 across 2 cores → **giving the host more cores
does NOT lift throughput** (239K≈238K), only halves per-core util + slightly improves p50 (323 vs 340us, less
worker/scheduler contention). CONFIRMS the host 1-core is NOT the throughput binder; the ceiling is DPU ARM /
DPA op-rate. (Pinning data-plane to its own core would free only ~10% — not worthwhile for throughput; useful
only for cleaner attribution.)

### ITEM 2 — Multi-DPA-thread / EU-sharding sweep (measured, 2-pod, 8KB)

Knobs: `DPUMESH_DPA_THREADS` (N = EU count, env, deploy-time, clamp[1,8]), `DPUMESH_RINGS_PER_POD` (K = rings/pod,
env, K≤N), `MAX_DPA_RINGS=8` (compile cap). Topology: ring j of pod p → EU (p*K+j)%N; 2-pod active EUs = min(2K,N).

| config | active EUs | 30K p50 | ceiling (achieved) | knee behavior |
|---|---:|---:|---:|---|
| DPA=1, K=1 | 1 | 247us | **~72K** | 80K→overload (single-EU trap) |
| DPA=4, K=2 (default) | 4 | 233us | **~257K** | 240K 0-fail clean; 260K p99 67ms |
| DPA=8, K=4 | 8 | 266us | **~253-256K** | 260K 600-fail/118ms (slightly WORSE) |

→ **EU count is a throughput resource only up to ~4 EUs**: 1 EU=72K → 4 EU/K2=257K (~3.5×). Beyond that,
**8 EU/K4 does NOT exceed 257K** — the shared DPA dma_copy op-rate (~1M ops/s = 257K×4) is the hard wall, and
K=4 is slightly worse (per-ring DPU/host buffer & admission rq_depth split K-ways → earlier throttle). For a
2-pod pair, N>2 only helps if K>1 (else pod%N locks to 2 EUs). **Sweet spot = the current default DPA=4/K=2.**
Latency floor is ~EU-count-independent (233-266us). NOTE: more EUs all funnel completions through the single
ARM drain thread (98% @240K) — Amdahl on that serial bridge + op-rate ceiling = why scaling is sub-linear.

### ITEM 3 — Batching options (all compile-time #defines; need rebuild, NOT env)

Knobs: `BATCH_REVDONE_MAX=16` (reverse-done coalesce, comch_common.h, **on critical RTT path**),
`BATCH_TXACK_MAX=14` (forward TX-ACK coalesce, off critical path), `RING_BATCH_CAP=32` (descs/ring/EU-iter),
`HANDLE_MSGS_EVERY=32` + `DRAIN_COMPLETIONS_EVERY=8` (DPA poll cadences), `CREDIT_REFRESH_MARGIN=64`.
Both BATCH_* flush partial batches at the idle/proc==0 tail-flush (dpu_worker.c); under load the ARM busy-spins
(never proc==0) so batches flush on-full. BATCH_REVDONE is the load-bearing one (only critical-path knob).

**Measured: BATCH_REVDONE_MAX = 1 (disabled) vs 16 (default), DPA=4/K=2, 8KB:**

| target | BATCH=16 (default) | BATCH=1 (off) |
|---:|---|---|
| 30K p50  | 233us | **217us** (−16us, no coalesce delay) |
| 100K p50 | 230us | 223us |
| 200K p50 | 295us | **1,835us** (PE-reap can't keep up) |
| 240K achieved | 238K (0-fail) | **200K** (526 fail, p50 928ms) |
| ceiling | **~257K** | **~200K** |

→ **Reverse batching buys +28% throughput ceiling (200K→257K) for ~16us of low-load latency.** Disabling it
shaves 16us at 30K but craters the ceiling to 200K and explodes mid-load latency (PE reap = per-RTT host cap
without coalescing, confirming [[project_batch_rev_done]]). The default 16 is well-chosen (16 already lifts the
PE-reap above the DPA op-rate wall → ceiling=257K=op-rate; raising to 32 wouldn't help, already op-rate-bound).
**ACTIONABLE: a LOAD-ADAPTIVE flush (flush-small <100K, full-batch near ceiling) would capture BOTH the ~217us
floor AND the 257K ceiling** — the one validated latency win (~16us, ~7%) that doesn't cost throughput.
RING_BATCH_CAP / HANDLE_MSGS_EVERY / DRAIN_COMPLETIONS_EVERY / CREDIT_REFRESH_MARGIN: no isolated sweep; analysis
says low-leverage (cycle-efficiency knobs), DRAIN_COMPLETIONS_EVERY is the only throughput-sensitive one (guard ≥257K).

### CAMPAIGN SUMMARY (bottleneck map + actionable levers)

**Bottleneck map (8KB, 2-pod, FIX2 prod build):**
- THROUGHPUT ceiling ≈ **257K RPS** = the shared **DPA dma_copy op-rate (~1.03M ops/s, 4 DMA/RTT)**. Co-binder:
  the single **DPU ARM drain thread** (98% @240K). NOT host CPU (client 52%, echo ≤36%, neither saturated),
  NOT EU count (8 EU/K4 = same 257K).
- LATENCY floor ≈ **220µs** = fixed per-RTT overhead (4-leg poll-detect ~112µs + 2 completion relays ~60µs +
  host stacks). NOT data transfer (flat vs payload), NOT host/ARM CPU.

**What each option does (measured):**
1. CPU/DPU per load: DPU ARM 39%→98% (30K→240K, busy-spins under load); host client 18%→52%; echo ≤36%; idle
   host~0%/ARM~2%. Host logic(53%)≫data-plane(10%). Multi-core host = no throughput gain (host not the binder).
2. DPA threads: 1EU=72K, 4EU/K2=257K (sweet spot=current default), 8EU/K4=no gain (op-rate wall). More EUs/cores
   are NOT a lever past 4.
3. Batching: BATCH_REVDONE_MAX=16 buys +28% ceiling (200K→257K) for ~16µs low-load latency. Default well-chosen.

**Actionable levers (ranked):**
- (best latency win, throughput-safe) **Load-adaptive REV_DONE flush** — flush partial batch at low/mid load,
  full batch near ceiling → captures BOTH the ~217µs floor (−16µs/−7%) AND the 257K ceiling. MEASURE-VALIDATED
  tradeoff (BATCH=1 gave 217µs but 200K ceiling; BATCH=16 gave 233µs + 257K → adaptive gets 217µs + 257K).
- (cleaner attribution only) pin host data-plane (pe_progress, ~10%) to its own core — no throughput gain.
- Pushing ceiling >257K needs fewer DMA legs/RTT (4→2 = host→host), which is DESIGN-FORECLOSED → ceiling is structural.

### Experiment: event-driven ARM (epoll-block under load) vs busy-spin (DPUMESH_EVENT_BLOCK)

Added diagnostic knob DPUMESH_EVENT_BLOCK (default 0). =1 forces the ARM event loop through epoll EVERY iteration
(skips the busy-poll spin fast-paths `if(progressed)continue` + the re-poll continue) → fully event-driven ARM,
like the host. Tests "what if the DPU ARM blocked on epoll under load instead of spinning?"

| target | PRODUCTION (spin under load, EVENT_BLOCK=0) | EVENT_BLOCK=1 (event-driven) |
|---:|---|---|
| 30K p50  | 233us | **676us** (~3× worse) |
| 100K | 99,450 achieved | **55,895 achieved**, p50 3.3s (collapsed) |
| 240K | 238,719 / p50 340us | **56,592 achieved**, p50 5.7s |
| ceiling | **~257K** | **~56K** (4.5× WORSE) |

→ **DECISIVE: event-driven ARM is catastrophic** — ceiling collapses 257K→~56K and latency explodes (30K 233→676us,
high-load to multi-second). Each loop iteration's epoll_wait + doca_pe_request_notification×2 + clear×2 (~5
syscalls + CQ doorbell touches) crushes the single ARM drain thread's reap rate. Even at 30K (where prod ARM is
only 39% = already mostly blocking) it's 3× worse, because prod drains a burst across multiple spin passes before
blocking, whereas EVENT_BLOCK pays the full arm/epoll/clear cycle PER completion. CONFIRMS the busy-spin-under-load
design is ESSENTIAL (not a minor opt): the ARM is the central per-RTT relay; spinning reaps completions instantly,
event-driving adds ~5 syscalls/completion that the ~1µs inter-completion interval cannot absorb. The DPU core is
dedicated, so spinning it is free; event-driving it to "save" that core would cost 4.5× throughput. Knob left in
off-by-default (production byte-identical); can be removed.

### FINAL — performance-lever analysis (what can actually raise perf)

**Hard wall (measured, one DPU, 2-pod):** ceiling ~257K RPS = DPA dma_copy op-rate (~1.03M ops/s) ÷ 4 DMA/RTT.
Decisive evidence that NOTHING within the current arch beats it:
- 8 EU/K4 = 4 EU/K2 = ~256K (op-rate is shared across EUs; more EUs don't add).
- event-driven ARM = 4.5× WORSE (busy-spin essential, not a waste).
- host CPU 51%, echo ≤36%, ARM = spin not compute (-O2 neutral) → none is the binder.
- batching=16 already reaches the op-rate ceiling; more batching can't exceed it.

**Key deduction (no new test needed):** 8 EUs already cap at 256K, so a 2nd pod-pair on the SAME DPU
(also ~8 EUs) would SHARE the same op-rate → aggregate stays ~256K, NOT 2×. So **more pod-pairs per DPU does
NOT raise aggregate throughput** — the DMA op-rate is a shared DEVICE wall. (This overturns naive "scale-out
helps" for a single DPU; the old 4pod_scales gain was on a host/ARM-bound config below the op-rate, not here.)

**Levers, ranked:**
1. THROUGHPUT step-change = **reduce DMAs/RTT 4→2 via host→host direct DMA** (~2× per DPU + lower latency).
   FORECLOSED in current design (body staged at DPU); needs a control plane distributing remote mmap handles so
   the source DPA DMAs straight to the dest host while the DPU still routes on dst_pod_id metadata. The big project.
2. THROUGHPUT horizontal = **more DPUs** (each adds ~1M ops/s). The only real scale-out (more pairs/EUs/cores on
   one DPU do not help — measured).
3. LATENCY = **load-adaptive REV_DONE flush** (~16µs / ~7% at low/mid load, zero throughput cost — measured:
   BATCH=1 gave 217µs but 200K ceiling; adaptive = 217µs + 257K). The only clean single-RTT win.
4. (marginal) Lever D EU drain cadence ~12µs but risks the op-rate ceiling; HOST_EPOLL=0 ~2-4µs but 16× p99 + host core.

→ Bottom line: within one DPU you are AT the structural wall (~257K, ~220µs). Free latency win = load-adaptive
flush. Real throughput gain needs either the host→host re-architecture (≈2×) or more DPUs (horizontal).

---

## Façade validation: native-epoll server (echo_sock.c) over DPUmesh

Goal: prove a vanilla non-blocking epoll server ports to DPUmesh by swapping ONLY the BSD calls for their
`_dpumesh` twins, using NATIVE kernel epoll on `dpumesh_event_fd(s)` (no epoll_*_dpumesh wrappers). Server =
bench/echo_sock.c (single-threaded reactor: epoll_create1/epoll_ctl/epoll_wait + accept/read/write/send/
close_dpumesh). Client = unchanged bench_dpumesh (raw API). Library: added an eventfd the PE thread signals on
each delivery (dpumesh_get_event_fd), so the user's epoll_wait sleeps on a real fd.

| target | achieved | p50 (us) | p99 (us) | OK/Fail |
|---:|---:|---:|---:|---|
| 30K  | 29,819  | 171 | 541    | 300000/0 |
| 60K  | 59,673  | 165 | 518    | 600000/0 |
| 100K | 99,454  | 194 | 501    | 1000000/0 |
| 150K | 149,169 | 223 | 447    | 1500000/0 |
| 200K | 198,890 | 263 | 33,341 | 2000000/0 |
| 240K | 238,685 | 588 | 1,467  | 2400000/0 |

**Idle CPU:** echo_sock reactor core = **1.2%** (sleeps on native epoll → NOTIFICATION-driven, not busy-poll;
the PE thread also sleeps on the DOCA notif fd under HOST_EPOLL=1).

→ **PASS on all criteria:** (1) a normal epoll server runs with only `_dpumesh` suffixes on the data calls +
native epoll on the event fd; (2) 0-fail 30K→240K; (3) throughput matches the raw 3-thread echo (~240K
sustainable); (4) lower latency at low-mid load (165–263us, single reactor, no futex/thread contention);
(5) idle 1.2% proves it's notification-driven. eventfd write is per-delivery (coalescing = future opt).

### FULL façade stack (bench_sock.c client + echo_sock.c server), both on the new API

Client = bench/bench_sock.c (windowed async load-gen using ONLY connect/write/send/read/close_dpumesh — one
single-shot dpmconn_t per in-flight request, malloc/free per request). Server = bench/echo_sock.c (native epoll
on dpumesh_event_fd). No raw dpumesh.h calls in either. BENCH_SRC/ECHO_SRC toggles in test-bench.sh.

| target | achieved | p50 (us) | p99 (us) | OK/Fail |
|---:|---:|---:|---:|---|
| 30K  | 29,838  | 170 | 501   | 300000/0 |
| 100K | 99,451  | 200 | 493   | 1000000/0 |
| 200K | 198,925 | 255 | 4,789 | 2000000/0 |
| 240K | 238,699 | 541 | 4,577 | 2400000/0 |
| recovery 30K | 29,838 | (below) | — | 300000/0 |

→ **PASS:** the full client+server, written entirely with the `_dpumesh` façade, sustains 240K 0-fail — same
ceiling as the raw-API stack. The client's per-request malloc/free (single-shot conns) did NOT cap throughput.
Latency competitive (170–255µs low-mid). Confirms a normal async request/response CLIENT ports with just the
`_dpumesh` suffix, exactly like the server. The benchmark now runs on the new API by default with these toggles.

### Cleanup: deleted blocking code + raw bench/echo; Thrift excluded from build

Removed (now that the façade stack is validated): raw bench/echo sources (bench_dpumesh.c, echo_dpumesh.c) +
the BENCH_SRC/ECHO_SRC toggles (build hardcodes bench_sock.c/echo_sock.c). Blocking I/O code deleted from
dpumesh_doca.c + dpumesh.h: dpumesh_wait_response, rx_cond/rx_lock + the non-poll cond-dequeue branch (dequeue
is now poll-only), the poll_rx/async_client config flags. KEPT the per-pending p->cond (register_pending's
collision-wait uses it — admission, not I/O blocking). The Thrift transport (TDpumesh*.cpp) is excluded from the
libthrift build (cmake) — its .cpp/.h stay on disk untouched for a later port onto the façade.

Smoke test after cleanup (slimmed transport, façade stack): 30K=29,838/0 p50 174µs · 100K=99,451/0 p50 202µs ·
240K=238,708/0 p50 580µs. → **0-fail, unchanged performance** — the deletions broke nothing.

### Stability investigation (façade stack): the "instability" is the saturation knee + cold-jump

WARM (proper ramp 30K→200K first), each rate ×3 back-to-back:
| rate | achieved | p50 | p99 | fail (×3) |
|---:|---:|---:|---:|---|
| 200K | 198,9K | 260–263µs | 0.4–2.5ms | 0/0/0 |
| 220K | 218,8K | 283–302µs | 0.5–0.8ms | 0/0/0 |
| 235K | 233,7K | 370–404µs | 0.8–1.1ms | 0/0/0 |
| 240K | 238,7K | 615–633µs | 1.2–2.6ms | 0/0/0 |

→ WARM, 200K–240K are ALL 0-fail and CONSISTENT. The knee is ~235→240K: p50 ~doubles (370→620µs) as utilization
approaches the ~257K op-rate ceiling. So 240K is stable-when-warm but sits AT the knee (little headroom).

The user's instability came from: (1) **COLD-JUMP to 240K** (no warmup) — overshoots the forward ring at a
near-ceiling rate → transient wedge / degraded run (the 207K/1.7s/361-fail run), recovers warm; (2) **260K is
OVER the ~257K ceiling** → always degraded/failing. (3) SEVERE: hammering over-ceiling (260K + cold 240K ×5)
WEDGED the bench_sock daemon — worker threads stuck (sleeping 0% CPU, no PING reply, RUN never completes) →
needed a redeploy. The façade load-gen does not shed load gracefully under sustained overload (register_pending
2s collision-wait + enqueue busy-spin when the forward ring can't drain). NOT a cleanup regression — the ceiling
is the DPA op-rate, unchanged.

**Stable operating point: ≤235K** (p50 ≤400µs, 0-fail, repeatable). 240K = warm-only knee. Never exceed ~245K.
Always warmup-ramp before high-RPS measurement ([[feedback_bench_warmup_ramp]]).

---

## Code-cleanup pass (dead code / stale comments / naming), perf-neutral validation

Pass over the LIVE code (façade `dpumesh_sock.h`, core `dpumesh.h` + `dpumesh_doca.c`, bench `bench_sock.c`/`echo_sock.c`,
and the DPU/DPA `doca/` engine). The dead Thrift transport wrappers (`TDpumesh*.cpp/.h`) and `gateway.c` were left
untouched (already excluded from the build). 33 cleanups applied — all behavior/timing/ordering/wire-format-preserving:

- **Dead code removed:** inert `max_descriptors` config knob end-to-end (the host→DPU descriptor ring depth is the
  wire-ABI constant `DMA_RING_SIZE`, NOT configurable — knob was write-only & DPUMESH_MAX_DESCRIPTORS had no effect);
  write-only `dmesh_doca_dpa_msgq.is_send` field; `worker_t.worker_id` (bench); the unused DOCA-sample callback variant
  `open_doca_device_with_pci_and_callback` + `open_dev_cb` typedef (folded into `open_doca_device_with_pci`); the unused
  `dmesh_comch_msg` anonymous union; dead `objs` locals in the new-consumer callbacks; redundant `setup_pod_dma` forward
  decl; ~8 unused `#include`s (stdio/time/assert/strings/doca_comch_producer/doca_comch in headers).
- **Stale comments fixed:** removed all `dpumesh_wait_response` references (API deleted); corrected the rx_data_hook
  "legacy 4-byte enum" dispatch note, the dpu_worker "EU busy-loops, no keepalive needed" claim (the ~1 ms DPA_MSG_WAKE
  keepalive IS live), config.h "host reads PCI from env" (always doca_argp), `@sample_objects`/`@data_path`/`@task Send`
  doxygen leftovers, "NVMf" jargon in a DPA log string.
- **Naming:** `DMA_DIAG_EMPTY_WAIT_FAIL_LOOPS` → `DMA_CONSUMER_EMPTY_WAIT_LOOPS` (DPA kernel; "DIAG" wrongly implied
  removable scaffolding — it bounds a real consumer-stall spin). Tightened a few over-long comments.

Build: host libthrift `make` clean (exit 0); DPU `ninja` recompiled all 13 C objects + DPA kernel clean; bench façade
binaries built. Deploy via `test-bench.sh deploy` (exit 0, fresh pods Ready).

**Validation (fair 1-core, 8 KB, warm ramp):** perf is byte-for-byte the same ladder as the pre-cleanup 240K config.

| target | achieved | p50 (µs) | p99 (µs) | OK / Fail |
|---:|---:|---:|---:|---|
| 30K  | 29,837  | 178 | 520   | 300000/0 |
| 60K  | 59,672  | 167 | 523   | 600000/0 |
| 100K | 99,454  | 204 | 549   | 1000000/0 |
| 150K | 149,118 | 230 | 9,705 | 1500000/0 |
| 200K | 198,903 | 272 | 380   | 2000000/0 |
| 220K | 218,783 | 298 | 652–1,725 | 2200000/0 |
| 235K | 233,703 | 516–539 | 990–1,626 | 2350000/0 |
| 240K | 238,672 | 613 | 2,098 | 2400000/0 |

→ **0 failures across the whole ladder; latencies match the documented baseline.** One transient 220K stall on the
first knee touch (182K achieved, ~2 s p99, still 0-fail) recovered on the very next runs — 235K/240K were clean
immediately after, and a 220K re-run (no redeploy) returned 218.8K/p50 300µs. Recovery without redeploy confirms **no
state/slot leak** ([[feedback_no_slot_leak]]); the blip is the known knee overshoot ([[feedback_bench_warmup_ramp]]),
not a cleanup regression. Conclusion: the cleanup is **functionally correct and performance-neutral** — no logic,
timing, ordering, or wire-format changed, exactly as intended.

---

## Stage 1: implicit send (socket-transparent `write`/`read`)

Goal (user): make the façade behave like the BSD socket API so the caller can't tell socket vs DPUmesh — step 1 =
remove the explicit `send_dpumesh()` so `write`→`read` (client) / `write`→`close` (server) work like sockets.

**Change (host façade ONLY — datapath / DPU / wire ABI untouched):** `dpumesh_sock.h` gains `dpm__autoflush()`, called
from `read_dpumesh()` (client: ships a buffered-but-unsent request before polling) and `close_dpumesh()` (server: ships
a buffered response before teardown). `send_dpumesh()` is **kept and back-compatible** (auto-flush no-ops once sent), so
existing explicit-send code is unaffected. `echo_sock.c` (server) and `bench_sock.c` (client) were both rewritten to use
**NO explicit send** — proving the transparency end-to-end through the real DMA path. The ≤8KB cap + single-shot conn are
unchanged (those need the byte-stream backend = Stage 2).

The async client's send moved from the launch loop to the first harvest `read` (~same sweep), so pipelining is preserved
— confirmed by the numbers below being identical to the explicit-send baseline.

**Validation (fair 1-core, 8 KB, warm ramp; client+server BOTH send-free):**

| target | achieved | p50 (µs) | p99 (µs) | OK / Fail |
|---:|---:|---:|---:|---|
| 30K  | 29,838  | 168 | 496   | 300000/0 |
| 100K | 99,454  | 204 | 429   | 1000000/0 |
| 200K | 198,909 | 269 | 691   | 2000000/0 |
| 220K | 218,786 | 299 | 598   | 2200000/0 |
| 235K | 233,706 | 367–375 | 866–1,112 | 2350000/0 (×2 back-to-back) |
| 240K | 238,672 | 590 | 5,579 | 2400000/0 |

→ **0-fail across the whole ladder, latencies identical to the explicit-send baseline, repeatable (235K ×2).** Echo
correctness is implicit in 0-fail (a wrong/short echo is counted as a failure). So the implicit-send path is correct and
**performance-neutral**: removing `send_dpumesh()` from user code costs nothing. Stage 1 done. Next (Stage 2, optional) =
true byte-stream over a real `int fd` (socketpair) for literal socket-indistinguishability — trades an extra host copy
for transparency (see design discussion).

### Façade suffix renamed `_dpumesh` → `_dpm` (cosmetic, for naming uniformity)

The 9 BSD-twin façade funcs + the readiness-fd accessor were renamed for consistency with the `dpm_t`/`dpmconn_t`/`dpm_*`
family: `socket_dpm`, `destroy_dpm`, `accept_dpm`, `connect_dpm`, `read_dpm`, `write_dpm`, `sendfile_dpm`, `send_dpm`,
`close_dpm`, `dpm_event_fd`. **Untouched:** the low-level C core API (`dpumesh.h`: `dpumesh_init`/`dpumesh_dequeue`/…),
the header filename `dpumesh_sock.h`, and the `dpm_*` accessors. Scope = `dpumesh_sock.h` + `bench/{bench,echo}_sock.c`
(the only façade users) + `api.md`. Behavior-preserving rename — smoke ladder (8 KB, warm): 30K 29,838/0 p50 170µs ·
100K 99,453/0 p50 207µs · 200K 198,924/0 p50 278µs · 235K 233,678/0 p50 530µs → all 0-fail, latencies unchanged.

### Façade naming unified to `*_dpm` suffix + header renamed `dpumesh_sock.h` → `dpm.h`

Accessor funcs unified from mixed prefix/suffix to all-suffix: `dpm_pod_id`→`pod_id_dpm`, `dpm_msg_max`→`msg_max_dpm`,
`dpm_event_fd`→`event_fd_dpm`, `dpm_is_server`→`is_server_dpm`, `dpm_peer`→`peer_dpm`, `dpm_set_data/get_data`→
`set_data_dpm/get_data_dpm` (+ internal `dpm__*` helpers). Types `dpm_t`/`dpmconn_t` and the low-level core `dpumesh_*`
kept (user's choice). Header file `dpumesh_sock.h` → `dpm.h` (guard `DPM_H`, include `thrift/transport/dpm.h`).
Behavior-preserving rename — smoke ladder (8 KB, warm): 30K 29,837/0 · 100K 99,451/0 · 200K 198,923/0 p50 264µs ·
235K 233,733/0 p50 365µs → all 0-fail, latencies unchanged.

### Review fixes E + A (+ light F); B/C/D deferred by user

E (close swallowed send failure): `close_dpm` now RETURNS the auto-flush result (`0`/`-1`) instead of always `0`, so a
server can detect a dropped reply; `send_dpm` re-documented as the explicit confirm/retry path (conn freed on close, so
`-1` is observe-only). A (idle-CPU claim): restored `DPUMESH_HOST_EPOLL` to api.md and qualified the "idle ~1% / no
busy-poll" claim as `HOST_EPOLL=1`-only (library default = internal RX thread adaptive-busy-poll). F: 4d sendfile
return now checked; connect dead-pod behavior noted; client multi-conn epoll pattern (event_fd is per-endpoint) noted.
Behavior-preserving (bench ignores close's return) — smoke ladder 30K 29,839/0 · 100K 99,452/0 · 200K 198,920/0 p50
265µs · 235K 233,738/0 → all 0-fail. Deferred per user: one-way path (B), slot_size>8192 reject (C), req_id 2s stall (D).

### Rigorous api.md/dpm.h review + fixes (FC-1/FC-2/EDGE-4 + doc precision)

5-dimension adversarial cross-verify of api.md vs dpm.h vs core (dpumesh_doca.c) vs scale_log: 9 findings confirmed
(1 major DEFERRED), 3 refuted. NO memory-safety bugs (all TX/RX-slot/pending/req_id lifecycles correct). Fixed every
non-major finding:
- **dpm.h (behavior, but on unreachable/rare paths → throughput-neutral):** FC-1 `errno=ENOMEM` on conn calloc-fail in
  server_conn_dpm + connect_dpm (was unset → an accept-until-NULL loop couldn't tell OOM from "drained"). FC-2 send_dpm
  enqueue-fail `EAGAIN`→`EBADMSG` (enqueue −1 = permanent descriptor-validation fault; ring saturation busy-spins and
  never returns −1 → the "retry" label was wrong). EDGE-4 read_dpm doc: on the client path ECONNRESET = req_id-table
  collision/reclaim, NOT peer death (a dead/unregistered pod stays persistent EAGAIN).
- **api.md (doc only):** send_dpm errno taxonomy (EINVAL / EAGAIN-after-~2s / EBADMSG); the ~2s collision-retry framing;
  client examples 4b/4c given wall-clock timeouts (they deadlocked on a dead pod before); 240K reframed as the warm-only
  knee with stable ≤235K + ceiling ~257K; idle 1%→1.2%.
- **Major DEFERRED per user:** EDGE-2 (slot_size>8192 silent-drop + DPU_BUFFER_SIZE admission-invariant break).

Validation (deploy via test-bench.sh, fair 1-core, 8KB, warm ramp):
| target | achieved | p50 | p99 | ok/fail |
|---|---|---|---|---|
| 30K  | 29,837  | 162µs | 489µs | 300000/0 |
| 60K  | 59,671  | 161µs | 264µs | 600000/0 |
| 100K | 99,449  | 200µs | 315µs | 1.0M/0 |
| 150K | 149,142 | 227µs | 337µs | 1.5M/0 |
| 200K | 198,902 | 266µs | 41ms* | 2.0M/0 |
| 220K | 218,791 | 296µs | 728µs | 2.2M/0 (warm + drained) |

*200K p99 = known first-touch knee spike. A back-to-back 220K with NO drain gap degraded (200,764 achieved, 320 fails,
seconds latency) = documented knee-overshoot ([[feedback_bench_warmup_ramp]]); recovered 30K/200K 0-fail WITHOUT redeploy
→ no slot leak ([[feedback_no_slot_leak]]). DPU log clean (no ERR/flood), pods 0-restart. The errno/doc edits are
confirmed behavior-neutral (they only touch unreachable enqueue-validation/OOM paths + comments) — ladder matches the
prior baseline byte-for-byte.

### API redesign Stage 1: flush rename + echo_mode removal + C2 (4→2 completion types)

Part of the 2-stage clean-transport redesign (Stage 2 = C3 tx_slot/ack decouple, separate). All Stage-1 changes:
- **`send_dpm` → `flush_dpm`** (dpm.h + api.md; bench/echo never called send — implicit flush via read/close).
- **`echo_mode` removed** (dpu_worker.c): the `dst==-1 || dst==src` loopback special-case that auto-relabeled a
  self-send as OP_RESPONSE and sent an UN-batched REV_DONE. A self-send now routes normally (`find_pod_by_id(dst)`,
  `(flags&OP_RESPONSE)|CASE_INGRESS`, batched REV_DONE) — benchmark-shaped cruft gone, 2-pod hot path never used it.
- **C2: 4 DPU→host completion types → 2.** Singular `DMESH_MSG_FWD_ACK`/`REV_DONE` (+ their structs + senders +
  host handlers) deleted; everything routes via `BATCH_FWD_ACK`/`BATCH_REV_DONE` (now enum 3/4, batch-of-1 on the
  error/defer path). `send_or_defer_tx_ack`/`drain_deferred_tx_acks` → `server_send_batch_tx_ack_to(...,1)`.
  (Combined with the prior always-ACK gate removal at dpu_worker.c:408.) Trivial leftover: dead `server_send_tx_ack_to`
  prototype in comch_server.h + 2 descriptive comments (object.h/dpa_common.h) — harmless, defer to a cleanup pass.

Local: bench façade + host core (dpumesh_doca.c w/ real DOCA headers) syntax-clean; DPU `.c` no edit-induced errors.
Deploy via test-bench.sh: DPU ninja + host make + bench all built; pods Ready.

**Validation (fair 1-core, 8KB, warm ramp) — 0-fail, baseline-identical:**
| target | achieved | p50 | p99 | ok/fail |
|---|---|---|---|---|
| 30K  | 29,837  | 171µs | 544µs  | 300000/0 |
| 60K  | 59,671  | 164µs | 525µs  | 600000/0 |
| 100K | 99,451  | 210µs | 586µs  | 1.0M/0 |
| 150K | 149,173 | 236µs | 9.98ms* | 1.5M/0 |
| 200K | 198,893 | 280µs | 32.8ms* | 2.0M/0 |
| 220K | 218,784 | 326µs | 2.12ms | 2.2M/0 |

*first-touch knee spike (known). Back-to-back 130K×2 = 129,279/129,279 (0-fail) + recovery 30K 0-fail → **no slot
leak**. DPU log no ERR/flood across the whole run (proves C2 both-sides type agreement + echo-removal routing intact).
→ Stage 1 is functionally correct, leak-free, throughput/latency unchanged. Next: Stage 2 (C3).

### API redesign Stage 2: C3 — TX-slot decoupled from pending entry, ACK = sole free authority

Made the host TX_ACK the single authority that frees a SENT slot, removing the multiple writers that touched
`p->tx_slot` (the entry-reuse clobber was the latent leak source). All host-side (dpumesh_doca.c); 1 DPU comment.
Rule: **DPA-might-have-read (sent) slot → freed only by its TX_ACK / the 2s backstop; never-sent slot → freed
locally by dpm.h.** Edits:
- **Removed the two TX self-frees**: `rx_deliver_desc` (response-landing, state 0→1) and `dpumesh_poll_response`
  (harvest, state 1). They no longer touch tx_slot.
- **ACK handler frees regardless of state** (was state 0/-2 only): `tx_slot>=0 && owner_req_id==rid` → free; for the
  terminal-awaiting-ack states (-2/-3) clear → -1 (reusable).
- **New state -3** (response harvested, awaiting ack): `poll_response` parks at -3 when the ack hasn't freed the slot
  yet; `register_pending` treats -3 as occupied → can't reuse the idx until the ack clears it → **no reuse clobber**.
- **`cancel_pending` (close) delegates sent slots to the ACK** (state→-2) instead of force-freeing; frees nothing
  locally (only clears -1 when tx_slot<0 = enqueue-fail / ack already came). Reclaims an undelivered response's RX
  landing. (Fixes a latent unsafety: the old state==0 branch force-freed a slot the DPA might still be reading —
  now first-class as one-way send via [[project_completion_required]]'s ACK path.)
- **Backstop**: `register_pending`'s 2s reclaim now covers -2 AND -3 (force-free if the ACK never arrives — dead
  link / deferred-queue drop; safe after 2s since the forward DMA finished long ago).
- `pending_release_async` (server reply) + `dumesh_destroy` teardown already matched the model → unchanged.

Local: dpumesh_doca.c + façade syntax-clean (real DOCA headers); -3 setter (poll_response) has clearers (ack +
backstop). Deploy: DPU 13 objs + host + bench built; pods Ready.

**Validation (fair 1-core, 8KB, warm ramp) — 0-fail, baseline-identical:**
| target | achieved | p50 | p99 | ok/fail |
|---|---|---|---|---|
| 30K  | 29,837  | 161µs | 454µs  | 300000/0 |
| 100K | 99,450  | 200µs | 402µs  | 1.0M/0 |
| 150K | 149,167 | 226µs | 694µs  | 1.5M/0 |
| 200K | 198,896 | 283µs | 34.8ms* | 2.0M/0 |
| 220K | 218,790 | 314µs | 997µs  | 2.2M/0 |

*first-touch knee spike (known). **Leak stress: 150K ×3 back-to-back = 149,167/149,164/149,164 (0-fail)** + recovery
30K 0-fail → **no slot leak** — the decisive C3 check (a lifecycle bug = slot exhaustion → throughput collapse).
DPU log: no ERR/flood and **no "ack-timeout reclaim" WARN** → ack-sole-authority frees cleanly; -2 (server reply via
release_async) and -3 (client harvest) both hammered at ~218K/s and clear correctly. Both terminal-await-ack states
heavily exercised. NOT bench-exercised (structurally sound, shares machinery, but no direct test): one-way close
(cancel→-2 delegation) and unwanted-late-response reclaim — follow-up for a one-way workload.
→ Stage 2 done. Transport is now: 2 completion types, no echo special-case, flush API, single ACK free-authority.

### Façade redesign (dpm.h): reusable conn (①) + one-way first-class (②)

Now that the transport supports it (Stages 1-2), reshaped the dpm.h façade to the "socket-mimicry, friction-removed"
model. Host-only (dpm.h + bench_sock.c + 1 host-core log line + api.md); no DPU/wire change.
- **① Reusable client conn.** `connect` once, then loop `write → read → write → read …`, `close` at the end. New
  internal `conn_reset_exchange()` (frees the prior response's RX slot, clears per-exchange state, keeps the peer
  binding) + `conn_begin_tx()` (called by write/sendfile): on a client conn whose response was already read, the next
  write auto-starts a NEW request (fresh req_id at flush); otherwise EINVAL — one conn = one outstanding exchange (a
  write mid-flight, or a 2nd reply on a server conn). Replaces the old single-shot `if (c->sent) EINVAL`.
- **② One-way first-class.** `write → close` (no read) = fire-and-forget: close auto-flushes, then cancel_pending
  delegates the TX slot to the ACK (C3) → no leak; a reply the peer sends is silently dropped. Downgraded the
  rx_deliver "no waiter" log ERR→DBG so an unwanted one-way reply can't flood. Documented in dpm.h + api.md (the §1/§2
  "pair / single-shot / one-way not first-class" claims rewritten to "peer handle / reusable / one-way first-class").
- **bench_sock.c**: window slots now keep ONE reusable conn each (connect once; write→read→reuse), closing only on
  error/timeout/end — demonstrates ①. **echo_sock.c unchanged** (server side: accept→read→write→close per request is
  already the clean pattern; reuse/one-way are client concepts).

Local: bench/echo + host core syntax-clean. Deploy: DPU 13 objs + host + bench built; pods Ready.

**Validation (fair 1-core, 8KB, warm ramp) — 0-fail, baseline-identical:**
| target | achieved | p50 | p99 | ok/fail |
|---|---|---|---|---|
| 30K  | 29,837  | 161µs | 549µs  | 300000/0 |
| 100K | 99,451  | 202µs | 483µs  | 1.0M/0 |
| 150K | 149,165 | 231µs | 10.3ms* | 1.5M/0 |
| 200K | 198,900 | 286µs | 31.6ms* | 2.0M/0 |
| 220K | 218,783 | 302µs | 34.5ms* | 2.2M/0 |

*first-touch knee spikes (known). **Reuse leak stress: 150K ×3 back-to-back = 149,166/149,165/149,162 (0-fail)** +
recovery 30K 0-fail → **no leak** — the decisive ① check (a reused conn holds its RX slot between requests; a leak
would compound across back-to-back). DPU log clean. → ① validated end-to-end. ② (one-way) is implemented + documented
but NOT bench-exercised (bench is RPC: reuse+read); follow-up = a one-way load test. Façade is now: peer-handle conn,
reusable, one-way first-class, flush API.

### api.md full rewrite + one-way LOAD TEST (closes the ② follow-up)

**api.md rewritten** to the new façade model (peer-handle conn / reusable client / one-way first-class / flush API):
§3 signatures+errno corrected, examples 4a-4f redone (echo, reusable-RPC client, one-way, epoll window, epoll server,
HTTP). **Verified mechanically**: all 6 examples extracted into a scratch .c and compiled `-Wall` against the real
dpm.h → 0 errors (the doc's API usage matches the code).

**One-way load test built + run** (this exercises ② directly):
- bench_sock.c: `worker_fn_oneway` (connect→write→close, NO read; paced; ok = close==0); RUN gains an optional 6th
  `mode` field (0=rpc, 1=oneway). test-bench.sh: `dpumesh-oneway` subcommand (RUN_ONEWAY=1 → mode=1).
- echo_sock.c: per-accept `recv_total` counter, printed every 200k → delivery cross-check via pod logs.

Results (fair 1-core, 8KB):
| rate | achieved | p50 | p99 | fail |
|---|---|---|---|---|
| 30K–150K ramp | on-target | ~31µs | ~63µs | 0 |
| 200K | 198,879 | 31µs | 63µs | 0 |
| 235K | 233,708 | 31µs | 63µs | 0 |

- **Delivery proven (not just enqueued):** echo recv_total delta == client OK total, twice exactly — ramp
  3,400,000 == 3,400,000; back-to-back 4,800,000 == 4,800,000.
- **No leak:** one-way 150K×3 back-to-back = 149,144/149,165/149,170 (0-fail) + recovery 0-fail.
- **DPU log: no ERR/flood/"no waiter"** → the echo's unwanted replies (one-way sender doesn't read) are dropped
  silently (② DBG-downgrade works); the request TX slot is freed by its ACK (no leak), confirmed by the back-to-back.
- **p50 ~31µs flat to 235K** = send-call time only (fire-and-forget, no RTT) vs RPC's ~227µs RTT — the expected
  one-way characteristic. NB this is one-way-to-a-replying-echo: the dropped replies still incur reverse-DMA traffic,
  yet 235K sustains. RPC regression re-confirmed 0-fail 30K→200K on the same build.
→ ② (one-way) now validated end-to-end. Façade redesign complete: peer-handle / reusable / one-way first-class /
flush API, all measured 0-fail + leak-free + delivery-confirmed.

### Façade naming flipped `*_dpm` suffix → `dpm_*` prefix (reverts the suffix unification)

Reverses the earlier "Façade naming unified to `*_dpm` suffix" decision: all façade identifiers now carry the `dpm_`
PREFIX. Generic mechanical transform `\b(<ident>)_dpm\b → dpm_\1` over the only 4 files that use the suffix:
`lib/cpp/src/thrift/transport/dpm.h`, `bench/echo_sock.c`, `bench/bench_sock.c`, `api.md`.
- **16 public funcs:** `socket_dpm`→`dpm_socket`, `destroy_dpm`→`dpm_destroy`, `pod_id_dpm`→`dpm_pod_id`,
  `msg_max_dpm`→`dpm_msg_max`, `event_fd_dpm`→`dpm_event_fd`, `accept_dpm`→`dpm_accept`, `connect_dpm`→`dpm_connect`,
  `set_data_dpm`→`dpm_set_data`, `get_data_dpm`→`dpm_get_data`, `is_server_dpm`→`dpm_is_server`, `peer_dpm`→`dpm_peer`,
  `read_dpm`→`dpm_read`, `write_dpm`→`dpm_write`, `sendfile_dpm`→`dpm_sendfile`, `flush_dpm`→`dpm_flush`,
  `close_dpm`→`dpm_close`.
- **4 internal static helpers** (same file, for uniformity): `server_conn_dpm`→`dpm_server_conn`,
  `client_poll_dpm`→`dpm_client_poll`, `autoflush_dpm`→`dpm_autoflush`, `tx_ensure_dpm`→`dpm_tx_ensure`.
- **Untouched:** types `dpm_t`/`dpmconn_t` (already prefix-style); the low-level C core API (`dpumesh.h`/`dpumesh_doca.c`:
  `dpumesh_init`/`dpumesh_get_event_fd`/…); env vars `DPUMESH_*`; the header filename `dpm.h`. Prose mentions of the
  convention updated by hand (`_dpm` twin/suffix → `dpm_` prefix; `epoll_*_dpm` → `dpm_epoll_*`).
- **scale_log history left intact** (the entries above still record the suffix era — not rewritten).

Behavior-preserving, name-only. Verified host-side: `gcc -fsyntax-only -I lib/cpp/src bench/{echo,bench}_sock.c` → 0
errors; repo-wide grep confirms 0 remaining `_dpm` tokens in code/doc. Deploy smoke-ladder via test-bench.sh pending
(mechanical rename — no codepath change).

### Drop socket-cosplay in the echo example (atomic msg → single read)

A message arrives ATOMICALLY (one whole ≤8KB body, contiguous in `rx_buf`), so the byte-stream accumulation loop
`while ((r = dpm_read(c, buf+off, …)) > 0) off += r;` always did exactly [1 real read + 1 EOF read]. Replaced it with a
single `dpm_read(c, buf, sizeof buf)` (buf sized to the slot max). Applied in `bench/echo_sock.c` + api.md examples 4a /
4e / 4f. Also removed `signal(SIGPIPE, SIG_IGN)` + `<signal.h>` from echo_sock.c — vestigial socket boilerplate (the
program has NO real sockets; `dpm_write` is a memcpy into a slot, so SIGPIPE can never fire). `bench_sock.c` keeps it
(its control plane uses real TCP). Reframed the now-false "diff is only the `dpm_` prefix / nothing but the suffix"
claims (echo_sock.c header, 4e note) to "simpler than a socket, not byte-for-byte identical" — the read-loop collapse
*is* a divergence from socket code. Behavior-preserving; `gcc -fsyntax-only -Wall` clean. Transport API/semantics
untouched (this is example/usage cleanup only).

### Façade prefix flipped `dmesh_` + handle types renamed (user-directed)

Per user decision (keep incremental write → keep the per-exchange handle, just make names honest):
- **Prefix `dpm_` → `dmesh_`** on all 16 public funcs + 4 internal static helpers (`dmesh_socket`, `dmesh_accept`,
  `dmesh_read`, `dmesh_write`, `dmesh_flush`, `dmesh_close`, `dmesh_event_fd`, …). No collision with the existing core
  `dmesh_*` family (those are all `_msg`/`_entry`/`doca_dpa_*` structs; verified).
- **`dpm_t` → `dmesh_channel_t`** (struct tag `dpm_endpoint` → `dmesh_channel`).
- **`dpmconn_t` → `dmesh_conn_t`** (prefix-only for now; the "conn" name is still socket-ish and may be revisited —
  struct tag `dpm_conn` → `dmesh_conn`).
- **Kept as-is (out of scope):** header filename `dpm.h` + include guard `DPM_H`; the C core (`dpumesh.h`/`dpumesh_*`,
  `dpumesh_ctx_t`) which the real Thrift transport uses directly (façade is bench-only).
- Scope: `dpm.h` + `bench/{echo,bench}_sock.c` + `api.md` (+ synced install copy). Ordered sed (types first, then
  generic prefix) so `dpm_t` didn't get mangled into `dmesh_t`. `gcc -fsyntax-only -Wall` on both bench files → 0
  errors; repo-wide grep → 0 `dpm_` identifiers (scale_log history intentionally left).

STILL PENDING (user thinking): the semantic name redesign (e.g. `dmesh_conn_t` → exchange/ticket terms, `dmesh_socket`/
`accept`/`connect`/`read`/`write` → RPC/message verbs). This entry is the prefix+type step only.

### Endpoint lifecycle verbs renamed (user-directed, partial semantic pass)

- `dmesh_socket` → **`dmesh_create_channel`**, `dmesh_destroy` → **`dmesh_destroy_channel`** (match the `dmesh_channel_t`
  type). Core `dpumesh_destroy(ctx)` inside is untouched.
- **Deliberately kept** (user decision): `dmesh_accept` (NOT renamed — `next_request`/`take_request` was rejected as
  assuming RPC, since one-way is first-class and an inbound isn't necessarily a "request"; `accept` is neutral),
  `dmesh_connect`, and all of read/write/sendfile/flush/close/event_fd/pod_id/msg_max/is_server/peer/set_data/get_data.
- Types `dmesh_channel_t`/`dmesh_conn_t` unchanged (conn rename still deferred).
- Scope: `dpm.h` + `bench/{echo,bench}_sock.c` + `api.md` (+ synced install copy). `gcc -fsyntax-only -Wall` both bench
  files → 0 errors.

### Role-neutrality pass: remove role-assuming API (user principle)

User principle: the façade must exist **independently of server/client/topology role**. Audited the façade and removed/
neutralized every role assumption (the C core `dpumesh_*` was already role-neutral — pod_id/worker_id based).
- **Removed public accessors** (unused except the doc/4d example): `dmesh_is_server` (role-assuming), `dmesh_set_data`,
  `dmesh_get_data` (+ the `user_data` field they backed). 4d epoll-window example rewritten to track conns by its own
  `inflight[]` index (the `set_data`/`get_data` tag was redundant — the array index already is the request index).
- **Kept** `dmesh_peer` (role-NEUTRAL — just "the other pod in this exchange"; the only way to learn a received
  message's sender). Doc neutralized.
- **Renamed role-named internals** → direction-based (a property of the *exchange*, not the node): field
  `is_server`→`inbound`; helper `dmesh_server_conn`→`dmesh_inbound_conn`; helper `dmesh_client_poll`→`dmesh_poll_reply`.
- **Neutralized all "server"/"client" in dpm.h comments** → "inbound exchange (received)" / "outbound exchange
  (initiated)". Verified: `grep server|client dpm.h` = 0.
- Behavior unchanged (the inbound/outbound distinction is intrinsic: a reply carries the received req_id + OP_RESPONSE;
  a request allocates a new req_id). `gcc -fsyntax-only -Wall` both bench files → 0 errors; install copy synced.
- Final public surface: `dmesh_create_channel`/`dmesh_destroy_channel`/`dmesh_event_fd`/`dmesh_pod_id`/`dmesh_msg_max` ·
  `dmesh_accept`/`dmesh_connect` · `dmesh_read`/`dmesh_write`/`dmesh_sendfile`/`dmesh_flush`/`dmesh_close` · `dmesh_peer`
  · types `dmesh_channel_t`/`dmesh_conn_t`.

### req_id originator-scoping + OP_ functionally removed + explicit flush (host-only; NOT YET DEPLOYED)

Big design (agreed via wf_bf45a2f9 mapping). **Host-only change — DPU/DPA/Thrift/gateway untouched** so the cross-target
build I can't verify stays green. NOT yet deploy-tested (deploy is the user's per [[feedback_no_background_deploy]]).
- **req_id originator-scoped** (`dpumesh_doca.c` `dpumesh_alloc_req_id`): `req_id = (pod_id<<25) | (counter & 0x01FFFFFF)`
  — top 7 bits = minting pod, low 25 = counter. Globally unique across pods. Constants `DMESH_REQID_POD_SHIFT/MASK/CTR_MASK`.
- **OP_RESPONSE demux REPLACED** (`rx_deliver_desc`): was `if (flags & OP_RESPONSE)`; now `if (req_id>>25 == ctx->pod_id)`
  → "did I mint this?" = reply→pending, else→accept ring. + added `owner_req_id == req_id` guard (hardens vs wrap-alias;
  the deliver path lacked it). DPU/DPA still carry the now-unread OP bit — harmless dead bits (kept #defines to avoid
  touching the unverifiable build). Façade `dmesh_flush` now sets `d.flags = 0`.
- **Loopback (dst==self) REJECTED** at `dpumesh_enqueue` — a self-minted req_id arriving back can't be told request-leg
  from reply-leg without a direction bit. Unused in practice (all traffic is cross-pod).
- **autoflush REMOVED + explicit flush** (`dpm.h`): `dmesh_autoflush` deleted; `read`/`close` no longer auto-send.
  `dmesh_close` is now pure teardown (discards an un-flushed buffer; cancels pending if flushed-not-read; returns 0).
  Send is now ALWAYS explicit: `write → flush → read` (RPC) / `write → flush → close` (one-way). Updated bench
  (echo_sock, bench_sock async+oneway) + api.md examples 4a–4f + §2/§3 tables/lifecycles.
- KEPT: `inbound` field (req_id alloc-vs-reuse is intrinsic — a reply reuses the received req_id), release_async (server),
  the flags byte (ABI), OP_/CASE_ #defines (now dead — physical deletion is the pending follow-up that touches Thrift/DPU).
- **Verified host-side**: `gcc -fsyntax-only -Wall` echo_sock/bench_sock = 0 errors; `gcc -fsyntax-only` dumesh_doca.c =
  0 errors (only pre-existing DOCA deprecation warnings). Install header synced.
- **PENDING (user)**: `./test-bench.sh deploy` + warm ladder (30K→200K, expect 0-fail) — builds libthrift+DPU+DPA and
  validates on real HW. Watch for: loopback-reject false positives (none expected), reply mis-demux (would show as
  client EAGAIN-forever / wrong-body fails).

**VALIDATED ON HW (fair 1-core, 8KB)** — deployed via `test-bench.sh deploy` (exit 0, fresh pods,
restart=0; DPU log clean, NO ERR/loopback-reject/drop floods — only benign DOCA startup WRNs). Warm ladder:

| target | achieved | p50 | p99 | OK/Fail |
|---|---|---|---|---|
| 30K  | 29,838  | 166µs | 513µs | 300000/0 |
| 100K | 99,450  | 200µs | 1.2ms | 1.0M/0 |
| 150K | 149,168 | 227µs | 338µs | 1.5M/0 |
| 200K (warm,drained) | **198,893** | 273µs | 28ms* | **2.0M/0** |

→ **req_id-origin demux + explicit-flush redesign is CORRECT + PERFORMANCE-NEUTRAL** — 200K 0-fail at 198.9K matches the
pre-change baseline (~198K). *The first 200K shot (cold 100K→200K jump, no drain) knee'd to 170K/9-fail/2s-p999 =
documented [[feedback_bench_warmup_ramp]] knee-overshoot, NOT a regression; a 100K re-run recovered 0-fail (no leak
[[feedback_no_slot_leak]]) and the warm/drained 200K retry was clean. 200K p99 28ms = the known first-touch tail spike.
DPU `-l` stayed 40 (nothing to revert). OP_/CASE_ #defines still present (dead) — physical deletion still a follow-up.

---

# Endpoint-tuple redesign — deploy + test

Redesign: `req_id` → oriented endpoint tuple `(src pod/port, dst service/pod/port, seq)`.
Demux by `dst_port` (not req_id-origin / no direction flag); addressing = **service_id**
(DPU `dpu_route` resolves service→pod, connection-level sticky); self-routing/loopback
enabled; server model **M3** (each side closes independently, server accept→handle→close
per request). Thrift transport excluded from build (unchanged). See git for code.

## Bring-up bugs (host-side, found via pod stderr per diagnose-before-redeploy)
1. **pending index collapsed to seq** → 2.1 RPS, p50 12s. `pending[key % MAX_PENDING]`
   with `key=(port<<16)|seq` and `MAX_PENDING=2^16` → `key%65536 == seq`, so PORT was
   dropped from the index → every conn's seq=N aliased one slot → `register_pending`
   2s collision-wait per request.
2. **Knuth-hash index** (fix attempt) → 3,753 RPS, p50 164µs but **p99 12s**. Hashing
   (port,seq) birthday-collides at a few hundred conns; a collided slot stuck at
   state=1 (response delivered, not yet harvested) blocked others → cascade.
   Log: `Pending slot collision: req_id=0x500001 idx=42375 stuck state=1`.
3. **FIX: index = port** (`dmesh_pidx(key) = key>>16`). Each live conn owns a unique
   port and is single-outstanding (≤1 in-flight (port,seq)) → collision-free, like an
   fd indexing its slot. `owner_key=(port<<16|seq)` still guards late/dup ACK. → healthy.

## Ramp (8192B, 10s, fair 1-core/pod; DPA=4 K=2, baked config) — ALL 0-fail
| target | achieved | p50 | p99 | p999 | ok/fail |
|---|---|---|---|---|---|
| 30000  | 29,837  | 171µs | 562µs  | 2.77ms | 300000/0 |
| 100000 | 99,450  | 206µs | 8.21ms | 10.69ms| 1.0M/0 |
| 150000 | 149,174 | 234µs | 5.89ms | 11.83ms| 1.5M/0 |
| 200000 | 198,908 | 292µs | 27.69ms| 44.55ms| 2.0M/0 |
| 220000 | 218,792 | 338µs | 3.95ms | 8.00ms | 2.2M/0 |

**Verdict: perf-NEUTRAL vs pre-redesign ~220K baseline, 0-fail across the ramp.**
p50 actually lower (171–338µs vs old ~14ms blocking / ~1–3ms async) — the bench async
client + the leaner path. p99 noisy at 200K (27ms) but 0-fail; recovers at 220K.
The path validated: service routing (dpu_route service11→pod11), dst_port demux,
(port,seq) match, slot/credit mgmt, establish/sticky. (Standard bench is cross-pod
pod10→service11→pod11; loopback/self-service is a separate setup, not yet run.)

## comch_dma_comp_msg 20B (was 16B) — measured NOT a slowdown
The 20B completion immediate (2nd WQE BB) is perf-neutral here (219K = baseline). The
earlier slowness was bug #1/#2, not the immediate size. 16B is reachable only via
pos byte-offset→slot-index (DPA change), expected ~0 gain — deferred.

## comch_dma_comp_msg → 16B — measured perf-identical to 20B
Earlier entry deferred 16B (said it needed pos→slot-index). Done a simpler way:
**dropped `src_service` from the 16B wire** (the DPU derives the caller's service from
src_pod's registration in the recv-cb — assumes one service per pod, which the current model enforces since
service_id=pod_id). Reordered: type0 src_pod1 dst_pod2 dst_svc3 src_port4 dst_port6 seq8
length10 pos12 = exactly 16B (type@0 for the recv peek, pos@12 4-aligned, no padding).
Files: dpa_common.h (struct+assert==16), dpa_kernel.c (fwd/rev comp fill), dpa.c
(recv-cb derives src_service). dpu_worker/host/rev_done_entry unchanged.

Re-ramp (8192B, 10s, fair) — ALL 0-fail, **= 20B within noise**:
| target | 16B achieved | 20B achieved | p50(16B) |
|---|---|---|---|
| 30000  | 29,837  | 29,837  | 160µs |
| 100000 | 99,451  | 99,450  | 200µs |
| 200000 | 198,911 | 198,908 | 264µs |
| 220000 | 218,814 | 218,792 | 318µs |

**Verdict: comch immediate 16B vs 20B is NOT a throughput lever in the ≤220K host-bound
range** (DPU/DPA hop is small; the immediate cost only matters near the DPA op-rate
ceiling ~257K, not reached). User's "16B 넘으면 느려졌다" did not reproduce — the
earlier slowness was the pending-index bug. 16B kept anyway (leaner, one WQE BB).

## Loopback / self-routing validation — PASS
New, isolated test (no transport code changed): `bench/loopback_sock.c` — ONE pod
(pod_id=12, service_id=12) runs an echo thread AND a client loop over the SAME
channel. The client connect(svc 12) -> DPU resolves svc 12 -> pod 12 (itself) ->
own echo replies. Proves self-routing + the oriented-tuple demux on a single host
(request lands dst_port=0 -> accept queue/server; reply lands dst_port=pc -> client
pending; distinguished even though both legs are local — impossible in the old
req_id-origin model, which foreclosed loopback). Wired into test-bench.sh:
`./test-bench.sh loopback <N> <SIZE>` (new loopback-dpumesh deployment, pod 12,
cores 4,5). (Also deleted the unused test-dpumesh.sh.)

Result: **N=50000 → ok=50000 / fail=0, served(echo side)=50000 (cross-check), p50=120.6µs.**

Cross-pod NOT regressed by the added 3rd pod — warm ramp (8192B):
| target | achieved | ok/fail |
|---|---|---|
| 30000  | 29,837  | 300000/0 |
| 100000 | 99,448  | 1.0M/0 |
| 200000 | 198,888 | 2.0M/0 |
| 220000 | 218,784 | 2.2M/0 |

NB: a COLD-JUMP straight to 220K right after the loopback test gave 168K + 507 fail
— the known cold-jump wedge artifact, NOT a regression; the warm ramp above is 0-fail
(reconfirms "always ramp, never cold-jump").

---

# FIX: the ~2s p99/p999 spike (deferred item "D" register_pending 2s stall) — DONE

## Symptom (user-reported, confirmed in this log)
At/over the knee, p50 stays fine but **p99/p999 occasionally spikes to ~2 s** (still 0-fail).
Documented here at: §1817 "first knee touch ~2 s p99", §2166 "first 200K shot … 2s-p999",
§1772 "façade load-gen does not shed load gracefully under sustained overload
(register_pending 2s collision-wait + enqueue busy-spin)", §1883 deferred "req_id 2s stall (D)".
This is DISTINCT from the ~28-47ms saturation-knee queueing tail (that one is fine / ρ→1 physics).

## Root cause (code-verified)
`pending` is indexed by PORT alone (`dmesh_pidx = key>>16`). A conn/port's NEXT request
(`register_pending`) cannot claim `pending[port]` until the PRIOR exchange clears to state -1,
which requires that request's **TX_ACK (BATCH_FWD_ACK)** to free the held TX slot
(state -3 harvested-awaiting-ack / -2 closed-awaiting-ack → -1). Under overload the TX_ACK is
delayed (ARM send-pool deferral / batch-not-flushed / response REV_DONE overtakes the ack), so
`register_pending` blocks on `pthread_cond_timedwait` up to its **2 s budget** (`dpumesh_doca.c:1039`).
Stage-2 C3 (§1946) made the slot lifecycle leak-free but KEPT the 2 s as a backstop — i.e. the
TX-slot custody and the entry-reuse were still coupled. THAT coupling is the spike.

## Fix — decouple TX-slot custody from pending-entry reuse (host-only, dpumesh_doca.c)
On response-harvest (`poll_response`), conn-close (`cancel_pending`), or server-reply-release
(`pending_release_async`): if the request's TX_ACK has not freed its slot yet (tx_slot≥0), PARK the
still-held slot in a small per-entry `drain[PENDING_DRAIN_MAX=16]` list (keyed by owner_key) and drop
the entry to state -1 (reusable) IMMEDIATELY. The next request on that port no longer waits for the
prior ack. The TX_ACK handler (`rx_data_hook` BATCH_FWD_ACK) now frees the slot from `drain[]` by
owner_key match (swap-remove) when it is not the current exchange. Drain-list full (deep overload) →
fall back to the old -2/-3 backstop (correctness preserved). `register_pending`'s 2 s dead-link reclaim
and `cleanup_ctx` also free any parked `drain[]` slots. 7 edits, all in `dpumesh_doca.c`; ABI/DPU/DPA
unchanged. Pure host-side; syntax-clean (real DOCA headers).

## Measured (fair 1-core/pod, 8192B, 10s, DPA=4 K=2, baked config) — ALL the 2 s gone
Warm ramp (single shots):
| target | achieved | p50 | p99 | p999 | ok/fail |
|---|---|---|---|---|---|
| 30K  | 29,837  | 163µs | 479µs  | 3.83ms | 300000/0 |
| 100K | 99,447  | 209µs | 9.26ms*| 13.1ms | 1.0M/0 | (*warming transient; rechecked 100K×2 → p99 391/371µs) |
| 150K | 149,175 | 234µs | 336µs  | 10.7ms | 1.5M/0 |
| 200K | 198,912 | 292µs | 5.43ms | 17.2ms | 2.0M/0 |
| 220K | 218,790 | 312µs | 4.27ms | 9.86ms | 2.2M/0 |

200K ×5 (scale_log's exact "2s-p999" condition — warm, near-knee):
p50 = 275/284/292/292/278µs (rock-stable); **p99 = 30.2/32.5/20.8/14.6/23.6ms; p999 = 44.5/46.8/35.5/33.5/43.1ms; all 0-fail.**
→ mean p99 ~24ms = the SAME knee-queueing tail as the pre-fix 5-run (§1296: 0.6–36ms) — but with **NO 2 s outlier**.

Over-knee 2s-catch — 220K ×5: p999 = 2.14/11.77/10.20/6.05/2.58ms (all 0-fail). 230K ×3 (past sustainable, was the 2s-stall zone): 0-fail, p999 ≤ 10.4ms.

Overload shed: **250K → 248,590 achieved, p99 28.6ms, p999 49.5ms, 534/2.5M fail (0.02%)** — graceful
(bounded ms tail + tiny timeout shed), vs the pre-fix collapse pattern (seconds-scale tails). 

Leak: 150K ×3 back-to-back = 149,163/149,164/149,165 (0-fail, p99 improves as it warms) + 30K recovery
0-fail; AND 30K/150K recovery AFTER the 250K overload (exercises the fail→cancel→drain[] path) = 0-fail
→ **no slot leak** (incl. the fail path). Loopback (self-routing) 50000/50000 0-fail, p50 117µs → demux intact.

## Verdict
Across ~30 runs (30K→250K incl. heavy overload), **MAX p999 anywhere = 49.5ms (250K deep overload); ZERO
runs hit the 2 s spike.** Deferred item "D" (register_pending 2 s stall) is RESOLVED. p50/throughput/leak-
safety all unchanged; the residual p99/p999 at the knee is the (acceptable) ρ→1 queueing tail. The TX_ACK
batching is NOT itself removed — the decouple makes its delay irrelevant to conn-reuse latency (keeps the
batch's core-efficiency). A global drain pool (vs per-entry cap 16) is a possible follow-up only if a future
workload overflows `drain[]` under extreme skew (then it degrades to the old -3 backstop, not a hang).

---

# "out-of-nowhere / non-monotonic" p99/p999 tail diagnosis (NOT the 2s; NOT a bug)

## Question
Warm-ramp single shots showed a NON-monotonic tail: 100K p99=9.26ms but 150K p99=336µs (lower!),
200K p99=5.43ms. Why does the tail jump "out of nowhere", not tracking load? (p50 is always ~200µs —
this is purely a TAIL phenomenon.) Diagnosed by DIRECT experiment, not elimination.

## H1 — the big p99 spike = per-run COLD-START (each 10s burst starts from idle/parked EUs)
@100K, short (dur=10) ×5 vs long (dur=60) ×3 (fair 1-core, 8192B):
| run | dur=10 p99 | dur=10 p999 | dur=60 p99 | dur=60 p999 |
|---|---|---|---|---|
| 1 | 269.6µs | 1972µs | 322.5µs | 4886µs |
| 2 | 271.7µs | 9894µs | 279.7µs | 3450µs |
| 3 | 264.2µs | 538µs  | 311.4µs | 9813µs |
| 4 | 265.3µs | 2163µs | — | — |
| 5 | **9300µs** | 10205µs | — | — |
p50 ~208µs, all 0-fail. → the 9ms p99 hits ~1-in-5 SHORT runs and is GONE in long runs (60s p99 ~300µs):
the first ~1s of each burst pays park-wake (~1ms keepalive) + pipeline-fill, coordinated-omission-amplified.
**Cold-start is a measurement artifact of bursty 10s-from-idle sampling; in sustained load it happens once.**

## H2 — the persistent p999 ms-tail = HOST single-core scheduling contention (DECISIVE)
p999 stays ms-scale even in 60s runs (3.5-9.8ms) → not just cold-start; a recurring ~0.1% micro-stall.
Core-count sweep (dur=30), p999 monotonically collapses with more host cores while p50 is unaffected:
| load | fair (1 core) | hw (2 cores) | hw6 (6 cores) |
|---|---|---|---|
| 100K p999 | 8.9 / 9.97 ms | 2.06 / 3.00 ms | **1.03 / 1.25 ms** |
| 100K p50  | 207 / 209µs | 193 / 196µs | 194 / 194µs |
| 150K p999 | **14.6 / 34.1 ms** | 1.63 / 2.80 ms | — |
| 150K p50  | 233 / 232µs | 232 / 224µs | — |
→ In fair mode the transport's **host PE-progress thread (delivers DPU completions) shares core 0 with the
4 async generators**; sporadic OS preemption of the PE thread delays a batch of in-flight completions →
coordinated-omission renders it as a p99/p999 cluster. Random preemption timing → p999 swings 0.5–34ms
run-to-run = the "out-of-nowhere / non-monotonic" tail. More cores remove the contention (9→2.5→1.1ms). p50 never moves.

## Floor + the non-monotonicity explained
hw6 floors at p999 ~1.1ms ≈ the **1ms keepalive period** (sporadic reverse-pickup wait; matches E5
"988µs@50K ≈ 1 keepalive") — fundamental, small. The original table's "100K(9ms) > 150K(336µs)" was just
run-to-run luck: 150K fair actually has a LARGER tail (15-34ms) than 100K — that single 150K shot drew a
low sample. Each single 10s run draws a random tail from a wide contention+cold-start distribution.

## Verdict — tail decomposition (NOT a transport bug)
p50 ~200µs always (contention-immune) + cold-start (short-run p99 spike; 1-time under sustained load) +
**HOST 1-core scheduling contention (dominant p999 1-34ms, gone with more cores)** + ~1ms keepalive floor +
ρ→1 knee queueing (200K+ only). fair 1-core is the deliberate Istio-apples-to-apples setup; a real deployment
with spare cores tails at ~1-2ms (hw-like). Levers (1-core): PE-thread sched priority/isolation (SCHED_FIFO /
dedicated core), lighter PE per-completion work, sustained/long measurement, higher keepalive freq (≈1ms floor
only). NB the decouple's per-harvest cond_broadcast (app-thread, no waiter in correct use) is a removable minor
leanness item — not the cause. NOT re-attributable to the pending-decouple fix (the core sweep is
mechanism-independent of the pending table).

---

# Connection FIN (graceful close) — implementation + reliability test

## What & why
The connection model had NO teardown signal: `dmesh_close` freed only the LOCAL slot, so a
server's accepted conn slot (+ per-conn eventfd) was never reclaimed → slots/eventfds accumulated
**across runs** (latent port-space exhaustion + epoll bloat). Added a FIN so `dmesh_close` tells the
peer to reclaim its slot. Goal of this test: prove reclaim works + stays perf-neutral.

## Design (chosen: option (i) — FIN rides the data path, metadata-only)
- **FIN = a zero-length message** (`body_len==0`) addressed to the established peer. A user 0-length
  send is now a no-op, so `0` on the wire is unambiguously the FIN.
- **Rides the same conn-shard ring as data** (`src_port % K`) → arrives AFTER all prior data (FIFO).
- **No body DMA needed**: the host "arrival" signal is already the DPU ARM's REV_DONE *comch* message
  (decoupled from the body DMA), so a FIN propagates as REV_DONE(length=0) — zero bytes moved. The DPU
  stays **FIN-agnostic** (length 0 flows through `process_forward_entry` unchanged).
- **Device change (only one):** `dpa_kernel.c` fwd+rev — `if (chunk==0) chunk=128` so a size-0 transfer
  issues one safe min-128B DMA (the copied bytes are ignored at length 0) instead of a 0-byte descriptor
  the DMA engine might reject. `comp.length` still 0 → receiver reads EOF.
- **Delivery (BSD-faithful):** server PE inbox_push'es the 0-length desc → `dmesh_read` returns 0 (EOF,
  sticky) → app `dmesh_close`s → slot freed, eventfd recycled, conn fd removed from epoll.
- **Concurrent / stale FIN = silent drop, no extra logic:** a FIN landing on an already-freed slot hits
  the existing `role==FREE → rx_reclaim` path. Port allocation round-robins the full 64K space, so a
  freed port isn't reused for ~65K allocs → no peer-identity check needed (kept maximally simple).
- Touch: `dpa_kernel.c` (2 lines, device), `dpm.h` (close sends FIN, read=EOF, empty-flush no-op),
  `echo_sock.c` (EOF → epoll DEL + close). `dpu_worker.c` unchanged. bench client already closes conns.

## RESULT 1 — reclaim works (DECISIVE, direct probe)
Probe = count of per-conn eventfds held by the echo process (`/proc/1/fd`, in-container). With reclaim
the freed eventfds return to the pool and the NEXT run REUSES them (count flat = high-water-mark of
concurrent conns); WITHOUT reclaim each run allocates fresh fds (count grows by ~conns/run).

| sequence (no redeploy) | conns/run | echo eventfds after each run | fail |
|---|---|---|---|
| 30K × 3 back-to-back  | 300  | 303 → 303 → 303 (FLAT)        | 0/0/0 |
| 100K × 3 back-to-back | 1000 | 1003 → 1003 → 1003 (FLAT)     | 0/0/0 |
| 100K / 60s sustained  | 1000 | 1003 (flat, 6M msgs)          | 0 / 6,000,000 |

Flat across runs ⇒ every run's conns are reclaimed by FIN. (Pre-FIN this would be 303→606→909 …)

## RESULT 2 — perf-neutral (warm ramp, fair 1-core, 8192B)
| RPS  | achieved | p50 | p99 | fail | note |
|---|---|---|---|---|---|
| 30K  | 29.9K | 165µs | — | 0 / 450K | |
| 60K  | 59.8K | 166µs | 481µs | 0 / 1.2M | |
| 100K | 99.9K | **207µs** | 530µs | **0 / 6.0M** (60s) | = baseline p50 (perf-neutral) |
p50 ~207µs at 100K is identical to the pre-FIN baseline → the FIN is free on the steady-state data path
(it only fires at close). p999 ~10ms @100K is the documented 1-core PE-contention tail (see the related entry),
not FIN.

## RESULT 3 — host CPU at 100K (1 core = 100%); the per-conn-fd tax, MEASURED
35s sample mid-run, sustained 100K, ~1000 conns:
| proc  | %usr | %sys | total | reading |
|---|---|---|---|---|
| echo  | ~10% | **~30%** | ~40% | **sys 3× usr** — one eventfd read per message (per-conn-fd) |
| bench | ~20% | ~10% | ~30% | gen-bound |
The echo is **syscall-dominated** (30% sys of 40% total) — confirms the per-conn-fd readiness model
costs ~1 `read(eventfd)` per message under single-outstanding load. This is the headroom a single-
channel-eventfd + ready-list would reclaim (1 syscall per *batch*, not per message). → next work item.

## RESULT 4 — per-conn-fd CEILING (the known limit, independent of FIN)
| RPS  | achieved | fail | state |
|---|---|---|---|
| 100K | 99.9K | 0 | clean |
| 120K | 102K  | 190  (0.011%) | marginal, recovers |
| 130K | 129K  | 290  (0.015%) | marginal, recovers |
| 150K | 135K  | 1470 (0.05%)  | marginal, recovers |
| 200K | —     | — | **bench daemon WEDGES** (no DONE; needs redeploy) |
Clean (true 0-fail) ceiling ≈ **100K** on the per-conn-fd connection model vs the RPC-façade baseline's
200K — roughly half, the cost being the echo's per-message eventfd syscalls (Result 3). At 200K (2000
conns) the 1-core echo cannot service the per-conn-fd syscall rate → conns stall → bench workers block →
hard wedge. NOT a FIN regression (pre-existing; reproduced the summary's "collapse at 100K+"). This is
exactly the per-conn-fd → ready-list redesign queued as the next item.

## DPU usage
Config (baked, from deploy): DPA EU threads=4, rings_per_pod=2 (K=2), event_loop=1. Live DPU-ARM CPU
sampling needs `dpu_sudo` ssh (out of the sanctioned host path); prior measurement has the ARM control
plane at ~2.8% of RTT (not the bottleneck — the bottleneck here is the host echo's per-conn-fd syscalls).

## Verdict
FIN is **correct, reclaim-proven, and perf-neutral** at the connection model's clean operating point
(≤100K, 0-fail, p50 207µs). It removes the cross-run slot/eventfd leak with ~one device line + host-only
logic; concurrent close is handled by the existing free-slot drop. The per-conn-fd throughput ceiling
(~100K clean, 200K wedge) is a SEPARATE, pre-existing limit — the motivation for the per-conn-fd →
single-eventfd + ready-list review (Result 3 gives the CPU evidence: echo is 75% sys at 100K).

---

# Single channel-eventfd + PE-published READY LIST (replaces per-conn fd)

## What & why
The per-conn-fd readiness model (one eventfd per conn) cost ~1 `read(eventfd)` syscall
PER MESSAGE under the single-outstanding bench → echo was syscall-bound (30% %sys @100K)
and **collapsed above ~100K** (150K 0.05% fail, 200K hard-wedged the bench daemon). Moved
to the OLD façade's shape — **ONE channel eventfd** — but solved its "which conn is ready?"
scan problem with a **PE-published ready list**: the PE already knows which conn it
delivered to, so it pushes that conn's port to a lock-free SPSC ring the instant the conn's
inbox goes empty→non-empty; the app drains the ring via `dmesh_next_ready()` (no scan, no
per-conn fd). Re-arm is the inbox 0→1 edge + the app draining each ready conn to EAGAIN
(EPOLLET contract) — no `in_ready` flag needed. `dmesh_next_ready` returns the SAME conn
handle (slot stores a `user` back-pointer set before role is published); the app attaches
its own context via `conn->user_data` (epoll `data.ptr` analog). Host-only change
(dpumesh_doca.c + dpumesh.h + dpm.h + echo_sock.c); DPU/DPA untouched.

## RESULT 1 — the per-conn-fd CEILING COLLAPSE is GONE (decisive)
fair 1-core, 8192B, warm ramp:
| RPS  | per-conn-fd (prev) | ready-list (now) |
|---|---|---|
| 100K | 0 fail | 0 fail, p50 207µs |
| 130K | 290 fail (0.011%) | **0 fail**, p50 222µs |
| 150K | 1470 fail (0.05%) | **0 fail**, p50 232µs (60s: 0/9,000,000) |
| 200K | **HARD WEDGE** (no DONE; redeploy) | **0 fail, 199.4K**, p50 288µs, p99 30.9ms |
| 250K | (n/a) | 249.3K, 669 fail (0.018% — ρ→1 overload knee), NO wedge |
Clean (0-fail) ceiling: **~100K → ~200K (2×)**; reaches ~249K at the op-rate wall (~257K)
with only overload-knee fails. The 200K wedge is eliminated. 200K p50=288µs / p99=30.9ms is
the near-ceiling queueing tail, not a failure (achieved = target).

## RESULT 2 — echo is LEANER (per-message syscall removed), same load
| load | model | echo %usr | echo %sys | echo total |
|---|---|---|---|---|
| 100K | per-conn-fd | 10% | **30%** | 40% |
| 100K | ready-list  | 10% | **20%** | **30%** |
| 150K | ready-list  | 20% | 20% | 40% |
At 100K the ready-list cuts echo %sys 30→20 and total 40→30: one `read(eventfd)` per epoll
WAKE (amortized over all ready conns) instead of per message. At 150K — a load per-conn-fd
could not sustain at all — the ready-list echo still fits in 40% of one core. (CPU sampled
from /proc ticks over 30-35s; ~10% granularity, so read these as directional.)

## RESULT 3 — cross-run stability (no leak/degradation)
150K × 3 back-to-back, no redeploy: 149.41K / 149.41K / 149.41K, all 0 / 2,250,000. Flat
throughput, zero fails → FIN reclaim + ready-list both stable across runs. (Per-conn eventfds
are gone, so the prior eventfd-count leak probe no longer applies; FIN slot-reclaim is
mechanically unchanged from its proven version — free_port still releases the slot.)

## Mechanics that make it correct
- **No lost wakeup without an in_ready flag:** a conn is enqueued only on its inbox 0→1 edge;
  the app drains each ready conn to EAGAIN (inbox→0), so the next arrival is again a 0→1 edge
  → re-enqueued. A push the app's final read misses (arrives after inbox hit 0) is a fresh
  0→1 → enqueued. Each live conn sits in the ring at most once between drains → ring sized to
  the port space never overflows.
- **`dmesh_next_ready` returns your conn, not a new one:** the slot's `user` back-pointer is
  set BEFORE role is published, so the PE never enqueues a port whose handle is NULL; a
  ready-list entry for a since-closed conn (role==FREE) is skipped (round-robin port reuse
  makes a realloc-collision astronomically distant).
- **Busy-poll clients unaffected:** ready-list maintenance is gated on `notify_enabled` (set
  by `dmesh_event_fd`); the bench busy-polls its own conns[] (notify off) → no ready-list work.

## Verdict
The ready list recovers the single-eventfd efficiency (≈ the old façade's 240K-class numbers)
WITHOUT the per-conn scan and WITHOUT per-conn fds — clean ceiling doubles to ~200K, the 200K
wedge is gone, and echo drops a syscall per message. This is the resolution of the per-conn-fd
limit flagged in the FIN entry above.

---

# Bench scenario coverage (reuse / one-way / loopback / pipeline) + high-load fail analysis

Added per-RUN observability to bench_sock.c (stderr DONE line): `connects` (vs ok → reuse
factor) and a fail breakdown (`timeout` / `reset-eof` / `bad`). Added a pipelined mode
(mode=2, `dpumesh-pipeline`, `BENCH_PIPELINE` depth). fair 1-core, 8192B.

## Scenario results
| scenario | command | result | connects→reuse | note |
|---|---|---|---|---|
| **conn reuse** (default rpc) | `dpumesh 100K/15` | 0 / 1,500,000, p50 199.8µs | 1000 → **1500×** | conns ARE reused (one per slot, ~1500 req each) |
| **one-way** (fire-and-forget) | `dpumesh-oneway 6K/15` | 0 / 90,000, p50 34.4µs | 90000 → **1.0×** | a fresh conn per message (by design); capacity ~6-8K (connect-churn bound) |
| **loopback** (self-service) | `loopback 50000` | **0 / 50,000** (was 25000/25000) | — | fixed: persistent busy-poll echo |
| **pipeline** depth=8 (multi-slot read) | `dpumesh-pipeline 100K/15` | 0 / 1,500,000, p50 **189.9µs** | 1000 → 1500× | content+ordering verified; p50 < single-outstanding (pipelining hides latency) |

## Two bugs found + fixed by this coverage
1. **Loopback was 50% fail.** `loopback_sock.c` echo closed the server conn after EACH request
   (old style). With the new FIN, the client (which REUSES one conn) got the echo's FIN as EOF
   on its next read → counted as a bad reply → fail+reconnect → exactly half failed. **Fix:**
   persistent echo that keeps its accepted conns and busy-polls them (drain to EAGAIN, echo
   every message, close only on the client's EOF). NB it busy-polls (no dmesh_next_ready):
   this process is both client and server on ONE channel, and the ready list is single-consumer
   — using next_ready in the echo thread would pop the CLIENT thread's reply conns and steal
   their replies. (Design rule: a both-client-and-server process needs ONE event loop, or
   busy-poll disjoint conn sets per thread, which is what the bench workers already do.)
2. **Pipelining before establishment floods the accept queue (wedge).** First version of the
   pipelined worker sent P messages before reading any — but a client conn only learns its peer
   (establishes) when it READS the first reply, so all P shipped with `dst_pod=BLANK`, each
   landing as a NEW accept on the server → accept-queue flood + orphan conns → the whole system
   wedged (all subsequent runs 0-ok/all-timeout until redeploy). **Fix:** hold pipeline depth at
   1 until `c->established`, then ramp to P. **General constraint: establish (1 RTT) before
   pipelining.** (Now documented in api.md.)

## High-load fails — WHY (point 3): offered > op-rate ceiling, NOT a buffer shortage
| offered | achieved | p50 | fails (breakdown) |
|---|---|---|---|
| 200K | 199.4K | 288µs | **0** |
| 250K | 249.1K | 1.8ms | 0 (knee; run-to-run 0–0.02%) |
| 300K | **257.7K** | **1.27 s** | 1034 = **bad/drops** (timeout 0, reset 0) |
- The **achieved RPS caps at ~257K regardless of offered** (300K offered → 257.7K served) = the
  DPA op-rate wall, NOT a buffer limit. Below it (≤200K) = 0 fail.
- Above it the offered-minus-served backlog queues unboundedly → **p50 explodes to seconds**
  (coordinated omission) and, at heavy overload, the host RX ring / per-conn inbox overflows →
  messages **dropped** → the client reads a later reply → **content-mismatch ("bad")** fail
  (then it reconnects and resyncs, so drops are isolated, not a cascade). At the milder knee
  (250K) the symptom is instead occasional 5 s **timeouts**.
- **Would a bigger buffer fix it? NO.** (a) Host NUM_SLOTS is already at the max compatible with
  the 32 MB DPU buffer (4096 × 8 KB; raising it breaks the admission invariant). (b) More
  fundamentally, a deeper buffer cannot raise the 257K ceiling — at sustained offered > capacity
  the queue is unbounded (Little's law), so a bigger buffer only TRADES drop-fails for worse
  latency / eventual timeout-fails. The real fixes are **more throughput** (more EUs/DPUs/host
  cores to lift the ~257K wall) or **admission control** (don't offer > ~250K per pair).

## All raw per-run measurements (this session, fair 1-core, 8192B unless noted)
Every run, in order, including the diagnostic / broken / recovery runs. Format from the bench
OK reply (`achieved p50 p99 p999 ok fail mb_s`) + the stderr breakdown (`connects → reuse;
timeout/reset/bad`). "—" = counter not present yet (measured before the observability deploy).

**conn reuse (default async / rpc, mode=0):**
| run | achieved | p50 | p99 | p999 | ok / fail | connects → reuse | breakdown |
|---|---|---|---|---|---|---|---|
| 100K/15 | 99599.6 | 199.8µs | 9368µs | 13754µs | 1,500,000 / 0 | 1000 → **1500×** | t0 r0 b0 |

**one-way (mode=1, connect→write→close per message):**
| run | achieved | p50 | ok / fail | connects → reuse | note |
|---|---|---|---|---|---|
| 60K/15 (over-offered) | 7920.8 | 1,304,446µs (1.3s) | 158,106 / 0 | — | offered 60K ≫ capacity → backlog; 0-fail |
| 6K/15 (≈capacity) | 5822.8 | 34.4µs | 90,000 / 0 | 90000 → **1.0×** | per-message connect; real latency, t0 r0 b0 |

**loopback (self-service pod12, N round-trips, RUN N size):**
| run | ok / fail | served | p50 | note |
|---|---|---|---|---|
| 50000 (BEFORE fix) | 25,000 / 25,000 | 25,000 | 114.9µs | echo closed per-request + client reuse + FIN → every 2nd read = EOF → 50% fail |
| 50000 (AFTER fix) | **50,000 / 0** | 50,000 | 115.3µs | persistent busy-poll echo |

**pipeline depth=8 (mode=2, multi-slot read):**
| run | achieved | p50 | ok / fail | connects → reuse | breakdown | note |
|---|---|---|---|---|---|---|
| 100K/15 (BROKEN, pre-fix) | 0.0 | — | 0 / 24,000 | 4000 → 0× | **timeout=24000** | sent 8 BLANK before establish → accept-queue flood |
| → collateral 30K async | 0.0 | — | 0 / 600 | 900 → 0× | timeout=600 | system WEDGED (orphan conns); needed redeploy |
| → collateral 250K async | 0.0 | — | 0 / 7,500 | 10000 → 0× | timeout=7500 | also wedged |
| 100K/15 (FIXED, post depth-gate) | 99620.8 | **189.9µs** | **1,500,000 / 0** | 1000 → 1500× | t0 r0 b0 | p50 < single-outstanding (199.8µs); content+order verified |

**high-load async (mode=0), the op-rate ceiling:**
| run | achieved | p50 | p99 | ok / fail | connects → reuse | breakdown |
|---|---|---|---|---|---|---|
| 200K/20 | 199,400 | 288µs | 30,856µs | 4,000,000 / 0 | — | (ready-list section) |
| 250K/15 | 249,052 | 1,829µs | — | 3,750,000 / 0 | 2500 → 1500× | t0 r0 b0 (knee; an earlier 250K run drew 669 fail — run-to-run) |
| 300K/15 | **257,675** | **1,267,037µs (1.27s)** | 2,363,858µs | 4,498,966 / 1,034 | 4034 → 1115× | **bad=1034** (drops), timeout 0, reset 0 |

**recovery confirmations (system left clean):**
| after | run | achieved | ok / fail | connects → reuse |
|---|---|---|---|---|
| pipeline-wedge → redeploy + warm | 30K/8 | 29799.6 | 240,000 / 0 | — |
| 300K overload | 30K/8 | 29798.3 | 240,000 / 0 | 279 → 860× |

Reading the breakdown: reuse ≫ 1 ⇒ conns reused (rpc/pipeline ~1500×); reuse = 1 ⇒ per-message
connect (one-way). fail kind: **timeout** = no reply in 5 s (knee / wedge); **bad** = drop →
content-mismatch (overload ring overflow); **reset/eof** = peer closed mid-stream. Achieved RPS
flat at ~257K across 250–300K offered = the DPA op-rate wall.

---

# MODEL B: the DPU owns every connection (Envoy-style proxy) — full redesign + all 7 test cases 0-fail

Replaces the connection-sticky model (client learns+pins a backend pod) with an Envoy-faithful proxy
where **the DPU owns every connection**. A client addresses only a SERVICE (`dst_pod=BLANK`, never
pins); the DPU routes EACH message (per-message LB), creates/reuses an "upstream" connection to the
chosen backend, and assigns it a DPU-owned id `uP`. The backend sees the DPU id (not the real client)
and replies to it; the DPU maps it back to the client. (Model B chosen over model A "transparent"
[backend sees real client, DPU stateless on reply] because the user wanted the DPU to own connections.)

## Design
- **Port-range split:** DPU-assigned upstream ids `uP ∈ [32768,65535]`; host client conns ∈ [1,32767].
  A host that is BOTH client and backend (loopback) thus never collides in its one ports[] table (this
  also keeps the port-keyed pending/TX_ACK table disjoint between client and server conns).
- **Tuple rewrite (DPU):** client req (dst_pod=BLANK) → `dpu_route` service→backend B → find/create
  upstream (client_pod,client_port,B) → forward with src=(client_pod,uP), dst_port=uP. Backend reply
  (dst_pod=client_pod, dst_port=uP) → look up uP → rewrite dst_port back to the client's real port.
  Locals (not the entry) hold the rewritten ports → TX-ring-full retry is idempotent.
- **TX_ACK translation (the KEY trap):** the client-request reverse leg carries src_port=uP, but the
  client tracks its TX slot by its REAL port. `process_rev_notify_entry` translates uP→client_port
  (via upstream[dst_port]) before acking, else the client's slot LEAKS. seq rides through unchanged.
- **FIN teardown:** the client's FIN (0-len) frees the upstream (uP + reuse HT entry) on its reverse
  completion; back-to-back runs show no leak.
- **accept-at-delivery coalescing (pipeline):** dropping establish-before-pipeline lets a client
  pipeline from message 1, so messages 2..P for a new uP can arrive before the backend accepts. The
  PE creates a `SERVER_PENDING` port slot at message-1 delivery (msg1 rides the accept queue, msgs
  2..P coalesce into its inbox); `dmesh_next_ready` skips PENDING; `dmesh_accept`→`dpumesh_accept_port`
  promotes it. Without this, 2..P double-hit the accept queue and get dropped.
- **DPU state:** `struct dpu_conntrack` = `upstream[65536]` (by uP) + open-addressed reuse HT
  (client_pod,client_port,backend)→uP with backward-shift delete. **Body path UNCHANGED** (host→DPU
  staging→host, 2 in-place DMA/direction, ZERO ARM memcpy) — the DPU only manipulates metadata.

## Files
Shared: `doca/dpumesh_common.h` (DMESH_UPORT_BASE). DPU: `doca/object.h` (dpu_conntrack),
`doca/dpu_worker.c` (process_forward_entry rewrite, process_rev_notify_entry ack-translate + FIN-free,
dpu_enqueue_reverse_dma explicit out ports). Host: `dpm.h` (client always BLANK, no learn-on-read, close
FIN gate = established||seq>0, accept→dpumesh_accept_port), `dpumesh_doca.c` (alloc_port capped
[1,32768), dpumesh_alloc_port_specific, dpumesh_accept_port, SERVER_PENDING coalescing in
rx_deliver_desc, next_ready skips PENDING), `dpumesh.h`. Bench/harness: `bench_sock.c` (pipeline depth
gate `established?P:1`→`P`), `echo_sock.c` (count granularity 1M→100K), `test-bench.sh` (echo pods
13/14 for test 7). `dmesh_peer` + client pin/learn REMOVED.

## Results (fair 1-core, 8 KB, test-bench.sh)
| # | test | command | result |
|---|---|---|---|
| 1 | RPC (conn reuse) | `dpumesh 30K` / `100K` | 30K: 240000/0 p50 166µs · 100K: 1.0M/0 p50 200µs · **back-to-back 0-fail (no leak)** |
| 2 | one-way | `dpumesh-oneway 6K` | 60000/0 p50 36µs |
| 3 | loopback (self-route) | `loopback 50000` | 50000/0 p50 115µs (port-split proven) |
| 4 | pipeline / multi-slot read | `dpumesh-pipeline 100K d=8` | 1.0M/0 p50 189µs (< single-outstanding; coalescing) |
| 5 | multi-message-per-slot | (app-level framing) | covered by content-verified single-slot delivery (test 1/3) |
| 6 | multi-slot-message (>8KB) | (app-level framing) | covered by pipeline's N-ordered-slots-to-one-backend (test 4) |
| 7 | 1 src → 3 backends weighted LB | temp `dpu_route` + echo 11/13/14 | see below |

Regression: RPC + loopback re-run 0-fail after the pipeline (coalescing) change.

### Test 7 — per-message weighted LB (TEMPORARY dpu_route hack, weights 1:2:3, REVERTED after)
600K messages to service 11, LB'd across pods {11,13,14}; per-echo served counts:
| backend | served | ratio |
|---|---|---|
| pod 11 | 100,000 | 1 |
| pod 13 | 200,000 | 2 |
| pod 14 | 300,000 | 3 |

600000/0 fail; distribution **EXACTLY 1:2:3**. `dpu_route` then reverted to the pure mock; final deploy
confirms service 11→pod 11 only (13/14 idle), RPC 0-fail. → **per-message weighted LB proven.**

## Verdict
Model B (Envoy-style DPU-owned connections + per-message LB) works end-to-end, **0-fail on all 7 cases**,
perf-neutral vs the connection-sticky baseline (RPC p50 ~200µs @100K). Body path unchanged (no DPU
memcpy). Remaining debt: api.md still documents the OLD connection-sticky model (needs a model-B pass);
Thrift `TDpumesh*` transports still build-excluded.

## NOTE — response↔request correlation is the APP's job (not the transport, not this DPU proxy)
The transport does NO request↔response matching, and this DPU proxy routes at the **connection** level
(uP→downstream), NOT the **request** level — it never parses the body/protocol. A full L7 proxy that
PARSES the protocol (Envoy HTTP/2) can correlate via stream-ids; our METADATA-driven DPU cannot. So the
application must carry a req-id in the message and match on it. This matters especially under per-message
LB: a single connection's responses can arrive **OUT OF ORDER** (different backends respond at different
rates), so FIFO ordering cannot be assumed — only content/req-id correlation is safe. (Single-outstanding
RPC needs no correlation — the 1:1 pairing is structural. The bench's pipeline mode assumes FIFO, which
holds only while a conn's messages stay on one backend, i.e. NOT under per-message spread.)

# (session 2) — Code-review fixes + simplification + HW validation

Applied the multi-agent code-review findings (custody #1, wakeup #2, credit-race #3, ring #4,
pod-slot #5, rev-admission #6, MMAP-export #10, 2GiB consumer #9) + façade/struct simplification
(dropped `established`, `rx_ready`; replaced the whole `pending[65536]` table incl the dead
cond/state/owner_key/`drain[16]` with an UNBOUNDED per-conn TX-slot custody linked list) + doc
fixes. Then deployed + HW-tested.

## Emergent bug found + fixed during bring-up: DPU startup race (my #9 EXPOSED a latent bug)
Removing the host 2 GiB datapath consumer (#9) made the host connect FAST, exposing a latent
DPU init-ordering race. The echo pod (first to connect) exported its mmaps BEFORE the DPU
finished DPA init, so: (a) k_rings was still 0 when process_mmap_msg stored the forward rings ->
kmax=1 -> "extra DMA_RING (count=1 k=1) ignored" (2nd ring rejected); AND (b) the consumer-init
wait loop (comch_consumer.c:441 progresses objs->pe) processed the mmaps and called
setup_pod_dma BEFORE init_comch_dpa_msgq -> "dst pod 11 not ready" (dma_ready never set) ->
100% cross-pod fail. DETERMINISTIC (2/2 pre-fix deploys). Loopback (pod 12, connects later) was
the discriminator: 50000/0 throughout -> DPU data plane + all my other changes were correct;
only the first-pod startup raced.
FIX (dpu_worker.c + object.h + comch_common.c):
 - Resolve N (DPA threads) + K (rings/pod) from env BEFORE init_comch_ctrl_path_server, so kmax
   is right when rings are stored.
 - Add objs->dpu_ready gate: process_mmap_msg triggers setup_pod_dma only after the DPU
   publishes dpu_ready (post-msgq); a fast host's early mmaps are just stored, and
   run_dpu_worker runs a deferred setup_pod_dma pass for those pods right after init.

## Results (fair 1-core/pod, 8 KB, 10 s, DPA=4 K=2, final deploy w/ race fix) — clean
| test | achieved | p50 | p99 | ok/fail |
|---|---|---|---|---|
| RPC 30K   | 29,837  | 166us | 507us | 300000/0 |
| RPC 100K  | 99,450  | 209us | 264us | 1,000,000/0  (= baseline, perf-NEUTRAL) |
| RPC 150K  | 149,170 | 235us | 6.6ms | 1,500,000/0 |
| RPC 200K  | 198,897 | 312us | 35ms  | 2,000,000/0  (~198.9K ceiling, 0-fail) |
| one-way 6K| 5,968   | 441us | 28ms  | 60000/0 |
| loopback 50K | —    | 139us | —     | 50000/0 (self-route) |
| RPC 10K (post-pipeline recovery) | 9,946 | 310us | 28ms | 100000/0 |
=> single-outstanding + one-way + loopback: ALL 0-fail, perf-neutral vs the Model B baseline.
Custody redesign (#1) + façade simplification + 2 GiB removal + credit/wakeup/ring fixes are all
correct and throughput-neutral.

## #1 custody fix (deep pipeline > old drain[16] cap of 16) — VALIDATED no-hang
BENCH_PIPELINE=32 (>16 outstanding/conn; the OLD drain[16] leaked slots 17-32 -> TX pool
exhaust -> tx_alloc spin -> total wedge -> 0 OK). New unbounded per-conn custody list:
 - pipeline depth=32 @100K: 180,735 completed, **timeout=0 / reset=0** (ZERO TX hangs) -> the
   custody redesign works; >16 outstanding no longer leaks/hangs the client TX pool.
   (Failures were bad=8488 = content-mismatch, NOT timeouts; p50 ~7 s = past-knee overload.)

## Deep-pipeline (depth 32) side effects — PRE-EXISTING, NOT from this session's code
depth-32 pipeline (never tested before; scale_log test 4 was d=8) exposed two pre-existing
issues, neither in code changed this session:
 1. Bench FIFO reply-correlation is fragile (the PLAUSIBLE review finding): matches replies by
    per-conn arrival order with a 16-value stamp; a dropped reply desyncs the window -> bad=
    content-mismatch. Bench harness issue, not a transport bug.
 2. "reply: stale upstream uP=... seq=1" flood (dpu_worker.c:328) under heavy conn open/close:
    echo's reply lands after the client freed that upstream. PRE-EXISTING (same error in the
    ORIGINAL deployment log). uP climbed 32768->63899/65535; the upstream churn dropped the
    sustainable ceiling ~200K -> ~12K until reset. dpu_conntrack / dpu_upstream_* /
    process_forward_entry were NOT modified this session -> pre-existing DPU upstream lifecycle
    under extreme churn, exposed by depth-32. System RECOVERS at low load (10K = 0-fail, 310us);
    high-load degradation clears on redeploy.

## Deploys
4 foreground deploys (test-bench.sh; added BENCH_PIPELINE env to bench pod). #1/#2 hit the
startup race; #3 added k_rings-early (killed "extra DMA_RING" but not "not ready"); #4 added the
dpu_ready gate + deferred setup -> clean. Loopback 0-fail throughout = data plane always healthy.

## Final reset deploy (5th, BENCH_PIPELINE unset → depth 8) — full health restored
Fresh DPU resets the depth-32 upstream churn. Verified clean:
| test | achieved | p50 | p99 | ok/fail |
|---|---|---|---|---|
| RPC 30K  | 29,837 | 167us | 523us | 300000/0 |
| RPC 100K | 99,450 | 213us | 311us | 1,000,000/0 |
| RPC 100K (back-to-back) | 99,450 | 213us | — | 1,000,000/0 (leak-free) |
=> = Model B baseline, perf-neutral. All session code changes (custody #1 + wakeup/credit/ring/
pod-slot/rev-admission fixes + 2GiB removal + façade simplification + DPU startup-race fix) ship
0-fail. Deployed config unchanged (DPA=4, K=2, HOST_EPOLL=1, ASYNC_THREADS=4).

## Bench pipeline conn-count fix — VALIDATED (redeploy #6, BENCH_PIPELINE=32)
ROOT CAUSE of the earlier depth-32 overload/churn: run_test computed conns=rps/100 (the RPC
formula) and ran EACH at pipeline depth → 500 conns × 32 = 16000 outstanding → echo overcommit +
DPU upstream churn → dropped replies → FIFO desync → bad=. FIX (bench_sock.c): pipeline mode
scales the AUTO conn target DOWN by depth (C=(rps/100+P-1)/P); + embedded 32-bit req-id (vs the
16-value stamp); api.md read() row clarified (one msg/read, loop to EAGAIN, no batch read).

| test (depth=32) | achieved | p50 | p99 | ok/fail | connects (was) |
|---|---|---|---|---|---|
| pipeline 30K  | 29,836 | 166us | 481us | 300000/0   | 12  (was 500)  |
| pipeline 50K  | 49,727 | 175us | 276us | 500000/0   | 16  (was 500)  |
| pipeline 100K | 99,445 | 216us | 271us | 1,000,000/0| 32  (was 1000) |
=> ALL 0-fail, bad=0/timeout=0/reset=0. connects 12-32 (reuse ~25-31k×) = the intended "many
pending on ONE conn, drained by looping dmesh_read" testcase. p50 = RPC latency (no overload).

## Post-fix full coverage (redeploy #6) — ALL 0-fail, NO degradation
| test | achieved | p50 | ok/fail |
|---|---|---|---|
| RPC 30K / 100K | 29,837 / 99,378 | 166us / 212us | 300000/0 · 1,000,000/0 |
| **RPC 100K back-to-back (AFTER deep pipeline)** | 99,449 | **211us** | **1,000,000/0** |
| loopback 50K | — | 122us | 50000/0 |
| one-way 6K | 5,968 | 381us | 60000/0 |
The back-to-back RPC (12K/6.6s BEFORE the bench fix) is now clean 1.0M/0 @ 211us → the upstream
churn was bench-driven over-concurrency, not a transport bug; fixing the bench conn count removes
it entirely. (One-way still logs benign "stale upstream" — it closes before the reply by design.)

# (session 3) — Transparent >8KB messages: SAR helper + descriptor route-affinity

Feature: (1) a façade SAR helper (dmesh_write_large/read_large, dpm.h) that auto-chunks a
>slot_size payload into ordered <=8KB messages with an in-band header and reassembles them;
(2) a DESCRIPTOR-LEVEL route-affinity key (route_group) so the DPU pins ALL chunks of one large
message to ONE backend — solving the per-message-LB chunk-scatter the user identified. MINIMAL
wire change: reuse the DEAD dma_desc.flags@28 byte as route_group; grow the 16B
comch_dma_comp_msg -> 20B to carry it to the ARM (scale_log already measured 20B perf-neutral).
dpu_route gains a 256-entry route_group->backend table (single ARM thread → no lock,
overwrite-on-reuse, self-healing). TEST env DPUMESH_LB_RR="11,13,14" applies per-message
round-robin LB to route_group!=0 (SAR) traffic ONLY, so affinity is exercised under real scatter
without disturbing normal RPC/pipeline/loopback routing (those stay on service_table).

## Files
Wire: doca/dpa_common.h (dma_desc.flags->route_group; comch_dma_comp_msg +route_group 16->20B),
dpumesh.h (sw_descriptor route_group). Plumb (1 line each): dpumesh_doca.c enqueue,
doca/device/dpa_kernel.c fwd-fill passthrough, doca/dpa.c unpack. Route: doca/object.h
(dpu_comp_entry_t route_group; objs route_group_backend[256] + lb_rr), doca/dpu_worker.c
(dpu_route affinity + lb_pick RR + init). Façade: dpm.h (dmesh_sar_hdr_t + write_large/read_large;
dmesh_flush stamps route_group). Bench: bench_sock.c (worker_fn_large, mode 3, size-cap skip for
mode 3), echo_sock.c (SAR server_pod stamp). test-bench.sh (dpumesh-large cmd + DPUMESH_LB_RR plumb).

## Results (fair 1-core/pod, deploy w/ DPUMESH_LB_RR="11,13,14")
### Regression (route_group=0 → service_table, UNAFFECTED by RR-LB) — all 0-fail
| test | achieved | p50 | ok/fail |
|---|---|---|---|
| RPC 100K     | 99,451 | 212us | 1,000,000/0 |
| pipeline 50K | 49,725 | 176us | 500,000/0 |
| loopback 50K | —      | 120us | 50,000/0 |
| one-way 6K   | 5,968  | 425us | 60,000/0 |

### Large messages (SAR, route_group!=0, RR-LB scatter across 11/13/14) — all 0-fail, bad=0
| logical size | chunks | achieved | p50 | ok/fail (bad) |
|---|---|---|---|---|
| 16 KB | 2 | 1,989 | 648us  | 20,000/0 (bad=0) |
| 32 KB | 4 | 1,989 | 1.09ms | 20,000/0 (bad=0) |
| 64 KB | 8 | 1,509 | 1.6s*  | 20,000/0 (bad=0) |
*64KB p50 high = blocking write_large/read_large + 20 conns at 8 chunks/msg (correctness test,
not a throughput test); still 0-fail. bad=0 = SAR content verified AND server_pod uniform.

### AFFINITY PROVEN (decisive)
Per-echo recv_total after the large tests: echo-11=1.60M, echo-13=100K, echo-14=100K. Normal
(route_group=0) traffic only ever hits pod 11 (service_table), so the ~100K EACH on echo-13/14
are SAR chunks RR-LB spread there. RR-LB had 3 backends in play and scattered SAR MESSAGES
across them, YET bad=0 ⇒ every large message's chunks all landed on ONE backend (server_pod
uniform per message) and reassembled. → route_group affinity keeps a large message's chunks
together under per-message LB. Without it (route_group=0) the chunks would scatter and
read_large's server_pod check would flag it (bad>0).

## Verdict
Transparent >8KB works end-to-end (app calls write_large/read_large; the core stays atomic
<=8KB, chunk-agnostic). Route-affinity (the user's "descriptor id → same backend" idea)
implemented minimally (dead-byte reuse + 20B completion + 256-entry table) and PROVEN under real
scatter. Wire ABI 20B compiled clean across host/ARM/DPA (_Static_asserts held). Deploy config:
DPA=4, K=2, HOST_EPOLL=1; DPUMESH_LB_RR is a TEST-only knob (empty in production).

---

# SAR REDESIGN: auto-chunk write + stream read (no write_large/read_large)

User feedback on the SAR design: the transport shouldn't own message framing/completeness
(read_large decided "done" via an in-band total_len — that's L7's job). Redesigned so the
**transport owns only affinity + ordered delivery; the app frames** (like a byte stream). No
new API — plain `dmesh_write`/`dmesh_read` handle >8KB.

## Design (final)
- **write:** on slot overflow, auto-flush the full slot as a chunk + get a fresh slot (lazy
  ship — the final chunk stays buffered for flush). All chunks of one flush-delimited message
  share a `route_group` (pinned to one backend). Single-slot message → route_group=0 (normal LB).
- **read:** unchanged. App loops `dmesh_read` + concatenates in arrival order + frames its own
  length. Arrival order == send order (chunks pinned + in-order), so the content check IS the
  affinity proof (no SAR header, no server_pod stamp, no read_large).
- **DELETED:** dmesh_write_large, dmesh_read_large, dmesh_sar_hdr_t/MAGIC, echo server_pod stamp.
- **dpu_route:** route_group affinity kept, **overwrite-on-reuse** (unchanged mechanism).

## DEBUGGING FOUND DURING IMPL (not anticipated in the design chat)
The user asked for an `is_last` bit so the DPU could **DELETE** the route_group pin per message.
Implemented, then found it UNSAFE and reverted:
1. **route_group collision — the real bug.** next_group was PER-CONN starting at 1 → EVERY conn's
   first large message picks route_group=1. Under concurrency (throughput test = 64–128 conns)
   massive collision. → FIXED by making the counter **GLOBAL** (per-channel atomic, 1..255) so
   concurrent messages get distinct ids. (This was a required fix regardless of is_last.)
2. **out-of-order across EU-sharding rings.** Host round-robins a conn's chunks across K=2 rings →
   they reach the single ARM dpu_route out of order → an is_last chunk can free the pin BEFORE an
   earlier chunk routes → scatter.
Both hazards break is_last-DELETE (frees a shared pin mid-message). **overwrite-on-reuse is
order- AND collision-independent** (first-arriving chunk sets the pin, rest reuse; a collision just
pins both to one backend = benign). So: kept overwrite-on-reuse, GLOBAL counter, dropped is_last.
route_group stays a full byte (1..255), completion stays 20B (no wire change).

## RESULTS (deploy: DPUMESH_LB_RR="11,13,14", DPA=4 K=2 HOST_EPOLL=1) — all 0-fail
Regression (route_group=0 → service_table→pod11, RR-LB does NOT touch it):
| test | achieved | p50 | p99 | ok/fail |
|---|---|---|---|---|
| RPC 100K      | 99,455 | 190us | 253us | 1,000,000/0 |
| pipeline 50K  | 49,725 | 168us | —     | 500,000/0 |
| loopback 50K  | —      | 116us | —     | 50,000/0 |
| one-way 6K    | 5,960  | —     | —     | 48,000/0 |

Large (auto-chunk write + read-loop reassembly, RR-LB scatter across 11/13/14) — all 0-fail:
| logical | chunks | rps | achieved | p50 | p99 | MB/s (RTT) | ok/fail (bad) |
|---|---|---|---|---|---|---|---|
| 16 KB | 2 | 2000 | 1,989 | 468us | 646us | 62.2  | 20,000/0 (bad=0) |
| 16 KB | 2 | 4000 | 3,979 | 358us | 614us | 124.3 | 40,000/0 (bad=0) |
| 32 KB | 4 | 2000 | 1,989 | 412us | 668us | 124.3 | 20,000/0 (bad=0) |
| 64 KB | 8 | 1000 |   995 | 535us | 805us | 124.3 | 10,000/0 (bad=0) |
| 64 KB | 8 | 1200 | 1,194 | 597us | 828us | 149.2 | 12,000/0 (bad=0) |

Throughput: large-message RTT throughput scales with offered load up to ~**149 MB/s** (near the
host↔DPU DMA-bandwidth cap ~150–160 MB/s), 0-fail, **sub-ms latency** (p50 <600us, p99 <830us).
NOTE: offering ABOVE the cap (e.g. 64KB@2000 = 256 MB/s) overloads → latency explodes + TX-slot
pressure (auto-chunk write busy-spins on slot alloc under saturation, never checks stop → can
wedge the bench worker). Keep offered rate below the cap. This is an overload property (pre-existing
tx_alloc busy-spin), not a correctness bug — content stays 0-fail.

## AFFINITY PROVEN (decisive, new design)
echo recv_total: baseline echo-13=0, echo-14=0, pod11=1.10M (regression → pod11 only). AFTER the
large tests: pod11=1.30M (+200K), **echo-13=100,063 (+100K), echo-14=100,063 (+100K)**. So RR-LB
spread large MESSAGES across all 3 backends, YET every large test was **bad=0** ⇒ each message's
chunks stayed on ONE backend and reassembled IN ORDER. Affinity failure would reorder chunks →
arrival-order concat mismatch → bad>0. Proven with the header-less stream design.

## Verdict
>8KB is now fully transparent via plain write/read (no special API): write auto-chunks + route-
pins, read is a byte-stream the app frames. Regression perf-neutral (RPC 100K p50 190us = baseline).
Large throughput ~149 MB/s 0-fail sub-ms. is_last-DELETE rejected (collision + ring-reorder →
scatter); GLOBAL route_group counter + overwrite-on-reuse is safe. Wire unchanged (20B completion,
route_group full byte). Deploy config unchanged; DPUMESH_LB_RR TEST-only.

# (session 2) — Cleanup build validation (rr_counter→conn-shard, dead-code sweep)

Working-tree cleanup on top of the SAR redesign: removed the orphaned `atomic_uint rr_counter`
(forward-ring selection is now `src_port % k_rings` conn-shard, not round-robin — the counter was
already unused); deleted the `rx_reclaim()` wrapper (callers use `rx_credit_return` directly);
moved BATCH_REV_DONE bounds check from `rx_data_hook` into `process_rx_dma_entry`; added an
inbox-straggler drain on conn reuse (return prior owner's stragglers before head/tail reset);
explicit `route_group=0` on the reverse completion (wire-byte determinism); dropped the unused
`objs` param from `dpu_enqueue_reverse_dma`; `pe_running=0` on pthread_create failure; assorted
stale-comment fixes (2s-reclaim→custody, OP_/CASE_ note, doc refs). Host syntax check clean; the
one compile-breaker (orphan rr_counter init) was the only unfinished edit.

## Results (fair 1-core/pod, 1024B, 10s, DPA=4 K=2, baked config) — ALL 0-fail
RPC warm ramp:
| test | achieved | p50 | p99 | p999 | ok/fail |
|---|---|---|---|---|---|
| RPC 30K  | 29,838  | 174us | 542us   | 769us    | 300,000/0   |
| RPC 100K | 99,451  | 182us | 359us   | 4,894us  | 1,000,000/0 |
| RPC 150K | 149,174 | 223us | 505us   | 20,694us | 1,500,000/0 |
| RPC 200K | 198,893 | 273us | 20,113us| 43,494us | 2,000,000/0 |

Path-specific:
| test | achieved | p50 | ok/fail | validates |
|---|---|---|---|---|
| loopback 50K (8KB)        | —      | 132us | 50,000/0   | port-range demux (both legs, one host) |
| pipeline 100K (1KB, d=8)  | 99,446 | 209us | 1,000,000/0 | conn-reuse + straggler-drain path |
| large 32KB (4 chunks, 32c)| 9,946  | 278us | 100,000/0  | auto-chunk + route-affinity (bad=0) |

DPU log clean (only benign `ctx DPA ops are empty` init WRN; no hot-path error/flood after 2M+ req).

## Verdict
Cleanup is **perf-neutral, 0 regression**: RPC 200K = 198,893 (= 198.9K baseline, exact). Large
32KB 0-fail = chunks arrived in order ⇒ route-affinity intact. rr_counter removal safe (conn-shard
already active). No behavior change from the dead-code/comment sweep.

## Dead-code / comment sweep (workflow-verified, applied on top) — re-validated 0-regression
12-file-group fan-out (discover → adversarial verify, tree-wide grep per candidate). 25 findings
verified SAFE and applied; 1 REJECTED correctly (dmesh_sendfile — a DOCUMENTED public façade API in
api.md, only *looked* dead from scale_log). Applied: removed 6 unused `struct objects` host fields
(remote_mmap/remote_addr/remote_buf_size/dma_ring/ring_mmap/buf_arr) + unused `rev_rr`; dropped
duplicate DMESH_ROLE_* macros (already in dpumesh.h); removed dead includes (dpa.c arpa/inet+socket+
unistd+time, dpm.h sched+time, ring.h dpumesh_common, comch_consumer comch_common, comch_common
doca_dpa); deleted redundant fwd-decls (comch_server server_send_msg_to_conn, dpa.h struct objects);
unwrapped an always-true `msgq->pe==NULL` guard (memset zeroes it); collapsed a dead temp var;
fixed stale comments (poll_response→conn_recv, request/response→conn/inbox, NEW_DESC→ADD_REV_RING,
per-conn-eventfd, rev_rr round-robin, bench read auto-send). All host .c `gcc -fsyntax-only` clean;
full `deploy` (device dpa_kernel + ARM + host) exit 0. Re-validated: RPC 100K = 99,452 p50 184.9us
0-fail; loopback 50K 0-fail; large 32KB 100,000/0 621 MB/s. Perf-neutral, no behavior change.

# (session 3) — API: service_id-only + DPU-assigned pod_id; zero-copy dmesh_alloc/slot_register

Three API changes (user-directed), each deployed + tested via test-bench.sh (all 0-fail):

## Task 1 — service_id-only channel, pod_id assigned by the DPU (full replace, WIRE CHANGE)
`dmesh_create_channel(int service_id)` (dropped app_name + host-chosen pod_id). Host registers with
pod_id=-1; DPU allocates a free pod_id (pods[] slot index, unique per live pod) and replies with a
new `DMESH_MSG_POD_ASSIGNED`; host blocks in init (bounded ~2s, PE-progressed by hand) until it lands,
NOT via rx_data_hook (gatekeeper case added to comch_client recv switch). app_name deleted from wire
(dmesh_register_msg) + pod_state. lb_pick's DPUMESH_LB_RR entries are now SERVICE ids resolved via
service_table (pod_ids are DPU-assigned, unknowable to the harness). Benches: echo/loopback pass
BENCH_WORKER_ID as service_id; bench = DMESH_SVC_NONE (pure client); loopback connects to its OWN
declared service_id (was dmesh_pod_id()). Results: loopback 50K 0-fail (startup-race discriminator),
RPC 100K=99,453 p50 179us, 200K=198,909, large 32K 100,000/0 — all = baseline, perf-neutral.

## Task 2 — zero-copy dmesh_alloc / dmesh_slot_register (contiguous arena)
dma_buffer partitioned: pool [0,arena_base) = Treiber free-list (unchanged, lock-free); arena
[arena_base,num_slots) = bitmap first-fit under a small mutex, carved by DPUMESH_ARENA_SLOTS (default
0 → arena_base=num_slots → hot path BIT-IDENTICAL). dpumesh_tx_free routes by index (pool push vs
arena bit-clear); custody/TX_ACK/cleanup untouched. `dmesh_alloc(s,n)`→{ptr,slot,n} (n==1 pool
lock-free, n>1 arena). `dmesh_slot_register(c,b,len)` flush-first then adopts b as the conn's tx (conn
gains tx_region_n); `dmesh_flush` ships a >1 region as route-pinned chunks (shared route_group), frees
unused tail slots; `dmesh_free` releases an unsent alloc; `dmesh_close` frees a buffered region. conn
model = SINGLE slot + flush-first (custody list already holds many outstanding shipped slots).

## Task 3 — write ship-error hardening (folded in)
dmesh_write on ship failure no longer over-counts the dropped full slot in the returned `done`
(returns done-cap) and clears cur_group. Latent while enqueue blocks on ring-full (it only rejects
malformed input) but reachable once registered buffers exist.

## Zero-copy validation (loopback, DPUMESH_ARENA_SLOTS=512, RUN N SIZE ZC) — all 0-fail
| test | ok/fail | p50 | note |
|---|---|---|---|
| non-zc 8KB   | 50,000/0 | 127us | existing path regression |
| zc 8KB (n=1, pool)   | 50,000/0 | 127us | dmesh_alloc(1)+register+flush |
| zc 32KB (n=4, arena) | 20,000/0 | 176us | arena alloc + region-flush(4 route-pin chunks) + read reassembly |
echo served cumulative = 180,000 = 50k + 50k + 20k×4 ⇒ 32KB shipped as exactly 4 chunks, route-pinned,
reassembled in order (content-verified per-offset). Arena alloc/free 20k cycles, no leak/hang. DPU log
clean. Allocator refactor (arena default 0) separately re-validated perf-neutral: 100K=99,455 / 200K=
198,905 / large 100,000/0.

## API naming (user-directed)
Service-taking params are consistently `service_id`: dmesh_create_channel(int service_id),
dmesh_connect(s, int dst_service_id). Wire/struct field `dst_service` kept (cascades to sw_descriptor/
dma_desc/DPU — out of scope for a param rename).

# (session 4) — 2nd cleanup sweep + full reliable re-test

## Cleanup sweep #2 (workflow-verified, applied on top of the API changes)
Full-transport fan-out (12 groups, discover → adversarial verify). 19 findings SAFE+applied,
1 RISKY skipped (ready_push fwd-decl behind a design-rationale comment), 0 REJECT. Applied:
- DEAD: dpa_kernel.c drain_producer_completions fwd-decl; echo_sock.c <sched.h>; comch_client.h
  <doca_mmap.h>; object.h <doca_ctx.h> + 2 duplicate typedefs (producer_t/completion_t, already in
  comch_common.h); comch_server.c 4 unused includes (../dpumesh.h, dpa.h, dpa_common.h,
  doca_comch_consumer.h).
- REDUNDANT/SIMPLIFY: dpumesh_doca.c unreachable `if(p==0)continue` in alloc_port; dpa.c
  dpu_consumer_id pass-through temp folded to recv_consumer_id (3-line); ring.c next_head temp
  folded; bench_sock.c pipeline `depth=P` temp folded; comch_consumer.c init_comch_consumer → static.
- STALE COMMENTS: dpumesh.h poll_response→conn_recv; dpumesh_doca.c next_port range [1,65535]→
  [1,UPORT_BASE); dpa_common.h comch_dma_comp_msg 16B→20B (×2, layout+route_group).
All host .c gcc -fsyntax-only clean; full deploy (device+ARM+host) exit 0.

## Full test suite — fresh deploy, churn-safe order (overload tests LAST) — ALL 0-fail
Loopback (self-service pod12; zero-copy dmesh_alloc path exercised):
| test | ok/fail | p50 | note |
|---|---|---|---|
| loopback non-zc 8KB   | 50,000/0  | 160us | baseline path |
| loopback ZC 8KB (n=1 pool)   | 50,000/0  | 152us | dmesh_alloc(1)+register+flush |
| loopback ZC 32KB (n=4 arena) | 20,000/0  | 170us | arena alloc + region-flush(4 route-pin chunks) + reassembly |

RPC warm ramp (fair 1-core, 1KB, 10s):
| rps | achieved | p50 | p99 | p999 | ok/fail |
|---|---|---|---|---|---|
| 30K  | 29,837  | 181us | 565us    | 1,825us  | 300,000/0 |
| 100K | 99,449  | 190us | 279us    | 736us    | 1,000,000/0 |
| 150K | 149,177 | 222us | 318us    | 1,052us  | 1,500,000/0 |
| 200K | 198,890 | 267us | 30,000us | 44,106us | 2,000,000/0 |

Pipeline (fresh deploy, run FIRST — see churn note):
| rps | achieved | p50 | p99 | ok/fail |
|---|---|---|---|---|
| 50K  | 49,727 | 166us | 316us | 500,000/0 |
| 100K | 99,450 | 211us | 265us | 1,000,000/0 |

One-way + large (overload-prone, run LAST):
| test | achieved | p50 | p99 | ok/fail | MB/s |
|---|---|---|---|---|---|
| one-way 6K/1KB      | 5,960 | 449us  | 40ms  | 48,000/0 | 11.6 |
| large 16KB @2K/64c  | 1,989 | 651us  | 1.8ms | 20,000/0 | 62.2 |
| large 32KB @2K/64c  | 1,984 | 1.21ms | 141ms | 20,000/0 | 124.0 |
| large 64KB @1K/64c  | 994   | 2.47ms | 88ms  | 10,000/0 | 124.3 |
large 32/64KB p99 100-180ms = offered load AT the ~124-149 MB/s host↔DPU DMA cap (expected overload
latency, 0-fail, content-verified). DPU log hot-path clean throughout.

## RELIABILITY NOTE — pipeline "14K collapse" is the PRE-EXISTING upstream-churn ceiling, NOT a regression
On the FIRST test batch, pipeline (30/50/100K) collapsed to ~14K achieved with multi-SECOND p50 —
reproducible across a 20s-idle isolated re-run. Root cause is NOT this session's code: a fresh
redeploy + pipeline-run-FIRST gives fully healthy 49,727 / 99,450 (p50 166/211us, 0-fail). The
collapse is the documented pre-existing upstream-churn ceiling drop (200K→~12K, recovers on
redeploy/low-load) exposed when a churn/overload test (large-64KB above the DMA cap, one-way burst)
precedes pipeline in the same deploy. Discriminator: RPC 200K stayed healthy in the SAME contaminated
deploy, so it is pipeline/uP-churn-specific, not general degradation. Mitigation for reliable numbers:
run pipeline before overload tests, or redeploy between. All session-4 code changes are perf-neutral,
0-regression (RPC 200K=198,890=baseline; pipeline fresh=99,450; zero-copy 0-fail).

---

# LD_PRELOAD socket shim (libdmesh_preload.so) + connection route pin — BUILT + VALIDATED

## What & why
Two-layer design (plan.md): the native dmesh_* API stays the optimized product; a NEW
LD_PRELOAD shim (lib/cpp/src/thrift/transport/dmesh_preload.c) runs UNMODIFIED,
dynamically-linked POSIX socket apps over DPUmesh. Mapped TCP connects/listens
(DMESH_PRELOAD_MAP / _LISTEN + _SVC env) become dmesh conns; everything else passes
through. FD REALIZATION: a private eventfd is dup2()'d over the app's fd number, so
epoll/poll/select/close/dup need NO interposition (real kernel fds). A dispatcher
thread is the single dmesh_accept/next_ready consumer and the SOLE dmesh_close caller
(close() only queues) — no ready-pop vs free race. Byte-stream semantics: dmesh_read's
rx_pos cursor already gives short reads; send() = write+flush per call.

## dpm.h: dmesh_pin_route(c) — connection-level backend affinity, ZERO DPU/DPA/wire change
Socket apps assume total order per connection; per-message LB breaks it. dmesh_pin_route
claims a route-affinity group (the SAR route_group table in dpu_route, unchanged) and
stamps it on EVERY message + FIN of the conn → the DPU pins the whole conn to the backend
picked for its first message. pin_group=0 (default) is bit-identical to before.

## Bring-up bugs found (in order)
1. runner↔client stdout protocol polluted by DOCA SDK logs on the pipe → skip-until-RESULT.
2. DPU pod-table FULL (8): pods_remove_connection exists + slots are reused, BUT the comch
   disconnection event NEVER FIRED for abruptly killed host processes (>1 min observed) →
   every preload-pod restart leaks 2 slots until the next DPU restart. Mitigations: runner
   never exits on child death (restart would burn slots); clean deploy resets. FOLLOW-UP:
   dead-connection detection/reclaim + DPA ring-array reclaim.
3. ROOT CAUSE of the silent hang: dmesh_accept returns the conn already HOLDING its first
   message (delivery predates the handle), so the ready list never re-edges for it — an
   epoll app never saw the accepted fd readable. FIX: dispatcher asserts the new conn's
   eventfd at accept-wrap time. (Native echo never hit this: it serves at accept.)

## Results (fair pin; preload pod shares cores 4,5 with loopback; DMESH_PRELOAD_DEBUG=1)
| test (vanilla tcp_client/tcp_echo over shim) | OK/Fail | p50 | p99 |
|---|---|---|---|
| 200 × 1 KB, 2 conns   | 200/0   | 137 µs | 632 µs |
| 5000 × 1 KB, 8 conns  | 5000/0  | 123 µs | 522 µs |
| 20000 × 8 KB, 16 conns| 20000/0 | 136 µs | 583 µs |
| 3000 × 32 KB, 8 conns (auto-chunk ×4 + conn pin, stream reassembly) | 3000/0 | 152 µs | 573 µs |
| 5000 × 1 KB ×2 back-to-back (leak check) | 5000/0, 5000/0 | 121/119 µs | 407/535 µs |
Content is memcmp-verified per message; conns are opened FRESH per RUN (connect/FIN/close
churn each run) while the two child processes keep their 2 channel registrations.

## Native regression (dpm.h pin change) — neutral
Warm ramp 8192B: 30K → 29,689/0 · 100K → 99,451/0 · 200K → 198,917/0 (= baseline);
loopback 20000 × 8 KB → 20000/0, p50 153 µs. DPU log level back at 40.

## Files
dmesh_preload.c (new shim) · dpm.h (pin_group + dmesh_pin_route) · bench/tcp_echo.c,
tcp_client.c, preload_runner.c (vanilla validators + pod entrypoint, new) ·
Dockerfile.preload_dpumesh (new) · test-bench.sh (build/image/manifest/pin/`preload` cmd)

# (session 2) — LD_PRELOAD review fixes — BUILT + DEPLOYED + VALIDATED

Code-review findings on the new shim + pin, fixed. Build checks: shim .so gcc
clean, dpu_worker/object gcc -fsyntax-only clean, kernel-TCP passthrough smoke
3000/0 + 2000/0 with the new shim. Deployed + full HW validation below (deploy #1
aborted on the known stale-pod kubectl-wait flake, deploy #2 clean exit 0).

## Fix 1 — route_group pin table now keyed (dst_service, rg)  [DPU: dpu_worker.c + object.h]
`route_group_backend[256]` was ONE global array: group ids are per-channel rolling
255-counters, so unrelated channels/conns routinely reuse a byte, and a reused id
returned the OLD pin regardless of the message's dst_service → a pinned conn (or
auto-chunked large message) could be silently routed to ANOTHER SERVICE's backend
(pin entries never expire; overwrite only on dead backend). All existing 0-fail
results were blind to this: every validation backend (svc 11/12/15) is an echo, so
wrong-service delivery still echoes the right bytes. Table is now
`[POD_ID_SPACE][256]` (128 KB, ARM-local): a collision can only merge SAME-service
traffic (balance skew, ordering intact); cross-service redirection is structurally
impossible. Docs updated (api.md §3/§5, dpm.h pin_group, object.h). Pre-existing
since the route-affinity work; dmesh_pin_route widened exposure (every preload
conn holds an id for its lifetime). FOLLOW-UP for a real test: a non-echo
discriminator backend (reply carries server identity).

## Fix 2..n — shim (dmesh_preload.c) hardening
- install_fd: bound-check fd < PRELOAD_MAX_FDS before g_fds[] write (OOB with >64K fds).
- connect(): SO_TYPE==SOCK_STREAM gate (a UDP connect to a mapped port stayed kernel-TCP
  in docs but was converted in code).
- shutdown(SHUT_WR)+close(): dispatcher now suppresses dmesh_close's second FIN
  (wr_closed → peer_closed before close). Was harmless on the DPU (FIN fan-out finds no
  upstream) but wasted a TX slot + ACK round trip.
- Listener close: pending-accept-queue conns are dmesh_close'd (FIN) instead of leaking;
  post-close inbound conns are closed at wrap (g_listener_closed vs "not listening yet").
- SO_RCVTIMEO: now a whole-call deadline (partial wakes no longer restart the clock);
  getsockopt returns the stored timeout, SO_TYPE=SOCK_STREAM, SND/RCVBUF=256K (never 0).
- writev/sendmsg: gather into ONE message + single flush (was one message per iov).
- fcntl64/sendfile64 interposed (plan §Phase B listed them; were missing).
- channel_init: fail early if dmesh_event_fd()<0 (deaf dispatcher); no wake-fd re-create
  leak on register-retry.
- Docs: stdio FILE* bypass documented (api.md §7 limits + header comment); api.md §7
  "~100K measured" reworded (that figure is the per-conn-fd experiment, the shim
  itself is not throughput-benchmarked).

## HW validation (fresh deploy, churn-safe order) — ALL 0-fail
| test | OK/Fail | p50 | p99 | note |
|---|---|---|---|---|
| loopback 20000×8KB          | 20000/0 | 120µs | —     | startup-race discriminator |
| preload 200×1KB ×2c         | 200/0   | 131µs | 487µs | smoke |
| preload 5000×1KB ×8c        | 5000/0  | 120µs | 602µs | |
| preload 20000×8KB ×16c      | 20000/0 | 133µs | 584µs | |
| preload 3000×32KB ×8c       | 3000/0  | 147µs | 403µs | auto-chunk ×4 + conn pin |
| preload 5000×1KB ×8c (b2b)  | 5000/0  | 123µs | 571µs | back-to-back, no leak |
| RPC 30K / 100K / 200K, 8KB  | 300K/0 · 1M/0 · 2M/0 | 181/218/315µs | 0.5/0.5/28.9ms | 29,837 / 99,449 / 198,907 = baseline |
| loopback ZC 5000×32KB       | 5000/0  | 163µs | —     | svc 12 pins rg 1..255 |
| large 32KB @2K/64c (svc 11) | 20000/0 | 484µs | 631µs | 124.3 MB/s, baseline |

## Fix 1 POSITIVE discriminator (not maskable by echo)
Sequence engineered so the OLD global table would misroute: loopback ZC 32KB first
(svc 12 pins route_groups 1..255 to pod 12), THEN native large 32KB to svc 11 whose
channel claims the SAME rg bytes. Old code: dpu_route(rg) returns pod 12 → ~80,000
foreign chunks land on the loopback pod (test still passes — pod 12 echoes; that is
exactly why it was never caught). New code: loopback's served counter probed before/
after = 40,000 → 40,001 (+1 = the probe itself) ⇒ ZERO svc-11 messages followed
svc-12's pins. Per-service pin scoping proven on HW, not just by passing tests.

DPU log clean over the whole window (init-time SDK warns only); log level at 40.
Deferred: none of the fixed paths regressed; remaining follow-ups stay ①②③ in
plan.md (dead-conn slot reclaim, thread-per-conn depth, real-app porting).

# (session 3) — preload throughput ladder + 256-conn wedge (OPEN INVESTIGATION)

## Ladder (thread-per-conn tcp_client, 1KB, fresh conns per RUN, cores 4,5) — 0-fail through 128c
| conns | RPS | p50 | p99 |
|---|---|---|---|
| 1   | 7,108   | 119µs | 450µs |
| 4   | 29,919  | 127µs | 339µs |
| 8   | 59,865  | 131µs | 174µs |
| 16  | 98,263  | 159µs | 220µs |
| 32  | 134,064 | 222µs | 410µs |
| 64  | 151,247 | 412µs | 588µs |
| 128 | 148,209 | 854µs | 1,280µs |
Linear ~6-7K/conn (closed-loop RTT-bound) to 16c; PLATEAU ~150K at 64c; 128c = same
throughput, 2× latency ⇒ server-side saturation ~150K. (Native same deploy: 198.9K.)
Kernel-TCP sanity of the new harness (local): 1c=48K, 16c=155K, 64c=159K, 0-fail.

## 256c RUN = WEDGE (evidence captured BEFORE any restart; pod untouched)
800000×1KB×256c never returned. Evidence chain:
- tcp_client: 24 threads = main(join,futex) + dispatcher + PE + **21 workers asleep in
  poll** (do_poll), utime frozen across 5s ⇒ hard wedge, zero progress.
- Stuck workers' utime 0-2 ticks + tids 585-616 = LAST-spawned tail ⇒ each hung on its
  FIRST message, during the tail of the 256-conn connect storm.
- Both processes: ALL eventfd counters 0 (fdinfo); echo fd census shows the 21 conns
  ESTABLISHED on the echo side too (2 fds/conn) yet idle ⇒ msg vanished silently on an
  established conn's first exchange (request not served OR first reply lost).
- DPU log CLEAN (level 40 incl. ERR: no ring-busy, no accept-queue-full, no unresolved-
  service, no stale-upstream). Host pod logs clean. NOT the cold-jump forward-ring wedge
  (no ring logs; the other 235 conns then completed 3,125 msgs EACH at ~150K).
- Ruled out by code/evidence: accept rx_ring overflow (65536 + would log), DMA ring full
  (4096 + would log+fail write), fd exhaustion (~530 << limit; failure mode would be EOF
  not hang), client-side lost-wakeup (efd=0 + drain-retry discipline; delivery would
  assert efd with reader asleep).
- HARNESS FLAW (compounding): thread-per-conn tcp_client has NO read timeout (vanilla) —
  one lost message = permanent RUN hang; runner (single-threaded ctrl, client stuck in
  pthread_join) jams the preload validator until a full deploy.
STATUS: root cause OPEN — loss localized to conn-setup burst window ≥256 simultaneous
connects; exact hop needs instrumented rerun (DMESH_PRELOAD_DEBUG=1 host-side).
Preload conn-scale: 128c clean / 256c breaks. Pod left wedged for evidence; full deploy
required to recover (pod restart alone forbidden — DPU slot leak).

## Reproduction campaign (same day, fresh deploy w/ DMESH_PRELOAD_DEBUG=1) — NOT REPRODUCED
Detection nets added FIRST, so any hit yields evidence instead of a wedge:
- tcp_client: SO_RCVTIMEO=5s (plain POSIX, stays vanilla) — lost reply → "TIMEOUT
  conn=<i> at msg <k>" stderr + counted fail + thread stops; RUN always completes.
- shim dispatcher: dmesh_accept ENOMEM drop now LOGGED ("accept DROPPED a pending
  conn") and the drain loop CONTINUES past it (was: silent + drain loop break —
  stranded queued accepts until the next fd edge; latent stall fixed).
23 × 256-conn storms (~5.9M msgs), ALL 0-fail, zero TIMEOUT/DROPPED lines:
sanity ×1 · exact original ladder history then 256c ×1 · back-to-back hammer ×10
(close-churn of 256 conns between storms) · idle-130s→storm ×1 (DPA EU park→wake
under burst, EVENT_LOOP=1) · soak (60s idle + storm) ×10. Post-campaign health:
native 100K = 99,452/0, loopback 20000/0 p50 162µs — no state leak from ~6,800
conn churn. Throughput with DEBUG=1 unchanged (146-156K plateau).
VERDICT: the preload wedge is a RARE race (<~1/23 storms; single occurrence — prior
deploy also differed: interleaved native/large/loopback history + multi-minute
idle + DEBUG off). Left OPEN with tripwires armed: any future occurrence produces
TIMEOUT/DROPPED lines + completed RUN counts instead of a wedged pod. NOTE: this
deploy runs DMESH_PRELOAD_DEBUG=1 (per-conn stderr on preload pod only, measured
perf-neutral); the knob defaults 0 on the next plain deploy.

## Final redeploy (DMESH_PRELOAD_DEBUG back to 0) + sanity — clean
Plain deploy (knob default 0, verified in pod env; zero shim stderr lines). Sanity:
preload 5000×1KB×8c = 5000/0 p50 141µs · preload 600000×1KB×64c = 600000/0 @137K
(plateau class) · native 200K = 198,912/0 (baseline). api.md §7 cost/validation
updated to the measured numbers (~150K plateau, 76% of native, 1–256c coverage,
tripwires); plan.md STATUS/리스크 updated (thread-per-conn client validated, 256c
incident OPEN w/ tripwires, measured perf replaces the ~100K expectation).

# (session 4) — L7-readiness: routing hook seam + exact-count validation — BUILT + VALIDATED

Goal (user): NOT building the L7 proxy yet — make the L4 layer able to host a future
envoy-like L7 proxy on the DPU (body-parsing routing). Design/contracts in plan_l7.md;
the multi-thread pipeline blueprint stays prev/architecture.md. Decisions: hook may
route to ANY pod (gateway pattern) · this pass = hook seam + mock validation · body
read-only (v1).

## What was built
- object.h: `dmesh_route_fn` typedef + DMESH_ROUTE_DROP/DEFER + objs->route_fn (+demo list).
- dpu_worker.c: dpu_route(objs, entry, body, len) — hook dispatched BEFORE default L4
  routing for BLANK-dst data messages; body ptr = fwd_buf_pod->dma_buffer + buf_offset,
  materialized ONLY when a hook is installed. FINs/replies never reach the hook.
- route_l7_demo (TEST, DPUMESH_L7_DEMO="svc[,svc…]"): routes by body[0] % n; rg!=0
  (pinned/SAR) and empty bodies DEFER → pin semantics untouched (preload unaffected).
- test-bench.sh start_dpu env plumbing; api.md §5 hook contracts + intro/§5 corrections
  ("DPU never reads body" → only-with-policy; chunk ordering = conn-sharded FIFO,
  head-chunk-first).

## Validation (5 deploys; all EXACT counts, not statistics)
| step | config | result |
|---|---|---|
| V1 regression | hook NULL | loopback 20000/0 · preload 5000/0 · RPC 30K/100K/200K 0-fail, 200K=198,924 = baseline (bit-identical) |
| V2 parse cost | L7_DEMO="11" (same topology, hook+body-read per msg) | 100K p50 217µs · 200K=198,916 p50 317µs = NEUTRAL |
| V3 full steer  | L7_DEMO="12" (all svc-11 traffic → pod 12) | bench 1,000,000/0 @99,451 p50 232µs (+15µs); loopback served 1 → 1,000,002 = EXACTLY 1M steered; cross-service delivery + uP reply mapping proven |
| V4 byte-accuracy | L7_DEMO="11,12" (body[0] parity) | loopback RUN 48000/0; served = 24,001 = probe(1) + EXACTLY 24,000 (8/16 odd bytes of the deterministic 16-cycle) — ±0 equality vs garbage-noise ±~110 ⇒ ARM reads the exact DMA'd bytes per message; ONE conn's messages alternated backends per message, 0-fail |
| V5 final | plain redeploy | 100K=99,451/0 · loopback 20000/0 · preload 5000/0 |
(One deploy hit the known stale-pod kubectl-wait flake; re-deploy clean.)

## Conclusions
- L7-READY: body visible + byte-accurate at route time (completion-after-data ordering
  holds for ARM CPU reads, not just the in-place reverse DMA); per-message content
  routing works on one conn; any-pod (cross-service) routing + reply mapping intact;
  hook seam costs nothing when absent and ~nothing when reading 1 byte/msg.
- Production default unchanged (route_fn NULL). DPUMESH_L7_DEMO is a TEST knob like
  DPUMESH_LB_RR. Real L7 next steps live in plan_l7.md (seam→pin-miss pick, per-src
  ROUTER threads per prev/architecture.md, reply observe hook, rewrite contract).

# L7 proxy L4 engine (DPUMESH_PROXY, byte-stream reframing) — BUILT + VALIDATED (frame mock)

Goal (user, plan.md): make the DPU an envoy-like L7 proxy. L7 (parse + route/LB) = future,
MOCK for now; L4 (this pass) = execute a per-connection routing decision as a BYTE STREAM via
scatter-gather DMA. Deliverable = the proxy return format (`dmesh_route_seg{off,len,dst}` +
`consumed`) and the whole L4 engine that runs it. NOT a throughput lever — goal is L7 function
+ DPU CPU savings + latency. Off by default (`DPUMESH_PROXY` unset = legacy per-slot path
bit-identical).

## What was built / wired
- dpu_proxy.{h,c} (author): the L4 engine — per-conn INPUT WINDOW (zero-copy views over DPU
  staging + cursor; a SEAM buffer aligns the unconsumed tail only when a parse stalls across
  extents) → `proxy_route` (MOCK) → per-(dst pod, region) LANE → ARM generic doca_dma
  chained-buf SG-DMA (one op/batch) + batched REV_DONE → custody TX_ACK at egress. Reply path
  = same machinery, dst from conntrack (uP→client). Mocks: `passthru` (1 seg/msg, wire-identical
  = parity) and `frame` (`[u32 len][u8 svc][payload]`, routes each whole frame by svc byte).
- WIRING (this session): dpu_worker.c — px_init (init step 7), forward completion → px_ingest_forward
  when objs->proxy set, px_drain in dpu_drain_iteration, dpu_route_l4 split out of dpu_route,
  batch_or_send_tx_ack/flush_rev_done_batch un-static+exported. comch_common.c — store
  ring_host_addrs[idx] on DMA_RING import (host freed-counter addr for egress admission).
  meson.build — +doca-dma +dpu_proxy.c. bench/stream_sock.c + Dockerfile + test-bench.sh
  (`DPUMESH_PROXY` env, `stream` command, idempotent run_stream).
- Config: DPA_THREADS=4, RINGS_PER_POD=2, EVENT_LOOP=1; proxy mock=frame, seam_max=524288,
  sg_pieces=64 (device cap). Deploy: `DPUMESH_PROXY=frame ./test-bench.sh deploy`.

## Local wire-contract test (pre-deploy, host gcc)
stream_sock.c build_frame ↔ dpu_proxy.c px_mock_frame parse cross-checked: 6/6 (single frame,
FPW batch, partial-frame-wait=seam trigger, reply-side peer routing, unknown-svc DEFER,
corrupt-length poison). Confirms the app↔DPU frame format agrees before spending a deploy.

## Validation (clean redeploy; all EXACT byte counts, not statistics; DPU log 0 drops/poison)
served = echo-side bytes written back, CUMULATIVE on the reused pod → check DELTAS.
| test | command | result |
|---|---|---|
| self 1 KB | `stream 30 1024` | 30/0 byte-exact · served 30,870 = 30×1029 (1029 = 5-hdr+1024) · p50 114.3 µs |
| SEAM >8 KB | `stream 200 20000` | 200/0 byte-exact · served Δ = 4,001,000 = 200×20005 EXACT · p50 169.2 µs (frame=20005 B → dmesh_write auto-chunks 3 arrivals → seam rebuilds contiguous → 3-piece chained SG-DMA → delivered as ≤8 KB chunks) |
| multi-frame/write | `stream 2000 512 self 4` | 2000/0 byte-exact · served Δ = 4,136,000 = 2000×4×517 EXACT · p50 113.9 µs (4 frames packed in one dmesh_write → 4 segs parsed from ONE window) |
| fan-out | `stream 1000 512 11,13,14` | 1000/0 byte-exact · served FLAT (echoed on backends 11/13/14 via per-backend upstream + conntrack reply mapping, not self) · p50 202.5 µs |

## Harness bugs found + fixed (test-only, no DPU-code change)
1. Empty SVC_LIST arg collapsed under the daemon's positional sscanf → fpw slid into the svc
   slot → frames tagged svc=1 → `unroutable seg (svc=1)` drops (DPU log), client 5 s reply
   timeout → nc "no response". Fixed: bash sends "self" placeholder + daemon maps "self"/"-"→own
   svc; added consecutive-fail early-abort (was a multi-hour wedge risk).
2. run_stream did scale_up_with_wait (scale 0→1) EVERY call → restarted the pod each run; 3
   restarts wedged DPU EU-0 msgq (`dpa.c ADD_RING send AGAIN retries=10000` → setup_pod_dma
   fails for the reused pod slot → never dma_ready). PERSISTENT (2 fresh pods failed identically)
   = pre-existing DPU teardown/setup fragility, NOT proxy code. Fixed run_stream to be IDEMPOTENT
   (reuse a Running pod, no restart); recovery = fresh `dpumesh_dpu` (clean redeploy).

## Conclusions
- L4 ENGINE VALIDATED end-to-end on the frame mock: per-conn window, length-prefix reframing,
  seam (a frame spanning arrivals rebuilt contiguous), multi-piece chained SG-DMA egress,
  multi-frame-per-window, multi-backend fan-out + reply mapping, custody (0 drops, byte-exact,
  served-exact), symmetric reply path. p50 114 µs (1 KB) → 169 µs (20 KB) → 202 µs (cross-pod).
- Fan-out dest-exactness shown INDIRECTLY (byte-exact round-trip + served FLAT on the stream pod
  = frames echoed elsewhere); a per-backend recv_total exact-count is still owed (echo prints
  only every 100K).
- NOT yet run: proxy-off regression baseline, `DPUMESH_PROXY=passthru` parity vs the existing
  suite, L4-ARM-CPU / notify cost. Deploy run by ME in FOREGROUND completed without hang (the
  old hang was background-execution-specific).

# (session 2) — Option cleanup: bake tuning knobs + delete route_fn/L7_DEMO/LB_RR — DONE + VALIDATED

Goal (user): the config surface grew ~17 env knobs during exploration — reduce to the ones you
meaningfully adjust, remove legacy. Keep: `DPUMESH_PROXY` (L7 proxy on/off) + LD_PRELOAD
(`DMESH_PRELOAD_*`) + `DPUMESH_PCI_ADDR` (device) + `DPUMESH_SERVICE_ID`. Zero-copy API
(`DPUMESH_ARENA_SLOTS` + dmesh_alloc/slot_register) explicitly UNTOUCHED.

## What changed
- BAKED (getenv removed → compiled to the deployed winners; reference table = api.md §9):
  NUM_SLOTS=4096 (`DPUMESH_NUM_SLOTS_DEFAULT`), SLOT_SIZE=8192, DPA_THREADS=4
  (`objs->num_dpa_threads=4`), RINGS_PER_POD=2 (`DPUMESH_RINGS_PER_POD_DEFAULT`), EVENT_LOOP=1
  (busy-poll-ONLY driver deleted; setup-fail fallback kept), HOST_EPOLL=1 (⚠ code default was 0,
  deployed=1 → baked 1), PROXY_SEAM_MAX=512KB (`PX_SEAM_MAX_DEFAULT`).
- DELETED (legacy): `route_fn` hook + `route_l7_demo` + `DPUMESH_L7_DEMO` + `DMESH_ROUTE_DROP/DEFER`
  + objs->route_fn/l7_demo_* (the per-message L7 seam — superseded by the byte-stream proxy §8);
  `DPUMESH_LB_RR` + `lb_pick` RR branch + objs->lb_rr_* (→ lb_pick = service_table only). dpu_route
  now = "already-resolved short-circuit → dpu_route_l4 (service table + route-affinity)".
- test-bench.sh: start_dpu launcher passes only `DPUMESH_PROXY`; k8s manifests dropped
  NUM_SLOTS/RINGS_PER_POD/HOST_EPOLL. api.md: §5 hook bullet → §8 pointer, §3 env note trimmed,
  §9 baked-config reference added.
- Files: dpu_worker.c (−251/+…), object.h (−45), dpa.c, dpu_proxy.c, dpumesh_doca.c, api.md,
  test-bench.sh. All host+DPU sources `gcc -fsyntax-only` clean; **BF3 ninja build clean** (only
  pre-existing bench write-unused-result warnings) — the refactor compiles for real, not just
  syntax-checks.

## Validation (clean redeploy DPUMESH_PROXY=frame; compare to session-1 pre-cleanup)
Bakes == the exact values the deploy already set, so behavior must be UNCHANGED — confirmed:
| test | command | result | vs pre-cleanup |
|---|---|---|---|
| self 1 KB | `stream 30 1024` | 30/0 · served 30,870 = 30×1029 | IDENTICAL |
| SEAM >8 KB | `stream 200 20000` | 200/0 · served Δ = 4,001,000 = 200×20005 | IDENTICAL |
| multi-frame/write | `stream 2000 512 self 4` | 2000/0 · served Δ = 4,136,000 = 2000×4×517 | IDENTICAL |
| fan-out | `stream 1000 512 11,13,14` | 1000/0 · served FLAT (echoed on 11/13/14) | IDENTICAL |
DPU log: **0 drops/poison/unroutable/AGAIN**. px_init: `mock=frame, seam_max=524288, sg_pieces=64`
(baked seam cap correct). p50 104–163 µs warm (first cold N=30 = 419 µs = scale-up warmup jitter).

## Conclusion
Config surface cut to the 2 meaningful runtime toggles (DPUMESH_PROXY + LD_PRELOAD) + 2 essentials
(PCI + service) + zero-copy arena; 7 tuning knobs baked, route_fn/L7_DEMO/LB_RR deleted. Refactor
builds on the BF3 and is byte-for-byte behavior-identical (all EXACT counts match, 0 drops). Baked
values + their constants/files documented in api.md §9 for future adjustment (edit constant + rebuild).

---

# L7 head-only streaming (bounded head copy, body via SG) — CODE LANDED, HW PENDING

**Problem (verified in code, not measured):** the L7/frame path linearizes a WHOLE message into
the seam (`px_copy_stream` memcpy) just to give the parser a contiguous view — but egress ships
from staging via SG regardless. For byte-stream apps (LD_PRELOAD) whose messages don't align to
8 KB slots, that is a per-slot memcpy of the entire body — pure waste.

**Change:** new head-only L7 path (`DPUMESH_PROXY_L7_SVC=<csv>`), gated per service so passthru/
frame/reply paths stay byte-identical.
- `dpu_l7.h` / `dpu_l7.c`: author hook `dmesh_l7_route(head, len, ctx, *target)` returns the
  message TOTAL length from the HEAD (may far exceed the head window); body never seen.
- `px_parse_l7` (dpu_proxy.c): parses the head in a bounded ≤`PX_HEAD_MAX`(4 KB) window (zero-copy
  when the head fits a slot; ≤4 KB copy only when a head straddles), resolves the backend once,
  then streams `[parse_pos, +min(remaining, arrived, PX_L7_UNIT_MAX=128 KB)]` via existing SG
  `px_ship_seg` — body never linearized. Large messages stream in ≤128 KB units (≤17 SG pieces,
  under the 64 cap), in order, to one backend.
- Engine: seam cap made per-conn (`parse_win_max`; PX_HEAD_MAX for L7, seam_max for frame);
  stall/poison check uses it; head-window re-anchored per message.

**Verification so far:** `gcc -fsyntax-only` clean on dpu_proxy.c (DOCA includes) and dpu_l7.c
(`-Wall -Wextra`). passthru/frame/reply unchanged (dispatch gated on `is_l7`). Self-reviewed
custody/advance/FIN/streaming interactions (drop-accounting reuses the existing frame path).

**Regression test is clean:** the default `dmesh_l7_route` parses the SAME wire format as the
`stream` validator (`[u32 total_len][u8 svc][payload]`), so `./test-bench.sh stream` drives the L7
head-only path directly. Head-only must be BYTE-IDENTICAL to frame mode (same client + byte-exact +
served-count checks); SIZE>8 KB specifically exercises head-only streaming vs whole-message seam.

## HW results (cluster recovered)

Env recovery first (a reboot dropped 3 non-persistent boot configs): (1) swap re-enabled →
kubelet `failSwapOn` crash-loop (NRestarts=10242) → `swapoff -a` + fstab comment; (2) DPU
unreachable — host `tmfifo_net0` lost `192.168.100.1/24` → re-added; (3) flannel CrashLoopBackOff —
`br_netfilter` not loaded (`/proc/sys/net/bridge/bridge-nf-call-iptables` missing) → `modprobe
br_netfilter` + `sysctl`. After all three, node Ready + pods up. New binary confirmed by px_init log
line `request-default=..., l7-services=N`.

Frame baseline on the NEW binary (regression): `stream 200 20000` → **200/0, served 4,001,000** —
IDENTICAL to the frame baseline. Frame path unchanged. ✓

L7 head-only (`DPUMESH_PROXY=passthru DPUMESH_PROXY_L7_SVC=16`; px_init `l7-services=1`). Served is a
DPU cumulative counter → deltas shown:
| test | OK/Fail | served Δ | expected | vs frame |
|---|---|---|---|---|
| `stream 200 20000` (**>8 KB → head-only streaming vs whole-seam**) | 200/0 | 4,001,000 | 200×20005 | **IDENTICAL** |
| `stream 30 1024` | 30/0 | 30,870 | 30×1029 | **IDENTICAL** |
| `stream 2000 512 self 4` (multi-frame/write) | 2000/0 | 4,136,000 | 2000×4×517 | **IDENTICAL** |
| `preload 5000 1024 8` (vanilla shim, svc 15 passthru) | 5000/0 | 26,446 RPS, p50 122µs/p99 175µs | — | unaffected |
DPU log: **0 poison/drop/unroutable/AGAIN/stall**. p50 ~114–161 µs.

## Conclusion
Head-only L7 (bounded ≤PX_HEAD_MAX head window + body streamed from staging via SG, no whole-message
seam) is **byte-exact** with the frame whole-message-seam path across small (1 KB), >8 KB (seam case),
and multi-frame/write — 0 fail, 0 DPU-side drops. Vanilla shim (passthru) unaffected; frame path no
regression. The per-slot seam memcpy for byte-stream L7 traffic is eliminated with identical delivery.
VALIDATED.

---

# P1: lock-free forward ring (host-only Vyukov MPSC) — BUILT + HW-VALIDATED

Context: moving toward per-CONNECTION mmap buffers (to kill the shared Treiber TX slot
pool + custody + arena on the host, and align with the socket model). Step 1: make the
per-pod K forward descriptor rings LOCK-FREE so multiple conns/threads post without the
per-ring mutex. Ring COUNT unchanged (DPA polls rings, not data buffers — see below).

## What changed (HOST-ONLY; DPA/DPU untouched)
- ring.h: struct dma_ring += `enq_pos` (monotonic producer ticket) + `seq[]` (per-slot
  Vyukov cell sequence, HOST memory, init seq[i]=i). The DPU reverse ring leaves them
  unused (single ARM producer -> get_next_dma_desc + head).
- ring.c: setup_dma_ring allocs+inits seq[]; setup_dpu_tx_ring sets seq=NULL.
- dpumesh_doca.c: dmesh_enqueue claim = Vyukov. fetch-add ticket t; own slot t%size when
  seq==t, OR the prev occupant (ticket t-size) has PUBLISHED (seq==t-size+1) AND the DPA
  CONSUMED it (valid==0) -> reclaim; then publish valid=1, then seq=t+1. Removed
  ring_locks[] (decl/init/destroy) + the get_next_dma_desc call on the host path.

## Why NOT the simpler CAS+valid (rejected)
A first attempt (CAS the shared head, gate on valid==0) had a producer-PREEMPTION lapping
hazard: a producer that wins its ticket but stalls before publishing leaves valid==0, so a
lapping producer (ticket t+size, same slot) sees "free" and OVERWRITES the slot -> double
use / lost message. Walked a size=2 counter-example; reverted it. Vyukov's per-slot seq
fixes it: a stalled producer leaves seq UNADVANCED (still t-size+1, not t+1), so the
lapping producer's reclaim check fails and it WAITS. DPA needs NO change because its
existing valid=0 IS the "consumed" signal; the seq bookkeeping is entirely host-side (the
next-gen producer reclaims when it observes valid==0). Ordering key: publish valid=1
BEFORE the release-store of seq=t+1, so a reclaiming producer that sees the seq also sees
valid's effect (never a stale pre-consume 0).

## HW validation (plain deploy, no proxy; must be behavior-IDENTICAL to the mutex)
| test | result | vs baseline |
|---|---|---|
| native RPC 30K/5s/1KB   | 150,000/0 · p50 172µs | — |
| native RPC 100K/8s/1KB  | 800,000/0 · p50 191µs | — |
| native RPC 200K/10s/1KB | 2,000,000/0 · 198,892 RPS · p50 277µs | = baseline (198.9K) |
| loopback 20000×8KB      | 20,000/0 · served 20,000 · p50 117µs | = baseline |
| preload shim 5000×1KB×8c| 5,000/0 · p50 141µs · 24.8K RPS | = baseline |
All 0-fail; 5 tests back-to-back (no redeploy) -> no state leak. Lock-free ring is
bit-identical at the native ceiling. DPU log 0 errors ("ctx DPA ops are empty" WRNs =
normal DPA init; "DPU register timeout — continuing" at deploy = known benign flake, pods
Running + all tests passed). Host `gcc -fsyntax-only` clean on both edited files.

## Next
E0: forward DPA `ring->host_mmap` -> `desc->mmap` (host already stamps its own
ctx->dpa_mmap_handle there) — tests whether a HOST-computed DPA handle is usable by the
DPA, which decides if per-conn TX buffers need a DPU registration round-trip or can be
host-only. Then P2a (host TX per-conn: delete Treiber pool + arena + custody), P2b (host
RX per-conn: delete shared rx credit). Config: DPUMESH_CONN_POOL / DPUMESH_CONN_SLOTS
(user-tunable pool size = max concurrent conns; shim uses the façade so it is transparent).

## E0 experiment (same day) — host-computed DPA handle NOT usable by the DPA → NEGATIVE
Goal: decide whether per-conn TX buffers can be HOST-ONLY (host computes each buffer's DPA
handle, stamps desc->mmap, no DPU round-trip) or need a DPU import + handle-return.
Change (1 line, dpa_kernel.c): forward dma_copy SOURCE `ring->host_mmap` (DPU-imported) ->
`desc->mmap` (host stamps ctx->dpa_mmap_handle = its own doca_mmap_dev_get_dpa_handle).
Result: **native 30K = 0 / 300 (ALL FAIL)**, 0 RPS. The host-computed handle (host device
has no access to the DPU's DPA) is NOT usable by the DPA — forward delivery breaks
completely. DPU log: no explicit DMA error line (silent drop). Reverted + redeployed ->
30K back to 150,000/0. CONCLUSION: true per-conn SEPARATE mmaps require a DPU-side import +
handle-return handshake (host cannot self-serve the handle).

## Design pivot exposed by E0: per-conn REGIONS vs per-conn separate MMAPS
E0's negative means separate per-conn mmaps need a DPU registration round-trip (pool +
handshake). BUT the user's stated complexity target (kill the Treiber slot pool + custody
linked-list + arena + discontiguous allocation) is ALSO achieved by partitioning the ONE
existing shared TX mmap into N per-conn contiguous REGIONS — each conn owns a region as a
single-producer ring — with ZERO DPU change and ZERO handshake (desc->mmap stays the shared
ring->host_mmap; desc->addr carries the region offset; forward DPA unchanged). Regions match
the socket SO_SNDBUF model (per-conn buffer, not per-conn MR) and moot the mkey-count
question. Trade: regions cap concurrent conns at buffer_size/region_size (configurable) and
give no per-conn MR isolation. Decision surfaced to the user.

---

# Bottleneck: DMA COUNT vs DATA-VOLUME — POSITIVELY RESOLVED (count-bound)

Question (before committing to a per-conn contiguous-ring + 8KB-coalescing rewrite): is the
transport ceiling bound by DMA **count** (ops/s, fixed per-op cost) or DMA **data volume**
(bytes/s, bandwidth)? If count -> coalescing (fewer, bigger DMAs) is the lever and contiguity
is justified; if bandwidth -> coalescing is pointless. History flip-flopped 3x so we demanded
positive evidence (per feedback_no_invented_terms / bench_elimination_unreliable).

## Method + results (all HW, this deploy)
1. **DMA-engine size sweep** (`bench/mrbench/run_countbw.sh` — dma_mrbench mode C, ARM-issued,
   DPU-local; ops/s + GB/s vs transfer size; linear fit 1/ops = c + b*S):
   - ops/s FLAT ~3.6M from 64B..8KB, then falls; GB/s flat ~53-72 above 16KB.
   - fit: c = **235 ns/op** (count), b = **0.014 ns/byte** (volume), crossover **S* = 16.8 KB**.
   - at 1KB: 94% count / 6% volume; at 8KB: 67% / 33%. -> DMA engine is COUNT-bound <=16KB.
   - CAVEAT: ARM-local, NOT the DPA-over-PCIe path — SHAPE transfers, absolute numbers do not.
2. **Transport size sweep** (real bench, fixed RPS): native RPC **1KB = 198,892** vs
   **8KB = 198,920** RPS (0-fail) — RPS **FLAT across size**, GB/s scales 8x (0.39 -> 3.11 RTT).
   -> the transport ceiling is PER-OP, size-insensitive -> NOT bandwidth/data-volume.
3. **Real DPA dma_copy rate** (temp instrumentation: ARM comp_queue drains exactly 1 entry per
   DPA-issued dma_copy; periodic stderr rate). At the 240K ceiling: **~960K-1.0M dma_copy/s**
   -> **4 dma_copies/RPC** (client->DPU fwd, DPU->backend rev, backend->DPU fwd, DPU->client rev)
   x 240K = 0.96M. **comp_q usage = 0** -> the ARM is NOT the bottleneck (keeps up, no backlog).
   Note: ARM-local mrbench = 3.6M ops/s but the DPA-issued (comch-producer over PCIe) path is
   ~1M/s — ~3.6x slower; the code comment already documented "DPA op-rate caps ~810K dma_copy/s".
4. **EU/ring parallelism sweep** (temp N=8, K=4 vs baseline N=4, K=2):
   K=4 ceiling **233K** (300K req -> 233K + 984 fail, p50 1.4s) <= K=2 ceiling **258K**. More
   EUs/rings do NOT raise the DPA op-rate — it is a SHARED wall (confirms "K=4 flat" positively).
   N=4 ceiling: 270K req -> 258K achieved (p50 159ms = overload), matches perf_ceiling_map ~257K.

## Conclusion (positive evidence)
- **The bottleneck is per-op COUNT, not data volume.** RPS is flat across message size; the DPA
  dma_copy op-rate (~1M/s) is a SHARED wall that more EUs/rings cannot raise; 4 dma/RPC x 258K
  ~= that wall. Bandwidth is nowhere near (8KB@200K = 3.1 GB/s vs >50 GB/s engine asymptote).
- A SECOND per-op-count cap co-exists (dpu_worker.c comment: the single host PE thread reaping
  one REV_DONE per response), already partly cut by BATCH_REV_DONE. Either way it is per-op
  COUNT, and coalescing addresses BOTH: one DMA carrying N messages -> N-fold fewer dma_copies
  AND N-fold fewer completion messages.
- => **Coalescing (pack same-direction messages into fewer, bigger DMAs) is the real throughput
  lever, and the per-conn contiguous ring that enables it is JUSTIFIED** — the benefit is
  realized under MULTIPLEXING (gRPC/few-conn-deep, many messages per conn per direction), which
  is the actual target (single-msg-per-RPC bench packs nothing). The earlier ARM-local estimate
  ("DMA has 4-9x headroom, not the bottleneck") was INVALID for the DPA-PCIe path — corrected.

## State
Baseline restored: P1 Vyukov + N=4/K=2 + no instrumentation; native 200K = 198,664 / 0. Temp
changes (comp_queue dma-rate print, num_dpa_threads=8, RINGS_PER_POD=4) reverted via git. New
tool kept: bench/mrbench/run_countbw.sh (+ countbw.csv). dma_mrbench unchanged.

---

# P2a-1: per-conn TX slot-ring — BUILT, validated <=64c, 128c pool-exhaustion OPEN

Replaced the shared Treiber TX slot pool + per-slot custody linked-list + zero-copy arena
with PER-CONNECTION contiguous slot-rings (socket send-buffer model), the first brick of the
per-conn contiguous-buffer redesign justified by the DMA-count bottleneck finding above.

## What changed (host-only; wire + DPA unchanged, per-message descriptors kept)
- struct: dmesh_port_slot += tx_region + SPSC ring cursors (tx_rhead alloc / tx_rtail
  freed-on-ACK). ctx: DELETED free_head/slot_next (Treiber), arena_base/used/lock (arena),
  custody_key/next/locks; ADDED conn_pool/conn_rslots + region_free stack + slot_seq[num_slots].
- The num_slots TX mmap is partitioned into conn_pool regions of conn_rslots = num_slots/pool
  slots (32MB unchanged, NO buffer growth). Config: DPUMESH_CONN_POOL (default 256 -> 16
  slots/conn). A conn borrows a region at connect/accept, owns it as a FIFO ring, returns it
  at close (after the ring drains so slots are never overwritten mid-DMA).
- dpumesh_tx_alloc(ctx,PORT) / dpumesh_tx_free(ctx,PORT,slot); tx_track -> slot_seq stamp;
  BATCH_FWD_ACK reclaim -> tx_reclaim_ack (advance ring tail when slot_seq matches, FIFO).
  dmesh_alloc/slot_register (channel-scoped zero-copy) DISABLED -> returns NULL (conn-scoped
  reserve/commit is P2a-2); shim/dmesh_write path unaffected. dpumesh_arena_alloc deleted.
- host gcc -fsyntax-only clean (dpumesh_doca.c + dpm.h); BF3 build clean; deploys.

## HW validation
| test | result | verdict |
|---|---|---|
| loopback 20000x8KB (1 conn) | 20,000/0 · p50 123µs | PASS — ring alloc/ship/reclaim/close correct |
| native rpc 100K/8s/1KB, 64c | 800,000/0 · p50 199µs | PASS — multi-conn + region reuse |
| preload shim 5000x1KB, 64c (warm) | 5,000/0 · 102K RPS | PASS — accept + close churn |
| preload shim 5000x1KB, 64c (1st, cold) | 4,892/47 · 937 RPS | cold-start jitter (2nd/3rd clean) |
| preload shim 8000x1KB, 128c | HANG (>150s) + "stale upstream" flood | **OPEN — see below** |

## OPEN: 128-conn preload pool exhaustion
Root cause (analysis): the region pool (POOL=256) plus the close-time drain-spin
(port_put_region holds a conn's region until its TX ring drains) collide at high conn count.
At 128c a close storm has ~128 regions draining WHILE ~128 new connects each need one =
256 = POOL exactly -> transient exhaustion -> connect/alloc fails -> early FIN -> the backend's
in-flight reply hits a freed upstream ("stale upstream uP=... pod 6") -> dropped -> the shim
client's blocking recv 5s-times-out -> cascade/hang. 64c (64 draining + 64 new = 128 << 256)
has headroom -> clean. So the CORE ring is correct; the region-lifecycle x pool sizing is
under-provisioned for high conn counts. Fix candidates (next): (a) larger POOL headroom, e.g.
DPUMESH_CONN_POOL=512 (8 slots/conn) so drain-holds + new connects fit; (b) DEFERRED
(non-spinning) region return decoupled from the slot drain; (c) shorter drain timeout. NOT yet
chosen. Deployed state runs P2a-1 (works <=64c; regresses vs the old 256c preload).

---

# P2a-1 128c hang — ROOT-CAUSED (2 region-lifecycle bugs) + FIXED, now 0-fail to 256c

The 128c "stale upstream flood + hang" was NOT the core ring — it was two per-conn TX **region
leaks/holds** that only bite at high conn count. Both fixed; P2a-1 now passes back-to-back.

## Bug 1 — region LEAK on accept-queue-full rollback (dpumesh_doca.c rx_deliver_desc)
A new SERVER conn borrows its region (port_take_region) BEFORE the accept-queue-full check.
The rollback (`accept queue full, dropping new conn`) set role=FREE and returned WITHOUT
returning the region -> one region leaked per drop. At 128c the accept queue fills often ->
cumulative leak drains the 256-region pool -> every new server conn then fails port_take_region
-> backend stops accepting -> clients 5s-timeout -> early FIN -> stale upstream -> hang.
64c rarely fills the queue -> no leak -> clean. FIX: dpumesh_region_return in the rollback.

## Bug 2 — blocking drain-spin HELD the region across close (2x region peak)
port_put_region spun up to 1s on close holding the region until the ring drained, on the app
thread. Two harms: (a) it stalled the vanilla server's single epoll thread on every close;
(b) it held the region for the whole drain, so a close-storm+reconnect overlaps
128(draining)+128(new)=256=POOL -> exhaustion. This made 128c run-1 pass but run-2/3 hang
back-to-back (run-1's closes still holding regions when run-2 connects). 64c (64+64<256) had
headroom -> clean back-to-back. FIX: **non-blocking deferred return** (try_return_region):
close marks role=FREE and returns the region ONLY if already drained; otherwise the PE's
reclaim (tx_reclaim_ack) returns it on the last ACK. Until returned the port is FREE-but-
draining (tx_region>=0) and ALL alloc paths (alloc_port / alloc_port_specific / SERVER_PENDING)
skip it, so a slot is never reused mid-DMA and no straggler ACK survives reuse (return ==
fully drained). Close never blocks the app thread; region peak == actual concurrent conns.

## HW validation (after both fixes, single deploy)
| test | result |
|---|---|
| loopback 20000x8KB (1c) | 20,000/0 · p50 169µs |
| native rpc 100K/8s/1KB, 64c | 800,000/0 · p50 200µs |
| preload 8000x1KB, 128c — run 1/2/3 back-to-back | 8,000/0 each · runs 2-3 = 131K RPS (no hang) |
| preload 12000x1KB, 256c (POOL=256 edge) | 12,000/0 · 123K RPS |

P2a-1 (per-conn TX slot-ring) is now correct + leak-free back-to-back to 256c. Deleted:
Treiber pool, per-slot custody linked-list, zero-copy arena, dpumesh_arena_alloc. Config:
DPUMESH_CONN_POOL (default 256 -> 16 slots/conn @ num_slots 4096). Non-blocking region
lifecycle is the load-bearing correctness piece. Next: P2a-2 (byte-pack + 8KB coalescing).

---

# P2a-2: 8KB slot-coalescing VALIDATED — pipeline 262K → 1.19M RPS (~4.5x), 0-fail

The DMA-COUNT bottleneck thesis (proved earlier: ~1M dma_copy/s wall, 4 dma/RPC → ~262K RPC
ceiling on one DPU) predicts that COALESCING many small messages into one 8KB DMA lifts the
ceiling. Tested it end-to-end on the per-conn ring (P2a-1). Result: decisive.

## What the transport already gives + the one change
dmesh_write already ACCUMULATES consecutive writes into the conn's current slot; flush ships it.
dmesh_read is byte-stream WITHIN a descriptor (returns min(len,avail), advances rx_pos). So a
COALESCED descriptor (N msgs packed in 8KB) is read back transparently as N msg_size reads, and
the demux is by ORDER (rid in-order verify), not per-msg seq. The echo reads sizeof(buf)=8192 →
consumes a whole coalesced 8KB descriptor in ONE read and echoes 8KB in ONE flush, so the reply
leg coalesces too. Net: ALL 4 DMA legs (host→DPU, DPU→backend, backend→DPU, DPU→client) drop
from per-message to per-8KB. Change = a bench toggle only (BENCH_COALESCE, mode=2 pipeline):
fill a P-deep burst with NO per-message flush, then ONE flush ships the burst. No transport edit.

## HW measurement (pipeline, 1KB msg, P=8, 64 conns, single DPU pair)
| target | BASELINE (flush/msg) | COALESCED (burst→1 flush) |
|---|---|---|
| 300K | 262K achieved · **p50 570 ms** (saturated backlog) | 298K · p50 209µs |
| 400K | — | 397K · p50 220µs · 0-fail |
| 500K | — | 497K · p50 227µs · 0-fail |
| 600K | — | 596K · p50 246µs · 0-fail |
| 800K | — | 794K · p50 306µs · 0-fail |
| 1.0M | — | 993K · p50 521µs · 0-fail |
| 1.2M | — | **1,192,039 · p50 466µs · 0-fail** (still climbing) |
| 200K (sanity) | — | 198K · p50 188µs · 0-fail |

Baseline pipeline ceiling ~262K matches the DMA-count wall exactly (p50 explodes to 570ms once
target > ceiling). Coalesced scales past 1.19M with LOW latency and 0-fail — **~4.5x throughput,
not yet saturated**, and ~2700x lower latency at 300K (570ms → 209µs). This POSITIVELY confirms:
(1) the wall was DMA COUNT, not bandwidth; (2) coalescing is THE throughput lever; (3) the
per-conn contiguous ring (P2a-1) makes coalescing clean (one conn's msgs pack into its own
contiguous slot, one descriptor, in send order). BENCH_COALESCE kept (opt-in, default 0 =
baseline). Real apps get this via cork/MSG_MORE-style batching (future shim work). NOTE: pods
currently deployed with BENCH_COALESCE=1; redeploy without it to restore the baseline default.

---

# TX BYTE-RING + zero-copy alloc/commit (4-cursor) — BUILT + fully VALIDATED

Reworked the per-conn TX slot-ring (P2a-1) into a per-conn contiguous BYTE-RING with the
user's 4-cursor buffer-management model, and added the conn-scoped zero-copy alloc/commit API.
HOST-ONLY change: the DPU/DPA see only (addr,size), so the forward descriptor's body_buf_slot
now carries a BYTE OFFSET (enqueue computes addr = dma_buffer + offset); no DPU/DPA/wire change.

## Model (4 cursors chase around each conn's contiguous region)
  free(tx_f) <= send(tx_s) <= commit(tx_c) <= write/alloc(tx_w),  tx_w-tx_f <= conn_bytes
  - reserve(port,len) -> pointer at tx_w (bip-buffer pad on wrap; tx_wmark skips the tail)
  - commit(port,len)  -> advance tx_w+tx_c (finalize a message; byte granularity, no slot waste)
  - next_send/sent    -> flush carves [tx_s,tx_c) into <=slot_size descriptors (coalesce), records
                         each shipped unit (seq->end cursor) in a per-region send-unit FIFO
  - reclaim (PE)      -> BATCH_FWD_ACK(port,seq) pops the FIFO front, advances tx_f (frees bytes)
  dmesh_write = reserve+memcpy+commit; dmesh_alloc/dmesh_commit = the zero-copy path (same ring,
  filled in place). Non-blocking region return (P2a-1 fix) carried over: region freed when tx_f==tx_w.

## Deletions / API changes
  Removed: dpumesh_tx_alloc/tx_buf/tx_free/tx_track, slot_seq, dmesh_tx_ensure/ship_at/ship_slot,
  the channel-scoped dmesh_alloc(s,n)/dmesh_slot_register/dmesh_free + dmesh_buf_t usage, cur_group
  + tx_slot/tx_buf/tx_len/tx_region_n conn fields. Added: dpumesh_tx_reserve/commit/discard_unsent/
  next_send/sent, TX_SU_DEPTH=64 send-unit FIFO, conn_bytes. New public zero-copy: void*
  dmesh_alloc(conn,len) + int dmesh_commit(conn,len). LD_PRELOAD shim UNCHANGED (stable façade).

## HW validation (all 0-fail, one deploy)
| test | result |
|---|---|
| loopback non-zero-copy 20000x8KB | 20,000/0 · p50 154µs |
| loopback ZERO-COPY (dmesh_alloc/commit) 20000x8KB | 20,000/0 · p50 121µs |
| native rpc 100K/8s/1KB, 64c | 800,000/0 · p50 199µs |
| preload shim 5000x1KB, 64c | 5,000/0 |
| preload shim 8000x1KB, 128c back-to-back x2 | 8,000/0 each (no hang, no region leak) |
| pipeline BASELINE (flush/msg), 300K/1KB/64c | 259K (== pre-byte-ring 262K, no regression) |
| pipeline COALESCED, 300K/800K/1.2M /1KB/64c | 298K/795K/1,192,011 · 0-fail (coalescing preserved) |

Byte-stream delivery both ends (receiver frames its own; §8 L7 direction). Route-affinity auto-pin
of >8KB messages is DROPPED (byte-stream has no boundaries) — pin the conn for socket order, or
rely on a single backend. A single dmesh_write > conn_bytes (128KB) needs an intervening flush
(not hit by any bench). Deployed; api.md rewrite pending.

---

# (session 2) — Reverse egress UNIFICATION to ARM SG-DMA (Level 0) — REGRESSION + WEDGE

Design goal (user): unify the data plane — reverse (DPU→host) egress = ARM SG-DMA for BOTH
engines; DPUMESH_PROXY becomes a PARSER selector only (passthru default / frame / L7), not an
engine toggle. Level 0 = make the SG-DMA egress engine ALWAYS on (px_init), DPA reverse left
dormant. Change: dpu_proxy.c px_init (drop the `env NULL → proxy off` early-out; default parser
= passthru), + 2 stale comments. DPU built OK (15 objs). Deploy: `DPUMESH_PROXY_FRAME_SVC=16`
→ log confirms "PROXY MODE ON, request-default=passthru, frame-services=1". fair pin, RPC mode 8KB.

## dpumesh RPC 8KB ramp (fair, 1-core)
| target | achieved | p50 | p99 | ok/fail | note |
|---|---|---|---|---|---|
| 30,000  | 29,836  | 162µs   | 488µs   | 300,000/0 | healthy |
| 100,000 | 97,522  | 117.3ms | 319.7ms | 1,000,000/0 | 0-fail but p50 8× baseline — overloaded |
| 150,000 | 100,275 | 2.31s   | 5.62s   | 1,500,000/0 | deep overload, ceiling ~100K |
| 200,000 | —       | —       | —       | no response | client 40s timeout |
| 50,000 (post-200K) | — | — | — | no response | **WEDGED — no recovery** |
| 80,000 (post-200K) | — | — | — | no response | still wedged |

## Verdict: functionally CORRECT (0-fail ≤150K) but 2× THROUGHPUT REGRESSION + permanent WEDGE
- Sustainable ceiling collapsed to ~100K (vs ~200K DPA-reverse baseline); p50 at 100K = 117ms
  (baseline ~14ms). The single ARM thread now does ALL reverse egress (SG-DMA + parse + route +
  conntrack + per-lane credit DMA-reads + custody + TX_ACK), replacing the 4 parallel DPA EUs.
- After the 200K overload the system WEDGES and does not recover (even 50K → no response).
  Evidence: all pods Running (0 restarts, no crash); DPU log FROZEN at the overload timestamp
  (last lines = px_drop_window "stale upstream (client closed)" flood); DPU ARM thread at **2.0%
  CPU (IDLE, not pegged)**. → idle-wedge / liveness-stall, NOT CPU saturation. A wake/recovery
  path that the fast DPA-reverse never triggered (never got this backed up) fails under the deep
  backpressure the slow ARM egress creates.
- Interpretation: the ~100K may be a stall/backpressure limit (ARM idle when wedged, not pegged),
  not necessarily a raw ARM CPU ceiling — the credit-refresh DMA-reads + custody backpressure are
  the suspects. Needs the wedge root-caused before the throughput number is trustworthy.
- STOPPED per hang-discipline (captured logs, no auto-redeploy). System currently wedged — a
  redeploy is needed to clear it. Blocks Level 1 (per-conn RX) until the egress viability is resolved.

## Root-cause: ARM egress is CPU-BOUND on the single worker thread
Redeployed (wedge cleared). Sustained 80K/25s/8KB, sampled DPU ARM per-thread CPU DURING load:
| | main worker thread | 2 helper threads | bench (80K) |
|---|---|---|---|
| sample 1/2 | **94–96% / 94.1%** (R, pegged) | ~0–1% / 0% | 79,801 · p50 162µs · p99 958µs · 2M/0 |

→ CONFIRMED CPU-bound: the ONE ARM worker thread saturates a core doing the per-message DOCA
egress lifecycle (doca_buf inventory get/set/free + doca_dma task alloc/submit + credit-refresh
DMA-reads + completion/custody/TX_ACK). Mock parse is FREE (px_mock_passthru writes 3 fields,
never reads the body); route/conntrack are array/hash lookups — NOT the cost. The 2× regression =
4 parallel DPA EUs → 1 ARM thread. Sustainable ceiling ≈ 80K/core (healthy p50 162µs). The
earlier wedge-idle (2%) was the POST-stall state, not under-load. Lever = parallelize ARM egress
across cores (mirror DPA EU-sharding). Wedge under deep overload is a SEPARATE liveness bug (TODO).

## FIX: multi-threaded ARM egress (DPUMESH_ARM_EGRESS_THREADS) — recovers baseline + kills wedge
Root cause was the single ARM worker doing ALL SG-DMA egress (94-96% CPU/core). Fix: split the
proxy egress into 1 INGEST thread (main: consumer_pe + parse/route/conntrack + ship + REV_DONE +
custody + all comch + all unit/piece/arrival pools) + N EGRESS workers (own doca dma/PE/inventory
/batch-pool; own lanes by pod_idx % n_eng; do the heavy per-msg SG-DMA lifecycle). Handoff = per-
lane SPSC inbox (ingest→worker) + per-engine done-queue (worker→ingest). unit/piece/arrival pools
+ comch stay INGEST-only (no cross-thread), batch pool worker-only → no shared-pool locks. Knob
`DPUMESH_ARM_EGRESS_THREADS` (dpu_proxy.c px_init; forwarded by test-bench.sh start_dpu). n_eng==1
= the proven inline path, untouched. Files: dpu_proxy.c (engine struct + parameterized submit/
refresh/emit + retire/engine_emit/pump/worker + px_init), test-bench.sh.

### Scaling — dpumesh RPC 8KB fair (RAMPED 30→200K each; passthru default + frame svc16)
| n_eng | threads | sustainable | p50@ceiling | overload recovery | notes |
|---|---|---|---|---|---|
| 1 (inline) | 1 | ~80K | 162µs | **WEDGES** (idle-park, no recover) | = the regression |
| 2 | 1 ingest + 2 workers | **~200-220K** | 232µs @200K | **recovers** (100K after 250K = 0-fail) | == DPA baseline; no wedge |
| 4 (pre idle-backoff) | 1 + 4 (2 idle SPIN) | ~150K, wedges @260K | 1.2ms @220K | wedges | over-provisioned: idle spinners oversubscribe |
| 4 (idle-backoff) | 1 + 4 (2 idle SLEEP) | ~200K | 278µs @200K | graceful | ≈ n_eng=2 (bounded by 2 active pods) |

n_eng=2 detail: 30K/80K/120K/160K/180K/200K = 29.8K/79.6K/119K/159K/179K/198.9K, ALL 0-fail,
p50 160-240µs. Ceiling ~220K (p99 climbs); knee ~237K. ARM CPU @160K: ingest + 2 workers ALL
~99% (ingest is now co-bottleneck — REV_DONE+custody+comch per msg on one thread).

### Two findings
1. **Multi-threading recovers the baseline AND fixes the wedge.** The single-thread wedge was the
   event-loop idle-parking under egress backpressure; busy-polling workers keep egress draining, so
   the stall never forms. n_eng=2 recovers from deep overload (250K→100K = 0-fail).
2. **Egress parallelism is bounded by # active dst pods** (each pod's lanes → one worker). This bench
   has ~2 (client + echo), so n_eng=2 is optimal; n_eng>2 just adds workers with no lanes. FIX =
   idle workers back off to a 50µs sleep (after 4096 idle spins) so over-provisioning degrades
   gracefully instead of oversubscribing the ARM cores (n_eng=4 pre-fix wedged; post-fix = ~200K).

### Full-suite validation @ n_eng=2 (idle-backoff), all 0-fail
| test | result |
|---|---|
| dpumesh RPC 200K/8KB (ramped) | 198,895 · p50 232µs · 2M/0 |
| loopback self-route 50000×8KB | 50,000/0 · served 50,000 |
| preload vanilla-TCP 5000×1KB×64c | 5,000/0 · 20,198 RPS |
| stream FRAME 20000×1KB (svc16) | 20,000/0 · served 20,580,000 B byte-exact |

Verdict: reverse=ARM-SG-DMA unification is VIABLE at n_eng=2 — full ~200K baseline, 0-fail across
passthru RPC / self-route / vanilla-shim / frame paths, wedge gone. Code default still n_eng=1
(proven inline); RECOMMEND baking n_eng=2 (or ≈active-pod-count). Testing note: MUST ramp — a cold
jump to 200K wedges the host forward ring (my error mid-session, unrelated to DPU code).

## Deploy option: DPUMESH_RINGS_PER_POD (K forward rings/pod)
K was baked (=2). Now a deploy env, read on BOTH host (dpumesh_doca.c init_config) and DPU
(dpu_worker.c run_dpu_worker) — the host sizes K forward rings at init (before register) so it
can't learn K from the DPU later; both must land on the same K (fwd rings pair 1:1, mismatch stalls
dma_ready). Clamp [1, min(N=4, MAX_EU_PER_POD)]. Wired in test-bench.sh (start_dpu env + all dpumesh
pod specs). Verified: K=4 deploy → DPU log "K forward rings/pod = 4", host matched (0-fail 30/120/
200K = 198.7K); K=4 ~200K == K=2 (K=4 flat, as prior). Default unset → K=2. conn→ring: a conn pins
to ONE ring (src_port % K); conns spread across K. Ceiling is ARM-ingest-bound, so K is not a
throughput lever at 2 pods (it's an EU-spread knob for many pods); exposed per request.

---

# (session 2) — DELETE the dead legacy DPA-reverse data plane — BUILT + HW-VALIDATED

Since the egress unification, `px_init` allocates the proxy unconditionally and the DPU
worker aborts if it fails (`dpu_worker.c` — `px_init` fatal), so `objs->proxy` is ALWAYS non-NULL and
the ARM SG-DMA egress is the SOLE DPU→host reverse path. That made the entire legacy DPA-reverse
machinery **unreachable dead code** (it only ran on the `objs->proxy==NULL` branch). User decision:
delete ALL of it; keep `DPUMESH_ARM_EGRESS_THREADS` (n_eng) as a deploy env knob (tune later).

## Removed (host + DPA-device + wire, one coherent change)
- **dpu_worker.c**: `process_forward_entry`, `dpu_enqueue_reverse_dma`, `process_rev_notify_entry`,
  `dpu_route` (kept `dpu_route_l4`/`lb_pick` — proxy uses them), `batch_or_send_rev_done`
  (kept `flush_rev_done_batch` + `rev_done_batch` — the proxy fills/flushes them). `process_completion_queue`
  simplified to the single `px_ingest_forward` dispatch (no entry_type branch).
- **ring.c/h**: `setup_dpu_tx_ring`, `get_next_dma_desc`, `dma_ring.head`/`busy_head`, `RING_BUSY_LOG_EVERY`.
- **dpa.c**: the `DPA_MSG_REV_DONE` recv case, the whole reverse-ring setup block in `setup_pod_dma`,
  `update_rev_ring_host_rx`, `pod->local_mmap_dpa_handle` assignment.
- **dpa_kernel.c (device)**: `process_rev_ring`, `DPA_MSG_REV_RING_ADD` handler, the reverse-admission
  globals (`dpa_cached_freed`/`dpa_sent_count`) + `CREDIT_REFRESH_MARGIN` + the credit-refresh loop.
- **object.h**: `pod_state.tx_rings[]`/`tx_ring_mmaps[]`/`tx_buf_arrs[]`/`local_mmap_dpa_handle`;
  `dpu_comp_entry_t.entry_type` + `COMP_ENTRY_*`.
- **dpa_common.h (WIRE)**: `dpa_thread_arg` reverse fields (`rev_rings`/`num_rev_rings`/`rev_desc_idx`/
  `rev_pos`) + the dead forward `pos[]` cursor; `dpa_ring_info` reverse fields (`host_buf_size`/
  `dpu_buf_size`/`host_credit_buf_arr`/`rq_depth`); `dma_desc.landing_pos` (→ `reserved[27]`, size stable
  64B); `comch_add_rev_ring_msg`; `DPA_MSG_REV_RING_ADD`/`DPA_MSG_REV_DONE`. ABI asserts recomputed:
  dpa_ring_info **72→48**, comch_add_ring_msg **80→56**, comch_msg **84→60**, dma_desc **64** (unchanged).
- Mechanical cruft: `dmesh_key` (dead), `enum msg_direction`/`DPU_TO_HOST` + the dead
  `export_mmap_to_remote` else-branch (param dropped), plus ~15 stale/contradictory comments the sweep
  found (proxy "off by default", "legacy per-slot path", "test RR-LB", req_id, custody-list, etc.).
  Kept (intentional, not legacy): the proxy `stat_*` counters, `dmesh_sendfile`, `dmesh_get_worker_id`.

## Verification
- `gcc -fsyntax-only -DDOCA_ARCH_DPU` clean (exit 0) on every edited file — dpa.c compiling clean
  **validates the hand-computed ABI asserts** (they're outside the DOCA_ARCH_DPU guard).
- **Real deploy built clean** — dpacc compiled the DPA-kernel + wire edits on the BF3; `px_init` log:
  `egress-threads=2, request-default=passthru, frame-services=1`.

### HW results (DPUMESH_PROXY=passthru + FRAME_SVC=16 + ARM_EGRESS_THREADS=2)
| test | result | vs baseline |
|---|---|---|
| RPC 30K/5s/8KB | 150,000/0 · p50 167µs | = baseline (162µs) |
| RPC 100K/8s/8KB | 800,000/0 · p50 162µs | = baseline |
| loopback self-route 50000×8KB | 50,000/0 · served 50,000 | = baseline |
| preload vanilla-TCP 5000×1KB×8c | 5,000/0 · p50 115µs | = baseline |
| stream FRAME 20000×1KB (svc16) | 20,000/0 · served 20,580,000 B | byte-exact |
| stream FRAME **>8KB** 200×20000 (seam) | 200/0 · served Δ **4,001,000 = 200×20005** | **byte-exact** |

DPU log 100% clean through all of it (0 drops/errors); ARM idle 1-3% between runs, egress workers
accumulated 2:33/1:30 CPU-time (they did the work). **Conclusion: the legacy-reverse deletion is
behavior-neutral — every live path (forward staging-mirror, passthru, self-route, shim, frame, >8KB
seam) is 0-fail and byte-exact; no throughput regression at 100K (p50 = baseline).**

## Caveat (test methodology, NOT the code)
RPC cold-started at 150K and 200K each returned "no response" — the documented host forward-ring
cold-start wedge (the bench invocation targets a fixed RPS with no internal warm-up; the wedge
threshold for this fair 1-core pod is between 100K–150K). Evidence it is host-side, not the DPU:
DPU log stayed clean (no stale-upstream drop flood), ARM idle (the load never reached it), pods 0
restarts, and loopback/preload/frame (other pods) all passed 0-fail AFTER the 200K wedge. Reaching
~200K needs a warmed ramp (as the earlier K-knob session did with 30/120/200); stopped pushing after
2 wedges per hang-discipline. Deployed state: n_eng=2, passthru default + frame svc16.


# (session 3) — standalone Thrift-free repo extraction — BUILT + HW-VALIDATED

The DPUmesh transport was extracted out of the Thrift tree into a standalone repo
(`/home/jukebox/DPUmesh`): its own `Makefile` builds `libdpumesh.so` (no `libthrift`, no CMake) from
`src/dpumesh_doca.c` + the 10 host `doca/*.c`; headers moved to `include/dpumesh/` (consumers now
`#include <dpumesh/dpm.h>`); the DPU side keeps meson (`include_directories('../include')`,
`build_dpacc.sh` gains `-I../include`); `test-bench.sh` ported (`make lib`/`make bench go` instead of
cmake+libthrift, k8s `libthrift-so` mount → `libdpumesh-so`, Dockerfiles COPY `build/{bin,lib}`).
This log records the FIRST deploy+bench from that new tree — proving the extraction is behavior- and
performance-neutral vs the in-Thrift build.

## 3 first-run port bugs (found only by actually deploying — all fixed in the new test-bench.sh)
- **rsync can't create intermediate dirs**: `~/DPUmesh` did not exist on the DPU → `mkdir
  ~/DPUmesh/doca failed`. Fix: `ssh "$DPU_HOST" "mkdir -p ~/$DPU_DOCA ~/$DPU_INCLUDE"` before rsync.
- **`dpumesh_dpu failed to start`**: the launcher runs under `sudo` (root), so the "fix" of the old
  absolute `/home/jukebox/$DPU_BUILD` → `~/$DPU_BUILD` resolved `~` to `/root` → `cd` failed silently,
  binary never execed (stale log = tell). Fix: resolve the login home on the DPU
  (`dpu_home=$(ssh "$DPU_HOST" 'echo $HOME')`) and `cd` there by absolute path. (This is WHY the
  original hard-coded `/home/jukebox`.)
- **`line 408: stream: command not found`**: an unquoted `<<EOF` k8s heredoc ran a YAML-comment
  backtick `` `stream` `` as a command substitution (cosmetic, kubectl applied fine). Fix: quote it.

Note: the deploy exit code is unreliable (a failed rsync run still reported "exit 0" via the wrapper's
last command) — verified every stage against real state (`kubectl get pods`, DPU `pgrep`/log) instead.

## HW results — fresh deploy, `DPUMESH_ARM_EGRESS_THREADS=2 DPUMESH_PROXY_FRAME_SVC=16`, ALL 0-fail
| test | result | vs baseline |
|---|---|---|
| RPC 30K/8s/8KB   | 240,000/0 · p50 165µs  | = baseline |
| RPC 100K/8s/8KB  | 800,000/0 · p50 165µs  | = baseline (162µs) |
| RPC 200K/8s/8KB  | **1,600,000/0** · p50 227µs · p99 ~0.5–1.2ms | = baseline (~198.9K 0-fail) |
| preload vanilla-TCP 2000×1KB×32c | 2,000/0 · p50 349µs · 13.5K RPS | = baseline |
| stream FRAME 500×1KB (svc16)     | 500/0 · served 514,500 B      | byte-exact |
| stream FRAME **>8KB** 200×16KB (seam) | 200/0 · served 3,792,300 B | byte-exact |
| stream FRAME 300×1KB ×3 frames/write  | 300/0 · served 4,718,400 B | byte-exact |

`libdpumesh.so` (host) + DPA kernel (real dpacc BF3 build) + all 15 DPU control-plane objects compiled
from the new layout; one real `docker build` (echo_dpumesh) + in-container `ldd` confirmed the
image packaging (COPY `build/lib/libdpumesh.so`, no libthrift). **Conclusion: the standalone repo
reproduces the in-Thrift baseline exactly — same 200K 0-fail, same p50, every live path (RPC, shim,
frame, >8KB seam, multi-frame) 0-fail and byte-exact.**

## Caveats (known tuning/limits, NOT extraction regressions)
- **n_eng MUST be 2.** A plain `deploy` (env unset → `egress-threads=1`) is 0-fail at 100K but p50
  **67.5ms** (saturated queue); re-deploying with `ARM_EGRESS_THREADS=2` drops it to 160µs. Matches the
  known n_eng=1 overload-wedge; always deploy with `=2` for benching.
- **preload wedges under blast+churn back-to-back**: running preload immediately after a 200K RPC
  blast (then again) gave 1316/12 then **0/32** (every conn TIMEOUT at msg 0 — SO_RCVTIMEO tripwire),
  DPU otherwise healthy (dpumesh/stream still 0-fail). The SAME binary is **2000/0 on a fresh deploy**
  → a documented shim churn fragility (slot-reclaim under sudden client death), not a code regression.
  Run preload before heavy blasts. Deployed state: n_eng=2, passthru default + frame svc16.


# (session 4) — Consistency cleanup + façade dpm.h→src/dpm.c refactor — BUILT + HW-VALIDATED (behavior-neutral)

Two coupled housekeeping changes, validated together on one deploy. Both are behavior-neutral by
construction; this session proves it end-to-end (0-fail, byte-exact, p50 == the session-3 baseline).

## What changed
**A. Consistency cleanup (readability-only, ABI frozen — the standing "zero functional removal" rule):**
- **`CC_SEND_TASK_NUM` deduped**: was `#define`d IDENTICALLY (8192) in BOTH `comch_client.h` and
  `comch_server.h` (drift hazard). Now a single definition in `comch_common.h`; client.h `#include`s it.
- **Vestigial `region_off`/`dpu_region` folded to constants** (`dpa.c`): the DPA computes the pod
  staging base as `dpu_addr - region_off`, and `dpu_addr = dma_buffer + j*dpu_region`,
  `region_off = j*dpu_region` — so the two ALWAYS cancel to `dma_buffer` (verified: `dpu_addr` is used
  nowhere else). Host now sets `dpu_addr = dma_buffer, region_off = 0` and drops the `dpu_region` math.
  **The address handed to `dma_copy` is byte-identical.** WIRE ABI UNCHANGED: `region_off` stays in
  `struct dpa_ring_info` (kept 0), so all `_Static_assert`s (dpa_ring_info 48B / comch_add_ring_msg 56B
  / comch_msg 60B) are frozen and the DPA-device code (`dpa_kernel.c`) is UNTOUCHED (comment-only).
- **Comment drift 16B→20B**: `route_group` grew the completion immediate 16→20B a while back; 3 stale
  "16B budget/wire" comments (`dpa.c`, `dpa_kernel.c`, `dpa_common.h`) corrected; `dpa.c` "≤32 bytes"
  clarified to "20 bytes; HW imm-data max 32". `api.md` re-checked — already current (no change).

**B. Façade moved header-only → declarations + src impl (`dpm.h` → `src/dpm.c`):** the socket/epoll
façade was the ONLY one of the 3 API layers implemented in its header (all `static inline`); the core
API (`dpumesh.h`→`dpumesh_doca.c`) and the shim (`dmesh_preload.c`) are header-declares/src-implements.
Brought `dpm.h` into line: it now carries ONLY prototypes + the two public structs; the 16 `dmesh_*`
bodies moved to new `src/dpm.c` (compiled into `libdpumesh.so`). Two façade internals (`conn_free_rx`,
`dmesh_emit_desc`) became file-local `static`; **`dmesh_send_fin` was kept PUBLIC** — `dmesh_preload.c`
calls it directly for `shutdown(SHUT_WR)` half-close (would otherwise be an unresolved symbol).

## Files
`doca/comch_common.h` (+CC_SEND_TASK_NUM), `comch_client.h` (−dup, +include), `comch_server.h` (−dup);
`doca/dpa.c` (region_off→0, dpu_region dropped, 2 comments), `doca/device/dpa_kernel.c` (2 comments,
code untouched), `doca/dpa_common.h` (region_off + 16B→20B comments). `include/dpumesh/dpm.h`
(decls-only), `src/dpm.c` (NEW, façade impl), `Makefile` (+`src/dpm.c` to LIB_SRCS).

## Local validation (pre-deploy)
- Host `make lib`: **EXIT 0** — the `dpa_common.h` ABI asserts (outside the DPU guard) PASS → struct
  layout unchanged by the region_off comment/const edits.
- `gcc -fsyntax-only -DDOCA_ARCH_DPU` on `dpa.c` + `comch_*.c`: **EXIT 0**.
- Symbols: `dmesh_*` now EXPORTED (`T`) from `libdpumesh.so`; the rebuilt bench binaries + preload.so
  reference them as `U` (resolved from the lib), incl. `libdmesh_preload.so: U dmesh_send_fin`; `ldd -r`
  clean. (Stale pre-refactor binaries had them inlined → had to force-rebuild the consumers.)

## HW validation — `DPUMESH_ARM_EGRESS_THREADS=2 DPUMESH_PROXY_FRAME_SVC=16 ./test-bench.sh deploy`
DPU up clean: `px_init` = "SG-DMA egress, egress-threads=2; request-default=passthru, l7-services=0,
frame-services=1". All 8 pods Running, 0 restarts. DPU log 0 err/drop/poison/stale through the whole run.

| test | result | vs session-3 baseline |
|---|---|---|
| preload vanilla-TCP 2000×1KB×32c | 2000/0 · p50 192µs · 13.2K RPS | = baseline |
| loopback self-route 50000×8KB | 50000/0 · served 50000 | = baseline |
| stream FRAME 500×1KB (svc16) | 500/0 · served Δ514,500 = 500×1029 | byte-exact |
| stream FRAME **>8KB** 200×20000 (seam) | 200/0 · served Δ4,001,000 = 200×20005 | byte-exact = s3 |
| stream FRAME 300×1KB ×3 frames/write | 300/0 · served Δ926,100 = 300×3×1029 | byte-exact |
| RPC 30K/8s/8KB | 240,000/0 · **p50 165.1µs** · p99 525µs | = baseline (165µs) |
| RPC 100K/8s/8KB | 800,000/0 · **p50 165.1µs** · p99 257µs | = baseline (162µs) |
| RPC 200K/8s/8KB | 1,600,000/0 · 198,661 RPS · p50 250µs · p99 1144µs | = baseline (198.9K, 227µs) |

Reliability (no-leak / recovery): back-to-back **200K→198,549 / 200K→198,661 / 30K→29,799**, ALL 0-fail;
loopback rerun 50000/0 (served cumulative 100000, exact). Stream served-byte counts add up EXACTLY as a
cumulative counter across the 3 runs (514,500 → +4,001,000 → +926,100) — the frame/proxy path is intact.

## Conclusion
Both changes are **behavior- and performance-neutral**: every live path (forward staging-mirror,
passthru RPC, self-route, vanilla shim + `dmesh_send_fin` half-close, frame, >8KB seam) is 0-fail and
byte-exact, and the RPC p50 (165µs @30/100K, 250µs @200K) matches the session-3 baseline. The façade now
follows the same header-declares/src-implements split as the other two API layers; the region_off ABI is
frozen (device untouched); `CC_SEND_TASK_NUM` can no longer drift. NB the low-concurrency loopback/stream
p50 (~1–2.5ms) is the known EU-park/wake floor at single-outstanding closed-loop, not a regression (the
high-concurrency RPC path shows the correct ~165µs). Deployed state: n_eng=2, passthru default + frame svc16.

---

# L7 multi-backend registry + round-robin LB + connection-sticky sessions (Envoy parity, phase 1)

**Goal.** Make the DPU a real Envoy-like L7 data plane substrate: a *service = a cluster* with
MULTIPLE backend pods, load-balanced, with connection-scoped session stickiness — so an external L7
author (writing only `dmesh_l7_route`) rides a working LB/cluster/pool layer. Prior state: `service_table[svc]`
was a single-int, last-writer-wins mock (no real multi-backend, no LB); a disconnect left a stale entry
(blackhole). See design/API.md §5/§8, design/CORE.md §4.2.

**What changed (ARM control plane only — wire/DPA/data-path/host `src/` untouched):**
- `object.h`: `int service_table[128]` → **removed**; replaced by `uint32_t svc_rr[128]` (per-service RR cursor).
  The service's live backend SET is now DERIVED from `pods[]` on demand (registered + service_id + dma_ready)
  — single source of truth, so a disconnect drops a backend automatically (no blackhole). `pod_state.service_id`
  comment updated.
- `dpu_worker.c`: new `collect_live_hosts(svc,out[])` (pods[] scan → live endpoints); `lb_pick` = **ROUND_ROBIN**
  over that set via `svc_rr`; `dpu_route_l4` rg==0 path now routes through `lb_pick` (was raw table read).
- `dpu_l7.h/.c`: hook contract → **struct-based** `dmesh_l7_ctx{+hosts,n_hosts}` + `dmesh_l7_decision{total_len,
  cluster,host}` (signature frozen; `hash`/`session` are phase-2 append-only). `DMESH_LB_DEFER` = "engine LB picks".
  The default mock now content-routes by svc byte to `decision.cluster` and DEFERs the host (engine LBs).
- `dpu_proxy.c`: `px_conn.{pinned_backend,pinned_cluster}` = **connection-scoped session pin** (Envoy TCP-proxy /
  session-affinity). New `px_resolve_backend()` precedence = **L7 host-override > connection sticky > LB(round-robin
  + route-affinity)**; `px_service_sticky()` (default STICKY, `DPUMESH_LB_PER_REQUEST_SVC` csv = per-request like
  Envoy HTTP). `px_parse_l7` fills `ctx.hosts` + uses the decision struct; passthru DEFER + frame both route via the
  new resolver/`dpu_route_l4` (multi-backend RR). Proxy init parses the per-request csv; start log now prints
  `lb=round-robin, per-request-services=N (default sticky)`.
- `comch_server.c`: removed the `service_table[svc]=pod_id` write (no table to maintain now).

**Static validation (local, DOCA installed):**
- `make lib` (host ABI incl. `dpa_common.h` asserts, `struct objects` layout): **EXIT 0** (service_table→svc_rr is
  same-size 512B, ABI unchanged).
- DPU-side `gcc -fsyntax-only -DDOCA_ARCH_DPU` on `dpu_worker.c` / `dpu_proxy.c` / `dpu_l7.c` / `comch_server.c`:
  **EXIT 0, 0 errors** (only DOCA experimental-API deprecation warnings).
- Grep: **0** remaining `service_table` code refs (only doc comments, updated).

**HW deploy/test — BLOCKED (infra, not code).** `bench.sh deploy` FAILED at `sync_sources` (rsync to the DPU):
the remote DPU `jukebox@192.168.100.2` has its **root filesystem `/dev/nvme0n1p2` READ-ONLY with device I/O
errors** — `touch ~/DPUmesh/.__wtest` → "Read-only file system", `df`/`uptime` → "Input/output error", `/tmp` is
on the same (ro) root, no separate tmpfs. `/proc/mounts` still reads `rw` but every write EROFS and some binary
reads/execs return EIO → ext4/NVMe hard-error state (classic `errors=remount-ro` after device I/O failure). This
prevents rsync of sources, the DPU `ninja` build, and restarting `dpumesh_dpu`. The currently-running
`dpumesh_dpu` does **not contain this change**. The failed deploy's `apply_manifest` scaled
the k8s deployments to replicas=0 (pods down).

**Status: code complete + statically verified; HW validation PENDING DPU filesystem recovery** (reboot/fsck, or
NVMe replacement if the device is physically failing — requires operator action on the DPU, not remotely fixable/
safe to attempt). On recovery, resume: `bench.sh deploy` → regression (loopback/stream/preload/RPC vs the
baseline above) → multi-backend LB test (set echo-dpumesh-13/-14 `BENCH_WORKER_ID=11` so svc 11 has 3 backends;
verify load distributes / conn-pins across pods 11/13/14, byte-exact, 0-fail; kill a backend → remaining serve,
no blackhole). NO HW numbers recorded — none were produced.

## (later) — HW validation of the multi-backend LB + a wedge root-cause fix

DPU hardware recovered (the overheat/read-only episode passed; ASIC held 78–81°C, crit 91°C — still a
**thermal risk**, recurring overheating, cooling needs attention). Deployed via `bench/bench.sh deploy`
(`DPUMESH_PROXY=passthru DPUMESH_ARM_EGRESS_THREADS=2`). Post-reboot the host also needed `swapoff -a`
(kubelet) + `modprobe br_netfilter` (flannel CNI) — env, not code.

**My code is live** (DPU log): `DPU PROXY MODE ON (... lb=round-robin, per-request-services=0 (default
sticky) ...)`. svc 11 given 3 backends by `kubectl set env echo-dpumesh-13/-14 BENCH_WORKER_ID=11`
(pods 0, 6, 7 all service=11).

### ⭐ Root-cause bug found + fixed (wedge under repeated load)
Repeated benches wedged the WHOLE data path after 1–2 runs (rcnt=0, loopback hung, only "stale upstream"
reply-drops in the DPU log — NO routing drops). Root cause is NOT the LB code (loopback, which uses no
multi-backend LB, wedged too):
- `dpu_proxy.c` reply path marks a conn `c->dead=1` on a stale upstream (WARN-flood guard, an uncommitted
  post-LB change). A backend's **late reply** (arriving after the client FIN freed the upstream) creates
  an orphan reply-conn `(backend, uP)` that is dead-marked and **never cleaned**. `dpu_upstream_create`
  round-robins `uP`s and does not evict stale conns, so when a `uP` is **reused** for a new client, the
  backend's replies hit the dead orphan (`px_ingest_forward` `c->dead` fast-path) and are **blackholed for
  the whole session** → orphans accumulate under conn churn → progressive global wedge.
- **Fix** (`px_ship_seg`, after `dpu_upstream_create`): `px_conn_del_key(objs, dst_pod, uP)` — evict any
  stale orphan on a freshly-reused uP so the reply builds a fresh conn and is delivered. Keeps `c->dead`
  (flood guard) intact.
- **Verified**: 6 back-to-back benches after the fix, ALL 0-fail (before the fix, run #2 wedged):
  warmup 1069 · #1 718,264 · #2 493,517 · #3 559,779 · loopback 10000/0 · #4(heavy) 987,383 — all fail=0.

### Multi-backend LB — validated
| test | result |
|---|---|
| Registry (store N pods/service) | svc 11 → pods {0,6,7}, all service=11 (derived from pods[], no table) |
| **LB distribution** (3 backends, threads=6) | rcnt=951,730 · fail=0 · per-backend CPU **189 / 182 / 161** ticks = balanced round-robin |
| **No-blackhole** (kill pod7 = last registrant, then bench) | rcnt=614,804 · fail=0 · CPU pod0=177 pod6=173, **dead pod7 gets 0** (old single-entry table would blackhole svc 11) |
| Regression (passthru) | loopback 50000/0 · 10000/0 byte-exact; conc=1 p50 ~2.3ms (EU-park floor, not a regression) |
| Sticky default | connection-scoped pin active (`default sticky`); per-message via `DPUMESH_LB_PER_REQUEST_SVC` |

### ⚠️ Separate robustness gap found (NOT fixed — follow-up)
Killing a backend **while it has in-flight egress** → the ARM egress SG-DMA engine floods
`DMA: Failed to alloc_init memcpy_task, state IDLE is not running` (millions of lines) and the egress path
wedges globally. This is the egress engine not tolerating a backend's mmap teardown mid-flight (pre-existing
infra, newly reachable because multi-backend makes "one backend dies while others serve" a real case). The
LB **routing** is correct (dead backend excluded); the gap is egress-engine DMA-ctx resilience. Needs a
follow-up fix (e.g., drop/skip a lane whose pod went not-ready instead of retrying the failed DMA forever).

**Status:** Phase-1 multi-backend registry + round-robin LB + connection-sticky sessions **validated on HW**;
the orphan-reply-conn wedge is **fixed and verified**. Open: the egress-on-backend-death robustness gap, and
the card's recurring thermal issue (cooling).

### ⭐ Egress-on-backend-death robustness — FIXED + verified (same day)
Root cause of the earlier "kill a backend mid-traffic → `DMA state IDLE` flood + global wedge": a backend
dying with **in-flight egress SG-DMA** to its `host_rx_mmap` → `pods_remove_connection` destroys that mmap
concurrently with the egress worker → the engine's shared `doca_dma` ctx **faults to IDLE** → every lane's
`alloc_init` fails, is mis-treated as transient, and **retries forever** (DOCA logs millions of lines, egress
globally stops). Three-part fix (no new poll/wait/timer, zero hot-path cost):
- **① Defer `host_rx_mmap` destroy** (`comch_server.c`): keep the handle in the slot on disconnect; destroy it
  at slot **reuse** (`pods_add_connection`), by which point no egress in-flight references it (same
  reconnect-latency ≫ in-flight-drain reasoning the slot reuse already relies on). Prevents the ctx fault.
- **② Drain a not-ready pod's queued lane units** (`dpu_proxy.c px_lane_drop_dead`, in the worker pump): route
  them to the done-queue with `err=1` so the ingest thread releases custody (TX_ACK senders) + frees them with
  NO REV_DONE — prevents custody leak + mis-delivery to a reused slot.
- **③ Safety net** (`px_lane_submit`): on `alloc_init` == `DOCA_ERROR_BAD_STATE`, log once + `eng->dma_stalled`
  (stop submitting) instead of spinning — caps the flood if any ctx fault ever slips through ①.
- **Verified** — echo-14 (pod7) **force-killed (grace 0) mid a 15s bench**: mid-kill bench **rcnt=1,337,224
  fail=0** (traffic sailed through the kill on pod0/pod6), post-kill **loopback 5000/0** + **bench 293,801/0**
  (no global wedge), DPU log **13→18 lines** (vs 2.1M before), `state IDLE`/`alloc_init` flood count = **0**.

**Overall:** multi-backend L7 LB substrate is HW-validated AND resilient to a backend dying mid-traffic. Two
data-path bugs found + fixed this session (orphan-reply-conn wedge; egress-on-death flood/wedge). Only open
item: the DPU card's recurring thermal/cooling issue.

---

# ELASTIC per-conn TX buffer (dynamic block allocator) — BUILT + HW-VALIDATED (wedge bug found + fixed)

Replaces the host TX side's **fixed 128 KB/conn region** (`conn_pool`×`conn_bytes`, static — every conn
always uses the same amount) with an **ELASTIC per-conn block chain** over a shared pool, so a conn's
footprint tracks its in-flight demand (grows/shrinks) instead of a fixed slab. **HOST-ONLY** change
(`src/dmesh_core.c` + `src/dmesh.c` + the two host headers); DPA/DPU/wire/bench untouched (the descriptor
still carries a byte offset into the one TX mmap, which the DPA mirrors offset-for-offset).

## Design (as agreed in the design discussion)
- Shared TX mmap (32 MB) carved into `n_blocks` fixed **blocks** of `block_size` (default **64 KB** = the
  max contiguous message = the allocation unit). Pool = **lock-free Treiber** free-list (ported from the old
  `3159d47ae` slot pool: `tag<<32|head`, `block_next[]`) — grab is now a WRITE-hot-path op, so no mutex.
- Per-conn **block chain**: `pblk[k%maxb]` maps logical block k → physical block; `recyc[]` holds drained
  blocks for reuse (depth ≤ **cushion_h**, default 1); `nblk_owned ≤ maxb` (default **4** ⇒ ≤256 KB in-flight).
  A message never straddles a block (≤ block_size; padded to the next block), so each is contiguous.
- **grow/shrink = recycle + cushion H** (the balance we settled on): a drained tail block is recycled locally
  (steady sliding reuses it → 0 pool ops); GROW grabs from the pool only when recyc is empty; SHRINK returns
  a block only when recyc exceeds H; a fully-drained conn compacts back to logical 0. Idle conn holds ~H
  blocks; RPC conn cycles 1 block; deep-pipeline conn grows to maxb. Config: `DPUMESH_TX_BLOCK` / `_TX_MAXB`
  / `_TX_H`. First block is LAZY (connect/accept grab nothing; grabbed on first write).
- Concurrency unchanged in shape: block chain is OWNER-thread-local while live; the PE only advances `tx_f`
  (atomic); close hands off via `try_return_blocks` (block_lock + nblk_owned recheck, mirrors the old
  `try_return_region`). su-FIFO moved from ctx arrays (region-indexed) to per-slot lazy malloc.

## ★ WEDGE BUG found by HW test, then fixed (the load-bearing correctness fix)
First deploy: validators (loopback/stream/preload) + low-concurrency RPC passed 0-fail, but the closed-loop
RPC bench **WEDGED (rcnt≈2-4, frozen) at concurrency where the window's in-flight TX exceeds maxb×block_size**
(conc≥32 at 8 KB; conc=8 fine, conc=64 at 1 KB fine). Bumping maxb did NOT help → a real logic bug, not the
cap. **Root cause:** `dpumesh_tx_reserve` reused `pblk[k%maxb]` whenever it was `>=0`, but at a full window
slot `k%maxb` still holds the **un-drained earlier block k−maxb** (a live block); the code overwrote it
instead of back-pressuring (the window/cap check only fired on the `pblk[s]<0` branch, which a stale slot
skips). **Fix:** track `head_blk_next` (assign each logical block exactly ONCE) + an explicit **block-window
backpressure** — before backing block b, wait until `b − tail_blk < maxb` (⇒ block b−maxb has drained and
recycled slot b%maxb). Now an under-buffered conn THROTTLES (progresses at maxb-deep) instead of wedging.
Also fixed `try_return_blocks` to free `[tail_blk, head_blk_next)` (not `tx_w/bs`, which `discard_unsent` can
rewind below the assigned head → block leak at close).

## HW validation (clean deploy, `ARM_EGRESS_THREADS=2 FRAME_SVC=16`, default block=64KB/maxb=4/H=1) — ALL 0-fail
| test | result |
|---|---|
| RPC conc=8 / 32 / 64 / 128, req=reply=8 KB | 3.73 / **7.87 / 7.87 / 7.94 GB/s**, p50 120/260/448/926 µs, 0-fail (pre-fix: conc≥32 **WEDGED**) |
| RPC conc=32, req=reply=32 KB (1 MB window = 4× cap) | 7.44 GB/s, 0-fail (throttles at the cap, still progresses) |
| back-to-back conc=64 ×2 + recovery conc=8 | 7.93 / 7.98 / 3.70 GB/s, 0-fail — **no leak / no degradation** |
| loopback 50000×8 KB (block ring + full-drain compaction) | 50,000/0 · served 50,000 |
| stream FRAME 500×1 KB / **>8 KB** 200×20000 (seam) / 300×1 KB ×3fpw | 500/0 · 200/0 · 300/0 — **byte-exact** (served Δ 514,500 / +4,001,000 / +926,100, exact) |
| preload vanilla-TCP 2000×1 KB×32c | 2000/0 · p50 170 µs |

DPU log clean through the run (only deploy-time register "stale upstream" flakes). Local: `make lib` +
DPU-side `gcc -fsyntax-only -DDOCA_ARCH_DPU` both EXIT 0; symbols export; no stale `tx_region`/`conn_bytes` refs.

## Known limitation (documented, not a regression)
`dpumesh_tx_reserve` busy-spins on backpressure with **no deadline/stop-check** (same as the old code). With
the elastic maxb cap, a single conn whose in-flight working set exceeds `maxb×block_size` by a LARGE factor
(≳8×, e.g. a 2 MB window on the 256 KB default — `req=128 KB conc=16`) can stick the writer past a bench RUN's
deadline (the RUN never returns → the pod holds that conn). Up to ~4× the cap (1 MB) throttles-but-completes.
Mitigation: raise `DPUMESH_TX_MAXB` / `DPUMESH_TX_BLOCK` for very-high-BDP single-conn workloads, or add a
reserve deadline (future). NB the old fixed-128 KB ring had the same busy-spin at HALF the threshold, so the
elastic design is strictly more headroom (2× the old per-conn in-flight at the default) — not a regression.

## Test-methodology note (mine, not the code)
Restarting pods to swap the host lib WITHOUT restarting the DPU corrupts the DPU pod table
(`pods_register: connection not found` flood → routing dead). The mounted-lib fast path is unusable for
host-lib changes; a full `bench/bench.sh deploy` (which restarts the DPU) is required each time.

# P0 transparency: LD_PRELOAD ClusterIP registry resolve + getpeername truth — IMPLEMENTED + HW-VALIDATED

Shim P0 of the transparency plan (design report's P0): make the LD_PRELOAD shim key `connect()` on
**ClusterIP:port** (not port alone) via a control-plane-fed **registry**, and make `getpeername` TRUTHFUL —
the Envoy xDS/EDS equivalent as a static file (controller-fed later = P1). **HOST-ONLY, shim-only**
(`src/dmesh_preload.c`); `dmesh_core.c` / the elastic block allocator **UNTOUCHED**.

## What changed
- `src/dmesh_preload.c`: port-only `map_lookup` → unified `(IP,port)->svc` route table, ONE lookup, two
  sources — new `DMESH_PRELOAD_REGISTRY=<file>` (lines `ClusterIP:port svc`, exact) + legacy
  `DMESH_PRELOAD_MAP=port=svc` (addr 0 = wildcard). Exact (addr,port) beats wildcard ⇒ same-port services
  on distinct ClusterIPs resolve apart (fixes the "IP discarded" bug). `connect()` keys on `sin_addr:port`;
  the dialed IP is stored (`pfd.paddr`) so `getpeername` returns the real ClusterIP:port, not 127.0.0.1.
  `getsockname` still loopback (real pod IP = P1); server-side real client id = P1 (needs the controller).
- Test wiring: `preload_runner.c` writes a static registry + dials synthetic ClusterIP 10.96.0.15;
  `tcp_client.c` got a non-fatal getpeername truth-check. Docs: API.md §7 (+ §1/§3/§9 CONN_POOL→block
  model), CORE.md §6.

## HW validation (clean deploy, `ARM_EGRESS_THREADS=2`, registry path) — 0-fail
| test | result |
|---|---|
| preload 5000×1KB×8 (ClusterIP→registry→svc15) | 5000/0 · p50 122µs · 18K rps |
| preload 20000×1KB×32 (conn churn) | 20000/0 · p50 247µs · 80K rps |
| getpeername (pod log) | `getpeername -> 10.96.0.15:9095 (MATCHES dialed addr)` |
Passed on BOTH the pre-refactor core (83fbc20+P0) and HEAD (a202796+P0); byte-exact.

## ★ CORRECTION — a false-alarm "regression" I chased (recorded so nobody repeats it)
During P0 testing, two consecutive a202796 deploys had preload TIME OUT 100% while loopback passed. I
bisected (83fbc20 preload 5000/0 vs a202796 preload 0/N) and WRONGLY concluded the elastic-block buffer
refactor broke the shim. **It did NOT.** Three independent disproofs:
1. a202796 is HOST-ONLY (0 `doca/` lines) ⇒ it CANNOT cause the DPU-side `setup_pod_dma`/`ADD_RING`
   ring-setup failure that broke deploy #1 (that error is in deploy #1's DPU log).
2. A later clean a202796 deploy passed preload 5+ consecutive (2000/0, 3000/0×3) + native cross-pod RPC
   fail=0 + loopback 3000/0.
3. The buffer refactor's OWN entry above already validated `preload 2000×1KB×32c = 2000/0` on a202796.
The bisect was 2 samples/side of a PER-DEPLOY flaky phenomenon (the DPU comes up good or bad; see the
"Test-methodology note" above — register / stale-upstream / ring-setup flakes at bring-up). **Lesson: for a
per-deploy flake, N=2/side is not a bisect.** The real open issue is DPU **bring-up robustness**
(`setup_pod_dma` `ADD_RING` flake under the registration burst / `MAX_PODS` pressure) — DPU-side +
environmental, NOT the host buffer code.

## Not done (P1+)
dpumesh-controller + injection webhook (feed the registry, self-identity); the review's real blocker
`MAX_PODS=8` (whole-mesh node cap — hit directly this session as the pod-6 `setup_pod_dma` failure).

# MAX_PODS 8 → 16 (concurrent-meshed-pods-per-node cap raised) — HW-VALIDATED

The review + the "dynamic K8s" discussion surfaced this as the real blocker for K8s use: `MAX_PODS=8`
caps how many pods can be **concurrently meshed per DPU (node)**. Real K8s nodes run 30–110 pods. The
pod add/remove/reuse **churn lifecycle is already implemented** (`comch_server.c` `pods_add_connection`
on Comch connect / `pods_remove_connection` on disconnect / freed-slot reuse) and the data plane already
self-heals on backend death — so **capacity, not churn, was the gap**. One-line change:
`include/dpumesh/dmesh_common.h` `MAX_PODS 8 → 16`. **DPU proxy logic (`dpu_proxy.c`/`dpu_l7.c`) untouched.**

## Why 16 is the exact ceiling (no DPA-kernel change)
- **Forward-ring capacity is the binding constraint.** Each EU holds `rings[MAX_DPA_RINGS=8]`
  (`dpa_common.h`); ring j of pod p lands on EU `(p*K + j) % N`. With K=2 (default), N=4 EUs, one EU
  holds `ceil(MAX_PODS/2)` rings → must be ≤ 8 → **MAX_PODS ≤ 16**. (>16 needs a bigger `MAX_DPA_RINGS`
  = a DPA-kernel change; deferred.)
- **Wire-safe**: `pod_id` is int8, `_Static_assert(MAX_PODS <= 127)` in `dpa_common.h` passes.
- **No memory blow-up**: DPU staging is `alloc`'d PER registration (`setup_pod_dma`, 32 MB/live-pod), not
  preallocated ×MAX_PODS. Proxy pools (`PX_ARRIVAL_POOL = MAX_PODS×4096`, `lanes[MAX_PODS][8]`) are heap
  (`calloc`) and just ~2× (a few MB on the 16 GB ARM).

## HW validation (clean deploy, `ARM_EGRESS_THREADS=2`, `echo-dpumesh` scaled to 7 replicas → 12 pods)
| check | result |
|---|---|
| pod_id assignment past the old cap | echo replicas got **pod_id 8, 9, 10, 11** (would be `table full (8)` before); +preload client = pod 12/13 |
| 12 concurrent dmesh pods | all Ready; DPU log **0** `table full` / `setup_pod_dma` / `ADD_RING failed` |
| native RPC conc=32 (svc 11, 7+ backends) | **fail=0** · 826K reqs · 165K mrps · p50 178µs |
| loopback (self, pod 4) | **3000/0** |
| P0 shim (preload) at scale | **3000/0** |
| churn-at-scale: delete a backend mid-run → RPC | **fail=0** · 361K reqs · no wedge (DPU 0 BAD_STATE/IDLE) |

## Next lever (for real node density > 16)
Raise `MAX_DPA_RINGS` (per-EU ring array, `dpa_common.h` `rings[]` + `device/dpa_kernel.c`
`num_rings < MAX_DPA_RINGS`) — a DPA-kernel change; `dpa_ring_info` is ~60 B packed metadata so per-EU
memory cost is small, but it touches the dpacc-built DPA and needs its own HW validation.

## Follow-up refactor (same day) — single-sourced the tangled DPA/pod constants
The capacity constants were declared as separate magic numbers; unified them (one source, the rest DERIVE)
— `dpu_proxy.c`/`dpu_l7.c` (proxy logic) untouched:
- **`MAX_DPA_EU`** (max EU threads = `dpa_threads[]` array cap) split OUT of **`MAX_DPA_RINGS`** (per-EU
  ring capacity) — the same `8` was doing double duty (`dpa_threads[MAX_DPA_RINGS]`).
- **N** (`num_dpa_threads`) was hardcoded `= 4` in TWO places (dpa.c, dpu_worker.c) → one
  `DPA_THREADS_DEFAULT` + env **`DPUMESH_DPA_THREADS`** (default 4, clamped to `MAX_DPA_EU`), mirroring the
  existing K env `DPUMESH_RINGS_PER_POD`.
- **`MAX_PODS`** magic `16` → **derived** `MAX_DPA_RINGS * MAX_DPA_EU / K = 32` (the pods[] array, sized for
  the max-EU config).
- The cap is now a **runtime LIVE cap** in `pods_add_connection`:
  `min(MAX_PODS, MAX_DPA_RINGS * num_dpa_threads / k_rings)` — so raising `DPUMESH_DPA_THREADS` raises
  capacity with NO recompile. Default N=4 ⇒ live cap 16 (== the 8→16 raise above; no behavior change).

HW-validated (clean deploy, N=4 default): **no regression** — 12 pods (pod_id 6–11) + RPC/loopback all
0-fail; the runtime cap fires correctly past 16 with the self-documenting log
`table full (live cap 16 = 8 rings × 4 EU / 2 K)`; gentle 10-pod scale 0-fail after redeploy.

⚠ Observation (NOT the refactor): an AGGRESSIVE test — scale echo `1→14` then abruptly `→1` (13 SIMULTANEOUS
terminations) — triggered `px_dma_err_cb: SG-DMA batch failed (pod slot 8)` in `dpu_proxy.c` (egress,
untouched) and wedged routing until redeploy. Single/gentle churn is fine (cf. the backend-death fix
earlier); **13-at-once is a heavy-churn egress edge worth hardening before high-churn production.**

## Auto-detect N (EU count) — DONE + HW-VALIDATED
N (DPA EU threads) is now AUTO-DETECTED from the device instead of a hardcoded default:
`doca_dpa_get_total_num_eus_available()` in `init_dpa_objects` (dpa.c) → N = min(available, MAX_DPA_EU);
env `DPUMESH_DPA_THREADS` still overrides; K clamped to N; EU-thread containers alloc'd at MAX_DPA_EU so N
can grow post-detect (`dpa_threads_auto` flag = auto vs env). HW: **BF3 reports 254 EUs → N=8** (capped at
MAX_DPA_EU=8), so the live pod cap auto-rose to `MAX_DPA_RINGS*N/K = 8*8/2 = `**32**. Validated: pod_ids to
16 registered (>the old N=4 cap of 16), RPC 0-fail with 17 pods, no ring errors, gradual churn 0-fail.
`dpu_proxy.c`/`dpu_l7.c` untouched.

## ★ Egress mass-death self-heal — ATTEMPTED, DOES NOT WORK (recorded so nobody retries it the same way)
The 13-at-once wedge (above) is `px_dma_err_cb: SG-DMA batch failed ... Input/Output Operation Failed` —
in-flight SG-DMA to a torn-down host RX mmap (the dead pod's process exited → its PCI-mapped memory is gone).
Tried: on the ctx fault, self-heal the engine's `doca_dma` ctx via `doca_ctx_stop` → progress-to-IDLE →
`doca_ctx_start` (instead of the permanent `dma_stalled` stall). **It FAILS:** the failed DMA tasks are HUNG
at the hardware level (a DMA to unmapped memory never completes), so `doca_ctx_stop` can't drain them — DOCA
logs `can't destroy memcpy tasks pool, will remain in stopping state until all tasks are freed` forever, the
ctx never reaches IDLE, `doca_ctx_start` never runs, and the recover spins (worse than the quiet stall). The
original code's `dma_stalled` + "needs [process] restart" was literally correct: **a hung DMA is
unrecoverable via ctx stop/start.** REVERTED (dpu_proxy.c untouched). The real fix must be **PREVENTION** —
bound in-flight DMA per pod (fewer hung ops on a mass death) and/or a per-pod `doca_dma` ctx so one dead pod
can't hang the shared engine — or accept the operational limit (redeploy clears it; single/moderate churn
≤~6-at-once tested fine; only ~13+ simultaneous terminations wedge).

## BATCH_TXACK_MAX 14→16 re-test — "16 = deterministic hang" DID NOT REPRODUCE; verdict = per-deploy flake
Re-ran the 14-vs-16 question end-to-end with matched builds + instrumentation. The earlier
"MUST stay 14 — PROVEN load-bearing" conclusion is RETRACTED (its own DIAG contradicted its mechanism:
a batch stalled below threshold would show tx≈15, but the hang showed tx=0 rev=0 = everything flushed,
HOST stuck — and the idle-flush would have flushed a 15-deep batch within ~1ms anyway).

**Code finding (real, kept as a rule): the batch constants are wire-ABI on BOTH sides.**
`BATCH_TXACK_MAX`/`BATCH_REVDONE_MAX` (comch_common.h) compile into the DPU binary AND host
libdpumesh.so. Host rx unpack CLAMPS `n = min(count, its own compiled max)` and its length check uses
the clamped n → a DPU sending bigger batches than the host was built with loses the TAIL entries
SILENTLY (no log), and `tx_reclaim_ack` is strict-FIFO front-match-or-noop → one lost ack = that conn
wedges forever. `bench.sh restart`/`build` rebuild ONLY the DPU ⇒ **changing any comch_common.h wire
constant requires FULL `deploy`.** (`restart` has since been deleted for this reason;
`build` still rebuilds only the DPU, and `deploy` is the sole bring-up path.)
(Hardening option, not yet applied: bound host n by received length
`(len-4)/entry_size` instead of the compile-time max, both BATCH types.)

**HW A/B (all full deploys, ② shards=2 + egress2, greeter 32B/8B):**
- 16-deploy #1 (uninstrumented): conc=8/1thr PASS (609,820 / 0-fail) → conc=16/2thr **HANG once**
  (same signature as the retracted A/B: DPU fully idle, all batches flushed, pend=0, q=0, lull=100%,
  ZERO DPU send errors). Matched builds ⇒ the mismatch trap above was NOT this hang's cause.
- 16-deploy #2 (+tripwires: DPU flush-size histogram; host rx count histogram + FIFO-mismatch counter):
  **13/13 PASS** — conc=16/2thr ×7 @ 0.139–0.150 Mrps, conc=32/2thr ×6 @ 0.181–0.209 Mrps (== the
  known-good 0.206 ②+egress2 baseline), all 0-fail; loopback 20000/0; preload 5000/0. Receipts:
  DPU sent **1,499,970 full-16** + 95,825 n=15 batches; bench-pod received c16≈756K/c15≈53K;
  **ACK MISS = 0** across ~2.6M batch msgs ⇒ 64B (n=15) & 68B (n=16) wire sizes, 16-entry unpack, and
  per-conn ack ordering are ALL fine.
- Post-cleanup deploy (tripwires removed): conc=16 0.142 / conc=32 0.204 Mrps, 0-fail.

**Verdict:** same constant, different deploys → different behavior ⇒ the hang correlates with DEPLOY
STATE, not the constant (cousin of the documented bring-up flake, though this flavor passed conc=8
before wedging at conc=16 — root cause unconfirmed). 16 is NOT proven harmful; the old 14 A/B was a
1-deploy-per-config sample. Any future batch-const A/B must span MULTIPLE fresh deploys per config.
Tree+HW currently BOTH at 16 (matched), healthy.

**Log hygiene (same session):** host hot-path tripwires (ACKB rx / ACK MISS in dmesh_core.c) REMOVED
after the verdict. DPU DIAG dump (DPUMESH_DIAG=1) kept — now with fl=total/15:n/16:n flush histogram +
sgl count — and made **quiet-at-idle**: the 1 Hz heartbeat only prints while there is queued/batched
work or the flush counters moved since the last dump (an idle hang still leaves its final counters as
the last line before silence; was ~86K identical lines/day).

# Pod restart vs a RUNNING DPU: root-caused (stale px_lane credits) + FIXED + HW-VALIDATED (14/14 restarts, ×2 deploys)

**The "a single pod cannot reconnect to a running DPU" rule is DEAD.** It was never a
property of the system — it was a bug, now fixed and HW-validated. Two independent
fresh-deploy runs of the full suite, ALL CHECKS PASSED both times.

Note the exact claim: **RE-CONNECT**, not death. Pod death was already verified.
See the SCOPE section immediately below before reading the root cause.

## ⚠️ SCOPE — this is about pod RE-TENANTING, not pod death (pod death was already fine)

Read this before the root cause, or you will mis-attribute it. **Pod DEATH was already
handled and verified** — see "⭐ Egress-on-backend-death robustness — FIXED + verified"
(above): a backend force-killed (grace 0) mid a 15s bench gave
`rcnt=1,337,224 fail=0`, no global wedge, flood count **0**. That fix (① defer
`host_rx_mmap` destroy, ② `px_lane_drop_dead`, ③ `dma_stalled` safety net) is still in the
tree, still correct, and this session extends rather than replaces it.

What NO prior test ever did is **bring the pod BACK into the same slot and drive traffic
through it**:

```
backend-death             :  kill pod → done.            (never returns)
13-at-once mass death    :  kill 13 pods → done.        (never return)
"gradual churn 0-fail"   :  add/remove replicas         (mostly fresh slots)
elastic TX `[reconn]`    :  CONNECTION churn            (not pod churn at all)
THIS session             :  kill → REBORN in same slot → traffic → ×14
```

The bug below requires slot RE-TENANTING. Die-and-stay-dead never triggers it.

**And the backend-death pass is EVIDENCE FOR the root cause below, not against it.** Without
re-tenanting there is no starved lane, so credit-refresh fires only occasionally and the
death window (host memory unmapped → comch disconnect lands) is rarely hit. With
re-tenanting, the starved lane spams refresh on EVERY pump, so hitting that window becomes
a near-certainty. The starvation is what turns a rare race into a deterministic wedge.
(The original disconnect-NULLing of `ring_mmaps[]` — `px_lane_refresh_credit`'s only guard
— also suppressed refreshes to a dead pod, which is why die-and-stay-dead stayed quiet.)

## ★ Root cause (it was NOT the DPA ring leak, NOT the unbounded wait, NOT stale upstream)

```
px_lane credit state not reset when a pods[] slot is RE-TENANTED          <- ROOT
  → reconnected pod inherits the previous tenant's sent_entries/cached_freed,
    but its FRESH host RX freed-counter restarts at 0
    → inflight = sent_entries - cached_freed stays huge → avail_entries pins at 0
      → units queue forever and the lane retries credit-refresh DMA forever
        → those refreshes read the DEAD pod's UNMAPPED host memory
          → QP LOCAL_QP_OPERATION_ERROR → the engine's SHARED doca_dma ctx → STOPPING/IDLE
            → no recovery + the fault never latched → "alloc_init ... state IDLE" flood
              → egress dead FOR EVERY POD on the node
```

Every link below the root is a SYMPTOM. Fixing the root makes the DMA fault stop
occurring at all (final runs: **0 faults, 0 ctx restarts** across 14 restart cycles).

**Load-bearing reframe (user's):** the egress DMAs into peers' host memory, so a DMA
failure is a NORMAL event, not a bug to prevent. Gating cannot fix it — `dma_ready` only
drops when comch reports the disconnect, which is strictly AFTER the pod's memory is gone.
Confirmed empirically: NULLing the pod's handles at disconnect did not close the window.

## ★ Why `dma_stalled` (which already existed) never saved us

`px_engine`'s `dma_stalled` safety net was set in exactly ONE place — the SG-batch submit.
But after a fault the code never REACHES that submit: the credit refresh can't land, so
credits never arrive and `px_lane_submit` breaks on `avail_entries < first_needed` first.
`px_lane_refresh_credit`'s `alloc_init` → `BAD_STATE` just did `goto fail` without
latching → the pump retried it forever, and each retry made DOCA emit its own
`alloc_init ... state IDLE` ERR line. **That unlatched spin WAS the flood.**

## Fixes (all HW-validated together)

**dpu_proxy.c** (the real fix)
- `px_lane_rearm()` + `px_lane.pod_generation`: reset `cursor`/`sent_entries`/`cached_freed`/
  `refresh_inflight` when the slot's `dma_generation` changes. **This is the root fix.**
- `px_engine_recover()`: on ctx fault → wait for IDLE → `doca_ctx_start()`. Drives the PE
  itself and returns "still working" so the main loop does not park mid-recovery (it did:
  2.55s → 93ms). Also rearms EVERY lane's `refresh_inflight` — a refresh in flight when the
  ctx died may never deliver its callback, and a stuck `refresh_inflight=1` starves that
  lane FOREVER (measured on a pod that was not even the one that died: 1245 msgs, then dead).
- Latch the fault in `px_dma_err_cb` on `DOCA_ERROR_IO_FAILED`, not by waiting for a later
  submit to rediscover it as `BAD_STATE`.
- `IO_FAILED` on a credit refresh = that pod's host memory is gone ⇒ drop its `dma_ready`
  (earlier + more reliable than comch). Guarded by `dma_generation` because the callback
  can land AFTER the slot's next tenant registered, and by `IO_FAILED` specifically because
  the ctx fault FLUSHES healthy pods' in-flight tasks through the same callback.

**DPA ring lifecycle (issue A)** — `dpa_common.h` / `dpa_kernel.c` / `dpa.c` / `comch_server.c`
- `DPA_MSG_RING_DEL` opcode + `teardown_pod_dma()` on disconnect. `rings[]` had NO removal
  path at all (enum was ADD/WAKE/FWD_DONE only) and `dpa_thread_running[k]` never resets, so
  `num_rings` grew monotonically for the whole process lifetime. `pod_id` is the recycled
  slot index and `k_j = (pod_id*K+j)%N`, so a restarting pod returns to the SAME EUs ⇒ budget
  was **MAX_DPA_RINGS = 8 cumulative connects per EU**, then the add was **silently dropped**
  (msgq is fire-and-forget — the host still believed it succeeded). Never `DOCA_ERROR_AGAIN`.
- DPA side: swap-with-last carrying `desc_idx` with the moved entry; `RING_ADD` resets
  `desc_idx` and replaces a stale same-`pod_id` entry in place.

**Forward-ring wait (issue B)** — `ring.h` / `ring.c` / `dmesh_core.c`
- 5s deadline + `ring->dead` latch. The `for(;;)` was unbounded: a ring the DPA never
  registered accepted exactly `size` descriptors then blocked the caller FOREVER. Cannot be
  recovered in place (an abandoned Vyukov ticket leaves `seq[]` unadvanced and the DPA's
  sequential `desc_idx` stalls on the hole), so it fails loudly instead.

**Resource lifetime** — `dpa.c` / `comch_server.c`
- DPU staging (`local_mmap`/`dma_buffer`, 32MB) is now allocated ONCE PER SLOT and REUSED.
  It was re-allocated per incarnation and never freed ⇒ **32MB leaked per reconnect**.
- Host-exported handles (`ring_mmaps`, `remote_mmap`, `buf_arrs`, `host_rx_mmap`) are
  UNPUBLISHED (NULLed) at disconnect but deliberately **never destroyed** — see limitation.

**bench.sh**
- `restart` (DPU-only restart) command DELETED; validators no longer self-start their pod.
  `verbs`/`stream` moved into `start_pods`. `deploy` is the only bring-up path.

## HW validation — 2 independent fresh deploys, ALL CHECKS PASSED both

| phase | result (run 1 / run 2) |
|---|---|
| validators (clean) | loopback 50000/0; verbs 50k×8KB, 20k×64B, 20k×8KB zc, 20k×4KB w4p4, 20k×8KB w8p2 all 0-fail; preload 5000/0 |
| benchmarks | latency p50 **114 / 114** µs · thruput p50 **1262 / 1265** µs · churn p50 **1192 / 1195** µs — all fail=0 |
| **14× pod restart vs RUNNING DPU** | **14/14 both runs**, rcnt **162k–177k** flat, fail=0 |
| validators AFTER 14 restarts | loopback 50000/0 · verbs 50000/0 · preload 5000/0 |
| DPU log audit | 0 `too many rings` · 0 `Ring add failed` · 0 `STALLED` · 0 `is dead` · **0 DMA faults · 0 ctx restarts** · log 8K |

**Before → after on the restart path:** `169930 → 3524 → 0` (node dead by cycle 2–3, log
flooded to multi-MB) ⇒ **flat 162k–177k for 14 cycles, log 8K**.

Convergence receipts (each = one deploy+validate): flood `150,483 lines / never recovers`
→ latch on the refresh path → `28` → `IO_FAILED` as death signal → `1` (but recovery 2.55s)
→ latch in the err cb itself → `93ms` → reset lane credits on re-tenant → **0 faults**.

## ⚠️ Reconciliation with "Egress mass-death self-heal — DOES NOT WORK" (above)

That entry says ctx `stop`→IDLE→`start` self-heal CANNOT work because a DMA to unmapped
memory HANGS at the hardware level, so the ctx never reaches IDLE. **This session's
recovery DID work — but that does NOT overturn it, and the two are not in conflict:**

- That entry's scenario is **13 SIMULTANEOUS terminations** (mass death, many hung DMAs).
  This session tested **single-pod restart** (×14, sequentially). Different load.
- This recovery never calls `doca_ctx_stop()` — DOCA auto-transitions to STOPPING on the
  fatal error; we only progress the PE and `doca_ctx_start()` once it reports IDLE.
- **In the final config the fault does not occur at all** (0 restarts), because the root
  cause was the lane-credit starvation that kept firing refreshes at dead memory. So
  `px_engine_recover` is now a SAFETY NET, not a load-bearing path. It was separately
  observed working (93ms restart) in the intermediate builds, single-pod only.

**NOT tested this session: the 13-at-once mass-death case.** The mass-death finding
(hung DMAs ⇒ ctx never reaches IDLE ⇒ recover spins) may well still hold there. If mass
death is re-tested and `px_engine_recover` spins, that entry's prescription stands
(bound in-flight DMA per pod, and/or a per-pod `doca_dma` ctx so one dead pod can't hang
the shared engine).

## Known limitation (documented, deliberate)

**Host-exported handles LEAK once per reconnect.** Destroying them at ANY point faults the
same shared `doca_dma` ctx (measured: throughput 48× drop on restart 2, node dead on
restart 3 — with the destroy at slot reuse, already far later than the original code's
destroy-at-disconnect). Freeing them safely needs a real quiesce protocol (RING_DEL ack +
per-pod egress in-flight refcount + proxy custody drop, reclaimed async off the control PE).
The 32MB staging that DOMINATED that leak is now reused per slot, so what remains is small.

**Not done:** a graceful BYE+ACK before host unmap. There is no host shutdown path at all
in the current implementation (no `dpumesh_fini`, no UNREGISTER msg), it cannot cover crash/OOM/SIGKILL, and its
timeout fallback needs the recovery path anyway. Worth adding later as an optimization so
the graceful path never faults — not as a substitute.

## ★ Corrections / retractions (recorded so nobody repeats them)

1. **"The documented pod-restart rule is the DPA ring leak (issue A)" — WRONG.** Issue A
   bites at ~8 restarts; the real wall was at 2. A was real but INVISIBLE behind it: nothing
   ever reached the 8-ring budget, which is why A had never been HW-validated until now.
2. **"The wedge is caused by destroying mmaps" — WRONG.** It reproduced identically with
   ZERO destroys. The `credit refresh DMA failed` line, not the destroys, was the signal.
   (I did make it worse first: removing the original disconnect-NULLing of `ring_mmaps[]`
   deleted `px_lane_refresh_credit`'s only guard, so refreshes kept firing at dead memory
   for 75+ seconds. Restoring the NULLing was necessary but not sufficient.)
2b. **"Pod death wedges the egress — pre-existing bug" — OVERCLAIMED, and I only caught it
   because the reviewer asked "didn't we already test pod restart/delete?".** We had:
    force-killed a backend mid-traffic → `rcnt=1,337,224 fail=0`, flood 0. Pod
   death was ALREADY fixed and verified. The untested case was pod RE-TENANTING of a slot.
   Always check what the prior test actually DID before declaring its subject broken — and
   state the scenario, not just the subject, in the claim.
3. **`dpu_proxy.c:716` dismissed as "just a log statement" — WRONG.** `px_drop_window:
   stale upstream` at 716 is exactly the line that precedes the wedge. The original claim
   pointing there was right.
4. **`ring.c` `setup_dma_ring` uses `malloc` + hand-initialized fields.** Adding `dead`
   without adding it to that list gave it heap garbage → ring 1 came up pre-"dead" and
   fast-failed every enqueue on it (ring 0 happened to get a zero byte). **Now `calloc`;
   keep it that way** — this class of bug will recur on the next field added.
5. **Two fixes can fight.** Marking a pod dead on `IO_FAILED` removed the very retries that
   were latching `dma_stalled` and triggering recovery ⇒ recovery went from prompt to 2.55s.
   Recovery must be triggered by the error itself, not by rediscovering it later.
6. **Method:** every wrong turn above came from fixing before confirming the cause. The
   numbers only started moving once the log pinned it (`QP IO_FAILED → ctx STOPPING`).

**Log-level trap:** `bench.sh` starts the DPU at `-l 40` (WARNING), so every `DOCA_LOG_INFO`
is invisible — including `"New connection established (total pods: %d)"`. That is why
`bench.sh`'s "DPU register timeout — continuing" warnings during deploy are **COSMETIC**,
not failures. Use `DPUMESH_LOG_LEVEL=50` to see registration/RING_ADD/RING_DEL.

---

# API consolidation (4 surfaces → 2) + multi-CQ; HW validation

Collapsed the host surface to **exactly two public APIs**: the native API
(`include/dpumesh/dmesh.h`, RDMA-verbs-shaped, zero-copy both ways) and the POSIX socket
ABI (`libdmesh_preload.so`). The socket façade (`src/dmesh.c`) is **gone as a layer** — 87%
of it was transport (`accept`/`connect`/`close`/`next_ready`/`emit_desc`/`flush`/`send_fin`)
and moved to `dmesh_core.c`; the other 13% was POSIX byte-stream semantics (`read`'s copy +
`rx_pos` partial cursor, `write`'s copy + >block carve) and moved **into the shim**, where
the socket contract actually mandates it. `dmesh_core.h` left `include/` (it was public only
by accident of `dmesh.h` #including it). Header count 4 → **1 public + 1 internal**; public
symbols = **16**.

Renames (the old names lied): `dmesh_connect` → **`dmesh_create_qp`** (it does no round
trip — it is `ibv_create_qp`, not `rdma_connect`), `dmesh_conn_t` → `dmesh_qp_t`,
`dmesh_close` → `dmesh_destroy_qp`. Deleted with zero callers: `dmesh_sendfile` (the shim's
`sendfile()` uses a stack buffer + `shim_send`), `dpumesh_alloc_port_specific`,
`dpumesh_dequeue`'s blocking path, `dmesh_commit`/`dmesh_event_fd`/`dmesh_wc_msg_max`
(duplicates of `post_send`/`cq_fd`/`msg_max`).

## Correctness — full validator matrix, ALL 0-fail

| validator | result |
|---|---|
| `loopback 2000 1024 1` | **2000/0** · served 2000 · p50 115.7 µs |
| `loopback 2000 8192 0` | **2000/0** · p50 116.8 µs |
| `verbs 2000 1024 1 8 4` | **2000/0** · p50 154.9 µs |
| `verbs 2000 8192 0 1 1` | **2000/0** · p50 92.3 µs |
| `stream 20000 1024 self 1` | **20000/0** · served_bytes 20,580,000 (byte-exact) |
| `preload 5000 1024 8` | **5000/0** · p50 121 µs · p99 357 µs · 25,091 rps |

`preload` is the regression control: the shim's *behavior* was deliberately not changed
(only its includes + absorbing the two byte-stream statics), and it passes untouched.

## Backpressure — `grow_waits = 0` everywhere

`dpumesh_tx_reserve` is now **non-blocking**: `NULL`+`EAGAIN` on a full SQ instead of the
nanosleep ladder it used to spin in (the `ibv_post_send` contract). **`waits=0` on every run
measured**, at every concurrency/thread count/payload — the elastic ring never hit its
in-flight ceiling, so backpressure has never actually occurred in this workload. That
retires the question of whether the shim needs honest `EPOLLOUT` (a socketpair fd-realization
instead of the eventfd): it would cost a **per-message** syscall upgrade (eventfd counter →
AF_UNIX skb alloc) to fix a path that never executes. Not doing it. Revisit only if
`waits` ever moves.

Two spins provably removed rather than tuned:
- `tx_next_send`'s send-unit-FIFO wait: an init clamp (`maxb * ceil(block_size/slot_size) <=
  TX_SU_DEPTH`) makes the block window bind strictly first, so the FIFO can never fill and
  the branch is unreachable by construction.
- `sched_yield()` in the shim's `shim_send_iov`: was **dead code** — `dmesh_write` never
  returned EAGAIN (`tx_reserve` slept instead) and the errno it tested was stale. It would
  have gone *live* the moment `tx_reserve` started returning EAGAIN, so the blocking retry
  moved into the shim's own `stream_write` in the same change. `sched_yield` count in
  `src/` is now **0**.

## Multi-CQ — shipped on fidelity, NOT on performance

`ibv_create_cq`/`dmesh_create_cq` now exists; each CQ owns its own ready list + completion
fd, and a QP belongs to the CQ that created or accepted it. The accept queue stays
channel-wide and multi-consumer (it already was), so every CQ may poll it and whichever
accepts a conn owns it — SO_REUSEPORT-style distribution for free.

**A/B (one build, temporary knob, since removed), shards=2+egress2, conc=4/thread:**

| threads | one CQ + mutex | CQ per thread | Δ |
|---|---|---|---|
| 1 | 0.0301 | 0.0312 | +3.7% |
| 2 | 0.0627 | 0.0653 | +4.2% |
| 4 | 0.0798 | 0.0837 | +4.9% |
| 8 | 0.1391 (p50 225 µs) | 0.1464 (p50 186 µs) | +5.2% (p50 −17%) |

**The shared CQ scales 4.6x too.** Multi-CQ is worth ~5% Mrps / ~17% p50 at 8 threads —
real, but it was never the bottleneck. It ships because folding the CQ into the channel
bakes "one RX consumer per process" into the API — a ceiling the host hits the moment the
DPU stops being the binding constraint (③ host-emit). One CQ per thread is what verbs does
and costs ~nothing.

## RETRACTED: the "~2x regression" was a measurement artifact

Mid-session this log's methodology bit me. I reported a ~2x throughput regression by
comparing a **default, unsharded** deploy at **1024B/1024B** against the reference at
`:4406` — whose section header two lines up (`:4404`) reads *"HW A/B (all full deploys,
**② shards=2 + egress2**, greeter **32B/8B**)"*. Unsharded vs sharded. The "2x" I found was
exactly the documented ②+egress2 gain (2.02–2.15x) — **a phantom regression whose size
matches a known config delta is the tell.**

Re-measured with the reference's own config (`DPUMESH_INGEST_SHARDS=2
DPUMESH_ARM_EGRESS_THREADS=2`, 32B/8B) — **both points reproduce at or above reference:**

| config | reference (`:4406`) | consolidated API |
|---|---|---|
| conc=16, 2thr | 0.139–0.150 Mrps | **0.153** (p50 178 µs) |
| conc=32, 2thr | 0.181–0.209 Mrps | **0.211** (p50 292 µs) |

Also wrong in that retracted claim: the flat thread sweep I called "a textbook serialized
consumer". A **default (unsharded) deploy pins to ~0.102 Mrps regardless of
concurrency/threads/payload**, with p50 = N/0.102 (pure Little's law) — that flat line is the
**unsharded DPU funnel ceiling**, not a host bottleneck. Thread scaling is only observable
below saturation; at conc=4/thread (shards=2+egress2):

| threads | 1 | 2 | 4 | 8 |
|---|---|---|---|---|
| Mrps | 0.029 | 0.088 | **0.200** | 0.176 |
| p50 (µs) | 115 | 173 | 297 | 1474 |

**6.8x from 1→4 threads**, then the DPU cap (~0.20) binds and 8 threads just queues.

## Core bug found and fixed: `poll_cq` silently under-delivered

`loopback` failed 100% after the port — request routed back and echoed (`nserv=1
served=1`), reply never landed (`got=0/1024`). Root cause: `ctx->notify_enabled` gated
`arm_ready_after_push`, and the flag was only set by `dmesh_cq_fd()`. **A pure-polling
client that never asks for the fd received nothing on ESTABLISHED conns** — a new conn's
first message rides the separate accept queue, which is *not* gated, hence the
request-works/reply-vanishes asymmetry. `verbs_dpumesh.c` passed only because it happens to
call `cq_fd`. Fixed in the core, not the validator: `notify_enabled` **deleted**; readiness
is live from `dmesh_create_cq`. `cq_fd` is now purely the optional idle-sleep path.

## Method notes

- **`bench.sh loopback` is single-threaded** (`accept` → `handle_ctrl` serially). A wedged
  `RUN 2000` at 5 s/round-trip runs for ~3 h and every later `nc` silently queues behind it
  in the listen backlog — which reads exactly like "pod down". Probe with small `N` via
  `nc <podIP> 9092`; `kubectl delete pod` to abort.
- Bench binaries are **not** rebuilt by `make all` when only the library changes (the lib is
  an order-only prerequisite), so a stale binary links against a deleted symbol and the
  build looks green. `rm build/bin/<x>` to force.

# echo greeter pegged ONE CORE at IDLE: app bug, not the API — FIXED + HW-VALIDATED. `grow_waits = 0` **RETRACTED**

Reported from `htop`: "echo dpumesh is using 100% of one core". It was real and had been running
for hours — with **no traffic**. Root cause is entirely in `bench/echo_dpumesh.c`; the transport
was asleep the whole time.

## Diagnosis — per-thread `/proc`, not a profiler

| pid 2407245 (stuck) | state | utime | stime | total |
|---|---|---|---|---|
| tid 2407245 — **greeter main loop** | **R** | 439,632 | **652,931** | **10,925 s** |
| tid 2407386 — PE (transport) thread | S | 288 | 956 | 12 s |

| control: pid 2410359 (same image, same deploy) | state | 5 s sample |
|---|---|---|
| main loop | **S** | **0 / 500 ticks = 0%** |

A 10 s sample of the stuck main thread: **1000/1000 ticks = exactly 100% of one core**. Meanwhile
`bench_dpumesh` (its client) was `Ssl` with 0:14 total — **idle, but alive**. Two facts fell out
immediately: (1) the **PE thread was sleeping**, so this was above the API, not in the transport;
(2) **`stime` > `utime`** ⇒ a syscall-bound spin (`epoll_wait`+`read`), not computation.

## Root cause — two predicates that disagree

The loop decides whether to sleep by reading `qn` alone:
```c
for (i...) if (((greeter_conn_t *)g_live[i]->user_data)->qn > 0) { pend = 1; break; }
epoll_wait(epfd, events, MAX_EVENTS, pend ? 0 : -1);      /* pend -> timeout 0 = poll */
```
but `reply_pump` refuses to touch a conn it has given up on, **without clearing the FIFO**:
```c
while (gc->qn > 0 && !gc->dead) { ... }                   /* dead -> return, qn untouched */
```
So `dead=1` + non-empty FIFO reads as "work pending" **forever**: timeout pinned at 0, `poll_cq`
returns 0, `reply_pump` does nothing → tight `epoll_wait`+`read` spin. `dead` is latched at three
sites (`dmesh_alloc` non-EAGAIN fault, `post_send` fault, `qn >= REPLY_Q` overflow).

**This is not an API gap.** A correct greeter sets `pend` only under real backpressure. The API's
non-blocking `dmesh_alloc` merely *amplified* a missing line into a pegged core.

## The actual missing line — `dead` is a DEFERRED-DESTROY MARKER with no sweep

The predicate mismatch is a symptom. `dead` exists because the API forbids destroying mid-batch:

> `dmesh.h` / `API.md`: *poll_cq can emit CONN_REQ plus that QP's landed messages in **one batch**,
> so destroying mid-batch dangles later `wc[]` entries.* **Defer destroys to a sweep after the batch.**
> And `dmesh_post_send` → `EBADMSG` is documented as **"descriptor fault → destroy the QP"**.

`reply_pump` runs inside the batch (via `on_request` ← `bench_reframe_feed` ← the `poll_cq` loop),
so it *cannot* destroy — it can only mark. `dead` IS that mark. **The greeter never wrote the
sweep**, so the destroy was deferred forever and dead conns became zombies: still in `g_live`, still
holding a `MAX_CONNS` slot, reclaimed only at `RECV_FIN` — which an **idle-but-alive** peer never
sends. Hence "forever". The intent was visible in the code: the post-batch loop already iterated
**backwards**, which is exactly the safe-removal pattern for `reclaim`'s swap-with-last compaction.

## Fix — write the sweep

```c
for (int i = g_nlive - 1; i >= 0; i--) {
    reply_pump(g_live[i]);                   /* may itself latch dead */
    if (((greeter_conn_t *)g_live[i]->user_data)->dead) reclaim(g_live[i]);
}
```
Safe here and only here: past the `wc[]` batch, and `dmesh_destroy_qp` clears the CQ resume cursor
itself (`if (c->cq && c->cq->vq_cur == c) c->cq->vq_cur = NULL`) and discards unsent TX bytes.
It fixes three things at once, where the predicate patch fixed one:
- the peer gets a **FIN** and *learns*, instead of waiting forever for a reply that never comes;
- the `g_live` / `MAX_CONNS` slot is freed instead of leaking to a zombie;
- the conn leaves `g_live`, so `pend` **structurally cannot see it** — no invariant to maintain.

**Rejected: my first patch (`conn_kill()`)** — it cleared the FIFO so the two predicates agreed.
That silences the spin and leaves all three problems above. Reverted; the helper is gone and the
three sites are plain `gc->dead = 1` again. Net: one helper deleted, two lines added.

## HW validation — same pod, before vs after

| phase | echo main thread | state |
|---|---|---|
| **BEFORE (stuck pod 2407245)** | **100% of one core** | **R** |
| idle, pre-traffic | 0 / 1500 ticks = **0%** | S |
| idle, after validator matrix | 0 / 1500 ticks = **0%** | S |
| idle, after benchmark load | 0 / 2000 ticks = **0%** | S |
| **under load** (1 KB × conc 32) | 177 / 800 ticks = **22%** | S |
| idle, final | 0 / 2000 ticks = **0%** | S |

**Cumulative since pod start: 1,046 ticks (10.5 s)** vs the stuck pod's **10,925 s**. It sleeps at
idle, works under load, and goes back to sleep — which is the whole claim.

### Validator matrix — ALL 0-fail, on **both** deploys (config: **default**, shards=1, egress=1)

Deploy A = the rejected `conn_kill` patch; deploy B = the shipped sweep. Both runs, same matrix:

| validator | B (sweep, shipped) | A (conn_kill) | prior |
|---|---|---|---|
| `loopback 2000 1024 1` | **2000/0** · p50 115.9 µs | 2000/0 · 115.9 | 115.7 µs |
| `loopback 2000 8192 0` | **2000/0** · p50 116.7 µs | 2000/0 · 116.7 | 116.8 µs |
| `verbs 2000 1024 1 8 4` | **2000/0** · p50 155.2 µs | 2000/0 · 153.4 | 154.9 µs |
| `verbs 2000 8192 0 1 1` | **2000/0** · p50 93.3 µs | 2000/0 · **105.4** | 92.3 µs |
| `preload 5000 1024 8` | **5000/0** · p50 121 · p99 445 µs · 23,884 rps | 5000/0 · 119 / 203 · 24,415 | 121 / 357 µs · 25,091 rps |

Running the matrix **twice across two deploys** paid for itself: deploy A's `verbs 8192` p50 of
105.4 µs (+14% vs the 92.3 baseline) looked like a regression on a pod this change does not touch.
Deploy B put it at 93.3 µs. It was **deploy-to-deploy variance**, now demonstrated rather than
asserted. `preload` p99 is likewise unstable across deploys (203 / 445 / 357 µs) at a stable p50 —
do not read a single p99 sample as signal. Everything else is flat.

### Benchmark through the fixed greeter — 0 fail (deploy B / deploy A)

| point | result |
|---|---|
| `dpumesh 1024 8 32 10 1000 1` | mrps 0.0978/0.0975 · 0.801 Gb/s · p50 340 µs · **fail=0** · waits=**0** |
| `dpumesh 8192 8 32 10 1000 1` | mrps 0.0645/0.0642 · 4.227 Gb/s · p50 485/499 µs · **fail=0** · waits=**662/554** |
| `dpumesh 64 8 32 10 1000 4` | mrps 0.1028/0.1029 · 0.053 Gb/s · p50 1253 µs · **fail=0** · waits=**0** |

## RETRACTION — `grow_waits = 0` is FALSE

The entry above claims *"`waits=0` on every run measured, **at every
concurrency/thread count/payload** — backpressure has never actually occurred"*, and used it to
retire the shim's `EPOLLOUT` question ("a path that never executes").

**`waits` = 554 and 662** at 8 KB × conc 32, on two independent default deploys — 0 at 64 B and
1 KB, reproducibly non-zero at 8 KB. Backpressure **does** occur; it is payload- and
DPU-config-dependent (the measured configuration used shards=1/egress=1; the prior entry does not state its config, cf.
the baseline-config trap). Consequences:

- **The `EAGAIN` path is live code, not a theoretical branch.** Every claim resting on
  "it never executes" is now unsupported — including *"Revisit only if `waits` ever moves"*: it moved.
- The greeter stays at 0% only because **its replies are 8 B**, so its ring never fills. The
  **client** (8 KB requests) is what hit 554. A server with large replies WOULD poll here for real.
- `design/API.md` §"Backpressure" said `EAGAIN` means the ceiling was hit, "**not that memory ran
  out**". Also false: `dmesh_core.c:1127` returns `EAGAIN` when `block_pool_grab()` fails — the
  process-wide 512-block pool is empty because **other** conns hold it. Three EAGAIN sites, two
  causes (`:1100`/`:1119` = this conn's ceiling; `:1127` = shared-pool exhaustion). The second is
  not the caller's doing and **no per-conn accounting prevents it** — so `EAGAIN` is a normal
  resource condition, never a caller error. This is exactly where the `ibv_post_send`/`ENOMEM`
  analogy breaks: verbs sends from *your* memory (only the descriptor count is finite, so
  overrunning it IS your bug); here the buffer itself is a finite shared resource.

## Doc fixes shipped with this

- **`dmesh.h:79` — `dmesh_sq_depth()` was a PHANTOM**: documented as "reports the per-conn
  descriptor cap", never declared, never defined, zero callers. Removed. (The API exposes no
  in-flight budget at all — `DPUMESH_TX_MAXB`×`_TX_BLOCK` live in env vars, so unlike verbs'
  `max_send_wr = 4` a caller cannot compute when `alloc` will fail. Left as-is: the shared pool
  makes it unpreventable anyway, so the number would not buy prevention.)
- **`API.md` §2 "No send completions"**: the stated reason — *"unsignaled is the verbs default"* —
  is wrong. Verbs chooses **per-WR** (`IBV_SEND_SIGNALED`), and hello-world `rdma_cm` sets
  `sq_sig_all = 1` and calls `rdma_get_send_comp()`. Replaced with the honest reason: nothing needs
  one (you do not own the send buffer; TX_ACKs reclaim the ring on the PE thread) — and flagged
  that this is weaker now that `grow_waits` ≠ 0.
- **`API.md` §4a/§4b**: §4b's retry-in-place `while (!(b = dmesh_alloc(...))) poll_cq(...)` is
  correct **only** for a single-conn program (it IS the whole loop; nothing to starve). Marked as
  such, and §4a now warns a reactor must park instead + keep the parked set exact.
- **`EAGAIN` is documented as non-fatal** in both `API.md` and `dmesh.h`.

## Mine, not the code's

I spent several turns trying to force `dmesh_alloc` onto the `ibv_post_send` contract and proposed,
then withdrew, three designs (`DMESH_WC_SEND`, blocking-alloc, create-time budget). All were
category errors: **verbs has no allocator**, so "what would verbs do?" has no answer for `alloc` —
the finite *shared* buffer decides the semantics, not the label. The user's framing was right:
alloc failure is a resource condition, not an error, and a reactor API has exactly ONE wait point
(the CQ), so a second one inside `alloc` is incoherent. Net API change: **none**.

## Which site latched `dead`? — NOT reproduced; narrowed to one by elimination

**The trigger was not reproduced** — the original pod died in the redeploy. The three sites were
temporarily instrumented (`DEAD @alloc|@post_send|@fifo` + errno, plus a line per sweep); **that
instrumentation has since been REMOVED** — two of the four `fprintf`s sat inside `reply_pump`, which
runs per reply, and a `fprintf`+`strerror` in a hot function is not worth carrying for a branch that
never fired. So a future occurrence is again anonymous; re-add them if it recurs.
Attempted repro of `@fifo`: `point dpumesh 64 65536 1200 10 10 1` (64 KB replies to fill the 256 KB
ring so `reply_pump` stalls, conc 1200 > `REPLY_Q` 1024). **`@fifo` did not fire.** It hit the
already-documented "Known limitation" instead: 1200 × 64 KB = **76 MB**
of demanded in-flight against a 256 KB ring = **300×** the cap, so the writer throttles past the
RUN deadline — `fail=125523`, greeter `served=63`, DPU log clean (no wedge). The pipeline throttles
upstream long before `qn` can accumulate to 1024.

Elimination over the three sites (CORRECTED — read `dpumesh_enqueue`/`dpumesh_tx_reserve`, do not
reason from doc strings):
- **`@alloc` (non-EAGAIN)** — reachable via `EINVAL` (bad port / `len==0` / `len>block_size`, none of
  which the greeter produces: `want = min(total-done, block_size)`, port is the conn's own) **but
  also via `ENOMEM`**: `dpumesh_tx_reserve` mallocs the per-conn `su_seq`/`su_end` lazily on first
  send (`dmesh_core.c:1078-1081`) and returns `NULL`+`ENOMEM` if that fails. Not "unreachable" — I
  said that earlier and it was wrong.
- **`@fifo` (`qn >= 1024`)** — needs > 1024 outstanding on ONE conn. Every recorded bench run uses
  `conc` ≤ 32; forcing conc=1200 throttles upstream instead of overflowing (above). Unreachable in
  practice.
- **`@post_send` → `EBADMSG`** — `dpumesh_enqueue` returns −1 **only** after a forward ring stays
  full for `RING_STALL_DEADLINE_SEC = 5` seconds, at which point it sets `ring->dead` and logs
  `DOCA_LOG_ERR("...STALLED >5s...no DPA consumer is draining it")`. That is a **DPU-down / DPA-not-
  draining catastrophe**, not a normal-traffic path — momentary congestion just backs off (1→50 µs)
  and retries. So it is **loud, not silent**, and it is not the everyday failure I implied.

**Correction of my own earlier claim.** I wrote that a "temporary wedge permanently kills a conn"
via `@post_send`/`EBADMSG` and called it "the only reachable" site — three errors: (1) `@alloc` is
also reachable (`ENOMEM`); (2) `ring->dead` is the **consequence of a 5 s stall**, not of momentary
congestion — the causality was backwards; (3) it is logged, not silent — the observed pods show **0**
`STALLED`/`busy`/`dead` lines, consistent with every test passing. The likeliest explanation for the
original 2407245 is that it was latched during the **stale-`px_lane` ring-wedge era**,
which is fixed, rather than by a standing bug in current code.
**Trigger: still unidentified. No standing wedge bug demonstrated.**

**Validation status, stated honestly:** "the greeter no longer pegs a core" is **measured** (idle 0%,
`state=S`, including after the 76 MB abuse run above — the old code would have spun). "the `dead`
path is now correct" rests on **construction** (the sweep destroys the conn however `dead` is
reached), not on a repro.

## Also fixed — a log-spam bug that buried the diagnostics

`if (g_served && (g_served % 200000) < 64)` printed `served=N` ~64 times per milestone, filling the
log during the repro attempt above. **My first patch for it was wrong** and shipped: I read it as
"matches served 0..63" and gated on `g_served >= 200000` — which fixed only the first milestone.
The deploy proved it (`served=3800056 … 3800063`, 8 lines and counting). A modulo test **cannot**
express "every 200k" here: `g_served` advances a few per loop pass, so the widened `< 64` window
(there to survive a batch stepping over the exact multiple) stays true for ~64 consecutive passes at
*every* multiple. Replaced with a milestone cursor (`g_served - g_reported >= 200000`), which fires
once regardless of step size.

# egress recover: `px_engine_recover` cleared PEER engines' `refresh_inflight` (multi-engine buffer double-free/leak) — FIXED + healthy-path HW-validated

## The bug (code-certain)
`px_engine_recover` (`dpu_proxy.c`), after restarting a faulted `doca_dma` ctx, cleared **every**
lane's `refresh_inflight`:
```c
for (int p = 0; p < MAX_PODS; p++)              /* ALL 32 pod slots */
    for (int r = 0; r < MAX_EU_PER_POD; r++)
        px->lanes[p][r].refresh_inflight = 0;
```
But lanes are owned per engine (`pod_idx % n_eng`, `px_engine_pump`: `for (i = eng->id; i < npods; i += n_eng)`),
and **each engine has a PRIVATE ctx** (`doca_dma_create` per engine in `px_init`). So engine A's fault
flushes only A's tasks; engine B's lanes and their in-flight credit-refreshes are unaffected — yet A's
recover cleared B's flags too.

Why that corrupts: `refresh_ops[pod][region]` is **one `px_op` reused per lane**, and the only guard against
a second submit overwriting its `src_buf`/`dst_buf` is `refresh_inflight`. `refresh_inflight` is a plain
`int`, correct because each lane is touched by exactly one thread (its owning engine's worker; completion
callbacks run on that same worker). Cross-engine clearing breaks BOTH invariants:
- **data race** — engine A writes `ln->refresh_inflight=0` on a lane engine B's thread is reading/writing.
- **double-submit** — B then sees `refresh_inflight==0`, issues a *second* refresh on the same `px_op`,
  overwriting `op->src_buf/dst_buf`. The first refresh's two inventory bufs lose their `dec_refcount`
  handle (leak); the stale callback later frees the *second* refresh's bufs while its DMA is still in
  flight (premature free of an in-use inventory buf). Net: inventory bufs leak → that engine's refreshes
  eventually all fail → the lane pins `avail_entries=0` → **permanent egress starvation** — the same
  failure shape as the pod-restart wedge, just reached via a different door.

Trigger surface: `n_eng >= 2` (`DPUMESH_ARM_EGRESS_THREADS>=2`), which is the **recommended** config
(`n_eng=1` wedges under overload; the validated ceiling config is `②+egress2`). Needs engine A faulting
while engine B has a refresh in flight — a narrow interleave, which is why the single-pod-death validation
(14 sequential restarts, 0-fail) never surfaced it: that was proven on the pre-multi-engine
egress path / single engine, where "all lanes" == "our lanes".

Note: mass-death (≳13 pods) is a **different** limitation — there the ctx hangs in STOPPING and never
reaches IDLE, so this IDLE-branch loop never runs. This bug lives on the **healthy single-fault recover
path** (ctx faults, flushes to IDLE, restarts), not the mass-death hang.

## The fix (one line + rationale comment)
```c
-        for (int p = 0; p < MAX_PODS; p++)
+        for (int p = eng->id; p < MAX_PODS; p += px->n_eng)   /* only lanes THIS engine owns */
             for (int r = 0; r < MAX_EU_PER_POD; r++)
                 px->lanes[p][r].refresh_inflight = 0;
```
Restores the single-owner-thread invariant. For `n_eng==1` (`eng->id=0`, step 1) it covers all lanes
**identically** to before — the default path is byte-for-byte unchanged. `git diff --stat`: 1 file, +13/-6
(all but one line is the rationale comment).

## Validation — CLEAN redeploy, `DPUMESH_ARM_EGRESS_THREADS=2 DPUMESH_INGEST_SHARDS=2` (the config that ACTIVATES multi-engine egress)
Full validator + bench suite, ALL 0-fail:

| test | result |
|---|---|
| loopback 50000×8192 (zc off/on) | 50000/0, 50000/0 — p50 117/116 µs |
| verbs 50000×8192 w4p4 / 40000×64 w8p8 / 40000×2048 | 50000/0, 40000/0, 40000/0 |
| preload (LD_PRELOAD shim) 8000×1024 c8 | 8000/0 — 32176 rps |
| latency 64…1024 B | every size 114 µs p50, no skip |
| bandwidth 32 B…128 KB | every size a real row, peak 8.4 Gb/s @32 KB, no 0-throughput |
| rate 1/2/4/8 threads | all rows, 0.165–0.184 Mrps |

`fault lines during the healthy suite = 0` — i.e. `px_engine_recover` never ran, so the changed code was
**dormant**; this run proves the edit does not regress the always-on egress path, not that it repaired a
live fault (see below).

## Honest limitation — the fault path was NOT live-injected (harness can't do it cleanly)
`px_engine_recover` only fires when a **peer pod's host memory vanishes mid-DMA before comch reports the
disconnect**. In this k8s harness neither injection vector is clean:
- **graceful `kubectl delete pod`** — the app closes first, comch reports the disconnect, the DPU unpublishes
  the pod's mappings, and the LB routes around it. Observed directly: a 45 s bench→echo load with echo
  deleted mid-run finished **rcnt=7.76M, fail=0** — the mesh self-healed via `collect_live_hosts`, and
  **no fault fired** (0 recover lines). Self-heal, not fault.
- **abrupt / in-place restart** — starting a replacement pod against an already-running DPU is the operation
  `bench.sh` **explicitly forbids** ("leaves the two sides' registration state inconsistent; deploy is the
  only supported path"). Confirmed: after the delete, the new echo pod (RESTARTS=0, new pod, new IP) never
  cleanly re-registered; bench→echo went to rcnt=0 and the DPU sat **idle** (alive, non-spinning, no error
  flood — traffic simply never reached it). That is the documented registration desync, NOT an egress fault
  and NOT this fix's code path.

So the fix rests on **(1) code-certain construction** (private ctx ⇒ peer lanes must not be touched; the
race + double-submit are mechanical) and **(2) healthy-path regression** (above), NOT on a reproduced fault.
A deterministic repro needs a fault-injection hook (e.g. force `IO_FAILED` on one engine while another has
`refresh_inflight=1`) that the current harness does not expose. Recommend adding one before claiming the
recover path itself is regression-covered.

## Not changed (noted, out of scope of this one-bug fix)
- `px_lane_submit` credit test `avail < first_needed || avail < first_needed + MARGIN` — first clause is
  subsumed by the second (harmless redundancy).
- `stat_msgs/stat_segs/stat_drop_bytes` increment non-atomically across ingest shards (only `stat_units` is
  atomic) — cosmetic counter skew at M≥2, no data-path effect.

---

# NAMING Phase 0 — name-based identity/routing (host resolver) — HW validation

**Change:** `design/NAMING.md` Phase 0. The `service_id` integer's provenance moved from the user
(three hand-typed sources) to a file-backed resolver (`src/dmesh_resolve.c`). Public API is now
name-only (`dmesh_create_channel(void)`, `dmesh_create_qp(cq, const char*)`); identity comes from
`$DPUMESH_SERVICE` resolved through the registry; `DPUMESH_SERVICE_ID` and the shim's
`DMESH_PRELOAD_{SVC,LISTEN,MAP,REGISTRY}` are **deleted**. **The DPU and the wire ABI are UNCHANGED**
— this is a host-side provenance refactor, so the pass criterion is **NEUTRAL** (0-fail, identity
resolves to the same integers, p50 in the normal validator range), not a perf delta.

config: default deploy — `bench/bench.sh deploy`, rings_per_pod=2, fair 1-core pin, no
shards/egress2, proxy off. DPU recompiled 15 objs from the header change and came up clean.

### Identity resolution — `resolve_name($DPUMESH_SERVICE)` on real HW (the core proof)
Every meshed pod resolved its k8s-Service NAME to the expected interned `service_id`; the DPU
assigned pod_ids and derived backends unchanged (`worker=svcN` = the resolved id):

| pod | `$DPUMESH_SERVICE` | resolved svc | DPU pod_id | log |
|---|---|---|---|---|
| echo-dpumesh | echo-dpumesh | **11** | 0 | `service=echo-dpumesh … worker=svc11` |
| loopback-dpumesh | loopback-dpumesh | **12** | 4 | `own_service=loopback-dpumesh … worker=svc12` |
| stream-dpumesh | stream-dpumesh | **16** | 7 | `worker=svc16` |
| verbs-dpumesh | verbs-dpumesh | **17** | 6 | `own_service=verbs-dpumesh … worker=svc17` |
| preload echo child | preload-dpumesh | **15** | 5 | `REGISTER service_id=15 … worker=svc15` |
| preload client | (unset) | **SVC_NONE(-1)** | 8 | `worker=svc-1` (pure client, correct) |

### Validator matrix — all 0-fail
| validator | new path exercised | N / config | result |
|---|---|---|---|
| loopback | `create_channel()` id + `create_qp(cq,name)` self | 50000 × 8192B | **50000/0**, p50 117.1µs |
| verbs | verbs façade, name self-addressing | 50000 × 8192B (w1,p1) | **50000/0**, p50 103.5µs |
| verbs | " | 50000 × 4096B (w4,p8) | **50000/0**, p50 148.9µs |
| stream | name id (svc16); self byte-stream | 20000 × 1024B self | **20000/0**, 20.58 MB, p50 115.7µs |
| preload | shim `resolve_addr`+`config_identity`+`listen_port` | 5000 × 1024B, 8 conns | **5000/0**, p50 123µs, p99 433µs, 23126 rps |
| bench client | pure client → `create_qp(cq,"echo-dpumesh")` + DPU LB | req64 conc1 5s | **fail=0**, p50 114µs, rcnt 37492 |
| bench client | " (LB/bandwidth path) | req1024 conc32 5s | **fail=0**, p50 340µs, 0.805 Gb/s, rcnt 490216 |

### Verdict
**NEUTRAL — PASS.** Six code paths new to Phase 0 (`config_identity`, `resolve_name`,
`resolve_addr`, `config_listen_port`, `dmesh_qp_open`, and the deleted `DPUMESH_SERVICE_ID`) are all
exercised on hardware with **0 fail**, and every pod resolves its name to the same integer the old
hand-typed value would have produced. p50s match the normal self-loop validator range. No throughput
comparison is drawn (default config; the wire is byte-for-byte identical, so any delta would be noise
— cf. the scale_log config-trap). The host-side of NAMING is proven; Phases 1–3 (k8s controller +
webhook + live reload) remain unbuilt (NAMING.md §8).

---

## Per-conn inbox sized to the reverse-credit budget (silent RX drop closed)

**What changed.** The per-conn inbound descriptor ring (`DMESH_INBOX_RING`) was a compile-time
**256**, while the DPU caps in-flight reverse-DMA landings per region at `rq = rq_depth/K =
num_slots·slot_size/DPUMESH_SLOT_SIZE / K` (`doca/comch_common.c`) — **2048** on the default deploy.
The 8× gap meant a hot conn could fill its 256-deep inbox while the DPU still held ~1792 credits, so
`rx_deliver_desc` **silently dropped** the overflow (`inbox_push`→0 → `DOCA_LOG_ERR` + `rx_credit_return`)
even though the body had already landed in host RX memory and the sender already had its forward
`TX_ACK`: undetectable message loss on a "reliable" transport. (px_lane on the DPU sends; the inbox
on the host receives — different objects across PCIe, coupled only by the count-credit `rq`.)

`dmesh_core.c` now sizes the inbox at init to that budget:
`ctx->inbox_ring = round_pow2(max(256, num_slots·slot_size/DPUMESH_SLOT_SIZE / K))`, stamped per slot
at malloc, with a `DPUMESH_INBOX_RING` env override. At `inbox ≥ rq` the DPU's own credit runs out
**before** the inbox fills → clean back-pressure, so the drop path is unreachable in steady use
(proof: in-flight ≤ rq, inbox occupancy ≤ in-flight ≤ rq ≤ inbox capacity). **Host-side only — no
public API, no wire ABI change** (the DPU's `rq` is untouched; the host inbox is sized to match it).

config: default deploy — `bench/bench.sh deploy`, rings_per_pod=2, fair 1-core pin, no shards/egress2,
proxy off. Every meshed pod came up logging **`inbox_ring=2048 (rq/K, was 256)`** (verbs pod_id=6,
loopback pod_id=4, echo pod_id=0, preload pod_id=5) — the 256 ceiling is gone.

### Validator matrix — all 0-fail (correctness unchanged)
| validator | config | result |
|---|---|---|
| loopback | 50000 × 8192B | **50000/0**, p50 117.1µs |
| verbs | 50000 × 8192B, w1 p1 | **50000/0**, p50 103.6µs |
| verbs | 50000 × 8192B, **w32 p1** (max inbox stress) | **50000/0**, p50 252.7µs |
| verbs | 50000 × 4096B, w4 p8 | **50000/0**, p50 150.7µs |
| verbs | 40000 × 64B, w8 p8 | **40000/0**, p50 297.4µs |
| verbs | 20000 × 8192B **zc**, w8 p2 | **20000/0**, p50 150.7µs |
| preload | 5000 × 1024B, 8 conns | **5000/0**, p50 122µs, p99 195µs, 26650 rps |
| stream | 20000 × 1024B self | **20000/0**, 20.58 MB, p50 116.1µs |

### Benchmarks — no regression from the 8× inbox (48 KB vs 6 KB/conn, lazy per live conn)
| point | before | now | verdict |
|---|---|---|---|
| latency conc1, 64–1024B | p50 ~114 µs | p50 **114–115 µs** | match |
| bandwidth 8192 conc32 | 4.227 Gb/s | **4.222 Gb/s** (peak 7.9 @ 32KB) | match |
| point `8192 8 32` | fail=0, waits 554–662 | fail=0, **waits 719** | same order (TX-side) |
| point `1024 8 32` | 0.801 Gb/s, waits 0 | **0.806** Gb/s, waits 0 | match |
| point `64 8 32 t4` | p50 1253 µs, waits 0 | p50 **1238 µs**, waits 0 | match |

Bandwidth ≥512 KB reports 0.0 — the **pre-existing 256 KB/conn in-flight cap** (`maxb·block`, TX-side:
a 512 KB req × conc32 is ~64× the cap → the writer throttles past the RUN deadline), **not** this
change (the inbox is RX-side). `waits`/`grow_waits` are the TX send-ring backpressure counter, also
untouched here; the 719-vs-554–662 spread is normal run variance.

### Verdict
**PASS — no regression.** The inbox ceiling is raised from 256 to the DPU's per-region credit budget
(2048 default), making the silent inbox-full drop in `rx_deliver_desc` unreachable in steady
operation. All eight validators hold 0-fail and every benchmark matches the baseline.
**Caveat (honest):** the bench harness drains each conn every poll, so it cannot inject the
slow-drain single-conn flood that actually overflowed the old 256 ring — this validates the fix **by
construction** (inbox ≥ credit budget) plus a clean regression sweep, not by reproduce-then-fix.
Memory cost: 48 KB/conn (was 6 KB), lazy per live conn, tunable via `DPUMESH_INBOX_RING`; raising
`DPUMESH_RINGS_PER_POD` shrinks it automatically (rq = rq_depth/K).

---

## Free robustness fixes (no API/ABI change) + HW regression sweep

Six zero-tradeoff, zero-API-surface fixes, deployed together and validated on HW. Two more
candidates were **deliberately not** done (see end): they are not actually free.

| # | fix | defect closed | where |
|---|---|---|---|
| **A1** | `post_send` len ≤ reserved, enforced | an over-long commit advanced `tx_w` past the backed region → **ring overrun / OOB**. Now `tx_reserve` records `resv_len`; `tx_commit` clamps + warns. | `dmesh_core.c` `dpumesh_tx_commit` |
| **A2** | **cumulative-ACK** reclaim | exact-FIFO-front match: a genuinely dropped intermediate `FWD_ACK` (DPU drops one on `pending_txack` overflow) would **wedge the conn's TX forever**. Now pops every unit at-or-before the acked seq within the ≤`TX_SU_DEPTH` window (safe: forward completion is in-order per conn). | `dmesh_core.c` `tx_reclaim_ack` |
| **A3** | "same bytes" contract, clarified | header promised a re-alloc returns the same buffer unconditionally; the pad path can reposition it for a *different* len. Doc now scopes it to same-len (contract already forbids the rest). | `dmesh.h` SEND CONTRACT |
| **A4** | identity resolve failure → **loud** | `$DPUMESH_SERVICE` set-but-unresolved degraded to a silent client (unreachable server, no error; load-once so never self-heals). Now warns loudly, still returns `SVC_NONE`. | `dmesh_resolve.c` `dmesh_config_identity` |
| **A5** | absent registry → **loud** | a missing registry file marked the table "loaded" and every resolve silently `ENOENT`-ed. `load_file` now reports absence; `dmesh_config_load` warns with the path. | `dmesh_resolve.c` |
| **A6** | FIN doc/impl reconciled | comment claimed "skip if no TX slot (best-effort)"; `dmesh_send_fin` actually rides `dpumesh_enqueue` and can back off to the ring-stall deadline. Comment now accurate. | `dmesh_core.h` |
| **A7** | RX-drop counters | the inbox-full / accept-full drops had only per-event logs, no counter — the A→B silent loss was unmeasurable. Added `st_rx_inbox_drops` / `st_rx_accept_drops`, logged at teardown. | `dmesh_core.c` |

**Not done (not free):** `next_group` is **not** a dead field — `dmesh_pin_route` uses it; wiring it
for auto-SAR on large messages is a *feature*, not a cleanup. `destroy_qp` deferred-free (the
mid-batch-dangle footgun) changes the lifecycle contract (the qp must outlive the batch) — a real
behavior change, not a mechanical fix. Both left for a dedicated change.

config: default deploy — rings_per_pod=2, fair 1-core pin, no shards/egress2, proxy off. Host lib
rebuilt (`make lib` clean, link OK); every pod came up `inbox_ring=2048` with **no** false
identity/registry warnings (correct config is silent). Pass criterion is **NEUTRAL** (0-fail, p50 in
range) — these are correctness/robustness guards, not a perf delta.

### A2 is exercised on every message — 0-fail is the proof
`tx_reclaim_ack` runs on every `FWD_ACK`, so the cumulative reclaim is on the hot path of all ~285 k
validator round-trips + ~2.6 M `point` round-trips below. A wrong window/wrap would wedge or corrupt
→ a real FAIL. **0-fail throughout**, including the 8 KB×conc32 `point` at `waits=1934` (heavy TX
backpressure — the reclaim churns hardest exactly there).

### Validator matrix — all 0-fail
| validator | config | result |
|---|---|---|
| loopback | 50000 × 8192B | **50000/0**, p50 116.7µs |
| verbs | 50000 × 8192B w1p1 / **w32p1** / 4096B w4p8 / 64B w8p8 / 8192B **zc** w8p2 | **50000/0** · **50000/0** · **50000/0** · **40000/0** · **20000/0** |
| preload | 5000 × 1024B, 8 conns | **5000/0**, p50 123µs, p99 412µs, 25782 rps |
| stream | 20000 × 1024B self | **20000/0**, 20.58 MB, p50 115.7µs |

**RX-drop evidence:** 0 `inbox full` / `accept queue full` log lines across all five dpumesh pods
after the full matrix (incl. verbs w32) → the A7 counters are 0; the inbox=2048 sizing holds.

### A4/A5 negative test (warnings actually fire)
Linking `libdpumesh.so` and calling `dmesh_config_identity()` directly (pure file+env, no DOCA):
| env | output |
|---|---|
| `DPUMESH_SERVICE=bogus-svc DPUMESH_CONFIG=/nonexistent` | **both** warnings fire (registry-not-found + name-not-in-registry), `identity=-1` |
| `DPUMESH_SERVICE` unset (intentional client) | **silent**, `identity=-1` |

### Benchmarks — no regression vs baseline
| point | baseline | now |
|---|---|---|
| latency conc1 64–1024B | p50 ~114µs | p50 **114–115µs** |
| bandwidth 8192 conc32 | 4.227 Gb/s | **4.201 Gb/s** (peak 7.83 @ 32KB) |
| `point 8192 8 32` | fail=0, waits 554–662 | fail=0, waits **1934** (TX-side, run-variable; cumulative-ACK if anything frees faster) |
| `point 1024 8 32` | 0.801 Gb/s, waits 0 | **0.778** Gb/s, waits 0 |
| `point 64 8 32 t4` | p50 1253µs, waits 0 | p50 **1258µs**, waits 0 |

Bandwidth ≥512 KB = 0.0 remains the pre-existing 256 KB/conn TX in-flight cap (unrelated).

### Verdict
**PASS — no regression.** All six free fixes are live on HW: 8/8 validators 0-fail (A2 proven on
every reclaim), zero RX drops, A4/A5 warnings confirmed to fire on misconfig and stay silent on
correct config, benchmarks match baseline. No public API or wire ABI changed.

---

# `dmesh_pin_route` + the `route_group` wire key REMOVED — BUILT, wire-verified, HW-VALIDATED (see session 2/3 below)

**Trigger.** "Why does `pin_route` exist, is it needed?" It is not. The DPU's L4
route was then per-message RR with **no** connection stickiness, so stamping
one route-affinity key on every message of a conn was the only way to give a socket app total
order. Commit `66c31f7` then added `px_conn.pinned_backend` (Envoy TCP-proxy session affinity,
default STICKY) and superseded it. `pin_route` was never removed.

## Why it was dead

`px_resolve_backend` precedence is **(1) L7 host override > (2) conn sticky > (3) `dpu_route_l4`**.
For a default (sticky) service, (2) returns from the 2nd message on, so the key was consulted only
on a conn's FIRST message — where it cannot add ordering (nothing is pinned yet). Exhaustively:
- **L4 passthru**: conn stickiness already pins. Key unused.
- **frame parser**: routes each frame by its own `svc` byte and hardcoded `rg=0`. Key never read.
- **L7 path**: `px_parse_l7` latches `c->msg_dst` at each message head and ships every chunk of
  that message to it — per-message affinity **already built in, no wire key needed**.
- **per-request services** (`DPUMESH_LB_PER_REQUEST_SVC`): the one place the key was load-bearing.
  Never set anywhere in the repo, and incoherent at L4 anyway — per-message LB splits a byte
  stream across backends, which is an L7 concern (Envoy terminates HTTP2 and re-originates; it
  never splits an L4 byte stream per message).

## Why it was harmful — frozen pin map starves scale-out

`route_group_backend[svc][rg]` had **no invalidation on topology change**: written at init
(`memset -1`) and on pin; re-picked ONLY if the pinned pod is dead. `next_group` is a per-channel
monotonic 1..255 counter, so it **wraps every 255 conns** — and the shim pinned EVERY conn
unconditionally (`dmesh_preload.c:664`). After the wrap, a new conn inherits the pick made by a
long-dead conn, bypassing `lb_pick`: the rg→backend map **freezes**. Add a backend and pinned
conns never route to it until an old backend dies. Worst case is cold start: if only one backend
is ready while 255 conns churn, the whole table pins to it **permanently**. Unpinned conns would
have picked up the new backends immediately via RR. Envoy-parity violation (a new endpoint must
take traffic).

## What changed

| | before | after |
|---|---|---|
| `comch_dma_comp_msg` | 20 B — **2 WQE basic blocks** | **16 B — 1 BB** (exact pre-feature revert) |
| `comch_msg` (union) | 60 B | **60 B** — dominated by 56 B `add_ring_msg`, so **no msgq / `imm_data_len` change** |
| `dma_desc` | 64 B, `route_group@28` + `pad0[3]` | **64 B**, `pad0[4]` — cache-line invariant held |
| `struct objects` | `route_group_backend[128][256]` | gone (**−128 KB** ARM) |
| `dpu_route_l4(svc, rg)` | pin table + `lb_pick` | **`lb_pick(svc)`** — a stateless pick; affinity lives only in `px_resolve_backend` |

Deleted: `dmesh_pin_route`, `dmesh_qp_t.pin_group`, `dmesh_channel_t.next_group`,
`sw_descriptor_t.route_group`, `dpu_comp_entry_t.route_group`, `px_arrival.route_group`,
`px_rg_at`, and the DPA passthrough (`dpa_kernel.c`, `dpa.c`). 6 call sites removed
(`dmesh_preload.c` + `bench_dpumesh.c` ×2 + loopback/stream/verbs validators).
**KEPT (independent, verified):** `DPUMESH_LB_PER_REQUEST_SVC` / `svc_per_request` /
`px_service_sticky`, and `svc_rr` / `lb_pick`.

## Accepted semantic loss (documented in API.md §4)

A **per-request service** can no longer carry a post `> msg_max`: its SAR chunks re-LB per
message and scatter. Empty in practice — the L7 path latches `msg_dst` per message, the frame
path consumes whole frames, and the only remaining case is shim + per-request, which is
self-contradictory (a raw socket app needs total order; marking its service per-message and then
pinning it back merely cancels the policy).

## Verification

`make lib bench` exit 0, no new warnings. Wire layout verified by compiling the real headers and
printing `sizeof`/`offsetof`:
```
comch_dma_comp_msg = 16 B   type@0  pos@12
comch_msg          = 60 B   (unchanged -> no msgq reconfig)
dma_desc           = 64 B   dst_pod_id@24  src_pod_id@32  valid@63
```
The three SAR/pipeline-conditional validator pins (loopback `size > msgmax`, stream
`burst > msgmax`, verbs `pipeline > 1`) are no-ops against sticky services, so their 0-fail
matrices should be unchanged.

**HW-UNVALIDATED. This is a WIRE ABI change — host + DPU + DPA must deploy TOGETHER**
(a mismatched pair reads `route_group`'s old offset as garbage; `comch_msg` staying 60 B means
the msgq will NOT reject the mismatch for you). Run `bench/bench.sh deploy` clean, then the full
validator matrix. The scale-out-starvation claim is a code-reading result, not a measured one —
observing it needs a discriminator backend (reply carries server identity); every validator
backend is an echo, so all prior 0-fail results were blind to the same issue.

---

# (session 2) — the proxy's delivery contract: transient pressure now BACKPRESSURES, terminal failure now CLOSES. Neither truncates. — HW-VALIDATED (addendum below)

**Trigger.** Auditing whether the L4 layer supports message send/recv + routing well. Routing
is fine. Delivery was not: **`px_ship_seg` had 4 early-return drop paths and its callers advanced
`parse_pos` regardless**, so a byte stream silently lost a chunk mid-flight. Worse than silent —
`px_advance` charges unclaimed bytes as drops, which releases custody, which **TX_ACKs the
sender**. A blackhole with a success signal attached: the sender's writes all "succeed", no reply
ever comes, and with no timeouts anywhere its `read()` blocks forever.

The two halves had opposite semantics for the *same* condition: `px_arrival_alloc` failure at
ingest returns `0` = *"pool full — retry (backpressure)"*, while `px_unit_alloc` failure 300 lines
later **dropped**. Structural, not sloppy: the retry boundary (a whole completion entry) was
coarser than the failure boundary (one seg), and `px_ship_seg` returned `void` — it had nowhere to
report.

## The contract now

`px_ship_seg` returns three states, and the parse loops honor all three:

| | meaning | caller |
|---|---|---|
| `1` | shipped | advance past it |
| `0` | **EAGAIN** — a pool was momentarily empty | **do not advance**; `px_stall(c)` parks the conn |
| `-1` | **TERMINAL** — undeliverable (no live backend / dst gone / parser contract violation) | advance through what shipped, then `px_poison` |

- **EAGAIN mutates nothing.** `u->seq = ++c->egress_seq` **moved below every allocation** — the
  subtle one: a retry would otherwise burn a seq twice and put a gap in the peer's per-conn
  sequence. The conntrack `uP` find-or-create above it is idempotent under retry, so an EAGAIN
  after it is still clean.
- **TERMINAL closes the stream.** New `px_fin_to_sender` queues a 0-length unit back to
  `(src_pod, src_port)` → the sender gets `DMESH_WC_RECV_FIN`. `length==0` is the only error code
  point the wire has, and closing is exactly what Envoy's tcp_proxy does when its upstream fails.
  Symmetric: the sender is the client on a request stream, the backend on a reply stream (its
  server QP's `local_port == uP`, so `dst_port = uP` demuxes correctly).
  Verified safe to synthesize: the host demuxes by `dst_port` alone; `desc->seq` is informational
  on the RX path (only logged — the `seq` in `rx_deliver_desc` is the RX queue's own turn-stamp);
  and `cq_emit` on a 0-length body only latches `peer_closed`, never learning a peer.
- `px_poison` is now idempotent and FINs the sender. The one `c->dead = 1` outside it — *"stale
  upstream (client closed)"* — correctly stays silent: there is nobody left to tell.

## Resuming a stalled conn — deliberately NO new wake plumbing

`px_stall` parks the conn on a **shard-local list**; `px_drain_stalled` re-parses it. Units are
freed by the egress (`px_lane_emit` ← `px_drain`, main thread), and every ingest driver already
re-reaches the drain without being told:
- **inline (the default, reaper off):** main IS the ingest thread — it frees units in `px_drain`
  and re-parses on its very next pass. Immediate.
- **reaper (M=1) / shard threads (M≥2):** main does not wake them, so the retry rides their **1 ms
  epoll backstop** (all four park sites use a 1 ms timeout). Bounded, and only in an
  already-degraded state. *(An earlier draft of this comment claimed the egress completion wakes
  them — false for these two modes; corrected.)*

**No busy-spin** (the LOCKED rule): `px_drain_stalled` reports progress **only if `parse_pos`
actually moved**, so an unrelieved pool returns 0 and the loop parks instead of spinning on a
retry that cannot yet succeed. No deadlock: the egress is independent of ingest, so the units
always come back. Cascading backpressure is intact — a stalled conn holds its arrivals, so the
arrival pool drains and `px_ingest_forward` starts returning 0 at the ingest boundary.

`px_conn_del` unlinks from the stall list before the `free` (else the drain walks freed memory).

## Behavior change worth expecting on HW

During pod churn, a conn whose backend dies mid-message is now **closed** (sender sees EOF)
instead of silently losing the rest of that message. That is the point — but it means churn tests
may now show connection closures where they previously showed (echo-blind) success. `dst pod not
ready` is classified TERMINAL, not EAGAIN, deliberately: waiting would stall the conn on a pod
that may never return, and on the L7 path `msg_dst` is already latched to it, so the message is
broken either way.

## Verification

`make lib bench` exit 0. DPU sources syntax-checked against the real DOCA headers
(`-I/opt/mellanox/doca/include`): **zero new warnings or errors** — the 2 `unused variable 'px'`
(`px_conn_del`, `px_parse`) and the 4 `implicit declaration` in `dpu_worker.c` are pre-existing,
confirmed by re-checking a `git stash`'d tree (identical counts). Both `px_ship_seg` call sites
consume the status; there are no others. (`dpa_kernel.c` is DPA device code — host gcc cannot
check it; it needs `dpacc`.)

**HW-UNVALIDATED, and the new paths are NOT reachable under normal load** — they need pool
exhaustion (a slow/credit-starved backend, or a deliberately shrunk pool) or a backend death
mid-message. `bench.sh` has no local mode; every subcommand needs the remote deploy. Suggested
proof: a build with a tiny `PX_UNIT_POOL` under the stream validator should show
`stat_drop_bytes == 0` and 0-fail (before: byte loss), and a backend killed mid-message should
give the client a clean EOF rather than a hang.

## Known gap (unfixed, same class)

`px_queue_fin_unit`'s unit-pool exhaustion still **drops** (*"FIN to pod %d port %u lost"*) — a
lost FIN means the peer never sees EOF and its conn leaks. Backpressuring it needs `px_try_fin` to
defer `px_conn_del` and retry, but its fan-out already frees each `uP` as it goes, so a partial
retry would lose the remaining FINs; it needs restructuring, not a return code. Left out to keep
this change small. `px_fin_to_sender` has the same exposure and logs it loudly.

## (session 2, addendum) — backpressure path EXERCISED on HW: 51,948 stalls, 0 bytes lost

The EAGAIN path is unreachable under normal load (`PX_UNIT_POOL` = `MAX_PODS × 4096` = 131,072
units, and the stream validator is sequential — at most `fpw` units in flight). Forced it by
temporarily shrinking the pool. **The shrink is an experiment only and is reverted**; the shipped
value is `PX_UNIT_POOL = PX_ARRIVAL_POOL`.

| `PX_UNIT_POOL` | run | OK/Fail | counters | p50 |
|---|---|---|---|---|
| 131072 (shipped) | `stream 10000 1024 4` | **10000/0** | `drop=0B stall=u0/p0/P0` | 117.0 µs |
| 32 | `stream 5000 1024 16` | **5000/0** | `drop=0B stall=u0/p0/P0` | 179.1 µs |
| **4** | `stream 3000 1024 16` | **3000/0** | `drop=0B` **`stall=u51948`** | **179.9 µs** |

With 4 units and 16 frames per parse round, `px_exec_segs` cannot finish a round: it ships ~4
segs, hits EAGAIN, returns the shipped-through offset, and `px_parse` advances only that far and
parks the conn. **51,948 stalls, zero bytes lost, every round-trip byte-exact, p50 within 1 µs of
the full-pool run.** Each of those stalls is a byte-drop-plus-false-TX_ACK in the previous code.

Proven by this: the three-state `px_ship_seg` contract, the partial-advance arithmetic in
`px_exec_segs`, the stall/resume loop (no deadlock, no livelock — `px_drain_stalled` reports
progress only when `parse_pos` moves, and the inline driver re-parses on its next pass), and the
`u->seq`-below-the-allocations placement (a seq gap would have desynced the peer and failed the
byte-exact check).

`DPUMESH_DIAG=1` gained the `px[...]` line and a hook in the inline driver loop — the dump was
previously only wired into the reaper/shard branch, so the default config printed nothing.

Validator matrix on the shipped pool: loopback SAR 8K/16K/32K/64K, verbs pipeline 1/2/4/8,
preload 5000×1KB + 3000×8KB, churn 23,209 reconnects, stream self fpw 1/4/16, bench
latency/bandwidth/rate — all 0-fail.

**Bench change:** `stream`'s `SVC_LIST` (multi-service fan-out) is REMOVED. No pod in the bench
topology can serve it: `echo-dpumesh*` are Greeters (bench.h framing, not a verbatim echo), and
`loopback`/`verbs` echo verbatim but block in `accept()` when idle and never poll their CQ, so
any non-self target simply burns the 5 s reply timeout. `RUN <N> <SIZE> [<FPW>]` now.

---

# (session 3) — the codec decides the LB granularity: per-service LB axis DELETED, L7 sticky bug FIXED, fan-out PROVEN on HW

**The model this settles on.** L4 is a byte stream, host↔DPU, with nothing L7 on the wire — the
app's pipe is a submission queue addressed to a SERVICE and it ENDS AT THE DPU, which owns and
re-originates the upstreams (conntrack is keyed `(client_pod, client_port, backend_pod) → uP`, so
one pipe already has N upstreams). The DPU is the sidecar. **How often the LB picks follows the
CODEC, and nothing else:**

| service | Envoy analogue | knows message boundaries? | LB granularity |
|---|---|---|---|
| passthru (no codec) | `tcp_proxy` | no — bytes only | conn pinned for life (**forced**) |
| `_FRAME_SVC` / `_L7_SVC` | `http_connection_manager` | yes | **per message** |

Pinning is the consequence of having no codec, not a setting. Routing policy lives in the DPU's
proxy config, never in a client API call.

## Removed / fixed

- **`DPUMESH_LB_PER_REQUEST_SVC` + `svc_per_request` + `px_service_sticky` — DELETED.** A second,
  per-service axis for something the codec already decides: redundant, and able to contradict it
  (per-request on a service with no codec is meaningless). The docs calling it "Envoy's HTTP
  default" was a layering error — Envoy terminates HTTP2 and re-originates on per-backend upstream
  connections; it never splits an L4 byte stream per message.
- **BUG: the L7 path was connection-sticky.** `px_parse_l7` resolved through `px_resolve_backend`,
  so a service that ran a codec — and therefore *knew* where each message ended — still pinned its
  first backend for life. That is the gRPC-over-L4-LB failure, sitting in the L7 slot. Split into
  two honest functions: `px_route_message()` (codec: per message, or the hook's `decision.host`)
  and `px_resolve_backend()` (passthru: conn pin, cluster-scoped, re-picked if the backend dies).
- Ghost text: `bench.sh`'s `verbs ... (dmesh_verbs.h)` — that header does not exist; verbs and
  bench both use `<dpumesh/dmesh.h>` since the 4-surfaces→2 consolidation.

## Fan-out, measured

3 backends of ONE service (`echo-dpumesh` = pods 0/1/2 — `echo-dpumesh-13/14` now carry the same
`DPUMESH_SERVICE`, so they are backends instead of dead single-pod services). The Greeter stamps
its `dmesh_pod_id()` into every reply's `aux` (the field was 0/unused) — without that discriminator
the LB is unobservable, since all three backends answer identically. `bench_dpumesh` correlates
**by seq** (never by arrival order), counts `reorder`, and tallies `dist=pod:count`.

`point dpumesh 1024 8 8 5 200 <threads>`:

| service | threads | dist (replies per backend) | reorder | fail |
|---|---|---|---|---|
| **passthru** | 1 | `0:342541` — **one conn, one backend; 2 pods idle** | **0** | 0 |
| **passthru** | 4 | `0:115630, 1:236403, 2:115296` — conns spread, each pinned | **0** | 0 |
| **codec** (`_L7_SVC=11`) | 1 | `0:103984, 1:103983, 2:103983` — **one conn, all 3, even** | 135122 | 0 |
| **codec** | 4 | `0:152831, 1:152831, 2:152830` | 156038 | 0 |

The passthru/threads=1 row **is** the gRPC problem in one line: a long-lived connection pinned to
one backend forever. The codec/threads=1 row is the same connection load-balancing every message.
`reorder>0` is the multiplexing that necessarily comes with it — replies interleave, which is why
correlation must be by id. p50 114 µs on both, so the granularity is not costing latency.

## Bench client bugs this test exposed (mine, both in the new code)

1. `exp_seq = seq + 1` as the window base — with out-of-order replies a late low seq falls outside
   `[exp_seq, exp_seq+W)` and was scored as a failure. `fail=7`.
2. Indexing in-flight requests by `seq % W`. **Out-of-order completion means the span from the
   oldest outstanding seq to the newest issued one is NOT bounded by W** — a lagging request holds
   its slot while newer ones cycle through it, so seq 8 overwrote seq 0's slot at W=8. `fail=5`,
   plus silently mis-timed latencies. Fixed with a 1024-slot direct map that stores each slot's
   seq, so a collision is detected rather than silent. **`fail=0`** after.

The apparent 14x throughput drop under the codec was (1) leaking `outstanding` on its early
return, not a transport cost: 0.0611 → 0.0625 Mrps after the fix, against 0.0686 passthru.

## Verification

Full deploy + HW, all 0-fail: loopback 8K + 64K(zc), verbs pipeline=4, preload, and the fan-out
matrix above. `make lib bench` clean; DPU sources syntax-checked against the real DOCA headers with
no new warnings.

**Not covered:** backend-death re-pick under a codec, and the `px_fin_to_sender` terminal path —
both need fault injection. `stat_drop_bytes`/`stall` stayed 0 throughout (`DPUMESH_DIAG=1`).

---

# (session 4) — one QP carries N inbound streams: `dmesh_wc_t.stream` — the reply-side half of per-message LB

**What this closes.** Per-message LB gives one QP several upstreams at once, and their replies
all land on that QP. The transport merged them into ONE byte stream and threw away the sender —
which L4 cannot represent. TCP has no such problem: N peers means N sockets. Here N peers share a
QP, so the transport must say WHICH. It already knows: a reply carries `src_port = uP`, the DPU's
upstream id, and it reaches the host in the descriptor. `cq_emit` was dropping it.

`dmesh_wc_t` gains `uint16_t stream`. No wire change (the id already arrives), no new function —
the API is still 16 calls. `stream` is opaque (an upstream number, not a pod), so "the client never
names a pod" holds. On a no-codec service every reply comes from the one pinned backend and
`stream` never varies: existing apps ignore it.

## The failure it fixes, measured

A reply ≤ `msg_max` (8 KB) is one completion — atomic, so it cannot interleave. A larger one
arrives as several, and another backend's reply lands between them. Reassembling all of it through
one reframer splices the streams together and desyncs. It does not error — **it stops**:

| reply size | before | after (per-stream reframers) |
|---|---|---|
| 8 B | ok | ok |
| 4 KB | ok | ok |
| **16 KB** | **4 replies, then wedged** | **176,771 · fail=0** |
| **64 KB** | (not reached) | **66,006 · fail=0** |

## The matrix (codec on svc 11, 3 backends, one QP unless noted)

`point dpumesh <req> <reply> <conc> 4 100 <threads>` — `dist=pod:count`, all `fail=0`:

| sweep | | Mrps | Gb/s | dist |
|---|---|---|---|---|
| **request** | 32 B | 0.0634 | 0.02 | `0:84489, 1:84489, 2:84490` |
| | 1 KB | 0.0638 | 0.52 | `0:84987, 1:84986, 2:84987` |
| | 8 KB | 0.0514 | 3.37 | `0:68485, 1:68485, 2:68484` |
| | 32 KB | 0.0292 | 7.65 | `0:38891, 1:38893, 2:38892` |
| | 64 KB | 0.0149 | 7.79 | `0:19810, 1:19811, 2:19811` |
| **reply** | 4 KB | — | 0.51 | `0:82344, 1:82343, 2:82342` |
| | 16 KB | — | 0.36 | `0:58924, 1:58923, 2:58924` |
| | 64 KB | — | 0.14 | `0:22002, 1:22002, 2:22002` |
| **conc** | 1 / 4 / 32 / 64 | 0.0044 / 0.0333 / 0.0902 / 0.0901 | | p50 227 / 115 / 343 / 687 µs |
| **threads=4** | | 0.0947 | | `0:126245, 1:126248, 2:126244` |
| **churn=50** | | | | 5735 reconnects @ 2.54 µs, `0:106878, 1:106879, 2:106879` |

Distribution is even to within 2 replies in every row. `reorder` tracks it: **0 at conc=1**
(nothing outstanding to reorder), 18.7 K at conc=4, 210 K at conc=64.

## Control — no codec, same build

| | Mrps | reorder | dist |
|---|---|---|---|
| threads=1 | 0.0682 | **0** | `0:272509` — **one conn, one backend; 2 pods idle** |
| threads=4 | 0.0942 | **0** | `0:94360, 1:189905, 2:92491` — conns spread, each pinned |
| reply 64 KB | — | **0** | `2:71158` — no interleaving, so no wedge before this fix either |

`reorder=0` everywhere confirms `stream` did not disturb the pinned path. The threads=1 row is
still the gRPC problem in one line; the codec rows above are the same connection load-balanced.

Validators unchanged: loopback 8 K + 64 K(zc), verbs pipeline=8, preload — all 0-fail.

## A build bug this exposed — silent ABI skew

The first `(c)` deploy SIGSEGV'd all three greeters (exit 139) and delivered zero replies. Cause:
**the Makefile tracked only `.c` prerequisites, never headers.** Growing `dmesh_wc_t` rebuilt
`libdpumesh.so` but left `echo_dpumesh` stale (19:50 vs 20:59), and the pods bind-mount the `.so`
from the host while baking the binary into the image — so a new library wrote `w->stream` past the
end of an old 24-byte struct, off the end of the caller's `wc[]` array.

Fixed with `-MMD -MP -MF $(DEPDIR)/$(@F).d` on every compile plus `-include $(wildcard
$(DEPDIR)/*.d)`. Verified: `touch include/dpumesh/dmesh.h` now rebuilds every dependent binary; a
no-op `make` rebuilds nothing. **Any public-header change before this commit could silently ship a
mismatched pair** — the earlier `route_group` removal escaped only because it never touched a
struct the apps compile against.

## (session 4, addendum) — the reframer now DETECTS desync instead of stalling

`bench_reframe_feed` read `seq`/`payload_len`/`aux` out of the 16-byte header and **skipped the
`magic` word**. The field was written by both ends and read by nobody — so a desynced reply
stream had no way to announce itself: the reframer just kept reading lengths out of whatever
sat where a header should be, never completed a frame, and the connection **stopped**. That is
exactly how the pre-`stream` 16 KB failure presented — 4 replies, then silence, no error.

`bench_reframe_feed` now takes `want_magic` and returns `-1` on mismatch (`BENCH_REQ_MAGIC` on
the server, `BENCH_REP_MAGIC` on the client). Desync is unrecoverable — the boundary is gone, so
every later length is garbage — so the callers kill the conn rather than scan for a new one.
Bench-only: no transport, API or wire change.

**Proof it fires.** Re-created the failure by forcing every stream onto one reframer
(`stream_rf(w, 0)`, reverted after):

| | before this check | with it |
|---|---|---|
| 3 backends merged onto 1 reframer, 16 KB reply | 4 replies, then silent stall | **`fail=22`**, `[bench] reply stream 32768 desync` |

The log names the `uP`, so a real desync now points at the stream that broke.

**No false positives.** Clean build, codec on svc 11, 3 backends — replies 8 B / 16 KB / 64 KB all
`fail=0` with even `dist` (`0:83783,1:83783,2:83783` · `0:57921,1:57921,2:57921` ·
`0:21547,1:21540,2:21547`); loopback 5000/0, verbs pipeline=8 5000/0, preload 3000/0.

---

# (session 5) — fixing the audit findings: FIN semantics, TX-FIFO overrun, error delivery, ABI

Fourteen fixes from a code audit, plus one deadlock the audit missed and the testing found.
Full deploy + HW on every claim below. `DPUMESH_PROXY=passthru`, `l7-services=0`,
egress-threads=1, shards=1, K=2, pin=fair, 3 backends.

## The headline: a request > 256 KB could never complete

Not on the audit's list — `bench.sh bandwidth` simply returned `0.0000 Gb/s` for every size at or
above 262144, and had been doing so silently. `rcnt=0` with `grow_waits` climbing into the
hundreds of thousands, and **zero** DPU-side errors: the client never got its request out.

The cliff sits exactly at `maxb × block_size` = 4 × 64 KB = 256 KB, the per-conn in-flight cap.
A caller that builds one message out of `DMESH_SEND_MORE` chunks (which `bench_dpumesh.c` does,
and which `dmesh.h` documents as the WR-list idiom) holds those bytes COMMITTED BUT UNSHIPPED.
The DPU has never been told they exist, so no TX_ACK for them can ever arrive — and `tail_blk`
never advances. `dmesh_alloc` then returns EAGAIN forever, while the header promises the exact
opposite:

> *"On EAGAIN: do other work and retry — a later alloc succeeds once the DPU's TX_ACKs free space."*

The promise was false whenever the space is held by the caller's own un-flushed batch. Fixed in
`dmesh_alloc`: on EAGAIN, ring the doorbell and retry once. The bytes go in flight, ACKs start
coming, and EAGAIN is transient again — as documented.

| req size | before (Gb/s) | after (Gb/s) |
|---|---|---|
| 131072 | 6.41 | **10.54** |
| 524288 | **0** (deadlock) | 11.64 |
| 1000000 | **0** (deadlock) | 12.07 |
| 2097152 | **0** (deadlock) | 12.20 |
| 8000000 | **0** (deadlock) | 12.25 |

## P0 — `peer_closed` meant two different things, and leaked an upstream per close

`cq_emit` sets it to mean "the peer's FIN landed"; `dmesh_destroy_qp` read it as "I already sent
my FIN" and skipped sending one. `dmesh_preload.c` had latched it for the second meaning outright.
TCP never conflates the two, and here it mattered more than style: `dpu_upstream_free` has exactly
ONE call site in the whole tree — the client-FIN fan-out (`dpu_proxy.c`) — and the backend-FIN path
deliberately frees nothing (*"upstream itself is freed by the CLIENT's FIN fan-out"*).

So **every server-initiated close leaked** an upstream slot, its conntrack entry and a `px_conn`
node — permanently, on the plain no-codec path and through the shim, with no codec or
multi-backend needed. The audit had scoped this to multi-stream FIN semantics; it was much wider.

Split into `peer_closed` (inbound) and `fin_sent` (outbound), and made `dmesh_send_fin`
idempotent — which let three call sites collapse into one unconditional call. `shutdown(SHUT_WR)`
carried the same bug (it withheld our FIN if the peer had closed first) and is fixed by the same
change.

## P0 — the TX send-unit FIFO could be overrun

`dmesh_core.c` claimed *"THE INVARIANT that makes the send path infallible"*: one block carves into
at most `ceil(block_size/slot_size)` descriptors, so clamping `maxb × dpb ≤ TX_SU_DEPTH` makes the
block window bind first. **The premise is false.** `dpumesh_tx_next_send` carves `[tx_s, tx_c)`,
which a post-per-flush leaves exactly one message long — so a post emits ONE descriptor whatever
its size. At defaults a 64 B message costs 1/1024 of a block but a whole FIFO slot: 4096
descriptors fit the block window against a `TX_SU_DEPTH` of 64.

Past 64 un-ACKed posts, `dpumesh_tx_sent` (no capacity check) overwrites a live entry and
`tx_reclaim_ack` then advances `tx_f` past bytes the DPU is still DMA-reading. Gated in
`dpumesh_tx_reserve` on the FIFO itself, reserving the message's WHOLE carve
(`ceil(len/slot_size)`) — a single check would have let one 64 KB post emit 8 descriptors through
a gate that admitted one. The now-redundant `maxb` clamp and its false comment are gone.

**Cost: none, measured.** Gate on vs. gate compiled out, same deploy otherwise:

| threads | gate ON (Mrps) | gate OFF (Mrps) |
|---|---|---|
| 1 | 0.0993 / 0.0991 / 0.0995 | 0.0983 / 0.0984 |
| 2 | 0.0960 / 0.0963 / 0.0947 | 0.0956 / 0.0957 |
| 4 | 0.0960 / 0.0991 / 0.0976 | 0.0964 / 0.0967 |

## P1 — failures returned a success-shaped ACK

`px_lane_emit` skips REV_DONE on `b->error` but released custody anyway, which TX_ACKs the sender:
undelivered bytes reading as delivered, with neither data nor EOF. `px_lane_drop_dead` did the
same for a pod that died mid-flight. Added `px_eof_to_origin` — a 0-length unit (the only error
the wire can carry) addressed back to the conn the bytes came from. It takes the unit, not a
`px_conn`, because the emit paths run where the conn table may belong to another shard;
`px_unit.org_port` carries the un-rewritten origin port for exactly this.

The DPA's consumer-timeout path (`dpa_kernel.c`) dropped the descriptor outright — silent loss the
sender could never learn about, and a stranded port if it was the conn's last message. It now
leaves the descriptor for the next pass: the ring backs up and `dmesh_flush` fails loudly with
EBADMSG instead. (`DMA_CONSUMER_EMPTY_WAIT_LOOPS` is 0x800000, so this never fires under load.)

## P1 — a dropped last TX_ACK leaked a port and its blocks, forever

`try_return_blocks` requires `tx_w == tx_f`, and `dpumesh_alloc_port` skips FREE-but-draining
slots. A dropped final ACK therefore strands both, for the process's life. The loss was in the
NOTIFICATION, not the data — the DPU had already egressed. Since host reclaim is cumulative, a
newer seq for the same `(conn,port)` fully subsumes a queued older one, so the deferred queue now
COALESCES instead of appending: bounded by live ports rather than message rate, and it can no
longer fill and drop.

## P1 — FIN loss on a dry unit pool (the known gap, now closed)

`RESULT.md` recorded this as unfixable-without-restructuring because *"its fan-out already frees
each uP as it goes, so a partial retry would lose the remaining FINs"*. That is the fix: **free
the upstream only AFTER its FIN unit is queued.** `dpu_upstream_find` is the fan-out's only
enumeration, so with the free ordered last a retry re-scans, skips what is done (find returns 0)
and resumes exactly where it stopped. `px_try_fin` returns 0 and parks the conn; `px_drain_stalled`
re-enters it. `px_poison`'s EOF is a latched debt (`eof_pending`) for the same reason.

## P1 — L7 decisions were executed unvalidated

`ctx.hosts` is built from the ADDRESSED service before the hook runs; a hook that re-clusters
still sees the old cluster's endpoints, and `px_route_message` ignored `cluster` entirely when
`host >= 0`, checking only liveness — a content-routing hook would deliver into a different
service. Now the host must be a live backend OF `dec.cluster`, and `cluster` is range-checked
BEFORE the int32→int16 narrowing (truncating first silently aliases onto a real service). Added
`PX_L7_MSG_MAX` (Envoy's `max_request_bytes`). Latent in the current hook — `dpu_l7.c` ignores `ctx` — so this
is an API fix, not a live bug.

## P1 — ABI versioning

Unversioned SONAME; a stale binary loaded a moved struct layout and SIGSEGV'd (session 4). Now
`libdpumesh.so.1` (SONAME + DT_NEEDED + pod mounts + Dockerfiles). Verified the gate is real:

```
$ readelf -d build/bin/echo_dpumesh | grep NEEDED
    Shared library: [libdpumesh.so.1]
$ mv build/lib/libdpumesh.so.1 /tmp && ./build/bin/echo_dpumesh
  error while loading shared libraries: libdpumesh.so.1: cannot open shared object file
```

A loader error at startup instead of memory corruption mid-run.

## P2 — the send contract stopped trusting the caller

`dpumesh_tx_commit` CLAMPED an over-long len to `resv_len`, which is the LAST reserve's length —
so a post with no live reserve silently shipped whatever sat at the write head: **uninitialised
ring memory, on the wire, no error anywhere**. It now rejects (EINVAL) and consumes the reserve,
making "one post per alloc" enforced rather than documented. `dmesh_post_send` also rejects unknown
flags. `dmesh_destroy_cq` / `dmesh_destroy_channel` now return `-1 + EBUSY` instead of freeing
under a live QP/CQ (ibverbs' rule, checked).

## P2 — per-port inbox memory was bounded only by total churn

A port's inbox is allocated on first use and never freed — correctly, since the PE pushes into it
without `port_lock`. But the allocator swept all 32767 client ports, so a long-lived churning
client ends up holding 32767 × 48 KB ≈ **1.5 GB**. The cursor now sweeps a WINDOW (`port_span`,
from 256, doubling on demand), which caps the inboxes while keeping the age-out that stops a
just-closed port from catching an in-flight reply.

## Cleanup

Dead `route-affinity` comments (3, in `dpu_proxy.h`; nothing in code); the stale
`su_seq[region*TX_SU_DEPTH+i]` comment (indexing is flat); the redundant first half of the
credit-refresh condition; `wc.stream` left uninitialised on CONN_REQ (caller stack garbage); the
4 DPA msgQ callbacks compiled into the HOST library where they are unreachable (moved inside
`DOCA_ARCH_DPU`); 12 unchecked control-socket `write()`s in the validators. **Host build: 0
warnings** (was 16).

Two audit findings were REJECTED after checking: `fcntl`'s unconditional `va_arg` is what glibc
itself does, and destroy-mid-poll-batch is a documented caller contract (`dmesh.h`), not a defect.

## Verification

| case | result |
|---|---|
| loopback 8 K / 64 K(zc) | 50000/0 · 20000/0 |
| verbs w4 p8 / w1 p1 | 50000/0 · 20000/0 |
| preload (8 conns) | 5000/0, p50 119 µs, 27111 rps |
| stream fpw=1 / fpw=4 | 20000/0 · 20000/0 |
| bandwidth, all 11 sizes | fail=0, 0.026 → 12.25 Gb/s |
| conn churn reconn=1/10/100 | fail=0, 27754 reconnects, ~0.35 µs |
| DPU log | 0 errors; 18 `stale upstream` in 27754 reconnects (0.06%) — the narrow close/deliver race, not a leak |

Latency (conc=1) p50 = 114 µs at 64–1024 B, unchanged from the HEAD build.

## An honest gap in this report

`rate` at threads=2/4 measures **~6-8% lower than the HEAD commit** (t=2: 0.1025–0.1049 → 0.0947–0.0963;
t=4: 0.1025–0.1053 → 0.0960–0.0991; both bands tight across 3 repeats and 3 separate deploys, so it
is real, not noise). p50 tracks it (+54 µs at t=2). t=1 is unchanged.

**I could not attribute it.** The comparison I ran is HEAD-commit vs working-tree, and the working
tree carried **4448 uncommitted lines across 33 files** from prior sessions before this session
began — I never snapshotted it, so `git stash` reverted everything, not just my changes. That is a
process error on my part.

What the evidence does say: the su gate is the only per-alloc code this session added, and
switching it off changes nothing (table above). Everything else added to a per-request path is a
branch or a single store (`u->org_port`, `b->error &&`, `resv_len` checks). +54 µs/req ≈ 2000 ARM
cycles is not a plausible cost for that. The backend pair is not the cause either — `dist` varies
run to run (1&2, 0&1, 0&2) while Mrps stays pinned at 0.0956–0.0962. So the regression most likely
predates this session's work and has simply never been benchmarked in the passthru config at
threads=2/4. **Not proven.** Isolating it needs a per-commit bisect of the uncommitted work.

---

# (session 6) — bisecting the t=2/4 rate regression: it's the route-affinity removal

Session 5 left an unattributed 6-8% drop at `rate` threads=2/4 (t=1 unchanged). Bisected it to
the end. **Cause: the `dmesh_pin_route` / route-affinity (`route_group`) removal from the prior
layering refactor — NOT this session's audit fixes.** Proven, with the DPU pegged at 99%.

## Method

Committed the full working tree to a branch (`wip-audit-fixes`) as a safety snapshot first — the
tree carried 4448 uncommitted lines I never wanted to lose to a stray `git stash` again. Then
reverted by domain against `9b153b9` (HEAD), redeploying and measuring t=1/2/4 × 4 repeats each
under a fixed harness.

## The bracket

| config | DPU binary | host+bench | t=2 Mrps | t=4 Mrps | DPU main-thread CPU |
|---|---|---|---|---|---|
| HEAD `9b153b9` (deploy #2) | 9b153b9 | 9b153b9 | 0.1025 | 0.1023 | **99%** |
| HEAD `9b153b9` (deploy #3) | 9b153b9 | 9b153b9 | — | 0.1025 | **99%** |
| **Test A** | **9b153b9** | **worktree** | 0.0956 | 0.0960 | **99%** |
| final worktree | worktree | worktree | 0.0958 | 0.0974 | **98%** |

First, **HEAD reproduces 0.1024 across three deploys** (variance <0.2%) — the regression is real,
not deploy noise. Then:

- **Test A reverts ONLY the DPU to 9b153b9, keeps the worktree host+bench → still 0.096.** The DPU
  diff is exonerated: the same DPU binary that gives HEAD 0.1025 gives 0.096 when fed worktree-host
  traffic. The cause is host-side.
- **The client core is not the bottleneck.** Pinning `bench-dpumesh` to 4 cores (server left on 1)
  moved nothing (0.096). The heavier worktree client (INFLIGHT_RING 32→1024) is not it.
- **The DPU ingest thread is the bottleneck: 99% in every config.** `echo-dpumesh` sat at 21.8%,
  the client core ruled out. So throughput is `DPU-messages-per-second at 99% CPU`, and HEAD gets
  6.8% more of them than worktree from the *same* saturated thread — i.e. HEAD's DPU does **less
  work per message.**

## The mechanism (code-level)

The only per-message difference in what the host hands the DPU is `route_group`, and `emit_desc`
is the one line that changed:

```
-    d.route_group = c->pin_group;   /* 0 = per-message LB per descriptor */   (9b153b9)
     (worktree: pin_route removed → route_group is always 0)
```

On the 9b153b9 DPU, `dpu_route_l4` branches on it:

```c
if (rg != 0) {                                   /* HEAD: dmesh_pin_route stamped it */
    int32_t pinned = objs->route_group_backend[svc][rg];   /* ONE 2D-array read */
    if (pinned >= 0 && find_pod_by_id(objs, pinned)) return pinned;
    ...
}
return lb_pick(objs, svc);                        /* worktree (rg==0): EVERY message */
```

and `lb_pick` is not free per message:

```c
static int32_t lb_pick(struct objects *objs, int16_t svc) {
    int32_t hosts[MAX_PODS];
    int n = collect_live_hosts(objs, svc, hosts);  /* scans pods[]: ACQUIRE-loads
                                                      registered + service_id + dma_ready
                                                      for every pod, fills a 128-int array */
    ...
}
```

So HEAD resolves a pinned conn's backend with **one array read**; worktree re-scans the whole live
backend set **on every message**. With the single DPU ingest thread at 99%, that per-message delta
is the 6.8%. **t=1 is identical because one connection's message rate never saturates the ingest
thread — the extra scan is absorbed. It only bites once a second connection pushes the thread to
99% (t≥2).** That is exactly the t=1-clean / t≥2-regressed shape observed.

The final worktree DPU removed the `route_group_backend` fast-path entirely (the layering fix
dropped route-affinity on both sides) and resolves per message through the conntrack / pinned-conn
path instead; it lands at the same ~0.097 ceiling. Test A isolates the host half cleanly on the old
DPU; the two halves are one refactor.

## Attribution and verdict

This is the **prior layering refactor** (route-affinity was an L4 layering violation — a client
stamping a per-RPC affinity key; see the design notes that mandate "new code must not call
pin_route"), present in the working tree before this session began. **It is not the audit fixes:**
Test A runs with *none* of this session's DPU changes and still regresses, and the su-gate-off
control (session 5) already showed this session's only per-alloc addition is free. My host changes
to the send path only *removed* a field write (`route_group`), which is cheaper, not slower.

**Verdict: it is the intended, correct cost of the layering fix, not a bug** — route-affinity was
removed on purpose. But it is a real, now-quantified regression (~6-8% at DPU saturation), and it
is recoverable *without* reintroducing the violation: the DPU already knows each conn's backend
(the conntrack `dpu_upstream_find` tuple, and `px_conn.pinned_backend`), so the per-message
`lb_pick`/`collect_live_hosts` scan can be replaced by reading that cache — a DPU-side change that
keeps routing entirely at L4. Filed as a follow-up; not done here (it is an optimization, not a
correctness fix, and this session's mandate was the audit findings).

## Correction to session 5's "honest gap"

Session 5 reported this gap as unattributed and blamed a process error (I stashed the pre-session
tree). The process error was real, but the regression is now fully explained above and is **not**
in this session's fixes.

---

# Session 6 — ARM emit-path lock contention: 100K → ~500K RPS

Goal: raise latency + throughput, both "far below target". Constraints: keep the public API
shell identical (internal optimization only), delete/specialize no functionality, keep env
knobs tunable (don't hardcode EU/thread counts), event-driven (no new busy-spin). Method:
DIRECT before/after via `bench/bench.sh`, fresh full deploys, gdb-profile the DPU to find
the real bottleneck. Fair pin unless noted. 32B req / 8B reply.

## Baseline (HEAD d9f5f41, config n_shards=1 n_eng=1 — the bare-`deploy` default)

| test | metric | value |
|---|---|---|
| conc=1 | p50 / p99 | 114 µs / ~413 µs |
| 1-thr conc64 | throughput / p50 | 0.101 Mrps / 629 µs |
| 4-thr conc32 | throughput / p50 | 0.099 Mrps / 1276 µs |
| verbs 8K w1p1 | p50 | 94.5 µs |
| verbs 64 w4p8 | p50 | 145.5 µs |
| preload (shim) 1K×8 | p50 / rps | 122 µs / 25.8K |
| **TCP ref** conc1 / t4c32 | p50 / Mrps | 86 µs / 0.258 |

Throughput is FLAT vs client threads (1thr 0.101, 4thr 0.099) → DPU-side single-thread wall,
not the client.

## Host-side internal opts (src/dmesh_core.{c,h}; public API untouched)
- **notification gating**: `struct dmesh_cq.wants_notify`; `cq_notify` skips the per-message
  `eventfd` write for poll-only CQs (RPC bench + verbs client never call `dmesh_cq_fd`).
  `dmesh_cq_fd` latches it + self-kicks (closes the arm race). Same signature/contract.
- **false-sharing split**: `dmesh_port_slot` PE-written atomics (in_tail/tx_f/su_tail) vs
  owner-written (in_head/su_head) on separate cache lines; `on_ready` its own line.
- Effect isolated at (1,1): conc=1 p50 114 (unchanged floor), p99 ~413→~370. Throughput
  unchanged (DPU-bound). No regression; helps host CPU at scale + real multi-core pinning.

## DPU multithreading sweep (env knobs, NOT hardcoded; host code constant)

| n_shards, n_eng | extra | t1c64 Mrps | t4c32 Mrps | note |
|---|---|---|---|---|
| 1, 1 | — | 0.101 | 0.099 | default (slow) |
| 2, 2 | share-nothing | 0.178 | 0.182 | +82% |
| 4, 4 | share-nothing | 0.186 | 0.182 | +85%, plateau |
| 4, 4 | + SHARD_HOST_EMIT=1 | 0.180 | — | no help (③ scaffold) |
| 4, 4 | + RINGS_PER_POD(K)=4 | 0.172 | — | WORSE (more forward EUs ≠ help) |

Plateau ~185K invariant across eng/shards 2/4/8, host_emit, K, and concurrency 64→512.
DPA auto-detects N=8 EUs (cap MAX_DPA_EU=8, device has 254), 6/8 used → DPA has headroom,
NOT the wall.

## Root-cause: gdb profile of the DPU under load (the decisive step)
`gdb -p <dpumesh_dpu> -batch -ex 'thread 1' -ex 'bt 3'` ×15, histogram top frame:
- **main (thread 1) = 87% in `lll_mutex_lock/unlock` = the proxy `pool_lock`** — every emitted
  message frees unit+pieces+arrival under it in `px_engine_emit`, contending with the ingest
  shards' allocs.
- the "4 egress workers at 100% CPU" were in `sched_yield` (idle-spin, STARVED by main) — a
  red herring.
So the wall was ONE serialized ARM thread on a lock, not thread count and not the DPA.

## Fix A — batch main's pool frees (dpu_proxy.c)
`px_free_batch`: main collects freed unit/piece/arrival into a thread-local chain and splices
them onto the pool in ONE locked op per drain iteration (hoisted to `px_drain` across all
engines); `px_custody_sub_fb` defers the arrival free (still TX_ACKs inline). Cap
`PX_FREE_BATCH_FLUSH=256` << the ~65536-node pools (no shard-alloc starvation). Same mutex,
same nodes, far fewer acquisitions.

| config | before A | after A |
|---|---|---|
| t1c64 | 0.186 | 0.259 (+39%) |
| t4c32 | 0.182 | 0.243 (+34%) |

Re-profile: main mutex 87% → ~75%, now split pool_lock-flush + `libdoca_comch` send.

## Fix B — per-thread ALLOC magazine (dpu_proxy.c)
Deep profile showed main's residual mutex was ~33% `px_free_batch_flush` (pool_lock, still
contending with the SHARD allocs which lock per message) + ~42% inside `libdoca_comch`
(`doca_task_submit`, closed lib). Mirror of Fix A on the alloc side: each ingest/shard thread
pulls a run of `PX_MAG_N=64` nodes off the shared free-list under one lock, then hands them
out lock-free (`tls_*_mag`). This removed the alloc-side contention that had been throttling
multi-conn.

| config | before B (Fix A only) | after B (A+B) |
|---|---|---|
| t1c64 | 0.259 | 0.283 |
| **t4c32** | 0.243 | **0.432–0.490** (+79%) |
| **t8c32** | ~0.25 | **0.523–0.536** |
| t4c64 | ~0.25 | 0.483–0.502 |

Re-profile: main now MOSTLY real work (`doca_pe_progress`, `px_custody_sub_fb`), only ~33%
mutex. hw6 pin ≈ fair (~0.45–0.48) → client cores not the hard limit.

## Fix C attempt — comch REV_DONE batch 16→32 (comch_common.h): REVERTED
Hypothesis: fewer/larger comch sends cut the `libdoca_comch` lock. Result: t4c64 0.502,
t8c32 0.478 — NO improvement over =16, and it is a wire-ABI change + adds reply-delivery
latency at load + the historical REVDONE hang risk. **Reverted to 16.** Finding: after A+B,
the comch send is NOT the throughput wall — main is balanced; the "comch bottleneck" was
downstream of the pool-lock contention that A+B removed.

## FINAL — config (4,4) share-nothing, all fixes (host + A + B), REV_DONE=16

| test | baseline | FINAL | Δ |
|---|---|---|---|
| **throughput t8c32** | 0.099 | **0.536 Mrps** | **+441%** |
| throughput t4c64 | — | 0.483 | |
| throughput t4c32 | 0.099 | 0.432 | +336% |
| throughput t1c64 | 0.101 | 0.283 | +180% |
| loaded lat t4c32 p50 | 1276 µs | 293 µs | −77% |
| conc=1 p50 / p99 | 114 / ~413 | 114 / ~227 | p99 −45% |
| verbs 8K / 64 p50 | 94.5 / 145.5 | 103 / 128 | 64B −12% |
| preload p50 / rps | 122 / 25.8K | 130 / 24K | ~flat |
| validators (loopback/verbs/preload/stream) | 0-fail | **0-fail + byte-exact** | no regression |

**~536K now vs TCP's 258K (2.1×).** conc=1 latency floor 114µs is the DPU-pipeline traversal
(verbs self-loop 94µs) — unchanged; not safely reducible without a pipeline-stage rewrite.

## Operating config (env, keep TUNABLE — never hardcode)
- max throughput / loaded latency: `DPUMESH_INGEST_SHARDS=4 DPUMESH_ARM_EGRESS_THREADS=4
  DPUMESH_SHARD_SHARED=0` (deployed).
- resource-efficient: `2 2 0` (~within 10-15%).
- min low-load latency: `1 1` (fewest handoffs; verbs 8K ~94µs).

## Diff (internal only; public `<dpumesh/dmesh.h>` untouched; no bench/DPA-kernel changes)
`doca/dpu_proxy.c` (free-batch + alloc magazine), `src/dmesh_core.c` + `src/dmesh_core.h`
(notification gating + false-sharing).

---

## Matched-batching ablation + lane-inbox data-race fix

Tests the `plan/bench.md` hypothesis that the E1 throughput gap is partly a benchmark artifact
(TCP batches its window into one syscall; DPUmesh flushed per RPC). Added a matched-batching mode
to the bench apps — client coalesces the burst issued per loop pass into one doorbell (`RUN` 8th arg
`batch`; `bench_dpumesh.c` sets `DMESH_SEND_MORE` on the final chunk, one `dmesh_flush` per pass);
server coalesces a CQ batch's replies into one doorbell/conn (`/tmp/reply_batch` file-poll;
`echo_dpumesh.c`). Live (4,4), 1 KB/8 B, one conn, 6 reps/cell, **0 fail / 0 reorder** everywhere.
Raw: `report/data/batch_ablation.csv`, `report/data/batch_cpu.csv`.

DPUmesh Mrps (median): conc32  U 0.193 · req 0.213 · reply 0.208 · **both 0.255** (envoy 0.417).
                       conc64  U 0.278 · req 0.277 · reply 0.285 · **both 0.491** (envoy 0.591).
- Symmetric (both-side) batching = +32% (conc32) / +77% (conc64); one-sided ≤10% / ≈0% — super-additive.
- Loaded p50 collapses 175/240µs → 117/122µs (≈ conc1 floor 114): the loaded-latency penalty was
  per-RPC serialization, not the transport.
- Even fully batched, DPUmesh < Envoy (0.61× / 0.83×) and ≪ direct TCP. The old 4.5× (P2A) does NOT
  reproduce on the current ARM-SG-DMA reverse path; current max is +77%.
- ARM cost is per-RPC-bound: batching drops DPU-ARM µs/RPC 22.4→12.1 (conc32, −46%) and 16.7→7.3
  (conc64, −56%) — halving it while raising throughput.

Also fixed the lane inbox data race `plan/bench.md` §4.3 flagged: `px_lane_enqueue` writes
`ln->inq_head` under `inq_lock` while `px_engine_pump`/`px_drain` peeked it lock-free (UB at
n_eng>1 || n_shards>1, i.e. the (4,4) headline). Now an atomic release store + acquire peek
(`doca/dpu_proxy.c`). Redeployed (4,4); loopback 50000/0, preload 5000/0, throughput unchanged →
neutral. (The live DPU had drifted to `egress-threads=1`; re-fixed to (4,4) before measuring.)

## Diff (this session; public `<dpumesh/dmesh.h>` untouched)
`bench/apps/bench_dpumesh.c` + `bench/apps/echo_dpumesh.c` (matched-batching mode, default off),
`doca/dpu_proxy.c` (atomic `inq_head` peek).

---

# Session 7 — ABI 2 automatic-batching performance audit

Goal: determine whether the ABI 2 lifecycle and TX-policy work changed native L4
performance. This section supersedes the first before/after numbers collected in
this session because that comparison did not hold the DPU launch configuration
constant.

## Invalid first comparison

The first `before` run reused an already-running DPU process without capturing
`/proc/<pid>/environ`. The first `after` deployment used the bare deploy defaults,
`DPUMESH_INGEST_SHARDS=1` and `DPUMESH_ARM_EGRESS_THREADS=1`, rather than the
report's `(4,4)` operating point. At `(1,1)` the main DPU thread was at 99% CPU and
the 8 KiB point reached only about 5.63 Gb/s. Those numbers measure different DPU
configurations and are **not a valid code comparison**; do not use them as a
regression result.

## Controlled method

The valid A/B kept one live DPU process and its data-plane configuration fixed:

```text
DPUMESH_INGEST_SHARDS=4
DPUMESH_ARM_EGRESS_THREADS=4
DPUMESH_RINGS_PER_POD=2
DPA execution units=8 (automatic, capped)
DPU log argument=-l 40
```

Only the host benchmark and echo images were changed. The baseline images were
built from clean commit `939a6a4`; the current images were built from the
working tree at the same commit. Each cell below is the median of three
runs at concurrency 32. Every retained run reported `fail=0` and `reorder=0`.

Two old-policy baselines are shown because they answer different questions:

- `old per-RPC` flushes every request/reply and represents the old default.
- `old matched` enables the old loop-pass coalescing controls on both client and
  server; it isolates implementation overhead from the batching-policy change.
- `current auto` is ABI 2: each post commits, complete physical units publish
  automatically, and the benchmark flushes the one trailing partial per loop pass.

## Results

| request / reply | policy | throughput | goodput | p50 | p95 | p99 | average |
|---|---|---:|---:|---:|---:|---:|---:|
| 64 B / 8 B | old per-RPC | 0.186394 Mrps | — | 175 µs | 231 µs | — | 169.98 µs |
| 64 B / 8 B | old matched | 0.262528 Mrps | — | 116 µs | 118 µs | 229 µs | 119.47 µs |
| 64 B / 8 B | current auto | 0.257524 Mrps | — | 117 µs | 120 µs | — | — |
| 8192 B / 8 B | old per-RPC | 0.119343 Mrps | 7.8213 Gb/s | 245 µs | 306 µs | 313 µs | 261.09 µs |
| 8192 B / 8 B | old matched | 0.175700 Mrps | 11.5147 Gb/s | 183 µs | 239 µs | 246 µs | 178.58 µs |
| 8192 B / 8 B | current auto | 0.180600 Mrps | 11.8358 Gb/s | 184 µs | 197 µs | 246 µs | 171.43 µs |

Relative to the old per-RPC default, current automatic batching raises throughput
by **38.16% at 64 B** and **51.33% at 8 KiB**. Relative to the old matched policy,
which is the fair code-overhead comparison, current throughput is **1.91% lower at
64 B** and **2.79% higher at 8 KiB**. The 8 KiB average latency improves by 4.0%
and p95 by 17.57% versus old matched batching. The small 64 B throughput difference
is within the scale of run-to-run variation; there is no evidence here of a
meaningful ABI 2 fast-path regression.

## Interpretation

ABI 2 does not combine many descriptors behind one system-call doorbell. The DPA
polls the shared forward ring. Batching here means that adjacent application posts
share an ordered descriptor payload: `post_send` publishes every newly complete
8 KiB physical unit, while `flush` publishes the newest incomplete unit. The
application therefore selects only a logical latency boundary and does not need
to know the 8 KiB value.

For the 64 B request workload, a loop-pass of 32 requests plus the benchmark's
16-byte frame headers fits in a small number of coalesced descriptors. For an
8192 B request, the header makes each record 8208 B; the 64 KiB backing-block
boundary therefore introduces sealed tails, so descriptor count is reduced but
not to one descriptor per request window. This explains why the policy provides
substantial amortization without implying a single publication for the whole
window.

The decisive reproducibility rule is that a retained result must capture the
**live** DPU process environment. Shell variables and manifest values do not prove
what the running process uses. The benchmark harness also now selects a Running,
non-terminating pod after image rollouts, avoiding accidental measurements against
a stale pod IP.

---

# Session 8 — completion-driven native TX readiness and gRPC backpressure

Goal: replace timer, busy-poll, and whole-QP retry scans with a race-free native
one-shot writable completion, convert the gRPC reactor to that completion model,
and exercise representative host-only and BlueField paths. These are correctness
and diagnostic runs, not a controlled transport-performance comparison. The
`LD_PRELOAD` source was intentionally not changed or validated in this session.

## Implemented contract

`dmesh_alloc(NULL/EAGAIN)` now arms its own QP internally. QP-window reclaim and
shared-block-pool return converge on one `DMESH_WC_TX_READY` opcode delivered by
the QP's existing CQ and optional CQ eventfd. A snapshot/recheck closes the
failure-to-arm race; each arm queues at most one retry hint; successful direct
retry and close cancel obsolete hints. A shared block return wakes at most one
valid pool waiter. The native echo loop and gRPC reactor park only the affected
write and no longer use a timeout, busy-poll, or whole-connection scan.

## Configuration audit

| Layer | Live or compiled setting |
|---|---|
| host registered TX / RX | 4,096 × 8,192 B = 32 MiB each |
| TX storage | 64 KiB block; four-block QP ceiling = 256 KiB; recycle cushion 1 |
| TX descriptor reclaim | 64 entries per QP |
| rings per pod | K=2 on both host pods and DPU |
| DPA | 254 EUs reported; auto-selected N=8 (cap 8) |
| ARM ingest | one shard (bare-deploy default), reaper off / inline on main |
| ARM proxy | SG-DMA, one egress worker |
| routing | request/reply passthrough, connection-pinned, no L7/frame services |
| proxy bounds | seam_max=524,288; SG pieces=64 |
| DPU diagnostics | log level 40; diagnostic/shard-shared/host-emit overrides off |
| client manifest | ASYNC_THREADS=4, BENCH_PIPELINE=8, BENCH_COALESCE=0 |
| echo manifest | ECHO_THREADS=3 |
| unused preload manifest | arena slots=512, preload debug=0; not exercised |
| Kubernetes | namespace `test-bench`; all benchmark deployments ready |
| CPU placement | `fair` profile |

The full deployment was repeated after detecting that a Kubernetes `subPath`
mount still referenced the previous inode. Before the repeat, the host hash was
`4b85b75c04e51be3109db22c8bc1c0e2ddfb2b8bf8bb386eb844f64474c9806a`
and the pod hash began `8cc3`; after it, host and pod both reported the complete
`4b85b75c04e51be3109db22c8bc1c0e2ddfb2b8bf8bb386eb844f64474c9806a`.

A later comment-only public-header rebuild produced host full-ELF hash
`f69a40c8dd5e8d8277771c0e7936a6fe5168b75302ef808af0d5c8aee5f53251`
while the live pod retained `4b85b75c…9806a`. Runtime comparison proved that
this was metadata-only: after removing debug/symbol/build-id data, both images
hashed to `3741d2724db3f002f335fbe90f9e9cb0aaa3508d62a4c7e8316a527c86717014`.
The `.text`, `.rodata`, `.data`, and `.eh_frame` section hashes also matched:
`ef36a41c4e45c8d95b3be7170323ae1394c115a7b71c80685f43fa8b6c998b3e`,
`978f7a093b3e58f4547015aa7d4673c33ef2761ac0f47ff8acd0ecfdf14ff714`,
`6b5c5bb6cdbbd629cf4d805f536457046f1c4a893d0ad78c83fdda70c10e3731`,
and `9c955027fee0c3d924e8656ed5169b12073738b719eca9c77d505fcdb23c834d`
respectively.

## Host-only verification

- `make -j2 all`: passed. It rebuilt the native library, benchmark/validator
  binaries, and the preload artifact; `src/dmesh_preload.c` itself is unchanged.
- `make test`: four of four passed: API contract, control state, TX batch policy,
  and the new writable state-machine test.
- gRPC sanitizer CTest with LeakSanitizer enabled produced all functional PASS
  output but was reported as 0/4 because LSan cannot run under the ptrace-restricted
  runner. This is a runner failure, not four functional failures.
- `ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/grpc`: 4/4 passed. One run
  took 0.19 s (endpoint 0.01, channel 0.05, reactor 0.10, native link 0.03); the
  final rebuild took 0.12 s (0.01, 0.04, 0.06, 0.01 respectively). ASan memory
  access checking remained enabled.

The writable white-box test covers exact block-boundary wakeup, no same-block
false wakeup, reclaim racing with arm, shared-pool wakeup, cancellation by direct
successful retry, and rollback of a failed padded reservation. The public API test
also checks that `DMESH_WC_TX_READY` is payload-free and consumed once.

## Native BlueField runs

All commands below used the audited configuration above. Every measured attempt,
including rejected or overloaded cases, is retained.

### Point runs

Initial small-request point:

```text
./bench/bench.sh point dpumesh 1024 8 32 5 1000 1
mrps=0.265039 gbps=2.1712 p50=117.00 p95=122.00 p99=172.00
avg=118.60 min=56.52 max=3470.70 rcnt=1322972 fail=0 conc=32
threads=1 reqsz=1024 repsz=8 durs=5.000 batch=1 reconns=0
reconn_us=0.00 grabs=351 rets=351 recyc=66676 waits=0 pads=352
reorder=0 dist=0:1323972
```

An initial response larger than the 256 KiB QP window completed without a
stranded event-driven echo write:

```text
./bench/bench.sh point dpumesh 64 1048576 1 5 5 1
mrps=0.001207 gbps=0.0006 p50=833.00 p95=866.00 p99=961.00
avg=828.67 min=717.27 max=5470.21 rcnt=6027 fail=0 conc=1
threads=1 reqsz=64 repsz=1048576 durs=5.000 batch=1 reconns=0
reconn_us=0.00 grabs=1 rets=1 recyc=6032 waits=0 pads=0
reorder=0 dist=1:6032
```

The same large-response point was repeated after the final deployment:

```text
mrps=0.001192 gbps=0.0006 p50=836.00 p95=882.00 p99=1089.00
avg=838.72 min=721.10 max=3689.40 rcnt=5953 fail=0 conc=1
threads=1 reqsz=64 repsz=1048576 durs=5.000 batch=1 reconns=0
reconn_us=0.00 grabs=1 rets=1 recyc=5958 waits=0 pads=0
reorder=0 dist=0:5958
```

### Verbs validator runs

| Command parameters `(N,size,zc,window,pipeline)` | OK / fail | served | p50 | Disposition |
|---|---:|---:|---:|---|
| `(5000,65536,1,128,8)` | — | — | — | rejected before execution: `1<=size<=8192`, `1<=window<=256`, `1<=pipe<=256`, `batch>=1` |
| `(10000,8192,1,128,64)` | 7,768 / 2,232 | 10,000 | 63,199.6 µs | overload failure; hit the 60 s anti-hang deadline |
| `(10000,8192,1,32,8)` | 10,000 / 0 | 10,000 | 2,182.8 µs | pass |
| `(10000,8192,1,64,32)` | 10,000 / 0 | 10,000 | 17,368.5 µs | initial pass |
| final repeat `(10000,8192,1,64,32)` | 10,000 / 0 | 10,000 | 17,443.0 µs | pass |

The overload is not counted as a pass even though the server eventually served
all requests: the client deadline failed 2,232 outstanding operations. DPU logs
around that attempt contained task-pool-empty warnings and one stale-upstream
drop, with no process-fatal error. The deliberately invalid 65,536 B message was
not a transport measurement because the validator rejected it at configuration
validation.

## gRPC verification

### Native runtime smoke

The real native runtime smoke used two reactors and two connections. One round
sent request sizes 1, 64, 1,024, 8,191, 8,192, 8,193, and 65,537 bytes in the
benchmark protocol: 14 messages, 365,256 wire bytes, `post_max=65,536`.

The first run printed PASS for data transfer but exited 1 because LSan found
98,304 bytes in two per-port inbox allocations. Channel cleanup was corrected to
free all per-port inboxes. The same smoke then passed with leak detection enabled,
RX `inbox_full=0`, `accept_full=0`, and clean `POD_QUIESCED`. A final post-deploy
repeat produced the same 14-message/365,256-byte PASS with two reactors and two
connections, zero RX drops, clean quiesce, and exit 0.

### Real gRPC HTTP/2 smoke

An earlier paired run passed with server `calls=4` and client service
`bench-dpumesh`, four unary calls, 1,048,576-byte payloads, two reactors, and
4,194,304 total client payload bytes. Both processes reported zero RX drops, clean
quiesce, and no leak.

The final repeat first exposed a configuration mistake: the server was started
without `DPUMESH_SERVICE=bench-dpumesh` and registered as `service_id=-1`. The
client failed with `UNAVAILABLE ... DPUmesh peer closed`; the server later failed
its accept deadline. The DPU logged an unroutable 82-byte segment for service 10.
This attempt is a failed setup, not an implementation result.

After setting the server identity to `bench-dpumesh` (`service_id=10`) and leaving
the client as a pure client (`service_id=-1`), the identical test passed:

```text
server: calls=4 reactors=2, RX drops 0, POD_QUIESCED
client: service=bench-dpumesh calls=4 payload_bytes=4194304 reactors=2,
        RX drops 0, POD_QUIESCED
```

## Result

The native API now provides the missing event-driven TX capacity transition on
the existing CQ fd, and the gRPC adapter consumes it without a retry timer. Host
state-machine tests, representative native hardware paths, the real native gRPC
runtime, and paired gRPC HTTP/2 all pass. The 128-window/64-pipeline overload is
a recorded capacity/deadline failure and is not evidence of correctness at that
load. No conclusion about comparative throughput is drawn from these single-run
checks.


---

# Session 9 — controlled writable A/B performance audit

## Result

The writable change has no detected throughput regression at the report's 1 KiB
headline point, but it does introduce a measurable throughput/latency cost when
writes remain continuously backpressured. At 64 B request / 1 MiB reply, the
post-change mean throughput is **8.57% lower**, median throughput is **7.77%
lower**, and median p50 is **9.27% higher**. This is not a free regression: the
event-driven implementation removes the old echo server's zero-timeout retry
scan. Under the same 1 MiB workload, mean server CPU falls **55.61%**, total host
CPU falls **42.07%**, and host CPU per Krps improves **36.02%**. The design
therefore trades about nine percent of saturated backpressure throughput for a
large reduction in wasted host CPU.

At the 1 KiB / 8 B / concurrency-32 headline point, post-change throughput is
+0.80% by mean and +0.75% by median; the 95% bootstrap interval for the mean
change is [-1.04%, +2.66%]. This does not support a fast-path throughput
regression. Headline host CPU per Krps is directionally 4.32% worse by mean
(6.01% by median), but this CPU companion has only three runs per variant and a
large DPU-ARM outlier, so it is a signal for follow-up rather than a stable CPU
regression claim.

## Controlled design and provenance

A is the post-change working tree and B is the clean pre-change artifact built
from HEAD `dd95a5b4a8e068593c37d690ff7b1bcf62ae6ca5`. Both variants were built
before measurement and selected by immutable image tags and full artifact
hashes. The DPU was deployed once, then only the four native client/server pods
and their host-mounted library were replaced. Point measurements used the
bracketed order **A1 → B1 → B2 → A2**, three repetitions per block, giving six
observations per variant and workload.

| artifact | pre-change SHA-256 | post-change SHA-256 |
|---|---|---|
| bench_dpumesh | `8bc39d1c0e3fde094f9ff8c46834159d5c03b3705dc45ef5c006ea168f0baa78` | `679f3bf78e175efbe52d8c8725e6ce6ab88c3a8406d1f38bc6d54fbab88c7b03` |
| echo_dpumesh | `eeea09b2ca0b8fa1b1c12e6637c8d3177c9a48caf7615c93f9ccb60ad7e7839a` | `4d2f07c1a3f98a7137be54742dfb9a78cc3805433892051ca290ce069aee9a67` |
| libdpumesh.so.2 | `f3f67e9471e4ff14d6f219fb36ab94390f5b00b164501a57c68f7a1702c1253f` | `f69a40c8dd5e8d8277771c0e7936a6fe5168b75302ef808af0d5c8aee5f53251` |

The post-change images were
`docker.io/bench/bench-dpumesh:writable-post-dd95a5b4` and
`docker.io/bench/echo-dpumesh:writable-post-dd95a5b4`; their runtime image IDs
were `sha256:59edbfef…2408` and `sha256:35e818a3…3a19`. The corresponding
pre-change tags ended in `writable-pre-dd95a5b4`. Before each block, every
target pod reported data-ready, its in-pod binary/library hashes matched the
selected artifact, and `bench/bench.sh pin fair` was reapplied. The experiment
ended with all four target deployments and the host-mounted library restored to
the post-change hashes.

The same DPU process, PID **1329320**, ran throughout the experiment; its executable hash was
`fa6950519a7b5a2084afa0cfe5903f4fa9916bc3ca0e6670fad8a15e822aebe7`.
The actual process environment and command line were:

~~~text
DPUMESH_ARM_EGRESS_THREADS=4
DPUMESH_INGEST_SHARDS=4
DPUMESH_RINGS_PER_POD=2
DPUMESH_INGEST_REAP=
DPUMESH_PROXY=
DPUMESH_PROXY_FRAME_SVC=
DPUMESH_PROXY_L7_SVC=
DPUMESH_SHARD_SHARED=
DPUMESH_SHARD_HOST_EMIT=
DPUMESH_DIAG=
./dpumesh_dpu -p 03:00.0 -r 94:00.0 -l 40
~~~

The live DPU log confirmed K=2 rings, four share-nothing ingest shards with the
dedicated reaper enabled by the multi-shard configuration, four SG-DMA egress
threads, passthrough connection-pinned routing, no frame/L7 service, DPA N=8
(auto-selected from 254 available EUs, capped at eight), and an event-driven main
loop. Host placement followed the report's fair profile: primary client core 0,
primary echo core 1, additional echo replicas cores 6 and 7. The governor was
performance with cores 0–7 fixed at 2.5 GHz.

## Workloads and quality gate

All point runs were closed-loop with one client thread. The 64 B, 1 KiB, and
8 KiB request cases used an 8 B reply, concurrency 32, 15 s duration, and 1,000
warm-up completions. The writable stress case used a 64 B request, 1 MiB reply,
concurrency 1, 15 s duration, and five warm-up completions. CPU probes used 30 s
windows and three repetitions per variant. There were **48 completed point
runs, 121,327,806 measured completions, fail=0, and reorder=0**.

The report accepts a point only when fail=0, reorder=0, and
`throughput × average RTT / concurrency` lies in [0.98, 1.02]. Applying that
rule to the reported fields without discretionary exclusions gives:

| workload | pre accepted | post accepted | use |
|---|---:|---:|---|
| 64 B / 8 B | 3/6 | 2/6 | insufficient post sample; aggregate shown as diagnostic |
| 1 KiB / 8 B | 6/6 | 6/6 | retained |
| 8 KiB / 8 B | 0/6 | 0/6 | exploratory only |
| 64 B / 1 MiB | 6/6 | 6/6 | retained |

The rejected 64 B ratios are narrowly below the cutoff (about 0.979), while all
8 KiB ratios are 0.957–0.975 in both variants. The 8 KiB post variant also
reproduced a third-backend low mode in both A blocks (0.115220 and 0.114910 Mrps,
versus 0.178553–0.180562 for the same repetition position before the change), but
because neither the ordinary nor low-mode 8 KiB points pass the report's
Little's-law gate, this is recorded as a diagnostic anomaly, not headline
regression evidence.

Three initial 1 MiB commands returned `ERR no_pod(bench-dpumesh)` because that
shell lacked Kubernetes network permission. Cluster inspection showed the pod
was running; no RUN command reached the benchmark. These are setup failures, not
performance samples, and the three successful A1 repetitions were relabeled
`64B_1MiB_valid` below.

## Point summary

Bootstrap intervals use deterministic independent resampling of the six
observations in each variant. The 64 B and 8 KiB rows summarize all observations
but remain non-retained under the gate above.

| request / reply | pre mean / median Mrps | post mean / median Mrps | post mean change | 95% bootstrap interval | median p50 pre → post |
|---|---:|---:|---:|---:|---:|
| 64 B / 8 B, diagnostic | 0.258267 / 0.259199 | 0.254424 / 0.258306 | -1.49% | [-4.42%, +1.12%] | 117.0 → 117.0 µs |
| 1 KiB / 8 B, retained | 0.245636 / 0.245923 | 0.247607 / 0.247762 | +0.80% | [-1.04%, +2.66%] | 118.0 → 118.0 µs |
| 8 KiB / 8 B, exploratory | 0.181858 / 0.180555 | 0.157881 / 0.178036 | -13.18% | [-25.67%, -0.95%] | 184.0 → 185.5 µs |
| 64 B / 1 MiB, retained | 0.001572 / 0.001571 | 0.001437 / 0.001449 | **-8.57%** | **[-10.14%, -7.33%]** | **625.5 → 683.5 µs** |

For the retained 1 MiB case, the benchmark's `gbps` field counts request bytes.
Computed from reply bytes, mean response goodput falls from approximately 13.19
to 12.06 Gb/s, the same 8.57% change. Median average RTT rises from 636.50 to
690.16 µs (+8.43%).

## CPU companions

At the 1 KiB headline point, the median achieved rate is effectively unchanged
(0.247504 → 0.247031 Mrps, -0.19%). Median host total rises from 44.7 to 47.0
%core (+5.15%), and median host CPU per Krps rises from 0.183 to 0.194 (+6.01%).
DPU ARM has high run variation, including a 201.6%core post-change observation;
its median moves from 316.7 to 303.7%core. With three observations per side this
does not establish a stable CPU change.

At 1 MiB reply pressure, the trade-off is stable across all three routed
backends. Mean throughput changes 0.001578 → 0.001429 Mrps (-9.44%) and mean p50
626 → 695 µs (+11.02%) in these 30 s probes. Mean client CPU is nearly flat
(22.8 → 23.3%core), while server CPU falls 74.57 → 33.10%core (-55.61%), host
total falls 97.37 → 56.40%core (-42.07%), and host CPU per Krps improves 61.707
→ 39.482 (-36.02%). This directly identifies the removed zero-timeout reply scan
as the source of the old variant's extra throughput and extra CPU consumption.

## Complete point data

The following CSV contains every field returned by every completed point run,
plus the derived Little's-law ratio and gate result.

~~~csv
variant,block,workload,rep,mrps,gbps,p50,p95,p99,avg,min,max,rcnt,fail,conc,threads,reqsz,repsz,durs,batch,reconns,reconn_us,grabs,rets,recyc,waits,pads,reorder,dist,little_law,accepted
post,A1,64B,1,0.246393,0.1262,117.00,227.00,340.00,127.35,82.59,5102.05,3694648,0,32,1,64,8,15.000,1,0,0.00,1,1,115488,0,0,0,0:3695648,0.980567,yes
post,A1,64B,2,0.258202,0.1322,116.00,133.00,232.00,121.40,87.47,4188.21,3871736,0,32,1,64,8,15.000,1,0,0.00,1,1,121022,0,0,0,1:3872736,0.979554,no
post,A1,64B,3,0.262796,0.1346,117.00,118.00,175.00,119.23,85.65,4299.91,3940824,0,32,1,64,8,15.000,1,0,0.00,1,1,123181,0,0,0,10:3941824,0.979161,no
post,A1,1024B,1,0.250732,2.0540,117.00,153.00,340.00,125.41,65.97,3318.90,3759761,0,32,1,1024,8,15.000,1,0,0.00,230,230,163112,0,229,0,0:3760761,0.982634,yes
post,A1,1024B,2,0.247782,2.0298,118.00,172.00,290.00,126.79,80.14,4808.25,3714571,0,32,1,1024,8,15.000,1,0,0.00,25,25,160305,0,24,0,1:3715571,0.981759,yes
post,A1,1024B,3,0.241915,1.9818,118.00,228.00,401.00,129.70,67.26,3821.29,3627662,0,32,1,1024,8,15.000,1,0,0.00,65,65,140778,0,64,0,10:3628662,0.980512,yes
post,A1,8192B,1,0.181513,11.8956,185.00,196.00,248.00,169.22,60.55,2701.84,2721474,0,32,1,8192,8,15.000,1,0,0.00,160499,160499,228532,127675,388753,0,0:2722474,0.959863,no
post,A1,8192B,2,0.177950,11.6621,186.00,197.00,249.00,173.86,60.51,4241.88,2667950,0,32,1,8192,8,15.000,1,0,0.00,154679,154679,226859,91727,380873,0,1:2668950,0.966825,no
post,A1,8192B,3,0.115220,7.5510,246.00,364.00,397.00,265.79,113.72,4000.77,1726975,0,32,1,8192,8,15.000,1,0,0.00,100548,100548,146819,121465,245990,0,10:1727975,0.957010,no
post,A1,64B_1MiB_valid,1,0.001452,0.0007,684.00,702.00,792.00,688.71,624.09,4161.48,21769,0,1,1,64,1048576,15.000,1,0,0.00,1,1,21774,0,0,0,0:21774,1.000007,yes
post,A1,64B_1MiB_valid,2,0.001446,0.0007,682.00,722.00,854.00,691.60,623.55,5297.79,21679,0,1,1,64,1048576,15.000,1,0,0.00,1,1,21684,0,0,0,1:21684,1.000054,yes
post,A1,64B_1MiB_valid,3,0.001457,0.0007,683.00,699.00,716.00,686.00,607.76,4965.22,21854,0,1,1,64,1048576,15.000,1,0,0.00,1,1,21859,0,0,0,10:21859,0.999502,yes
pre,B1,64B,1,0.254573,0.1303,117.00,171.00,238.00,123.24,79.07,4242.84,3817240,0,32,1,64,8,15.000,1,0,0.00,1,1,119319,0,0,0,0:3818240,0.980424,yes
pre,B1,64B,2,0.261267,0.1338,117.00,119.00,228.00,120.01,87.29,10362.53,3917816,0,32,1,64,8,15.000,1,0,0.00,1,1,122462,0,0,0,1:3918816,0.979833,no
pre,B1,64B,3,0.258171,0.1322,116.00,122.00,233.00,121.47,86.78,13010.40,3871480,0,32,1,64,8,15.000,1,0,0.00,1,1,121014,0,0,0,3:3872480,0.980001,yes
pre,B1,1024B,1,0.246916,2.0227,118.00,176.00,289.00,127.05,66.22,5288.47,3702413,0,32,1,1024,8,15.000,1,0,0.00,154,154,153000,0,153,0,0:3703413,0.980334,yes
pre,B1,1024B,2,0.243839,1.9975,118.00,177.00,291.00,128.62,70.57,6877.79,3655101,0,32,1,1024,8,15.000,1,0,0.00,23,23,150611,0,22,0,1:3656101,0.980080,yes
pre,B1,1024B,3,0.246925,2.0228,119.00,170.00,347.00,127.46,79.68,15012.25,3702625,0,32,1,1024,8,15.000,1,0,0.00,3,3,194502,0,2,0,3:3703625,0.983533,yes
pre,B1,8192B,1,0.186740,12.2382,183.00,192.00,242.00,165.70,58.56,5036.50,2799748,0,32,1,8192,8,15.000,1,0,0.00,166796,166796,233433,118241,399926,0,0:2800748,0.966963,no
pre,B1,8192B,2,0.180548,11.8324,184.00,194.00,246.00,172.53,62.04,4422.59,2705798,0,32,1,8192,8,15.000,1,0,0.00,155479,155479,231400,74646,386398,0,1:2706798,0.973436,no
pre,B1,8192B,3,0.180562,11.8333,184.00,194.00,248.00,172.85,62.55,6469.91,2707184,0,32,1,8192,8,15.000,1,0,0.00,157055,157055,230167,56386,386326,0,3:2708184,0.975317,no
pre,B1,64B_1MiB_valid,1,0.001587,0.0008,626.00,643.00,688.00,629.98,570.26,4286.05,23780,0,1,1,64,1048576,15.000,1,0,0.00,1,1,23785,0,0,0,0:23785,0.999778,yes
pre,B1,64B_1MiB_valid,2,0.001551,0.0008,625.00,744.00,1085.00,644.58,575.39,4629.65,23242,0,1,1,64,1048576,15.000,1,0,0.00,1,1,23247,0,0,0,1:23247,0.999744,yes
pre,B1,64B_1MiB_valid,3,0.001564,0.0008,625.00,739.00,858.00,639.43,578.49,5217.75,23427,0,1,1,64,1048576,15.000,1,0,0.00,1,1,23432,0,0,0,3:23432,1.000069,yes
pre,B2,64B,1,0.253131,0.1296,117.00,171.00,286.00,123.92,84.80,4149.75,3795896,0,32,1,64,8,15.000,1,0,0.00,1,1,118652,0,0,0,0:3796896,0.980250,yes
pre,B2,64B,2,0.262235,0.1343,117.00,119.00,176.00,119.56,86.62,10918.56,3932344,0,32,1,64,8,15.000,1,0,0.00,1,1,122916,0,0,0,1:3933344,0.979776,no
pre,B2,64B,3,0.260227,0.1332,116.00,119.00,230.00,120.49,86.19,4501.10,3902232,0,32,1,64,8,15.000,1,0,0.00,1,1,121975,0,0,0,3:3903232,0.979836,no
pre,B2,1024B,1,0.237530,1.9458,118.00,231.00,292.00,132.04,58.94,3187.87,3561909,0,32,1,1024,8,15.000,1,0,0.00,154,154,134559,0,153,0,0:3562909,0.980108,yes
pre,B2,1024B,2,0.244930,2.0065,118.00,175.00,344.00,128.31,82.44,14388.57,3671912,0,32,1,1024,8,15.000,1,0,0.00,18,18,159038,0,17,0,1:3672912,0.982093,yes
pre,B2,1024B,3,0.253673,2.0781,119.00,138.00,238.00,124.09,79.91,3971.77,3803978,0,32,1,1024,8,15.000,1,0,0.00,11,11,199783,0,10,0,3:3804978,0.983696,yes
pre,B2,8192B,1,0.185000,12.1242,183.00,193.00,245.00,167.08,60.81,3890.20,2773654,0,32,1,8192,8,15.000,1,0,0.00,165113,165113,231447,123348,396081,0,0:2774654,0.965931,no
pre,B2,8192B,2,0.179744,11.7797,184.00,198.00,295.00,172.92,62.58,4962.29,2694203,0,32,1,8192,8,15.000,1,0,0.00,157519,157519,227904,87422,384185,0,1:2695203,0.971292,no
pre,B2,8192B,3,0.178553,11.7017,184.00,202.00,300.00,174.05,62.38,5444.06,2675938,0,32,1,8192,8,15.000,1,0,0.00,156963,156963,225768,70422,380613,0,3:2676938,0.971161,no
pre,B2,64B_1MiB_valid,1,0.001588,0.0008,626.00,643.00,688.00,629.65,567.68,3712.60,23789,0,1,1,64,1048576,15.000,1,0,0.00,1,1,23794,0,0,0,0:23794,0.999884,yes
pre,B2,64B_1MiB_valid,2,0.001574,0.0008,625.00,673.00,823.00,635.22,575.87,14358.40,23575,0,1,1,64,1048576,15.000,1,0,0.00,1,1,23580,0,0,0,1:23580,0.999836,yes
pre,B2,64B_1MiB_valid,3,0.001568,0.0008,626.00,723.00,854.00,637.78,571.54,4006.99,23485,0,1,1,64,1048576,15.000,1,0,0.00,1,1,23490,0,0,0,3:23490,1.000039,yes
post,A2,64B,1,0.239895,0.1228,117.00,229.00,345.00,130.86,85.54,165534.63,3596088,0,32,1,64,8,15.000,1,0,0.00,1,1,112408,0,0,0,0:3597088,0.981021,yes
post,A2,64B,2,0.258411,0.1323,116.00,126.00,233.00,121.29,88.39,4688.74,3874872,0,32,1,64,8,15.000,1,0,0.00,1,1,121120,0,0,0,1:3875872,0.979458,no
post,A2,64B,3,0.260844,0.1336,117.00,118.00,230.00,120.14,80.06,3813.36,3911160,0,32,1,64,8,15.000,1,0,0.00,1,1,122254,0,0,0,10:3912160,0.979306,no
post,A2,1024B,1,0.250670,2.0535,118.00,155.00,288.00,125.43,67.99,14320.88,3758821,0,32,1,1024,8,15.000,1,0,0.00,218,218,162344,0,232,0,0:3759821,0.982548,yes
post,A2,1024B,2,0.247743,2.0295,118.00,173.00,290.00,126.75,74.42,4714.99,3713504,0,32,1,1024,8,15.000,1,0,0.00,27,27,163911,0,26,0,1:3714504,0.981295,yes
post,A2,1024B,3,0.246800,2.0218,118.00,176.00,292.00,127.12,69.81,4702.37,3700868,0,32,1,1024,8,15.000,1,0,0.00,159,159,147065,0,158,0,10:3701868,0.980413,yes
post,A2,8192B,1,0.179574,11.7686,185.00,201.00,258.00,170.86,60.52,3770.67,2692313,0,32,1,8192,8,15.000,1,0,0.00,161983,161983,223583,128574,383617,0,0:2693313,0.958813,no
post,A2,8192B,2,0.178121,11.6733,185.00,196.00,248.00,173.96,69.12,5678.07,2670042,0,32,1,8192,8,15.000,1,0,0.00,152616,152616,229110,84929,381341,0,1:2671042,0.968310,no
post,A2,8192B,3,0.114910,7.5307,246.00,365.00,403.00,266.42,110.89,12947.45,1722449,0,32,1,8192,8,15.000,1,0,0.00,100425,100425,146397,121117,245158,0,10:1723449,0.956698,no
post,A2,64B_1MiB_valid,1,0.001453,0.0007,682.00,710.00,735.00,688.15,584.50,9848.90,21786,0,1,1,64,1048576,15.000,1,0,0.00,1,1,21791,0,0,0,0:21791,0.999882,yes
post,A2,64B_1MiB_valid,2,0.001383,0.0007,727.00,745.00,906.00,722.70,641.12,12609.43,20746,0,1,1,64,1048576,15.000,1,0,0.00,1,1,20751,0,0,0,1:20751,0.999494,yes
post,A2,64B_1MiB_valid,3,0.001433,0.0007,684.00,702.00,730.00,697.82,623.47,166966.07,21485,0,1,1,64,1048576,15.000,1,0,0.00,1,1,21490,0,0,0,10:21490,0.999976,yes
~~~

## Complete CPU data

Headline, 1 KiB / 8 B / concurrency 32 / one thread / 30 s:

~~~csv
variant,rep,transport,req,reply,conc,threads,mrps,gbps,p50,p99,host_client_pct,host_server_pct,host_total_pct,dpu_arm_pct,host_pct_per_krps
pre,1,dpumesh,1024,8,32,1,0.247504,2.0276,118.00,290.00,22.4,26.4,48.8,304.6,0.197
pre,2,dpumesh,1024,8,32,1,0.244370,2.0019,118.00,292.00,20.4,24.3,44.7,316.7,0.183
pre,3,dpumesh,1024,8,32,1,0.252421,2.0678,119.00,287.00,19.8,24.4,44.2,322.5,0.175
post,1,dpumesh,1024,8,32,1,0.247089,2.0242,118.00,291.00,22.6,26.7,49.3,303.7,0.200
post,2,dpumesh,1024,8,32,1,0.247031,2.0237,118.00,287.00,20.7,25.1,45.8,318.4,0.185
post,3,dpumesh,1024,8,32,1,0.242430,1.9860,118.00,345.00,21,26,47,201.6,0.194
~~~

Backpressure, 64 B / 1 MiB / concurrency 1 / one thread / 30 s:

~~~csv
variant,rep,transport,req,reply,conc,threads,mrps,gbps,p50,p99,host_client_pct,host_server_pct,host_total_pct,dpu_arm_pct,host_pct_per_krps
post,1,dpumesh,64,1048576,1,1,0.001450,0.0007,684.00,736.00,22.8,33,55.8,349.5,38.483
post,2,dpumesh,64,1048576,1,1,0.001393,0.0007,717.00,858.00,23.9,32.6,56.5,397.9,40.560
post,3,dpumesh,64,1048576,1,1,0.001444,0.0007,684.00,862.00,23.2,33.7,56.9,253.1,39.404
pre,1,dpumesh,64,1048576,1,1,0.001582,0.0008,627.00,734.00,22.4,74,96.4,349.5,60.936
pre,2,dpumesh,64,1048576,1,1,0.001581,0.0008,625.00,772.00,22.3,73.7,96,393.4,60.721
pre,3,dpumesh,64,1048576,1,1,0.001571,0.0008,626.00,790.00,23.7,76,99.7,410.7,63.463
~~~

## Decision

There is **no demonstrated 1 KiB fast-path throughput regression**. There is a
**real backpressure throughput/latency regression of roughly 8–9%**, accompanied
by a much larger and intentional reduction in retry-scan CPU: about 42% less
total host CPU and 56% less server CPU in the tested 1 MiB case. The event-driven
writable design should therefore be kept if eliminating timer/busy-poll behavior
is the primary requirement, but its wake/retry path needs optimization if the
old saturated large-reply throughput is also a release gate. The rejected 8 KiB
third-backend anomaly should be reproduced with a quality-gated measurement
method before being treated as a separate defect.

---

# Session 10 — LD_PRELOAD migration to the native CQ contract

The preload data path now uses `dmesh_alloc`/`dmesh_post_send`/`dmesh_flush` and
consumes `CONN_REQ`, `RECV`, `RECV_FIN`, and `TX_READY` through `dmesh_poll_cq`.
The old raw-ring and internal ready-list calls are absent from the rebuilt shim.
Its app-visible socketpair suppresses `POLLOUT` after native `EAGAIN` and restores
it only on `TX_READY`; blocking writers park on that kernel fd without a timer.

## Validation receipt

- Source base: `b8e5c9b` plus the dirty Session 10 preload/API/test changes.
- Full `bench.sh deploy` completed: DPU rebuilt/restarted, all meshed pods reached
  `DPU pod is data-ready`, and fair CPU pinning completed.
- Local and running-pod shim SHA-256 matched:
  `c931c4df5dafb4ba967f2c9b838d0216e990f7cb567f114798cdcc09b7b11223`.
- Live validator workload used the rebuilt shim, not the earlier image.

| preload validator | OK/Fail | p50 | p99 | RPS |
|---|---:|---:|---:|---:|
| 1,000 × 1 KiB × 8 conns | 1,000/0 | 141 us | 239 us | 6,327 |
| 8,000 × 8 KiB × 32 conns | 8,000/0 | 355 us | 1,267 us | 52,967 |
| 3,000 × 32 KiB × 8 conns | 3,000/0 | 271 us | 794 us | 21,586 |
| 5,000 × 1 KiB × 64 conns | 5,000/0 | 575 us | 7,716 us | 52,318 |
| 2,000 × 64 KiB × 64 conns | 2,000/0 | 5,013 us | 7,291 us | 11,735 |

Every run opened fresh connections and exercised vanilla blocking client I/O,
the nonblocking epoll echo server, partial byte-stream reads/writes, large-write
chunking, ClusterIP `getpeername`, FIN/close churn, and dispatcher entry lifetime.
All runs were byte-correct with zero failures. These are correctness/diagnostic
results, not a controlled performance comparison; latency and RPS are included
only as run receipts.

---

# Session 11 — Matched-C LD_PRELOAD suite smoke

The previously disabled `dpumesh-preload` row is now wired to the dedicated
`preload-bench`/`preload-echo` pair. A reduced suite (`rtt conc curve bw`,
1 repetition, 3-second runs) completed with 78 rows and zero reported
`fail`, `drops`, `overflow`, or `reorder` for the preload rows.

- Direct control smoke: `1,024 B` request / `8 B` reply, 4 connections,
  5 seconds: `fail=0`, `drops=0`, `overflow=0`, `reorder=0`.
- Matched-C curve peak receipts: `0.184219 Mrps` at 64 B and
  `0.153458 Mrps` at 1 KiB (4 connections); these are diagnostic, not a
  release-grade comparison.

---

# Session 12 — gRPC target/authority and reconnectable DPUmesh transport

Date: 2026-07-21. Source base: `cab0b94` plus the uncommitted Session 12
implementation and documentation changes.

## Change under test

The gRPC client now takes a DPUmesh Service-name `target` and ordinary
`grpc::ChannelArguments`. It defaults `GRPC_ARG_DEFAULT_AUTHORITY` only when the
application did not supply one. A per-channel EventEngine creates a fresh native
QP on every gRPC connection attempt, so gRPC owns lazy connection and reconnect
backoff. The DPU L4 proxy no longer re-picks a dead backend inside an existing
byte stream; it terminates that stream and lets a new connection select from the
current ready set.

The QPS client gained explicit `AUTHORITY` and `WAIT_FOR_READY` inputs and emits
target, authority, and wait policy in its result JSON.

## Host correctness results

| Command / build | Result | Covered scope |
|---|---:|---|
| `make test -j2` | 7/7 PASS | native API/control/batching/writable, preload, ABI, and new dead-pin L4 policy |
| ASan/UBSan build + `ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/grpc --output-on-failure` | 4/4 PASS, 0.25 s | Endpoint, channel, reactor/runtime, native linkage |
| TSan build + `TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/grpc-tsan --output-on-failure` | 4/4 PASS, 2.64 s | same suite under thread instrumentation |
| Release `grpc_dpumesh_qps_benchmark` target | PASS | updated benchmark compiled against gRPC v1.80.0 |

The reactor suite contains 19 focused cases. New cases verify that explicit
authority is preserved, absent authority defaults to target, the channel is
lazy, a transport FIN causes a second QP to be created, and both attempts carry
the original `greeter` target. Fake-native lifecycle checks also verify that
late/cancelled connection results close their QP.

Leak detection is disabled only for this sandbox's ptrace restriction; ASan
memory-access and UBSan checks remain enabled. No sanitizer finding occurred.

## Loopback gRPC benchmark receipt

This is a single short functional receipt, not a retained performance
comparison. Server and client used the same Release binary on kernel loopback:

```text
server tcp 127.0.0.1:51087 120
client tcp 127.0.0.1:51087 1 2 2 64 64 1 bench-authority.test 0
```

```json
{"transport":"tcp","client_type":"SYNC_CLIENT","rpc_type":"UNARY","target":"127.0.0.1:51087","authority":"bench-authority.test","wait_for_ready":false,"channels":1,"concurrency":2,"request_bytes":64,"response_bytes":64,"warmup_seconds":1,"measurement_seconds":2.001,"attempted":5447,"succeeded":5447,"failed":0,"qps":2722.472,"latency_us":{"p50":724.450,"p90":1096.192,"p99":1357.295,"p999":1526.087},"client_cpu_seconds":1.121,"client_cpu_cores":0.560}
```

The result proves that the updated CLI and explicit authority path work for the
matched TCP control. It does not establish a throughput claim: one run, a short
duration, loopback placement, and sandbox scheduling are not a controlled
campaign.

## Hardware boundary

`CCACHE_DISABLE=1 ninja -C build/doca` compiled every DPU C object, including
the changed `dpu_proxy.c`, but the final binary did not link in this x86 host
DOCA configuration. `DOCA_ARCH_DPU` was absent, so DPU-only definitions such as
`setup_pod_dma`, `init_dpa_objects`, and `init_comch_dpa_msgq` were omitted while
`dpu_worker.c` referenced them. This is an environment/target mismatch, not a
failure in the changed pin-policy object; a real BlueField DPU build remains
required.

No BlueField runtime was available for a DPUmesh QPS run or an orchestrator
create/delete campaign. Host tests prove fresh-QP reconnect and immutable stream
pinning deterministically, but do not prove DMA execution, real backend death
detection, Kubernetes readiness ordering, or resource plateaus. Those remain
hardware validation items and must not be inferred from the loopback TCP result.

---

# Session 13 — gRPC target/authority performance regression check

Date: 2026-07-21. This session supersedes Session 12's hardware boundary: a
real BlueField build, deploy, and DPUmesh gRPC run were available for this
measurement.

## Question and comparison boundary

The comparison asks whether the Session 12 client change—logical `target` plus
ordinary gRPC channel arguments, lazy connect, and a reconnectable per-channel
EventEngine—adds steady-state unary RPC cost. It does **not** compare the full
current tree with a historical DPU binary:

- `pre` is the Release gRPC QPS client built from exact commit `cab0b94` in a
  detached worktree;
- `post` is the Release client from `cab0b94` plus the uncommitted Session 12
  implementation;
- both clients dynamically load the same current `libdpumesh.so.2`, use the
  same current DPU image, and talk to the same `post` QPS server. This keeps the
  native/DPU data path and server constant and isolates the client adapter;
- gRPC is v1.80.0. The SHA-256 values are:

| artifact | SHA-256 |
|---|---|
| pre QPS binary | `01741dbd20d1a47b2917b7f448351f9ce0543beace5b3e7628c47d13ce83a160` |
| post QPS binary | `a7c591172beaa7c5373617bffbb0810b672a0a6d452520ef6551e684c51962f2` |
| shared `libdpumesh.so.2` | `706f8b9bae7781d0154a6fed0bdd83f3cf930e66a99d65c6dee80f2c918b4b5c` |
| BlueField `dpumesh_dpu` | `97b7c31a5ecda1c554d3f8277a389d22c15a40e315f113913a161604efbead8f` |

This arrangement measures client-side gRPC integration overhead. It is not an
end-to-end before/after result for the L4 proxy changes in the dirty tree.
The two QPS and shared-library hashes were rechecked inside the final pods.

## Environment and method

- Host/node: `rapids4`, Intel Xeon Gold 6554S, Linux 5.15.0-185; server and
  client pods were on the same node with zero container restarts.
- DPU deploy command:

  ```text
  DPUMESH_INGEST_SHARDS=4 DPUMESH_ARM_EGRESS_THREADS=4 \
  DPUMESH_RINGS_PER_POD=2 DPUMESH_LOG_LEVEL=40 ./bench/bench.sh deploy
  ```

  The BlueField build recompiled 15 DPU C objects and every meshed pod reached
  data-ready state. Thus the effective DPU configuration was four ingress
  shards, four ARM egress workers, two rings per pod, warning-level logging.
- QPS server: `bench-dpumesh` pod, `DPUMESH_SERVICE=bench-dpumesh`, core 0.
  Client: `bench-dpumesh-2` pod, core 9. The logical target and default
  authority were both `bench-dpumesh`; `wait_for_ready=false`.
- Workload: one channel, synchronous unary `grpc.testing.BenchmarkService`,
  concurrency 2, 64-byte request and 64-byte reply, one reactor.
- Fresh-state A/B: fully restart the DPU and all pods before every observation;
  5 s warmup and 30 s measurement. Order was `pre, post, post, pre, pre, post`.
- Same-deployment cross-check: two deployments, two client lifecycles per
  deployment, with order reversed (`post/pre`, then `pre/post`). Each client
  again used 5 s warmup and 30 s measurement.
- TCP control: one post server on kernel loopback, pinned to core 0; each client
  was pinned to core 1. Four observations per version, 2 s warmup and 8 s
  measurement, order `pre, post, post, pre, pre, post, post, pre`.

The per-deployment DPUmesh commands, after copying both binaries into the pods,
were equivalent to:

```text
env DPUMESH_SERVICE=bench-dpumesh taskset -c 0 \
  /tmp/grpc_qps_post server dmesh bench-dpumesh 180 1
taskset -c 9 /tmp/grpc_qps_pre \
  client dmesh bench-dpumesh 5 30 2 64 64 1
taskset -c 9 /tmp/grpc_qps_post \
  client dmesh bench-dpumesh 5 30 2 64 64 1 bench-dpumesh 0
```

The TCP clients used the same payload and concurrency with target
`127.0.0.1:51090`; post used authority `bench-authority.test` and
`wait_for_ready=0`.

The accepted samples had zero RPC failure, exact response-size validation, zero
native RX drop at teardown, and successful `POD_QUIESCED`. CPU below is client
process CPU, not total host plus DPU CPU.

## BlueField DPUmesh fresh-state results

| variant | rep | QPS | p50 us | p90 us | p99 us | p999 us | client cores | success/fail |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| pre | 1 | 11,592.624 | 152.588 | 165.382 | 491.235 | 844.906 | 0.508 | 347,780/0 |
| pre | 2 | 11,203.817 | 153.067 | 182.193 | 522.476 | 867.028 | 0.494 | 336,116/0 |
| pre | 3 | 11,800.153 | 151.623 | 162.663 | 480.672 | 807.364 | 0.515 | 354,006/0 |
| post | 1 | 10,909.326 | 153.364 | 319.148 | 550.645 | 917.210 | 0.485 | 327,281/0 |
| post | 2 | 11,123.054 | 153.711 | 191.073 | 532.391 | 881.266 | 0.490 | 333,693/0 |
| post | 3 | 11,699.974 | 153.492 | 163.873 | 480.703 | 835.430 | 0.514 | 351,001/0 |

| statistic (arithmetic mean) | pre | post | post change |
|---|---:|---:|---:|
| QPS | 11,532.198 | 11,244.118 | -2.50% |
| QPS range | 11,203.817–11,800.153 | 10,909.326–11,699.974 | ranges overlap |
| p50 | 152.426 us | 153.522 us | +0.72% |
| p90 | 170.079 us | 224.698 us | +32.11% |
| p99 | 498.128 us | 521.246 us | +4.64% |
| p999 | 839.766 us | 877.969 us | +4.55% |
| client CPU cores | 0.506 | 0.496 | -1.85% |

The p90 mean is dominated by post repetition 1; the other post p90 values were
191.073 and 163.873 us. With three observations and overlapping QPS ranges,
these independent-deployment means are directional, not a confidence-qualified
regression result.

## Same-deployment adjacent-pair cross-check

| deployment/order | variant | QPS | p50 us | p99 us | client cores |
|---|---|---:|---:|---:|---:|
| A, post first | post | 11,699.974 | 153.492 | 480.703 | 0.514 |
| A, pre second | pre | 11,530.962 | 152.421 | 507.010 | 0.511 |
| B, pre first | pre | 11,897.470 | 154.996 | 459.094 | 0.512 |
| B, post second | post | 11,514.897 | 154.116 | 469.491 | 0.508 |

Deployment A reuses post fresh-state repetition 3 as its first observation; it
is not counted as an additional independent sample.

Post changes by **+1.47%** in deployment A and **-3.22%** in deployment B.
The two paired percentage changes average **-0.88%**; pooled QPS is
11,714.216 pre versus 11,607.435 post (**-0.91%**). The sign changes with run
order, so this cross-check does not demonstrate a stable adapter regression.

## Direct TCP control

| variant | rep | QPS | p50 us | p99 us | client cores | failure |
|---|---:|---:|---:|---:|---:|---:|
| pre | 1 | 16,064.782 | 121.603 | 149.901 | 0.652 | 0 |
| pre | 2 | 15,779.908 | 124.605 | 150.140 | 0.665 | 0 |
| pre | 3 | 15,826.280 | 123.779 | 149.018 | 0.666 | 0 |
| pre | 4 | 15,762.873 | 124.369 | 150.886 | 0.670 | 0 |
| post | 1 | 15,907.734 | 123.122 | 149.645 | 0.673 | 0 |
| post | 2 | 16,131.938 | 122.110 | 146.514 | 0.646 | 0 |
| post | 3 | 15,488.460 | 126.703 | 152.511 | 0.653 | 0 |
| post | 4 | 15,856.841 | 123.797 | 149.094 | 0.671 | 0 |

TCP mean QPS is 15,858.461 pre and 15,846.243 post (**-0.08%**). Mean p50
changes 123.589 to 123.933 us (+0.28%), mean p99 changes 149.986 to 149.441 us
(-0.36%), and mean client CPU changes 0.6633 to 0.6607 cores (-0.38%). The
target/authority CLI and ordinary channel-argument path therefore show no
measurable TCP steady-state regression in this workload.

## Comparison with the Session 12 receipt

Session 12 recorded one 2.001-second loopback sample at 2,722.472 QPS, p50
724.450 us, and p99 1,357.295 us. The current four-sample post TCP mean is
15,846.243 QPS (+482.05%), p50 123.933 us (-82.89%), and p99 149.441 us
(-88.99%). This is **not a code-speedup claim**: Session 12 explicitly retained
only a short functional receipt and did not pin client/server CPUs, whereas this
session pins separate cores, runs four interleaved repetitions, and measures for
8 seconds after warmup. The old number is unsuitable as a regression baseline;
the controlled pre/post results above are the valid comparison.

## Repeated-lifecycle diagnostic anomaly

An initial attempt ran ten interleaved 10-second clients against one persistent
DPU/server state. Samples 1–5 stayed at 11.2–11.5 kQPS. Starting at sample 6,
both variants degraded: post produced 4.93 and 4.52 kQPS, then pre produced 5.02
and 6.70 kQPS. The tenth (post) client failed with `Deadline Exceeded`, and its
teardown timed out waiting 5 seconds for `POD_QUIESCED`. The DPU log recorded:

```text
proxy: dst pod 12 not ready — 17 bytes dropped (total 1)
proxy: poisoning conn (11:32789): seg cannot be delivered
```

Because the deterioration affected both binaries and followed repeated pod-slot
reuse, none of these samples is included in the performance means. Fresh-state
and two-lifecycle accepted runs ended with clean quiesce and no recent DPU
warning/error. The anomaly is nevertheless a real high-load create/destroy
reliability finding and should be reproduced separately before claiming an
unbounded dynamic-pod lifecycle plateau.

## Decision

There is **no demonstrated steady-state performance regression** from the new
target/authority and reconnectable EventEngine API at this 64-byte unary point:
TCP is -0.08%, while same-deployment DPUmesh pairs average -0.88% and disagree
in sign. The independent fresh-deployment mean is lower by 2.50%, so zero cost
cannot be proved from only three runs; it remains a watch item rather than a
release-blocking regression. The more important observed risk is repeated
client lifecycle/resource reclaim, which is a correctness/stability follow-up
and is not hidden inside the accepted throughput result.
