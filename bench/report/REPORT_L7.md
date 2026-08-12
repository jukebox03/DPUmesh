# DPUmesh L7 Evaluation

DPUmesh carries a service's bytes between pods without the host seeing them. An
L7 layer on the DPU changes that arrangement in one respect only: it decides
where a message goes. This report measures what that decision costs, as a
function of how often it is taken.

Five operating points carry the same request/response workload over the same
transport, the same stack and the same pods. They differ in whether the L7 layer
sees the payload at all, and in how often it may change its mind about the
destination:

| Mode | The L7 layer sees | Destination chosen |
|---|---|---|
| `dataplane` | nothing — no service is assigned to the layer | once, at connect |
| `decision` | the flow, once per connection | once per connection |
| `opaque` | every byte, as an unframed stream | once per connection |
| `l7-conn` | every byte, framed into messages | once per connection |
| `l7-message` | every byte, framed into messages | once per message |

`decision` and `opaque` bracket the cost of carrying bytes through the layer
without framing them. `l7-conn` and `l7-message` share one code path and differ
only in a single setting, so the difference between them is the selection
granularity and nothing else.

These are measured with the reference consumer in `linkerd/shim/l7_null.c`, not
with linkerd2-proxy in the data path. What the numbers describe is the cost
envelope the adapter contract imposes on any consumer, and the granularity a
consumer chooses within it. Per-message selection is what a real proxy does for
HTTP/2 and gRPC; per-connection is what an opaque or TCP route does.

## What the decision costs

![ARM cores per delivered Mrps at matched rates](figures/l7_arm_cores.png)

Cost per request depends on the rate it is measured at — batching depth follows
queue occupancy, and the offered rate sets that. Modes compared at their own
operating points are therefore not comparable at all. These are the ARM cores
the data-path process spends per delivered Mrps at rates every mode serves,
median of three runs with the range in brackets:

| Mode | 50,000/s | 100,000/s | 150,000/s |
|---|---|---|---|
| `dataplane` | 69.58 (66.2–71.0) | 13.59 (12.9–14.1) | 14.38 (13.1–14.5) |
| `decision` | 65.26 (62.2–71.7) | 14.02 (13.0–14.2) | 13.04 (13.0–13.2) |
| `opaque` | 65.78 (60.5–66.1) | 13.53 (13.5–13.6) | 13.23 (13.2–13.2) |
| `l7-conn` | 63.00 (62.3–63.2) | **12.91** (12.5–13.2) | **12.82** (12.8–13.9) |
| `l7-message` | 58.10 (54.3–60.5) | **16.21** (15.2–17.6) | **16.47** (14.1–16.5) |

At 100,000/s and 150,000/s the four modes that fix a destination for the life of
a connection are indistinguishable: their medians span 12.8 to 14.4 and every
range overlaps its neighbours. Carrying the payload through the layer, framing
it, and re-emitting it costs nothing this measurement can separate from the
plain data path — `l7-conn` is if anything the cheapest of the four.

Per-message selection is separable. At 100,000/s its range, 15.2 to 17.6, clears
every other mode's, for about 20% more ARM per request. That is the whole of the
per-request cost of moving the choice from once per connection to once per
message.

The lowest rate is a different regime and does not compare modes. Every mode
costs four to five times as much per request there, and the ordering inverts:
`l7-message` is the cheapest at 50,000/s and the most expensive at 100,000/s.
The absolute cost falls as load rises — 3.48 cores at 50,000/s against 1.36 at
100,000/s on the plain data path — so below roughly 100,000/s something
load-independent dominates, and whichever mode is measured there wins by
accident. The effect was confirmed against a long-running stack rather than a
fresh deployment: three repeats of the 50,000/s point gave 350.9%, 345.1% and
356.0%, so it is reproducible and not a warm-up artefact.

## The limit is deliveries, not bytes

![Fraction of the offered rate delivered](figures/l7_delivery.png)

What per-message selection costs is not cycles per request but capacity, and the
evidence is that it stops at the same message rate whatever the message size.
Offered rates were ramped until delivery failed, with every rung offered twice
and accepted only when both repetitions delivered:

| Mode | Frame | Highest rate delivered | First rate refused | Achieved there |
|---|---|---:|---:|---:|
| `dataplane` | 64 B | 6,487,299 | 9,730,948 | 9,033,807 |
| `decision` | 64 B | 6,487,299 | 9,730,948 | 9,044,164 |
| `l7-message` | 64 B | **168,750** | 200,000 | **174,093** |

