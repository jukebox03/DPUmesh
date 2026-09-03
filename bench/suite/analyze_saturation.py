#!/usr/bin/env python3
"""Derive saturation and retained-knee stability from an L4 dataset.

Input is an L4 open-loop dataset directory (results.csv, rates.csv, optional
knees.csv and generator_limits.csv); the files are read-only.  Repetitions at
the same offered rate are collapsed to a median point before saturation
detection and regression, so an interrupted run cannot give one rate
disproportionate weight.
"""

import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path


SATURATION_FIELDS = [
    "config",
    "frame_bytes",
    "fixed_budget_clean_rps",
    "sat_start_rps",
    "sat_endpoint",
    "sat_basis",
    "sat_client_cores",
    "sat_server_cores",
    "sat_clean_reps",
    "sat_total_reps",
    "n_unsaturated",
    "slope_us_per_req",
    "client_slope_us_per_req",
    "server_slope_us_per_req",
    "intercept_cores",
    "r2",
    "arm_slope_us_per_req",
    "knee_rps",
    "post_sat_amplification",
    "note",
]

KNEE_STABILITY_FIELDS = [
    "config",
    "frame_bytes",
    "knee_rps",
    "retained_reps",
    "retained_clean",
    "min_achieved_ratio",
    "max_p99_us",
    "knee_unstable",
    "generator_ceiling_rps",
    "generator_ceiling_status",
    "generator_headroom_ratio",
]

RETAINED_CAPACITY_FIELDS = [
    "config",
    "frame_bytes",
    "stable_clean_rps",
    "next_bad_rps",
    "status",
    "stable_clean_reps",
    "stable_total_reps",
    "next_bad_clean_reps",
    "next_bad_total_reps",
    "stable_achieved_rps",
    "stable_p99_us",
    "stable_client_busy_cores",
    "stable_server_busy_cores",
    "stable_host_busy_cores",
    "stable_dpu_arm_cores",
    "capacity_signal",
    "isolated_clean_points_after_bad",
]


def read_csv(path):
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def number(value):
    if value in (None, "", "NA"):
        return None
    try:
        result = float(value)
    except ValueError:
        return None
    return result if math.isfinite(result) else None


def median(values):
    usable = [value for value in values if value is not None]
    return statistics.median(usable) if usable else None


def fmt(value, digits=6):
    if value is None or not math.isfinite(value):
        return "NA"
    return f"{value:.{digits}f}"


def linear_fit(points):
    """Return slope, intercept, and R², or None for a degenerate fit."""
    if len(points) < 2:
        return None
    x_mean = statistics.fmean(point[0] for point in points)
    y_mean = statistics.fmean(point[1] for point in points)
    sxx = sum((x - x_mean) ** 2 for x, _ in points)
    if sxx == 0:
        return None
    slope = sum((x - x_mean) * (y - y_mean) for x, y in points) / sxx
    intercept = y_mean - slope * x_mean
    residual = sum((y - (intercept + slope * x)) ** 2 for x, y in points)
    total = sum((y - y_mean) ** 2 for _, y in points)
    r2 = 1.0 if total == 0 and residual == 0 else (1.0 - residual / total if total else None)
    return slope, intercept, r2


def load_knees(path):
    knees = {}
    if not path.is_file():
        return knees
    for row in read_csv(path):
        knee = number(row.get("highest_clean_rps"))
        if knee is not None:
            knees[(row["config"], int(row["frame_bytes"]))] = knee
    return knees


def aggregate_load_points(results):
    groups = defaultdict(list)
    for row in results:
        if row.get("phase") != "load":
            continue
        payload = number(row.get("frame_bytes"))
        offered = number(row.get("offered_rps"))
        if payload is None or offered is None:
            continue
        groups[(row["config"], int(payload), int(offered))].append(row)

    by_series = defaultdict(list)
    for (config, payload, offered), rows in groups.items():
        def field(name):
            return median(number(row.get(name)) for row in rows)

        app = field("client_app_cores")
        sidecar = field("client_sidecar_cores")
        server_app = field("server_app_cores")
        server_sidecar = field("server_sidecar_cores")
        by_series[(config, payload)].append(
            {
                "offered_rps": offered,
                "achieved_rps": field("achieved_rps"),
                "client_cores": None if app is None or sidecar is None else app + sidecar,
                "server_cores": (
                    None
                    if server_app is None or server_sidecar is None
                    else server_app + server_sidecar
                ),
                "host_busy_cores": field("host_busy_cores"),
                "client_core_busy": field("client_core_busy_cores"),
                "server_core_busy": field("server_core_busy_cores"),
                "dpu_arm_cores": field("dpu_arm_cores"),
                "clean_reps": sum(row.get("served_clean") == "1" for row in rows),
                "total_reps": len(rows),
            }
        )
    return by_series


