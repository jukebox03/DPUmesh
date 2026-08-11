#!/usr/bin/env python3
"""Audit a running or finished L7 campaign for results that cannot be trusted.

The collector already rejects a point that failed its own admission test. This
checks the things it does not: that the core budget held, that the CPU the pods
report and the CPU their cores actually burned agree, that a meshed path is
really paying for a sidecar, and that cost rises with load. Anything it prints
is a reason to look before the number reaches a report.

  usage: check_campaign.py RUN_DIR [CLOSED_CSV ...]
"""

import csv
import sys
from collections import defaultdict
from pathlib import Path

# Paths whose pod runs a proxy container beside the application.
MESHED = {"grpc-envoy-permissive", "grpc-envoy-strict",
          "grpc-linkerd", "grpc-linkerd-opaque"}
# One exclusive core per endpoint. Above this the pinning did not hold.
CORE_BUDGET = 1.0
CORE_TOLERANCE = 0.06
# Runqueue runtime counts kernel work owned by no pod, so it should meet or
# exceed the cgroup sum. A cgroup total far above it means the two are not
# describing the same core.
ATTRIB_TOLERANCE = 0.10
# The collector's own saturation threshold. At or above it a core has no room
# left, so its busy figure stops tracking load.
SATURATED = 0.95


class Audit:
    def __init__(self):
        self.problems = []
        self.notes = []
        self.checked = 0

    def fail(self, msg):
        self.problems.append(msg)

    def note(self, msg):
        self.notes.append(msg)


def num(row, key, default=0.0):
    try:
        return float(row[key])
    except (KeyError, ValueError, TypeError):
        return default