At 64 B `l7-message` saturates at about 174,000 messages per second. At 1 KiB it
plateaus at 168,731, and at 8 KiB at 157,541. Across a 128× range of message
size the ceiling moves by 10%. A destination fixed for the connection lets the
transport pack many messages into one delivery to the host; a destination that
changes per message cannot be packed, so each message becomes its own delivery.
The payload rides along; the delivery is the cost.

Reordering follows the same line. At matched load `l7-message` reorders 25,899
replies at 50,000/s and 317,909 at 150,000/s; every other mode reorders none,
because only per-message selection lets replies from different backends
interleave.

The figure also shows where the modes agree. At 8 KiB a message fills a delivery
on its own, so per-message selection gives up nothing: every mode lands between
157,541 and 173,882, as does the matched TCP path at 161,430. At 8 KiB this
measurement cannot separate the modes, and does not claim to.

## Why a single capacity number is misleading here

![Delivered fraction and tail across the collapse band](figures/l7_band.png)

A ramp that stops at the first rate a path fails to deliver reports a number
that is not the path's capacity. The plain data path at 1 KiB refuses
1,281,442/s — and delivers 3,491,032/s. Two repetitions at each rate, on one
deployment:

| Offered/s | Achieved/s | Drops | p99 |
|---:|---:|---:|---:|
| 900,000 | 899,514 | 0 | 861 µs |
| 1,100,000 | 991,909 | 426,371 | 169,510 µs |
| 1,281,442 | 1,009,874 | 1,076,602 | 115,543 µs |
| 1,600,000 | 1,074,036 | 1,294,629 | 78,836 µs |
| 1,922,163 | 1,060,426 | 3,430,352 | 64,713 µs |
| 2,400,000 | 1,108,080 | 5,147,693 | 28,725 µs |
| 2,883,244 | **2,877,434** | 20,606 | **892 µs** |
| 3,500,000 | **3,491,032** | 33,091 | **1,034 µs** |

Between about 1.0 and 2.5 Mrps the path pins at roughly 1.05 Mrps and holds a
tail two orders of magnitude above its own median. Offer more and it recovers
completely, at 2.7× the rate it refused. Both repetitions of every rate agree,
and the recovery is as sharp on the way up as the collapse is on the way in.

The same path at 64 B is flat and clean over 4.0 to 4.5 Mrps — no drops, p99
between 703 and 784 µs — so the excursion is not the load generator, and it is
not present at every frame. What the regime shares is that the offered rate
exceeds what the path delivers per unit time while messages still arrive too
slowly to fill a delivery, so the queue grows instead of the batch. That reading
is consistent with every measurement here but is not established by them; the
band's boundaries were mapped, its mechanism was not.

The consequence is procedural. Every capacity above is the **lowest** rate at
which delivery first failed, which for a path that recovers is a lower bound
rather than a ceiling. It is the same rule the L4 evaluation applies, and it is
quoted here for continuity, not because it is the largest number the path can
produce.

## Latency

At one outstanding request, where no queue forms:

| Mode | 64 B | 1 KiB | 8 KiB |
|---|---|---|---|
| `dataplane` | 151 / 534 µs | 154 / 531 µs | 161 / 538 µs |
| `decision` | 158 / 550 µs | 152 / 532 µs | 177 / 581 µs |
| `opaque` | **122 / 210 µs** | **126 / 224 µs** | 155 / 524 µs |
| `l7-conn` | 125 / 253 µs | 124 / 204 µs | **154 / 526 µs** |
| `l7-message` | 230 / 494 µs | 232 / 522 µs | 258 / 608 µs |

`opaque` and `l7-conn` are not merely as fast as the plain data path, they are
faster, and the tail improves more than the median — p99 210 µs against 534 µs
at 64 B. Both copy the payload into a DPU-local buffer before sending it, which
releases the arrival staging a round earlier than the data path does, so the
sender's transmit credit returns sooner. The copy that looks like pure overhead
buys back more than it costs at this operating point.

`l7-message` pays about 80 µs of median regardless of size, which is the
reassembly the framed path performs before it can route.

## What the closed and open loops each measure

The two loops disagree, and the disagreement is informative rather than a fault
in either. Under a window of 32 requests on each of 8 connections the modes
deliver 1.330, 1.349, 1.344 and 1.372 Mrps at 1 KiB — and `l7-message` 0.226,
the same sixfold gap the capacity table shows. But that window delivers 1.33
Mrps where the open-loop ramp refuses 1.28: a self-clocked client never offers
more than the path absorbs, so it cannot enter the collapse band the open loop
walks into. At 64 B the ordering reverses, 0.216 Mrps against the open loop's
6.49, because 256 outstanding requests cannot reach 6.49 Mrps at this path's
latency; some 6,500 would be needed.