def saturation_rows(results, knees, threshold):
    idle = defaultdict(list)
    for row in results:
        if row.get("phase") == "idle":
            idle[row["config"]].append(number(row.get("host_busy_cores")))
    idle_medians = {config: median(values) for config, values in idle.items()}

    output = []
    for (config, payload), points in sorted(aggregate_load_points(results).items()):
        points = [point for point in points if point["achieved_rps"] is not None]
        points.sort(key=lambda point: point["offered_rps"])
        use_physical_cores = all(
            point["client_core_busy"] is not None
            and point["server_core_busy"] is not None
            for point in points
        )
        sat_basis = (
            "endpoint_physical_core"
            if use_physical_cores
            else "app_sidecar_cgroup"
        )

        def client_saturation_cores(point):
            return (
                point["client_core_busy"]
                if use_physical_cores
                else point["client_cores"]
            )

        def server_saturation_cores(point):
            return (
                point["server_core_busy"]
                if use_physical_cores
                else point["server_cores"]
            )

        saturated = [
            point for point in points
            if (
                client_saturation_cores(point) is not None
                and client_saturation_cores(point) >= threshold
            )
            or (
                server_saturation_cores(point) is not None
                and server_saturation_cores(point) >= threshold
            )
        ]
        sat_start = saturated[0]["achieved_rps"] if saturated else None
        sat_client_cores = (
            client_saturation_cores(saturated[0]) if saturated else None
        )
        sat_server_cores = (
            server_saturation_cores(saturated[0]) if saturated else None
        )
        sat_endpoint = "NA"
        if saturated:
            client_sat = (
                sat_client_cores is not None and sat_client_cores >= threshold
            )
            server_sat = (
                sat_server_cores is not None and sat_server_cores >= threshold
            )
            sat_endpoint = (
                "both" if client_sat and server_sat
                else "client" if client_sat
                else "server"
            )
        sat_offered = saturated[0]["offered_rps"] if saturated else None
        unsaturated = [
            point for point in points
            if sat_offered is None or point["offered_rps"] < sat_offered
        ]

        notes = []
        if sat_start is None:
            notes.append("never_saturated")
        elif not unsaturated:
            notes.append("all_points_saturated")

        host_fit = None
        client_fit = None
        server_fit = None
        arm_fit = None
        if len(unsaturated) < 3:
            notes.append("insufficient_unsaturated_points")
        else:
            idle_value = idle_medians.get(config)
            host_points = [
                (point["achieved_rps"], point["host_busy_cores"] - idle_value)
                for point in unsaturated
                if point["host_busy_cores"] is not None and idle_value is not None
            ]
            client_points = [
                (point["achieved_rps"], point["client_cores"])
                for point in unsaturated
                if point["client_cores"] is not None
            ]
            server_points = [
                (point["achieved_rps"], point["server_cores"])
                for point in unsaturated
                if point["server_cores"] is not None
            ]
            arm_points = [
                (point["achieved_rps"], point["dpu_arm_cores"])
                for point in unsaturated
                if point["dpu_arm_cores"] is not None
            ]
            if len(host_points) >= 3:
                host_fit = linear_fit(host_points)
            if len(client_points) >= 3:
                client_fit = linear_fit(client_points)
            if len(server_points) >= 3:
                server_fit = linear_fit(server_points)
            if len(arm_points) >= 3:
                arm_fit = linear_fit(arm_points)
            if host_fit is None:
                notes.append("insufficient_host_cpu_points")
            elif host_fit[2] is not None and host_fit[2] < 0.9:
                notes.append("nonlinear_host_fit")
            if arm_fit is None and any(
                point["dpu_arm_cores"] is not None for point in unsaturated
            ):
                notes.append("insufficient_arm_cpu_points")

        knee = knees.get((config, payload))
        amplification = (
            knee / sat_start
            if knee is not None and sat_start is not None and sat_start > 0
            else None
        )
        output.append(
            [
                config,
                payload,
                fmt(knee, 3),
                fmt(sat_start, 3),
                sat_endpoint,
                sat_basis,
                fmt(sat_client_cores),
                fmt(sat_server_cores),
                saturated[0]["clean_reps"] if saturated else "NA",
                saturated[0]["total_reps"] if saturated else "NA",
                len(unsaturated),
                fmt(host_fit[0] * 1_000_000.0 if host_fit else None),
                fmt(client_fit[0] * 1_000_000.0 if client_fit else None),
                fmt(server_fit[0] * 1_000_000.0 if server_fit else None),
                fmt(host_fit[1] if host_fit else None),
                fmt(host_fit[2] if host_fit else None),
                fmt(arm_fit[0] * 1_000_000.0 if arm_fit else None),
                fmt(knee, 3),
                fmt(amplification),
                ";".join(notes) if notes else "ok",
            ]
        )
    return output