def check_open(run_dir, audit):
    results = run_dir / "results.csv"
    if not results.exists():
        audit.note(f"{results} not written yet")
        return
    rows = [r for r in csv.DictReader(open(results))]
    load = [r for r in rows if r["phase"] == "load"]
    audit.checked += len(load)

    # The capacity boundary the collector discovered for each path and frame.
    knee = {}
    knees_path = run_dir / "knees.csv"
    if knees_path.exists():
        for r in csv.DictReader(open(knees_path)):
            knee[(r["config"], int(r["frame_bytes"]))] = float(r["highest_clean_rps"])

    # A point the collector rejected is data about the far side of a path's
    # capacity, not a defect: it is recorded and then excluded from every
    # reported rate. Only the points that survived that test have to be sound.
    by_config_frame = defaultdict(list)
    rejected = defaultdict(int)
    for r in load:
        cfg, frame = r["config"], int(r["frame_bytes"])
        clean = r.get("served_clean") in (None, "1")
        if not clean:
            rejected[f"{cfg} ({r.get('clean_reason', '?')})"] += 1

        if clean:
            for column in ("fail", "overflow", "reorder"):
                if num(r, column) != 0:
                    audit.fail(f"{cfg}/{frame}B@{r['offered_rps']}: retained clean "
                               f"with {column}={r[column]}")
            offered, achieved = num(r, "offered_rps"), num(r, "achieved_rps")
            if offered > 0 and achieved / offered < 0.98:
                audit.fail(f"{cfg}/{frame}B@{offered:.0f}: retained clean but "
                           f"delivered {achieved/offered:.3f} of offered")
        offered = num(r, "offered_rps")

        for side in ("client", "server"):
            busy = num(r, f"{side}_core_busy_cores")
            app = num(r, f"{side}_app_cores")
            sidecar = num(r, f"{side}_sidecar_cores")
            if busy > CORE_BUDGET + CORE_TOLERANCE:
                audit.fail(f"{cfg}/{frame}B@{offered:.0f}: {side} core busy "
                           f"{busy:.3f} over the one-core budget")
            if app + sidecar > busy + ATTRIB_TOLERANCE:
                audit.fail(f"{cfg}/{frame}B@{offered:.0f}: {side} cgroup "
                           f"{app + sidecar:.3f} exceeds core busy {busy:.3f}")
            if cfg in MESHED and app > 0.02 and sidecar <= 0.0:
                audit.fail(f"{cfg}/{frame}B@{offered:.0f}: {side} sidecar "
                           f"reported 0 cores on a meshed path")
            if cfg not in MESHED and sidecar > 0.0:
                audit.fail(f"{cfg}/{frame}B@{offered:.0f}: {side} sidecar "
                           f"{sidecar:.3f} on a path that has none")

        if cfg == "grpc-dpumesh" and r.get("dpu_arm_cores") in ("NA", "", None):
            audit.fail(f"{cfg}/{frame}B@{offered:.0f}: no DPU ARM core sample")

        if clean:
            by_config_frame[(cfg, frame)].append(r)

    # Below saturation, host cost rises with load; a fall means the sample did
    # not come from the core the load ran on. Two regions are exempt. Once an
    # endpoint core is pegged the metric is clamped by definition — further rate
    # is bought with latency, not with CPU. And past the discovered capacity
    # boundary the host can legitimately do less work, because whatever bounded
    # the path there is now throttling it.
    for (cfg, frame), group in sorted(by_config_frame.items()):
        pts = sorted(group, key=lambda r: num(r, "offered_rps"))
        for lo, hi in zip(pts, pts[1:]):
            lo_rate, hi_rate = num(lo, "offered_rps"), num(hi, "offered_rps")
            if hi_rate <= lo_rate:
                continue
            if max(num(lo, "client_core_busy_cores"),
                   num(lo, "server_core_busy_cores")) >= SATURATED:
                continue
            if lo_rate >= knee.get((cfg, frame), float("inf")):
                continue
            lo_cpu = num(lo, "client_core_busy_cores") + num(lo, "server_core_busy_cores")
            hi_cpu = num(hi, "client_core_busy_cores") + num(hi, "server_core_busy_cores")
            if hi_cpu + 0.05 < lo_cpu:
                audit.fail(f"{cfg}/{frame}B: host cost falls with load below "
                           f"saturation, {lo_rate:.0f}->{hi_rate:.0f} rps gives "
                           f"{lo_cpu:.3f}->{hi_cpu:.3f} cores")

    knees = run_dir / "knees.csv"
    if knees.exists():
        for r in csv.DictReader(open(knees)):
            if r["status"] != "bracketed":
                audit.fail(f"knee {r['config']}/{r['frame_bytes']}B: {r['status']}")
            if float(r["bracket_ratio"]) > 1.05:
                audit.fail(f"knee {r['config']}/{r['frame_bytes']}B: loose bracket "
                           f"{r['bracket_ratio']}")

    recovery = run_dir / "recovery.log"
    if recovery.exists() and recovery.stat().st_size:
        n = sum(1 for _ in open(recovery))
        audit.note(f"{n} forced redeploy(s) recorded in recovery.log")

    clean_n = sum(1 for r in load if r.get("served_clean") in (None, "1"))
    audit.note(f"open loop: {len(load)} load rows ({clean_n} clean, "
               f"{len(load) - clean_n} rejected), "
               f"{len([r for r in rows if r['phase'] == 'idle'])} idle rows")
    for key, n in sorted(rejected.items()):
        audit.note(f"  rejected {n}x {key}")


def check_closed(path, audit):
    rows = [r for r in csv.DictReader(open(path))]
    audit.checked += len(rows)
    for r in rows:
        cfg = r["config"]
        tag = f"{cfg}/{r['frame']}B/conc={r['conc']}"
        if num(r, "fail") != 0:
            audit.fail(f"{tag}: fail={r['fail']}")
        if num(r, "achieved") <= 0:
            audit.fail(f"{tag}: no delivered rate")
        for side in ("client_core", "server_core"):
            v = r.get(side)
            if v in ("NA", "", None):
                audit.fail(f"{tag}: {side} not sampled")
        if cfg == "grpc-dpumesh" and r.get("dpu_arm_cores") in ("NA", "", None):
            audit.fail(f"{tag}: no DPU ARM core sample")
    audit.note(f"{path.name}: {len(rows)} points, "
               f"{len({r['config'] for r in rows})} configurations")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    audit = Audit()
    check_open(Path(sys.argv[1]), audit)
    for extra in sys.argv[2:]:
        p = Path(extra)
        if p.exists():
            check_closed(p, audit)

    for n in audit.notes:
        print(f"note: {n}")
    if audit.problems:
        print(f"\n{len(audit.problems)} problem(s) in {audit.checked} rows:")
        for p in audit.problems:
            print(f"  {p}")
        raise SystemExit(1)
    print(f"\nPASS: {audit.checked} rows, nothing to flag")


if __name__ == "__main__":
    main()
