#!/usr/bin/env python3
"""Select the repeated, monotonic clean prefix of a gRPC open-loop sweep."""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path


OUTPUT_FIELDS = [
    "channels",
    "reactors",
    "frame_bytes",
    "highest_clean_rps",
    "next_bad_rps",
    "status",
    "max_p50_us",
    "highest_healthy_rps",
    "next_unhealthy_rps",
    "healthy_status",
    "clean_reps",
    "valid_reps",
    "total_reps",
    "achieved_rps",
    "achieved_ratio",
    "p50_us",
    "p99_us",
    "p999_us",
    "client_cores",
    "server_cores",
    "dpu_arm_cores",
]

REQUIRED_NUMBERS = [
    "channels",
    "reactors",
    "frame",
    "rep",
    "offered",
    "achieved",
    "p50_us",
    "p99_us",
    "p999_us",
    "fail",
    "drops",
    "pending",
    "worker_fail",
    "credit_hold_dropped",
    "eq_budget_exhausted",
    "client_core",
    "server_core",
    "dpu_core_workers",
]

ZERO_FIELDS = [
    "fail",
    "drops",
    "pending",
    "worker_fail",
    "credit_hold_dropped",
    "eq_budget_exhausted",
]


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def number(value: str | None) -> float | None:
    if value in (None, "", "NA"):
        return None
    try:
        result = float(value)
    except ValueError:
        return None
    return result if math.isfinite(result) else None


def median(rows: list[dict[str, str]], field: str) -> float | None:
    values = [number(row.get(field)) for row in rows]
    usable = [value for value in values if value is not None]
    return statistics.median(usable) if usable else None


def fmt(value: float | None, digits: int = 3) -> str:
    return "NA" if value is None else f"{value:.{digits}f}"


def valid(row: dict[str, str]) -> bool:
    return all(number(row.get(field)) is not None for field in REQUIRED_NUMBERS)


def clean(row: dict[str, str], min_ratio: float) -> bool:
    if not valid(row):
        return False
    offered = number(row["offered"])
    achieved = number(row["achieved"])
    assert offered is not None and achieved is not None
    return (
        offered > 0
        and achieved / offered >= min_ratio
        and all(number(row[field]) == 0 for field in ZERO_FIELDS)
    )


