# Node density: what raising the Pod cap actually costs

A node serves the smaller of two caps: `MAX_PODS`, the registration slots the
ARM holds, and `MAX_DPA_RINGS × N / K`, the forward rings the DPA can hand out.
Nodes routinely run more Pods than the 32 that answers today, so this is a real
limit — but it is two limits wearing one number, they bind on different
hardware, and they do not cost the same to move.

This is the sizing PLAN's F7 asks for before a number is promised. Nothing here
changes the cap; it says what each way of changing it would cost.

## Where the 32 comes from

| Constant | Value | Where | What it bounds |
|---|---:|---|---|
| `MAX_DPA_RINGS` | 8 | `include/dpumesh/dmesh_common.h:7` | forward rings one EU can hold |
| execution units (N) | 32 | device query at start-up | EUs the BlueField reports |
| `DPUMESH_RINGS_PER_POD_DEFAULT` (K) | 2 | `include/dpumesh/dmesh_common.h:9` | EUs one Pod spans |
| `MAX_PODS` | 32 | `include/dpumesh/dmesh_common.h:17` | registration slots the ARM holds |
| `POD_ID_SPACE` | 128 | `include/dpumesh/dmesh_common.h:20` | Service-id keyed tables |

N is read from the device (`DPA multi-EU: num_dpa_threads=32`), and
`DPA_THREADS_DEFAULT = 8` is only the fallback when that query is unavailable.
`8 × 32 / 2 = 128` ring slots, so on this hardware the ring array is not what
binds at the default K — `MAX_PODS = 32` is. The two meet only at `K = 8`, which
is what the bench deployment runs.

## What the ring array costs

`MAX_DPA_RINGS` sizes five arrays inside `struct dpa_thread_arg`
(`doca/dpa_common.h:105-117`), one copy of which lives in device memory per EU:

| Array | Element | Bytes per ring slot |
|---|---|---:|
| `rings[]` | `struct dpa_ring_info` (48 B, asserted at `dpa_common.h:205`) | 48 |
| `consumer_head[]` | `uint64_t` | 8 |
| `ring_generation[]` | `uint32_t` | 4 |
| `pending_del_pod[]` | `int32_t` | 4 |
| `pending_del_generation[]` | `uint32_t` | 4 |
| | **total** | **68** |

The rest of `dpa_thread_arg` is 68 bytes that do not scale. So one EU's thread
argument is `68 + 68 × M` bytes, and the whole device-side cost is `N` copies of
it:

| `MAX_DPA_RINGS` (M) | per EU | 32 EUs | ring slots at K=2 |
|---:|---:|---:|---:|
| 8 (today) | 612 B | 19.1 KiB | 128 |
| 16 | 1,156 B | 36.1 KiB | 256 |

The memory is not the constraint, and on this hardware neither is the ring
array. Two other things are.

## The two real costs

**The per-poll scan is linear in the rings an EU holds.** `run_dma_manager`
walks `for (r = 0; r < num_rings; r++)` on every pass
(`doca/device/dpa_kernel.c:377`), and each iteration reads that ring's control
block out of host memory. `num_rings` is what it walks, not `MAX_DPA_RINGS`, so
an idle slot costs nothing. What costs is occupancy: at `K = 2` and 32 EUs,
127 Pods put roughly eight rings on every EU, and the scan is eight
control-block reads per poll instead of the two a lightly loaded node does.
That is the figure to measure before promising a density, and it cannot be
measured off this deployment: it needs a node carrying that many Pods.

**The DPA kernel is device code with its own toolchain and its own validation.**
`doca/device/dpa_kernel.c` is compiled by `dpacc` on the BlueField, not by the
host build, and `dpa_common.h`'s `_Static_assert`s are wire-ABI: `dpa_ring_info`
is asserted at exactly 48 bytes, `comch_add_ring_msg` at 56, `comch_msg` at 60.
Changing `MAX_DPA_RINGS` does not touch any of those — the arrays are sized by
the constant and the messages are not — so the change is a recompile of both
sides against the same header, and both sides must be redeployed together. A
mismatched pair would have the ARM writing `rings[12]` into an EU that allocated
eight.

## The cheap lever, and what it trades

At the default `K = 2` this hardware already has 128 ring slots for 32
registration slots, so the first move is `MAX_PODS`, not the ring array:
raising it to 127 costs nothing on the DPA side and no wire format changes with
it. `K` is the second lever — it is an environment variable,
`DPUMESH_RINGS_PER_POD` (`doca/dpu_worker.c:783`), clamped to the EU count
(`doca/dpa.c:382`) — and lowering it buys ring slots this hardware does not
need.

What `K` costs is per-Pod parallelism: a Pod spanning one EU has its forward
traffic served by one EU's DMA budget instead of several. That is a throughput
trade for a density gain, and which way it goes depends on whether the node's
Pods are individually hot or collectively many.

**This deployment runs `K = 8`, not the default `K = 2`.** The bench harness
sets `DPUMESH_RINGS_PER_POD=8` so each workload's forward traffic spreads over
eight EUs, which leaves `8 × 32 / 8 = 32` ring slots — exactly `MAX_PODS`.
Density and per-Pod throughput are the same dial, and the harness has it turned
to throughput.

## Above 127

`MAX_PODS` may rise to 127 without touching any wire format: pod ids travel as
`int8_t` in `comch_dma_comp_msg` and in `struct px_unit`
(`doca/dpu_proxy.c:193,204`), with `-1` reserved as the unresolved-destination
sentinel, and `_Static_assert(MAX_PODS <= 127)` at `dpa_common.h:179` is what
holds the line. Past 127 those fields widen, which is a host-and-DPU wire-ABI
change of the kind `BATCH_TXACK` already showed can be silently lossy when the
two ends disagree.

`POD_ID_SPACE` is 128 and keys the Service tables (`svc_mode`,
`svc_protected`), so it moves with the same change.

## Recommendation

Order of cost, cheapest first:

1. **`MAX_PODS = 127`** — a host constant, no DPA change, no wire-format change.
   At `K = 2` this hardware's 128 ring slots already back it. This is the whole
   move up to 127 Pods.
2. **`MAX_DPA_RINGS = 16`** — 17 KiB more device memory, a `dpacc` rebuild, a
   paired redeploy, and a per-poll scan that doubles at full occupancy. Needed
   only for a device with fewer EUs than this one, or for `K` above 2.
3. **`MAX_PODS > 127`** — a wire-ABI change. Not worth scheduling until a
   deployment needs it.

No number should be published as supported until a node has been run at it.
Today's supportable claim is 32, which is `MAX_PODS`, and the deployment this
tree benchmarks runs `K = 8` and therefore 32 ring slots for those 32 Pods.
