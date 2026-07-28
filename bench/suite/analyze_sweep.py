#!/usr/bin/env python3
"""Validate and summarize sweep_final.sh output."""

import csv
import statistics
import sys
from collections import Counter, defaultdict


POINT_FIELDS = (
    "stage",
    "n",
    "k",
    "a",
    "pin",
    "numa_policy",
    "transport",
    "req",
    "reply",
    "conc",
    "threads",
)
STAGE_POINTS = {
    "topology": 20,
    "concurrency": 12,
    "transport": 8,
    "pinning": 8,
    "numa": 2,
}


def read_rows(path):
    with open(path, newline="") as source:
        return list(csv.DictReader(source))


def number(row, field):
    try:
        return float(row[field])
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError(f"invalid {field}: {row}") from exc


def require_zero(row, fields):
    for field in fields:
        if number(row, field) != 0:
            raise ValueError(f"{field} != 0: {row}")


def mean(values):
    return statistics.fmean(values)


def cv_pct(values):
    average = mean(values)
    return statistics.pstdev(values) / average * 100 if average else 0.0


def validate_results(rows, reps, numa_reps):
    expected_rows = 48 * reps + 2 * numa_reps
    if len(rows) != expected_rows:
        raise ValueError(f"results rows={len(rows)}, expected={expected_rows}")
    run_ids = [row["run_id"] for row in rows]
    if len(set(run_ids)) != len(run_ids):
        raise ValueError("duplicate run_id in results.csv")

    groups = defaultdict(list)
    for row in rows:
        require_zero(row, ("fail", "drops", "reorder"))
        key = tuple(row[field] for field in POINT_FIELDS)
        groups[key].append(row)

    stage_counts = Counter(key[0] for key in groups)
    if dict(stage_counts) != STAGE_POINTS:
        raise ValueError(f"stage point counts={dict(stage_counts)}, expected={STAGE_POINTS}")
    for key, group in groups.items():
        expected = numa_reps if key[0] == "numa" else reps
        if len(group) != expected:
            raise ValueError(f"point {key} has {len(group)} reps, expected={expected}")
    return groups


def write_summary(path, groups):
    header = list(POINT_FIELDS) + [
        "runs",
        "gbps_mean",
        "gbps_median",
        "gbps_min",
        "gbps_max",
        "gbps_cv_pct",
        "p50_mean",
        "p99_mean",
        "host_total_pct_mean",
        "dpu_arm_pct_mean",
        "grow_wait_runs",
        "grow_waits_mean",
        "grow_waits_max",
    ]
    with open(path, "w", newline="") as target:
        writer = csv.writer(target, lineterminator="\n")
        writer.writerow(header)
        for key in sorted(groups):
            rows = groups[key]
            gbps = [number(row, "gbps") for row in rows]
            waits = [number(row, "grow_waits") for row in rows]
            writer.writerow(
                list(key)
                + [
                    len(rows),
                    f"{mean(gbps):.6f}",
                    f"{statistics.median(gbps):.6f}",
                    f"{min(gbps):.6f}",
                    f"{max(gbps):.6f}",
                    f"{cv_pct(gbps):.3f}",
                    f"{mean([number(row, 'p50') for row in rows]):.3f}",
                    f"{mean([number(row, 'p99') for row in rows]):.3f}",
                    f"{mean([number(row, 'host_total_pct') for row in rows]):.3f}",
                    f"{mean([number(row, 'dpu_arm_pct') for row in rows]):.3f}",
                    sum(wait > 0 for wait in waits),
                    f"{mean(waits):.3f}",
                    f"{max(waits):.0f}",
                ]
            )


def validate_backends(rows, backend_reps):
    expected_rows = 9 * backend_reps
    if len(rows) != expected_rows:
        raise ValueError(f"backend rows={len(rows)}, expected={expected_rows}")
    run_ids = [row["run_id"] for row in rows]
    if len(set(run_ids)) != len(run_ids):
        raise ValueError("duplicate run_id in backends.csv")

    groups = defaultdict(list)
    for row in rows:
        require_zero(row, ("fail", "reorder"))
        groups[(row["n"], row["backend"])].append(row)
    expected_keys = {(str(n), str(backend)) for n in (8, 16, 32) for backend in (0, 1, 2)}
    if set(groups) != expected_keys:
        raise ValueError(f"backend groups={sorted(groups)}, expected={sorted(expected_keys)}")
    for key, group in groups.items():
        if len(group) != backend_reps:
            raise ValueError(f"backend point {key} has {len(group)} reps, expected={backend_reps}")
    return groups


def write_backend_summary(path, groups):
    header = [
        "n",
        "backend",
        "runs",
        "gbps_mean",
        "gbps_median",
        "gbps_min",
        "gbps_max",
        "gbps_cv_pct",
        "p50_mean",
        "p99_mean",
        "grow_wait_runs",
    ]
    with open(path, "w", newline="") as target:
        writer = csv.writer(target, lineterminator="\n")
        writer.writerow(header)
        for key in sorted(groups, key=lambda value: (int(value[0]), int(value[1]))):
            rows = groups[key]
            gbps = [number(row, "gbps") for row in rows]
            waits = [number(row, "grow_waits") for row in rows]
            writer.writerow(
                list(key)
                + [
                    len(rows),
                    f"{mean(gbps):.6f}",
                    f"{statistics.median(gbps):.6f}",
                    f"{min(gbps):.6f}",
                    f"{max(gbps):.6f}",
                    f"{cv_pct(gbps):.3f}",
                    f"{mean([number(row, 'p50') for row in rows]):.3f}",
                    f"{mean([number(row, 'p99') for row in rows]):.3f}",
                    sum(wait > 0 for wait in waits),
                ]
            )


def main():
    if len(sys.argv) != 8:
        print(
            "usage: analyze_sweep.py RESULTS BACKENDS SUMMARY BACKEND_SUMMARY "
            "REPS NUMA_REPS BACKEND_REPS",
            file=sys.stderr,
        )
        raise SystemExit(2)
    results_path, backends_path, summary_path, backend_summary_path = sys.argv[1:5]
    reps, numa_reps, backend_reps = map(int, sys.argv[5:8])

    result_groups = validate_results(read_rows(results_path), reps, numa_reps)
    backend_groups = validate_backends(read_rows(backends_path), backend_reps)
    write_summary(summary_path, result_groups)
    write_backend_summary(backend_summary_path, backend_groups)
    print(
        f"analyze_sweep: PASS results={len(result_groups)} points, "
        f"backends={len(backend_groups)} points",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
