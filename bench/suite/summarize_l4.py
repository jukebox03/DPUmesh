#!/usr/bin/env python3
"""Summarize a complete or interrupted l4_proxy_data.sh dataset.

Raw collector files are never modified. Derived CSVs are written below
<dataset>/summary by default.
"""

import argparse
import csv
import re
import statistics
from collections import Counter, defaultdict
from pathlib import Path


METRICS = [
    "achieved_rps",
    "achieved_ratio",
    "p50_us",
    "p99_us",
    "endpoint_app_cores",
    "endpoint_app_user_cores",
    "endpoint_app_system_cores",
    "endpoint_sidecar_cores",
    "endpoint_sidecar_user_cores",
    "endpoint_sidecar_system_cores",
    "host_cgroup_cores",
    "host_cgroup_user_cores",
    "host_cgroup_system_cores",
    "host_busy_cores",
    "client_app_cores",
    "server_app_cores",
    "client_sidecar_cores",
    "server_sidecar_cores",
    "host_irq_cores",
    "host_softirq_cores",
    "system_softirq_cores",
    "dpu_arm_cores",
]


def read_csv(path):
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def perf_enabled(path):
    match = re.search(r"\bENABLE_PERF=([01])\b", path.read_text())
    return match is None or match.group(1) == "1"


def number(value):
    if value in (None, "", "NA"):
        return None
    return float(value)


def median(values):
    usable = [value for value in values if value is not None]
    return statistics.median(usable) if usable else None


def fmt(value, digits=6):
    return "NA" if value is None else f"{value:.{digits}f}"


def metric_values(rows, name):
    return [number(row.get(name)) for row in rows]


def metric_stats(rows, name):
    values = [value for value in metric_values(rows, name) if value is not None]
    if not values:
        return ["NA", "NA", "NA"]
    return [fmt(statistics.median(values)), fmt(min(values)), fmt(max(values))]


def summed(rows, name):
    return sum(int(float(row.get(name, 0) or 0)) for row in rows)


def add_derived_cpu(row):
    for output, inputs in {
        "endpoint_app_cores": ["client_app_cores", "server_app_cores"],
        "endpoint_app_user_cores": [
            "client_app_user_cores",
            "server_app_user_cores",
        ],
        "endpoint_app_system_cores": [
            "client_app_system_cores",
            "server_app_system_cores",
        ],
        "endpoint_sidecar_cores": [
            "client_sidecar_cores",
            "server_sidecar_cores",
        ],
        "endpoint_sidecar_user_cores": [
            "client_sidecar_user_cores",
            "server_sidecar_user_cores",
        ],
        "endpoint_sidecar_system_cores": [
            "client_sidecar_system_cores",
            "server_sidecar_system_cores",
        ],
    }.items():
        values = [number(row.get(name)) for name in inputs]
        row[output] = (
            "NA" if any(value is None for value in values) else str(sum(values))
        )


def parse_meta(meta_path):
    text = meta_path.read_text()
    reps = re.search(r"\bREPS=(\d+)", text)
    idle_reps = re.search(r"\bIDLE_REPS=(\d+)", text)
    configs = re.search(r"\bCONFIGS=\{([^}]*)\}", text)
    if not reps:
        raise SystemExit(f"cannot parse REPS in {meta_path}")
    selected = configs.group(1).split() if configs else []
    return selected, int(reps.group(1)), int(
        idle_reps.group(1) if idle_reps else reps.group(1)
    )


