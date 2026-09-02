#!/usr/bin/env python3

import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "bench" / "suite" / "analyze_grpc_sweep.py"
SPEC = importlib.util.spec_from_file_location("analyze_grpc_sweep", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
ANALYZE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYZE)


def row(rate: int, rep: int, ratio: float = 1.0, **changes: object) -> dict[str, str]:
    values = {
        "channels": "8", "reactors": "8", "frame": "64", "rep": str(rep),
        "offered": str(rate), "achieved": str(rate * ratio), "ratio": str(ratio),
        "p50_us": "800", "p99_us": "1600", "p999_us": "3000",
        "fail": "0", "drops": "0", "pending": "0", "worker_fail": "0",
        "credit_hold_dropped": "0", "eq_budget_exhausted": "0",
        "client_core": "1.0", "server_core": "1.0",
        "dpu_core_workers": "4.0",
    }
    values.update({key: str(value) for key, value in changes.items()})
    return values


def main() -> None:
    rows = []
    for rep in range(1, 4):
        rows.append(row(1000, rep))
        rows.append(row(2000, rep, ratio=0.95 if rep == 1 else 1.0,
                        p50_us=1200))
        rows.append(row(3000, rep, ratio=1.0 if rep == 1 else 0.8))
        rows.append(row(4000, rep))  # isolated clean point after the first bad rate

    result = ANALYZE.analyze(rows, [], min_reps=3, min_ratio=0.99,
                             max_p50_us=1000)
    assert len(result) == 1
    knee = dict(zip(ANALYZE.OUTPUT_FIELDS, result[0]))
    assert knee["highest_clean_rps"] == 2000
    assert knee["next_bad_rps"] == 3000
    assert knee["status"] == "bracketed"
    assert knee["clean_reps"] == 2
    assert knee["valid_reps"] == 3
    assert knee["highest_healthy_rps"] == 1000
    assert knee["next_unhealthy_rps"] == 2000
    assert knee["healthy_status"] == "bracketed"

    # A restart is an invalid repetition, not a vote that can be hidden by two
    # otherwise clean rows.
    crash = {"channels": "8", "reactors": "8", "frame": "64",
             "offered": "1000", "rep": "3"}
    result = ANALYZE.analyze([row(1000, 1), row(1000, 2)], [crash],
                             3, 0.99, 1000)
    knee = dict(zip(ANALYZE.OUTPUT_FIELDS, result[0]))
    assert knee["status"] == "no_clean_prefix"
    assert knee["highest_clean_rps"] == "NA"

    # Reaching the top of the grid cleanly is a lower bound, not a bracketed knee.
    result = ANALYZE.analyze([row(1000, rep) for rep in range(1, 4)], [],
                             3, 0.99, 1000)
    knee = dict(zip(ANALYZE.OUTPUT_FIELDS, result[0]))
    assert knee["highest_clean_rps"] == 1000
    assert knee["status"] == "right_censored"
    print("analyze_grpc_sweep_test: PASS")


if __name__ == "__main__":
    main()