def analyze(
    rows: list[dict[str, str]],
    crashes: list[dict[str, str]],
    min_reps: int,
    min_ratio: float,
    max_p50_us: float,
) -> list[list[object]]:
    groups: dict[tuple[int, int, int, int], list[dict[str, str] | None]] = (
        defaultdict(list)
    )
    for row in rows:
        key_values = [number(row.get(field)) for field in
                      ("channels", "reactors", "frame", "offered")]
        if any(value is None for value in key_values):
            continue
        groups[tuple(int(value) for value in key_values)].append(row)  # type: ignore[arg-type]

    # The sweep removes a point that coincided with a Pod restart. Preserve the
    # failed repetition in the vote even when no usable result row survived.
    for row in crashes:
        key_values = [number(row.get(field)) for field in
                      ("channels", "reactors", "frame", "offered")]
        if any(value is None for value in key_values):
            continue
        groups[tuple(int(value) for value in key_values)].append(None)  # type: ignore[arg-type]

    series: dict[tuple[int, int, int], list[dict[str, object]]] = defaultdict(list)
    for (channels, reactors, frame, offered), repetitions in groups.items():
        present = [row for row in repetitions if row is not None]
        valid_rows = [row for row in present if valid(row)]
        clean_rows = [row for row in valid_rows if clean(row, min_ratio)]
        majority = len(repetitions) // 2 + 1
        p50 = median(clean_rows, "p50_us")
        accepted = (
            len(repetitions) >= min_reps
            and len(valid_rows) == len(repetitions)
            and len(clean_rows) >= majority
        )
        series[(channels, reactors, frame)].append(
            {
                "offered": offered,
                "rows": present,
                "valid": valid_rows,
                "clean": clean_rows,
                "total": len(repetitions),
                "accepted": accepted,
                "healthy": accepted and p50 is not None and p50 <= max_p50_us,
            }
        )

    output: list[list[object]] = []
    for (channels, reactors, frame), points in sorted(series.items()):
        points.sort(key=lambda point: int(point["offered"]))
        stable = None
        next_bad = None
        bad_seen = False
        healthy_stable = None
        next_unhealthy = None
        unhealthy_seen = False
        for point in points:
            if not bad_seen and point["accepted"]:
                stable = point
            elif not point["accepted"]:
                if not bad_seen:
                    next_bad = point
                bad_seen = True
            if not unhealthy_seen and point["healthy"]:
                healthy_stable = point
            elif not point["healthy"]:
                if not unhealthy_seen:
                    next_unhealthy = point
                unhealthy_seen = True

        healthy_rate = (
            int(healthy_stable["offered"]) if healthy_stable else "NA"
        )
        healthy_status = (
            "no_healthy_prefix" if healthy_stable is None
            else "bracketed" if next_unhealthy else "right_censored"
        )

        if stable is None:
            output.append([
                channels, reactors, frame, "NA",
                int(next_bad["offered"]) if next_bad else "NA",
                "no_clean_prefix", fmt(max_p50_us), healthy_rate,
                int(next_unhealthy["offered"]) if next_unhealthy else "NA",
                healthy_status, 0, 0, 0,
                "NA", "NA", "NA", "NA", "NA", "NA", "NA", "NA",
            ])
            continue

        clean_rows = stable["clean"]
        assert isinstance(clean_rows, list)
        achieved = median(clean_rows, "achieved")
        offered = int(stable["offered"])
        output.append([
            channels,
            reactors,
            frame,
            offered,
            int(next_bad["offered"]) if next_bad else "NA",
            "bracketed" if next_bad else "right_censored",
            fmt(max_p50_us),
            healthy_rate,
            int(next_unhealthy["offered"]) if next_unhealthy else "NA",
            healthy_status,
            len(clean_rows),
            len(stable["valid"]),
            stable["total"],
            fmt(achieved),
            fmt(achieved / offered if achieved is not None else None, 6),
            fmt(median(clean_rows, "p50_us")),
            fmt(median(clean_rows, "p99_us")),
            fmt(median(clean_rows, "p999_us")),
            fmt(median(clean_rows, "client_core"), 6),
            fmt(median(clean_rows, "server_core"), 6),
            fmt(median(clean_rows, "dpu_core_workers"), 6),
        ])
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="points.csv or its directory")
    parser.add_argument(
        "--crashes", type=Path,
        help="crash CSV (default: crashes.csv beside the input)",
    )
    parser.add_argument("--out", type=Path, help="output knees.csv")
    parser.add_argument("--min-reps", type=int, default=3)
    parser.add_argument("--min-ratio", type=float, default=0.99)
    parser.add_argument("--max-p50-us", type=float, default=1000.0)
    args = parser.parse_args()
    if args.min_reps < 1:
        parser.error("--min-reps must be positive")
    if not 0 < args.min_ratio <= 1:
        parser.error("--min-ratio must be in (0, 1]")
    if args.max_p50_us <= 0:
        parser.error("--max-p50-us must be positive")

    points = args.input / "points.csv" if args.input.is_dir() else args.input
    directory = points.parent
    crashes = args.crashes or directory / "crashes.csv"
    output = args.out or directory / "knees.csv"
    result = analyze(
        read_csv(points), read_csv(crashes),
        args.min_reps, args.min_ratio, args.max_p50_us,
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(OUTPUT_FIELDS)
        writer.writerows(result)
    print(f"analyze_grpc_sweep: series={len(result)} knees={output}")


if __name__ == "__main__":
    main()