Read together: the open loop measures what the path does when something else
sets the rate, and the closed loop what it does when the client waits. Neither
is the capacity on its own, and neither is a cost measurement — cost per request
is quoted only at matched load, above.

## Contract

| Axis | Value |
|---|---|
| Modes | `dataplane`, `decision`, `opaque`, `l7-conn`, `l7-message` |
| L7 consumer | `linkerd/shim/l7_null.c`, the reference consumer; `l7_backend=null` |
| Frames | symmetric request/response: 64 B, 1 KiB, 8 KiB |
| Frame format | 16 B benchmark header plus 48 B, 1008 B or 8176 B body |
| Transport unit | 8192 B (`DPUMESH_SLOT_SIZE`) |
| Load | constant-rate open loop and closed-loop window; 8 connections; 4–10 s per rate |
| Repetitions | 2 per point; 3 for the matched-load cost; capacity rungs accepted only when every repetition delivers |
| Deployment | one per mode; the gate is read when the data-path process starts |
| DPU | `N/K/A=32/8/8`; ARM cores from per-tid ticks of the data-path process over the wall-clock of the run |
| Backends | 3 ready servers, so a mode that balances has somewhere to balance to |
| Instrument | matched TCP path (`bench-tcp`/`echo-tcp`), same generator, measured as a sixth mode |
| Platform | `rapids4`, Intel Xeon Gold 6554S, Linux 5.15.0-186-generic |
| Dataset | [`data/l7-20260812-062044`](data/l7-20260812-062044) |

A rate is delivered when achieved/offered ≥ 0.98, drops ≤ 0.1% of completions,
and the latency histogram did not overflow. Latency is not part of that test, so
every capacity is quoted with the tail observed at it.

One measurement of ARM cost moves by about a tenth between runs on an unchanged
deployment — three repeats of one rate gave 13.18, 14.17 and 14.32 cores per
Mrps. Differences of that size between modes are not differences.

The instrument is measured, not assumed. On the matched TCP path the same
generator plateaus at 1,930,234/s at 64 B, 878,612/s at 1 KiB and 161,430/s at
8 KiB. At 64 B and 1 KiB every mode except `l7-message` runs above that, so
those figures are the paths'. At 8 KiB the instrument and every mode land within
8% of the instrument, and no mode-to-mode conclusion is drawn there.

Cross-checks. Of 434 runs, 18 recorded a histogram overflow: seven on ramp rungs
that were rejected anyway, ten on 8 KiB closed-loop latency, and one on an
accepted point. `fail` and `worker_fail` are zero on every retained run.

One event is unexplained. A single `decision` run at 1 KiB and 126,548/s
recorded p99 1.64 s, 86,934 drops, 12,614 histogram overflows and 61,240
allocation waits, where its repetition at the same rate recorded p99 1,089 µs
and no drops. Ten further runs of that mode at that rate on a fresh deployment
reproduced nothing: p99 between 1,031 and 1,060 µs, no drops, no overflow, no
waits. It is recorded rather than explained, and the collector now refuses any
run whose histogram overflowed.

Per-point rows are in
[`points.csv`](data/l7-20260812-062044/points.csv); the ramp is in
[`capacity.csv`](data/l7-20260812-062044/capacity.csv) and
[`saturation.csv`](data/l7-20260812-062044/saturation.csv); the repeated
capacity checks are in
[`knee_verify.csv`](data/l7-20260812-062044/knee_verify.csv); the band scans are
in [`band.csv`](data/l7-20260812-062044/band.csv); matched-load cost is in
[`cores_matched.csv`](data/l7-20260812-062044/cores_matched.csv).

## Reproduction

Create `.env` with `HOST_PASS`, `DPU_HOST` and `DPU_PASS`, then from the
repository root:

```sh
sudo swapoff -a

# Five modes plus the instrument, one deployment each.
./bench/suite/l7_modes.sh --out /tmp/l7run

# Cost per request at rates every mode serves.
REPS=3 ./bench/suite/l7_modes.sh --out /tmp/l7run --cores-only

# Push each mode past the rung its ramp stopped on.
./bench/suite/l7_refine.sh /tmp/l7run

# Re-measure one capacity with repetitions, against the deployed mode.
./bench/suite/l7_knee.sh /tmp/l7run dataplane 64 "4324866 6487299 9730948"

python3 bench/suite/summarize_l7.py /tmp/l7run
python3 bench/suite/plot_l7.py /tmp/l7run bench/report/figures
```

The collector deploys each mode, discovers its capacity, and measures the common
grid, each mode's own operating point, the closed-loop window and the ARM cores.
It checks that the data-path process is alive after every capacity search and at
the end of every mode: a dead data path reports as a failing client on every run
that follows, and without that check the dataset fills with zeroes and says
nothing is wrong.
