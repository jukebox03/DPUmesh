#!/usr/bin/env python3
"""D2 judgment over grpc_conns_sweep points.csv.

Pre-registered criteria (set before the campaign ran):
  Sustained point: ratio >= 0.99 and fail == 0 and drops == 0.
  Stall fingerprint (the D2 defect) at a sustained rate, across reps:
    - a rep whose p99 >= 5x the median p99 of that rate while its p50 stays
      within 1.5x the median p50  (p99 jumps alone), OR
    - any sustained rep with p99 >= 50 ms while p50 < 5 ms (global-stall form).
  Ordinary queueing: p50 and p99 rise together with rate -> not the defect.
"""
import csv, statistics, sys

path = sys.argv[1]
rows = []
with open(path) as f:
    for r in csv.DictReader(f):
        try:
            rows.append(dict(
                rate=int(float(r["offered"])), rep=int(r["rep"]),
                ratio=float(r["ratio"]), p50=float(r["p50_us"]),
                p99=float(r["p99_us"]), p999=float(r["p999_us"]),
                fail=int(r["fail"]), drops=int(r["drops"]),
                ach=float(r["achieved"])))
        except (ValueError, KeyError) as e:
            print(f"skip row: {e} :: {r}", file=sys.stderr)

rates = sorted({r["rate"] for r in rows})
stall_hits = []
print(f"{'rate':>6} {'n':>2} {'sust':>4}  {'p50 med':>9} {'p50 max':>9}  "
      f"{'p99 med':>10} {'p99 max':>10}  {'p999 max':>10}  verdict")
for rate in rates:
    grp = [r for r in rows if r["rate"] == rate]
    sust = [r for r in grp if r["ratio"] >= 0.99 and r["fail"] == 0 and r["drops"] == 0]
    tag = "not-sustained"
    if sust:
        p50m = statistics.median(r["p50"] for r in sust)
        p99m = statistics.median(r["p99"] for r in sust)
        tag = "clean"
        for r in sust:
            jump = r["p99"] >= 5 * p99m and r["p50"] <= 1.5 * p50m
            glob = r["p99"] >= 50_000 and r["p50"] < 5_000
            if jump or glob:
                stall_hits.append((rate, r["rep"], r["p50"], r["p99"],
                                   "jump" if jump else "global"))
                tag = "STALL"
        print(f"{rate:>6} {len(grp):>2} {len(sust):>4}  "
              f"{p50m:>9.0f} {max(r['p50'] for r in sust):>9.0f}  "
              f"{p99m:>10.0f} {max(r['p99'] for r in sust):>10.0f}  "
              f"{max(r['p999'] for r in sust):>10.0f}  {tag}")
    else:
        print(f"{rate:>6} {len(grp):>2} {len(sust):>4}  {'-':>9} {'-':>9}  "
              f"{'-':>10} {'-':>10}  {'-':>10}  {tag} "
              f"(min ratio {min(r['ratio'] for r in grp):.4f}, "
              f"max fail {max(r['fail'] for r in grp)})")

print()
if stall_hits:
    print("VERDICT: REPRODUCED — stall fingerprint present:")
    for rate, rep, p50, p99, kind in stall_hits:
        print(f"  rate={rate} rep={rep} p50={p50:.0f}us p99={p99:.0f}us ({kind})")
else:
    print("VERDICT: NOT REPRODUCED — no sustained rep shows a p99 jump with a flat p50.")
