#!/usr/bin/env python3
"""Reduce an L4 open-loop dataset (results.csv + rates.csv) to one per-point CSV.

Joins results.csv with rates.csv so every retained load point carries the rate
family it came from, then takes the median across repetitions. Host CPU is the
runqueue runtime of the physical endpoint cores, which counts the application,
its sidecar and the kernel threads working on their behalf. The cgroup sum of
application and sidecar is carried alongside as the attribution figure; the
difference between the two is kernel work owned by no pod.

  usage: distill.py RUN_DIR OUT_CSV
"""

import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

OUT_HEADER = ["config", "frame", "kind", "offered", "achieved", "client",
              "server", "client_cgroup", "server_cgroup", "p99", "drop",
              "clean"]


def read_csv(path):
    with open(path) as handle:
        return list(csv.DictReader(handle))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    run_dir, out_csv = Path(sys.argv[1]), Path(sys.argv[2])

    # (frame, config, rate_index) -> rate family
    family = {}
    for row in read_csv(run_dir / "rates.csv"):
        family[(row["frame_bytes"], row["config"], row["rate_index"])] = row["source"]

    grouped = defaultdict(list)
    for row in read_csv(run_dir / "results.csv"):
        if row["phase"] != "load":
            continue
        key = (row["frame_bytes"], row["config"], row["rate_index"])
        source = family.get(key, "span")
        kind = "common" if source == "common" else "bound"
        grouped[(row["config"], int(row["frame_bytes"]), kind,
                 int(float(row["offered_rps"])))].append(row)

    rows = []
    for (config, frame, kind, offered), reps in sorted(grouped.items()):
        def med(column):
            values = [float(r[column]) for r in reps if r[column] not in ("", "NA")]
            return statistics.median(values) if values else 0.0

        def med_sum(*columns):
            """Sum the columns within a repetition, then take the median."""
            values = []
            for r in reps:
                parts = [r[c] for c in columns if c in r]
                if parts and all(p not in ("", "NA") for p in parts):
                    values.append(sum(float(p) for p in parts))
            return statistics.median(values) if values else 0.0

        clean = 1 if all(r["served_clean"] == "1" for r in reps) else 0
        rows.append([config, frame, kind, offered,
                     med("achieved_rps"),
                     med("client_core_busy_cores"),
                     med("server_core_busy_cores"),
                     med_sum("client_app_cores", "client_sidecar_cores"),
                     med_sum("server_app_cores", "server_sidecar_cores"),
                     med("p99_us"),
                     med("admission_drop_ratio"),
                     clean])

    out_csv.parent.mkdir(parents=True, exist_ok=True)
    with out_csv.open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(OUT_HEADER)
        writer.writerows(rows)
    print(f"{len(rows)} points -> {out_csv}")


if __name__ == "__main__":
    main()