def load_generator_limits(path):
    limits = {}
    if not path.is_file():
        return limits
    for row in read_csv(path):
        ceiling = number(row.get("highest_clean_rps"))
        if ceiling is not None and ceiling > 0:
            payload = number(row.get("frame_bytes"))
            if payload is not None:
                limits[(row["generator"], int(payload))] = (
                    ceiling,
                    row.get("status") or "NA",
                )
    return limits


def retained_capacity_rows(results, rates_path, saturation_threshold=0.95):
    """Select the monotonic, majority-clean prefix of repeated retained points."""
    if not rates_path.is_file():
        return []
    planned = {}
    for row in read_csv(rates_path):
        if row.get("source") == "span":
            continue
        planned[
            (row["config"], int(row["frame_bytes"]), int(row["offered_rps"]))
        ] = row

    grouped = defaultdict(list)
    for row in results:
        if row.get("phase") != "load":
            continue
        key = (
            row["config"],
            int(float(row["frame_bytes"])),
            int(float(row["offered_rps"])),
        )
        if key not in planned:
            continue
        grouped[key].append(row)
    if not grouped:
        return []

    series = defaultdict(list)
    for (config, payload, offered), rows in grouped.items():
        clean_rows = [
            row
            for row in rows
            if row.get("validation_status") == "ok" and row.get("served_clean") == "1"
        ]
        valid_rows = [row for row in rows if row.get("validation_status") == "ok"]
        majority = len(rows) // 2 + 1
        series[(config, payload)].append(
            {
                "offered": offered,
                "rows": rows,
                "clean_rows": clean_rows,
                "valid_rows": valid_rows,
                "accepted": (
                    len(valid_rows) == len(rows)
                    and len(clean_rows) >= majority
                ),
            }
        )

    output = []
    for (config, payload), points in sorted(series.items()):
        points.sort(key=lambda point: point["offered"])
        stable = None
        next_bad = None
        bad_seen = False
        isolated = 0
        for point in points:
            if not bad_seen and point["accepted"]:
                stable = point
            elif not point["accepted"]:
                if not bad_seen:
                    next_bad = point
                bad_seen = True
            else:
                isolated += 1
        if stable is None:
            continue

        stable_rows = stable["clean_rows"]
        field = lambda name: median(number(row.get(name)) for row in stable_rows)
        client = field("client_core_busy_cores")
        server = field("server_core_busy_cores")
        host_saturated = (
            (client is not None and client >= saturation_threshold)
            or (server is not None and server >= saturation_threshold)
        )
        if host_saturated:
            signal = "host_core"
        elif config.startswith("dpumesh-"):
            signal = "dmesh_or_queue"
        else:
            signal = "sla_before_host_saturation"
        output.append(
            [
                config,
                payload,
                stable["offered"],
                next_bad["offered"] if next_bad else "NA",
                "bracketed" if next_bad else "right_censored",
                len(stable["clean_rows"]),
                len(stable["rows"]),
                len(next_bad["clean_rows"]) if next_bad else "NA",
                len(next_bad["rows"]) if next_bad else "NA",
                fmt(field("achieved_rps"), 3),
                fmt(field("p99_us"), 3),
                fmt(client),
                fmt(server),
                fmt(field("host_busy_cores")),
                fmt(field("dpu_arm_cores")),
                signal,
                isolated,
            ]
        )
    return output


