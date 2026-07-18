#!/usr/bin/env python3
"""analyze.py — aggregate repeated benchmark points into publishable statistics.

Reads a TIDY csv (one row per repetition, produced by run_suite.sh)
and collapses repetitions of the same point into a median and a 95% bootstrap
confidence interval, for throughput and every latency percentile. This is the step
that turns single-shot engineering runs into a defensible measurement: N reps, a
central value, and an interval that says how much run-to-run noise there is.

Tidy input columns (header required):
    stage,transport,workload,mode,arrival,req_size,reply_size,conc,threads,
    offered_mrps,rep,mrps,gbps,p50,p95,p99,p999,p9999,avg,min,max,drops,overflow,fail

A "point" is every column up to and including offered_mrps (everything that is not a
measured output). Rows sharing a point are its repetitions.

    python3 analyze.py <tidy.csv> <summary.csv>

Output summary.csv adds, for each metric M in {mrps,gbps,p50,p95,p99,p999,p9999,avg},
three columns  M_med, M_lo, M_hi  (median and 95% CI), plus n_reps and summed
drops/overflow/fail so truncated tails or dropped load stay visible.
"""
import csv, sys

# Deterministic bootstrap (no system randomness — reproducible CIs across machines).
class LCG:
    def __init__(self, seed): self.s = seed & 0xFFFFFFFFFFFFFFFF
    def next(self):
        self.s = (self.s * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        return self.s >> 11
    def below(self, n): return self.next() % n

POINT_COLS = ["stage","transport","workload","mode","arrival","req_size","reply_size",
              "conc","threads","offered_mrps"]
METRICS    = ["mrps","gbps","p50","p95","p99","p999","p9999","avg"]
SUM_COLS   = ["drops","overflow","fail"]

def median(xs):
    s = sorted(xs); n = len(s)
    if n == 0: return 0.0
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])

def bootstrap_ci(xs, iters=2000, seed=0x2b7e151628aed2a6):
    """95% CI of the MEDIAN by resampling. With <3 reps the CI is just min/max —
    honest about there being too little data to bound noise."""
    n = len(xs)
    if n == 0: return (0.0, 0.0)
    if n < 3:  return (min(xs), max(xs))
    rng = LCG(seed); meds = []
    for _ in range(iters):
        sample = [xs[rng.below(n)] for _ in range(n)]
        meds.append(median(sample))
    meds.sort()
    lo = meds[int(0.025 * iters)]; hi = meds[int(0.975 * iters)]
    return (lo, hi)

def main():
    if len(sys.argv) != 3:
        print("usage: analyze.py <tidy.csv> <summary.csv>", file=sys.stderr); sys.exit(2)
    rows = list(csv.DictReader(open(sys.argv[1])))
    groups = {}
    for r in rows:
        key = tuple(r[c] for c in POINT_COLS)
        groups.setdefault(key, []).append(r)

    out_cols = list(POINT_COLS) + ["n_reps"]
    for m in METRICS: out_cols += [f"{m}_med", f"{m}_lo", f"{m}_hi"]
    out_cols += SUM_COLS

    w = csv.writer(open(sys.argv[2], "w"))
    w.writerow(out_cols)
    for key in sorted(groups):
        reps = groups[key]
        row = list(key) + [len(reps)]
        for m in METRICS:
            vals = [float(r[m]) for r in reps if r.get(m, "") not in ("", None, "NA")]
            if not vals:                      # metric never emitted (e.g. native has no
                row += ["NA", "NA", "NA"]     # p99.9) — keep NA distinct from a measured 0
                continue
            med = median(vals); lo, hi = bootstrap_ci(vals)
            row += [f"{med:.4f}", f"{lo:.4f}", f"{hi:.4f}"]
        for c in SUM_COLS:
            row.append(sum(int(float(r.get(c, 0) or 0)) for r in reps))
        w.writerow(row)
    print(f"analyze: {len(rows)} rows -> {len(groups)} points -> {sys.argv[2]}", file=sys.stderr)

if __name__ == "__main__":
    main()
