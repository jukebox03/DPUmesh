#!/usr/bin/env python3
"""Reject incomplete, mismatched, or erroneous current L4 campaign data."""
import csv
import sys


MAIN_TRANSPORTS = {"dpumesh-native", "tcp-envoy", "tcp-direct"}
CPU_TRANSPORTS = {"dpumesh", "tcp", "direct"}


def validate_main(path, min_reps):
    rows = list(csv.DictReader(open(path)))
    seen = {r["transport"] for r in rows}
    if seen != MAIN_TRANSPORTS:
        raise ValueError(f"main transports {sorted(seen)} != {sorted(MAIN_TRANSPORTS)}")
    groups = {}
    for row in rows:
        for field in ("drops", "overflow", "fail", "reorder"):
            if int(float(row.get(field, 0) or 0)) != 0:
                raise ValueError(f"{field} != 0: {row}")
        if row.get("mrps") in ("", "NA", None):
            raise ValueError(f"missing metrics: {row}")
        point = tuple(row[c] for c in ("stage", "req_size", "reply_size", "conc", "threads"))
        groups.setdefault((row["transport"], point), set()).add(row["rep"])
    points = {t: {p for (tr, p) in groups if tr == t} for t in MAIN_TRANSPORTS}
    reference = points["dpumesh-native"]
    for transport, transport_points in points.items():
        if transport_points != reference:
            raise ValueError(f"point mismatch for {transport}")
    for key, reps in groups.items():
        if len(reps) < min_reps:
            raise ValueError(f"only {len(reps)} reps for {key}")
    return len(rows), len(reference)


def validate_cpu(path, min_reps):
    rows = list(csv.DictReader(open(path)))
    seen = {r["transport"] for r in rows}
    if seen != CPU_TRANSPORTS:
        raise ValueError(f"CPU transports {sorted(seen)} != {sorted(CPU_TRANSPORTS)}")
    groups = {}
    for row in rows:
        point = tuple(row[c] for c in ("req", "reply", "conc", "threads"))
        groups.setdefault((row["transport"], point), set()).add(row["rep"])
    points = {t: {p for (tr, p) in groups if tr == t} for t in CPU_TRANSPORTS}
    reference = points["dpumesh"]
    for transport, transport_points in points.items():
        if transport_points != reference:
            raise ValueError(f"CPU point mismatch for {transport}")
    for key, reps in groups.items():
        if len(reps) < min_reps:
            raise ValueError(f"only {len(reps)} CPU reps for {key}")
    return len(rows), len(reference)


def main():
    if len(sys.argv) != 4:
        print("usage: validate_l4.py <tidy.csv> <cpu.csv> <min_reps>", file=sys.stderr)
        raise SystemExit(2)
    min_reps = int(sys.argv[3])
    main_rows, main_points = validate_main(sys.argv[1], min_reps)
    cpu_rows, cpu_points = validate_cpu(sys.argv[2], min_reps)
    print(f"validate_l4: PASS main={main_rows} rows/{main_points} points/transport "
          f"cpu={cpu_rows} rows/{cpu_points} points/transport reps>={min_reps}")


if __name__ == "__main__":
    main()