def retained_capacity_map(rows):
    return {
        (row[0], int(row[1])): float(row[2])
        for row in rows
        if row[2] != "NA"
    }


def knee_stability_rows(results, knee_path, generator_limits=None):
    if not knee_path.is_file():
        return []
    load = defaultdict(list)
    for row in results:
        if row.get("phase") != "load":
            continue
        payload = number(row.get("frame_bytes"))
        offered = number(row.get("offered_rps"))
        if payload is None or offered is None:
            continue
        load[(row["config"], int(payload), int(offered))].append(row)

    output = []
    generator_limits = generator_limits or {}
    for knee_row in read_csv(knee_path):
        knee = number(knee_row.get("highest_clean_rps"))
        if knee is None:
            continue
        config = knee_row["config"]
        payload = int(knee_row["frame_bytes"])
        rows = load.get((config, payload, int(knee)), [])
        clean = sum(row.get("served_clean") == "1" for row in rows)
        ratios = [number(row.get("achieved_ratio")) for row in rows]
        p99s = [number(row.get("p99_us")) for row in rows]
        generator = "native" if config == "dpumesh-native" else "posix"
        ceiling, ceiling_status = generator_limits.get(
            (generator, payload), (None, "NA")
        )
        output.append(
            [
                config,
                payload,
                fmt(knee, 3),
                len(rows),
                clean,
                fmt(min((value for value in ratios if value is not None), default=None)),
                fmt(max((value for value in p99s if value is not None), default=None)),
                int(clean < len(rows)),
                fmt(ceiling, 3),
                ceiling_status,
                fmt(ceiling / knee if ceiling and knee > 0 else None),
            ]
        )
    return output


def write_csv(path, fields, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(fields)
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(
        description="derive saturation.csv and knee_stability.csv from an L4 dataset"
    )
    parser.add_argument("input", type=Path, help="dataset directory or results.csv")
    parser.add_argument("--knees", type=Path, help="override knees.csv path")
    parser.add_argument("--out", type=Path, help="override saturation.csv path")
    parser.add_argument("--knee-out", type=Path, help="override knee_stability.csv path")
    parser.add_argument(
        "--capacity-out", type=Path, help="override retained_capacity.csv path"
    )
    parser.add_argument(
        "--sat-threshold",
        type=float,
        default=0.95,
        help="physical endpoint-core busy fraction that marks saturation (default: 0.95)",
    )
    args = parser.parse_args()
    if not 0 < args.sat_threshold <= 1:
        parser.error("--sat-threshold must be in (0, 1]")

    source = args.input.resolve()
    dataset = source if source.is_dir() else source.parent
    results_path = dataset / "results.csv" if source.is_dir() else source
    knees_path = (args.knees or dataset / "knees.csv").resolve()
    generator_limits_path = dataset / "generator_limits.csv"
    rates_path = dataset / "rates.csv"
    saturation_path = (args.out or dataset / "saturation.csv").resolve()
    knee_output_path = (args.knee_out or dataset / "knee_stability.csv").resolve()
    capacity_output_path = (
        args.capacity_out or dataset / "retained_capacity.csv"
    ).resolve()

    results = read_csv(results_path)
    retained_rows = retained_capacity_rows(results, rates_path, args.sat_threshold)
    capacities = retained_capacity_map(retained_rows)
    if not capacities:
        capacities = load_knees(knees_path)
    sat_rows = saturation_rows(results, capacities, args.sat_threshold)
    generator_limits = load_generator_limits(generator_limits_path)
    stability_rows = knee_stability_rows(results, knees_path, generator_limits)
    write_csv(saturation_path, SATURATION_FIELDS, sat_rows)
    write_csv(knee_output_path, KNEE_STABILITY_FIELDS, stability_rows)
    write_csv(capacity_output_path, RETAINED_CAPACITY_FIELDS, retained_rows)
    print(
        f"analyze_saturation: series={len(sat_rows)} knees={len(stability_rows)} "
        f"retained={len(retained_rows)} saturation={saturation_path} "
        f"knee_stability={knee_output_path}"
    )


if __name__ == "__main__":
    main()
