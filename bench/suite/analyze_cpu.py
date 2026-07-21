#!/usr/bin/env python3
"""Aggregate repeated host and DPU ARM CPU probes by workload point."""
import csv
import sys

from analyze import bootstrap_ci, median


POINT_COLS = ["transport", "req", "reply", "conc", "threads"]
BASE_METRICS = [
    "mrps", "gbps", "p50", "p99", "host_client_pct", "host_server_pct",
    "host_total_pct", "dpu_arm_pct",
]
DERIVED_METRICS = [
    "host_us_per_req", "dpu_arm_us_per_req", "host_cores_per_gbps",
    "dpu_arm_cores_per_gbps",
]


def derived(row):
    mrps = float(row["mrps"])
    gbps = float(row["gbps"])
    host_cores = float(row["host_total_pct"]) / 100.0
    dpu_cores = float(row["dpu_arm_pct"]) / 100.0
    row = dict(row)
    row["host_us_per_req"] = host_cores / mrps if mrps > 0 else 0.0
    row["dpu_arm_us_per_req"] = dpu_cores / mrps if mrps > 0 else 0.0
    row["host_cores_per_gbps"] = host_cores / gbps if gbps > 0 else 0.0
    row["dpu_arm_cores_per_gbps"] = dpu_cores / gbps if gbps > 0 else 0.0
    return row


def main():
    if len(sys.argv) != 3:
        print("usage: analyze_cpu.py <cpu.csv> <cpu_summary.csv>", file=sys.stderr)
        raise SystemExit(2)
    rows = [derived(r) for r in csv.DictReader(open(sys.argv[1]))]
    groups = {}
    for row in rows:
        key = tuple(row[c] for c in POINT_COLS)
        groups.setdefault(key, []).append(row)

    metrics = BASE_METRICS + DERIVED_METRICS
    out_cols = POINT_COLS + ["n_reps"]
    for metric in metrics:
        out_cols += [f"{metric}_med", f"{metric}_lo", f"{metric}_hi"]

    with open(sys.argv[2], "w", newline="") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(out_cols)
        for key in sorted(groups, key=lambda k: (int(k[1]), k[0], int(k[3]))):
            reps = groups[key]
            out = list(key) + [len(reps)]
            for metric in metrics:
                vals = [float(r[metric]) for r in reps]
                med = median(vals)
                lo, hi = bootstrap_ci(vals)
                out += [f"{med:.6f}", f"{lo:.6f}", f"{hi:.6f}"]
            writer.writerow(out)
    print(f"analyze_cpu: {len(rows)} rows -> {len(groups)} points -> {sys.argv[2]}", file=sys.stderr)


if __name__ == "__main__":
    main()
