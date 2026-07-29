#!/usr/bin/env python3
"""Host-only contract test for bench/suite/analyze_saturation.py."""

import csv
import importlib.util
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "bench" / "suite" / "analyze_saturation.py"
SPEC = importlib.util.spec_from_file_location("analyze_saturation", MODULE_PATH)
ANALYZE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYZE)


def write_csv(path, fields, rows):
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def main():
    fields = [
        "phase",
        "config",
        "frame_bytes",
        "offered_rps",
        "achieved_rps",
        "client_app_cores",
        "client_sidecar_cores",
        "server_app_cores",
        "server_sidecar_cores",
        "host_busy_cores",
        "client_core_busy_cores",
        "server_core_busy_cores",
        "dpu_arm_cores",
        "sla_clean",
        "achieved_ratio",
        "p99_us",
        "validation_status",
        "rep",
    ]
    rows = [
        {
            "phase": "idle",
            "config": "envoy-permissive",
            "frame_bytes": 0,
            "offered_rps": 0,
            "achieved_rps": 0,
            "client_app_cores": 0,
            "client_sidecar_cores": 0,
            "server_app_cores": 0,
            "server_sidecar_cores": 0,
            "host_busy_cores": 0.1,
            "client_core_busy_cores": 0,
            "server_core_busy_cores": 0,
            "dpu_arm_cores": "NA",
            "sla_clean": "NA",
            "achieved_ratio": "NA",
            "p99_us": 0,
        }
    ]
    for offered, achieved, client, clean, p99 in [
        (100_000, 100_000, 0.20, 1, 100),
        (200_000, 200_000, 0.40, 1, 110),
        (300_000, 300_000, 0.60, 1, 120),
        (400_000, 390_000, 0.96, 1, 130),
        # Achieved throughput can collapse after overload. Saturation ordering
        # must follow offered load, not the collapsed achieved value.
        (800_000, 250_000, 0.98, 0, 20_000),
        # A clean point after the first majority-bad rate is isolated and
        # cannot reopen the retained capacity prefix.
        (900_000, 300_000, 0.99, 1, 150),
    ]:
        for rep in range(2):
            rows.append(
                {
                    "phase": "load",
                    "config": "envoy-permissive",
                    "frame_bytes": 64,
                    "offered_rps": offered,
                    "achieved_rps": achieved,
                    "client_app_cores": client,
                    "client_sidecar_cores": 0,
                    "server_app_cores": client * 0.75,
                    "server_sidecar_cores": 0,
                    "host_busy_cores": 0.1 + offered * 2e-6,
                    "client_core_busy_cores": client,
                    "server_core_busy_cores": client * 0.75,
                    "dpu_arm_cores": "NA",
                    "sla_clean": str(clean if rep == 0 else 1),
                    "achieved_ratio": 1,
                    "p99_us": p99,
                    "validation_status": "ok",
                    "rep": rep + 1,
                }
            )

    # A targeted confirmation can add another complete three-run point at an
    # already retained offered rate. Aggregation must use all five votes.
    for rep in range(3):
        rows.append(
            {
                "phase": "load",
                "config": "envoy-permissive",
                "frame_bytes": 64,
                "offered_rps": 400_000,
                "achieved_rps": 390_000,
                "client_app_cores": 0.96,
                "client_sidecar_cores": 0,
                "server_app_cores": 0.72,
                "server_sidecar_cores": 0,
                "host_busy_cores": 0.9,
                "client_core_busy_cores": 0.96,
                "server_core_busy_cores": 0.72,
                "dpu_arm_cores": "NA",
                "sla_clean": "1",
                "achieved_ratio": 1,
                "p99_us": 130,
                "validation_status": "ok",
                "rep": rep + 1,
            }
        )

    with tempfile.TemporaryDirectory() as temp:
        dataset = Path(temp)
        write_csv(dataset / "results.csv", fields, rows)
        write_csv(
            dataset / "knees.csv",
            ["config", "frame_bytes", "highest_clean_rps"],
            [{"config": "envoy-permissive", "frame_bytes": 64,
              "highest_clean_rps": 800_000}],
        )
        write_csv(
            dataset / "generator_limits.csv",
            ["generator", "frame_bytes", "highest_clean_rps", "status"],
            [{"generator": "posix", "frame_bytes": 64,
              "highest_clean_rps": 2_000_000,
              "status": "bracketed"}],
        )
        write_csv(
            dataset / "rates.csv",
            ["frame_bytes", "config", "rate_index", "offered_rps", "source"],
            [
                {
                    "frame_bytes": 64,
                    "config": "envoy-permissive",
                    "rate_index": index,
                    "offered_rps": offered,
                    "source": "common" if index < 4 else "knee",
                }
                for index, offered in enumerate(
                    [100_000, 200_000, 300_000, 400_000, 800_000, 900_000], 1
                )
            ] + [{
                "frame_bytes": 64,
                "config": "envoy-permissive",
                "rate_index": 101,
                "offered_rps": 400_000,
                "source": "target-confirm",
            }],
        )
        results = ANALYZE.read_csv(dataset / "results.csv")
        knees = ANALYZE.load_knees(dataset / "knees.csv")
        saturation = ANALYZE.saturation_rows(results, knees, 0.95)
        limits = ANALYZE.load_generator_limits(dataset / "generator_limits.csv")
        stability = ANALYZE.knee_stability_rows(
            results, dataset / "knees.csv", limits
        )
        retained = ANALYZE.retained_capacity_rows(
            results, dataset / "rates.csv"
        )

    assert len(saturation) == 1
    row = dict(zip(ANALYZE.SATURATION_FIELDS, saturation[0]))
    assert row["fixed_budget_clean_rps"] == "800000.000"
    assert row["sat_start_rps"] == "390000.000"
    assert row["sat_endpoint"] == "client"
    assert row["sat_basis"] == "endpoint_physical_core"
    assert row["sat_client_cores"] == "0.960000"
    assert row["sat_server_cores"] == "0.720000"
    assert row["sat_clean_reps"] == 5
    assert row["sat_total_reps"] == 5
    assert row["n_unsaturated"] == 3
    assert row["slope_us_per_req"] == "2.000000"
    assert row["post_sat_amplification"] == "2.051282"
    assert row["note"] == "ok"

    knee = dict(zip(ANALYZE.KNEE_STABILITY_FIELDS, stability[0]))
    assert knee["retained_reps"] == 2
    assert knee["retained_clean"] == 1
    assert knee["max_p99_us"] == "20000.000000"
    assert knee["knee_unstable"] == 1
    assert knee["generator_ceiling_rps"] == "2000000.000"
    assert knee["generator_ceiling_status"] == "bracketed"
    assert knee["generator_headroom_ratio"] == "2.500000"
    capacity = dict(zip(ANALYZE.RETAINED_CAPACITY_FIELDS, retained[0]))
    assert capacity["stable_clean_rps"] == 400_000
    assert capacity["next_bad_rps"] == 800_000
    assert capacity["status"] == "bracketed"
    assert capacity["stable_clean_reps"] == 5
    assert capacity["stable_total_reps"] == 5
    assert capacity["isolated_clean_points_after_bad"] == 1
    print("analyze_saturation_test: PASS")


if __name__ == "__main__":
    main()