def write_csv(path, header, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(header)
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dataset", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()

    dataset = args.dataset.resolve()
    output = (args.out or dataset / "summary").resolve()
    results = read_csv(dataset / "results.csv")
    for row in results:
        add_derived_cpu(row)
    rates = read_csv(dataset / "rates.csv")
    configs, expected_reps, expected_idle_reps = parse_meta(dataset / "meta.txt")
    if not configs:
        configs = list(dict.fromkeys(row["config"] for row in rates))
    config_rank = {config: index for index, config in enumerate(configs)}

    idle_rows = [row for row in results if row["phase"] == "idle"]
    load_rows = [row for row in results if row["phase"] == "load"]
    idle_by_config = defaultdict(list)
    for row in idle_rows:
        idle_by_config[row["config"]].append(row)

    idle_host = {
        config: median(metric_values(rows, "host_cgroup_cores"))
        for config, rows in idle_by_config.items()
    }
    idle_dpu = {
        config: median(metric_values(rows, "dpu_arm_cores"))
        for config, rows in idle_by_config.items()
    }

    groups = defaultdict(list)
    for row in load_rows:
        key = (
            int(row["frame_bytes"]),
            row["config"],
            int(row["rate_index"]),
            int(row["offered_rps"]),
        )
        groups[key].append(row)

    rate_rows = sorted(
        rates,
        key=lambda row: (
            int(row["frame_bytes"]),
            config_rank[row["config"]],
            int(row["rate_index"]),
        ),
    )
    point_header = [
        "frame_bytes",
        "config",
        "rate_index",
        "offered_rps",
        "source",
        "min_knee_rps",
        "config_knee_rps",
        "expected_reps",
        "observed_reps",
        "missing_reps",
        "validation_ok_reps",
        "clean_reps",
        "clean_fraction",
        "clean_reasons",
    ]
    for metric in METRICS:
        point_header.extend([f"{metric}_med", f"{metric}_min", f"{metric}_max"])
    point_header.extend(
        [
            "host_incremental_cores_med",
            "host_cores_per_mrps_med",
            "dpu_incremental_cores_med",
            "admission_drops_sum",
            "fail_sum",
            "overflow_sum",
            "reorder_sum",
        ]
    )

    point_rows = []
    missing_rows = []
    common_rows = []
    knee_rows = []
    for rate in rate_rows:
        key = (
            int(rate["frame_bytes"]),
            rate["config"],
            int(rate["rate_index"]),
            int(rate["offered_rps"]),
        )
        rows = sorted(groups.get(key, []), key=lambda row: int(row["rep"]))
        point_expected_reps = 1 if rate["source"] == "span" else expected_reps
        present = {int(row["rep"]) for row in rows}
        for rep in range(1, point_expected_reps + 1):
            if rep not in present:
                missing_rows.append(
                    [
                        f"load-{key[0]}-q{key[2]}-{key[1]}-r{rep}",
                        key[0],
                        key[1],
                        key[2],
                        key[3],
                        rate["source"],
                        rep,
                    ]
                )

        clean_reasons = Counter(row["clean_reason"] for row in rows)
        clean_reasons_text = ";".join(
            f"{reason}:{count}" for reason, count in sorted(clean_reasons.items())
        )
        clean_count = sum(row["served_clean"] == "1" for row in rows)
        point = [
            key[0],
            key[1],
            key[2],
            key[3],
            rate["source"],
            rate["min_knee_rps"],
            rate["config_knee_rps"],
            point_expected_reps,
            len(rows),
            point_expected_reps - len(rows),
            sum(row["validation_status"] == "ok" for row in rows),
            clean_count,
            fmt(clean_count / len(rows) if rows else None),
            clean_reasons_text or "NA",
        ]
        for metric in METRICS:
            point.extend(metric_stats(rows, metric))

        host_incremental = []
        host_per_mrps = []
        dpu_incremental = []
        for row in rows:
            host = number(row["host_cgroup_cores"])
            achieved = number(row["achieved_rps"])
            dpu = number(row["dpu_arm_cores"])
            if host is not None:
                incremental = max(0.0, host - (idle_host.get(key[1]) or 0.0))
                host_incremental.append(incremental)
                if achieved and achieved > 0:
                    host_per_mrps.append(incremental * 1_000_000.0 / achieved)
            if dpu is not None:
                dpu_incremental.append(max(0.0, dpu - (idle_dpu.get(key[1]) or 0.0)))
        point.extend(
            [
                fmt(median(host_incremental)),
                fmt(median(host_per_mrps)),
                fmt(median(dpu_incremental)),
                summed(rows, "admission_drops"),
                summed(rows, "fail"),
                summed(rows, "overflow"),
                summed(rows, "reorder"),
            ]
        )
        point_rows.append(point)
        if "common" in rate["source"]:
            common_rows.append(point)
        if "knee" in rate["source"]:
            knee_rows.append(point)

    idle_header = [
        "config",
        "expected_reps",
        "observed_reps",
        "host_cgroup_cores_med",
        "host_cgroup_cores_min",
        "host_cgroup_cores_max",
        "host_busy_cores_med",
        "host_busy_cores_min",
        "host_busy_cores_max",
        "dpu_arm_cores_med",
        "dpu_arm_cores_min",
        "dpu_arm_cores_max",
    ]
    idle_summary = []
    for config in configs:
        rows = idle_by_config[config]
        idle_summary.append(
            [config, expected_idle_reps, len(rows)]
            + metric_stats(rows, "host_cgroup_cores")
            + metric_stats(rows, "host_busy_cores")
            + metric_stats(rows, "dpu_arm_cores")
        )

    integrity_header = ["check", "value"]
    integrity_rows = [
        [
            "expected_load_rows",
            sum(1 if row["source"] == "span" else expected_reps for row in rates),
        ],
        ["observed_load_rows", len(load_rows)],
        ["missing_load_rows", len(missing_rows)],
        ["expected_idle_rows", len(configs) * expected_idle_reps],
        ["observed_idle_rows", len(idle_rows)],
        [
            "invalid_validation_status_rows",
            sum(row["validation_status"] != "ok" for row in results),
        ],
        ["fail_sum", summed(load_rows, "fail")],
        ["overflow_sum", summed(load_rows, "overflow")],
        ["reorder_sum", summed(load_rows, "reorder")],
        ["clean_load_rows", sum(row["served_clean"] == "1" for row in load_rows)],
        ["unclean_load_rows", sum(row["served_clean"] == "0" for row in load_rows)],
        [
            "expected_perf_profiles",
            (
                len(configs) * len({row["frame_bytes"] for row in rates})
                if perf_enabled(dataset / "meta.txt")
                else 0
            ),
        ],
        [
            "observed_perf_profiles",
            max(0, sum(1 for _ in (dataset / "perf_manifest.csv").open()) - 1),
        ],
    ]

    write_csv(output / "points.csv", point_header, point_rows)
    write_csv(output / "common_points.csv", point_header, common_rows)
    write_csv(output / "knee_points.csv", point_header, knee_rows)
    write_csv(output / "idle.csv", idle_header, idle_summary)
    write_csv(
        output / "missing_runs.csv",
        [
            "run_id",
            "frame_bytes",
            "config",
            "rate_index",
            "offered_rps",
            "source",
            "rep",
        ],
        missing_rows,
    )
    write_csv(output / "integrity.csv", integrity_header, integrity_rows)

    print(
        f"summarize_l4: load={len(load_rows)}/"
        f"{sum(1 if row['source'] == 'span' else expected_reps for row in rates)}, "
        f"idle={len(idle_rows)}/{len(configs) * expected_idle_reps}, "
        f"missing={len(missing_rows)}, output={output}"
    )


if __name__ == "__main__":
    main()
