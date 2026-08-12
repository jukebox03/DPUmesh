#!/usr/bin/env python3
"""Summarize an L7-mode dataset produced by bench/suite/l7_modes.sh.

Reads points.csv, capacity.csv and cores.csv from the dataset directory and
writes the tables a report quotes: capacity per mode, latency at a common
offered rate, closed-loop throughput, and ARM cores per delivered Mrps.
"""

import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

MODE_ORDER = ["instrument", "dataplane", "decision", "opaque",
              "l7-conn", "l7-message"]
INSTRUMENT = "instrument"
# A knee that lands on the instrument's own is not a property of the path. Only
# a ratio near one says that; a mode far above the instrument is simply cheaper
# for the generator to drive than a kernel socket is.
INSTRUMENT_BAND = (0.95, 1.05)


def num(x, default=None):
    try:
        return float(x)
    except (TypeError, ValueError):
        return default


def load(path):
    if not path.exists():
        return []
    with path.open() as f:
        return list(csv.DictReader(f))


def order(modes):
    known = [m for m in MODE_ORDER if m in modes]
    return known + sorted(m for m in modes if m not in MODE_ORDER)


def table(header, rows):
    widths = [len(h) for h in header]
    for r in rows:
        for i, cell in enumerate(r):
            widths[i] = max(widths[i], len(str(cell)))
    out = ["  ".join(h.ljust(widths[i]) for i, h in enumerate(header))]
    out.append("  ".join("-" * w for w in widths))
    for r in rows:
        out.append("  ".join(str(c).ljust(widths[i]) for i, c in enumerate(r)))
    return "\n".join(out)


def median(values):
    vals = [v for v in values if v is not None]
    return statistics.median(vals) if vals else None


def fmt(v, spec="{:.3f}"):
    return spec.format(v) if v is not None else "-"


def main(argv):
    if len(argv) != 2:
        print("usage: summarize_l7.py <dataset-dir>", file=sys.stderr)
        return 2
    root = Path(argv[1])
    points = load(root / "points.csv")
    sat = load(root / "saturation.csv")
    caps = load(root / "capacity.csv")
    cores = load(root / "cores.csv")
    matched = load(root / "cores_matched.csv")
    if not points:
        print("no points.csv rows", file=sys.stderr)
        return 1

    modes = order({p["mode"] for p in points})
    out = []

    # Integrity: a lost request invalidates a row. Reordering does not — it is
    # what per-message routing does — so it is reported, not counted as a fault.
    bad = [p for p in points
           if num(p["fail"], 0) or num(p["drops"], 0) and p["kind"] == "closed"]
    reordered = sorted({p["mode"] for p in points if num(p["reorder"], 0)})
    out.append("integrity")
    out.append(f"  rows            : {len(points)}")
    out.append(f"  rows with fail  : {len(bad)}")
    out.append(f"  modes reordering: {', '.join(reordered) if reordered else 'none'}")
    out.append("")

    if caps:
        ceiling = {c["frame"]: num(c["knee_rps"])
                   for c in caps if c["mode"] == INSTRUMENT}
        rows = []
        for m in order({c["mode"] for c in caps}):
            for c in caps:
                if c["mode"] != m:
                    continue
                knee = num(c["knee_rps"], 0)
                top = ceiling.get(c["frame"])
                share = knee / top if top else None
                note = c["limit_reason"]
                if m != INSTRUMENT and share and \
                        INSTRUMENT_BAND[0] <= share <= INSTRUMENT_BAND[1]:
                    note = "matches instrument"
                rows.append([m, c["frame"], f"{int(knee):,}",
                             fmt(share, "{:.2f}") if m != INSTRUMENT else "-", note])
        out.append("open-loop capacity (offered until the achieved rate falls behind)")
        out.append(table(["mode", "frame", "knee rps", "vs instr", "limit"], rows))
        if ceiling:
            out.append("  vs instr: knee over what the load generator sustains on the")
            out.append("  matched TCP path. Between %.2f and %.2f the number is the"
                       % INSTRUMENT_BAND)
            out.append("  instrument's; far above it the path is simply cheaper to drive.")
        out.append("")

    # Closed loop: throughput at a fixed concurrency window, and the latency
    # floor at one outstanding request.
    for conc, title in ((None, "closed loop, concurrency window"),
                        (1, "closed loop, one outstanding request")):
        by = defaultdict(lambda: defaultdict(list))
        for p in points:
            if p["kind"] != "closed":
                continue
            c = num(p["conc"])
            if conc == 1 and c != 1:
                continue
            if conc is None and c == 1:
                continue
            by[p["mode"]][p["frame"]].append(p)
        if not by:
            continue
        frames = sorted({f for m in by for f in by[m]}, key=int)
        header = ["mode"] + [f"{f}B" for f in frames]
        rows = []
        for m in order(set(by)):
            row = [m]
            for f in frames:
                rs = by[m].get(f, [])
                if conc == 1:
                    p50 = median([num(r["p50_us"]) for r in rs])
                    p99 = median([num(r["p99_us"]) for r in rs])
                    row.append(f"{fmt(p50,'{:.0f}')}/{fmt(p99,'{:.0f}')}us")
                else:
                    row.append(fmt(median([num(r["achieved_rps"]) for r in rs])))
            rows.append(row)
        out.append(title + (" (Mrps)" if conc is None else " (p50/p99)"))
        out.append(table(header, rows))
        out.append("")

    # Open loop at each mode's own knee: the point it would actually run at.
    own = defaultdict(lambda: defaultdict(list))
    for p in points:
        if p["kind"] == "knee":
            own[p["mode"]][p["frame"]].append(p)
    if own:
        frames = sorted({f for m in own for f in own[m]}, key=int)
        rows = []
        for m in order(set(own)):
            for f in frames:
                rs = own[m].get(f, [])
                if not rs:
                    continue
                rows.append([
                    m, f, f"{int(num(rs[0]['offered_rps'], 0)):,}",
                    fmt(median([num(r["achieved_rps"]) for r in rs])),
                    fmt(median([num(r["p50_us"]) for r in rs]), "{:.0f}"),
                    fmt(median([num(r["p99_us"]) for r in rs]), "{:.0f}"),
                ])
        out.append("open loop at each mode's own knee (0.90 of its capacity)")
        out.append(table(["mode", "frame", "offered", "achieved Mrps",
                          "p50 us", "p99 us"], rows))
        out.append("")

    # Open loop: latency at the same offered rate across modes.
    by_rate = defaultdict(lambda: defaultdict(list))
    for p in points:
        if p["kind"] != "rate":
            continue
        by_rate[(p["frame"], p["offered_rps"])][p["mode"]].append(p)
    if by_rate:
        keys = sorted(by_rate, key=lambda k: (int(k[0]), int(k[1])))
        rows = []
        for frame, offered in keys:
            per = by_rate[(frame, offered)]
            for m in order(set(per)):
                rs = per[m]
                rows.append([
                    frame, f"{int(offered):,}", m,
                    fmt(median([num(r["achieved_rps"]) for r in rs])),
                    fmt(median([num(r["p50_us"]) for r in rs]), "{:.0f}"),
                    fmt(median([num(r["p99_us"]) for r in rs]), "{:.0f}"),
                    fmt(median([num(r["drops"], 0) for r in rs]), "{:.0f}"),
                ])
        out.append("open loop at a common offered rate")
        out.append(table(["frame", "offered", "mode", "achieved Mrps",
                          "p50 us", "p99 us", "drops"], rows))
        out.append("")

    # The ramp stops at the first rate a mode fails to deliver, which is a rung
    # of the ladder. The plateau is what the path carries once offering more
    # stops helping, and that is what separates the modes.
    if sat:
        knees = {(c["mode"], c["frame"]): num(c["knee_rps"]) for c in caps}
        best = {}
        for r in sat:
            k = (r["mode"], r["frame"])
            a = num(r["achieved_rps"], 0)
            if a > best.get(k, (0, None))[0]:
                best[k] = (a, r)
        rows = []
        for m in order({k[0] for k in best}):
            for f in sorted({k[1] for k in best if k[0] == m}, key=int):
                a, r = best[(m, f)]
                knee = knees.get((m, f))
                plateau = max(a, knee or 0)
                rows.append([m, f, f"{int(knee):,}" if knee else "-",
                             f"{int(plateau):,}",
                             fmt(plateau / knee, "{:.2f}") if knee else "-",
                             f"{int(num(r['offered_rps'], 0)):,}"])
        out.append("saturation (highest rate carried, past the rung the ramp stopped on)")
        out.append(table(["mode", "frame", "ramp knee", "plateau", "plateau/knee",
                          "offered at plateau"], rows))
        out.append("")

    unstable = []
    groups = defaultdict(list)
    for p in points:
        groups[(p["mode"], p["kind"], p["frame"], p["conc"], p["offered_rps"])].append(p)
    for key, rs in sorted(groups.items()):
        if len(rs) < 2:
            continue
        p99 = [num(r["p99_us"]) for r in rs if num(r["p99_us"]) is not None]
        ach = [num(r["achieved_rps"]) for r in rs if num(r["achieved_rps"])]
        if len(p99) < 2 or min(p99) <= 0:
            continue
        spread = max(p99) / min(p99)
        if spread >= 10 or (ach and max(ach) / min(ach) >= 1.5):
            mode, kind, frame, conc, offered = key
            unstable.append([mode, kind, frame, offered or f"conc{conc}",
                             fmt(min(p99), "{:.0f}"), fmt(max(p99), "{:.0f}"),
                             fmt(spread, "{:.0f}") + "x",
                             fmt(max(num(r["drops"], 0) for r in rs), "{:.0f}")])
    if unstable:
        out.append("repetitions that disagree (p99 spread >= 10x or rate spread >= 1.5x)")
        out.append(table(["mode", "kind", "frame", "offered", "p99 min", "p99 max",
                          "spread", "worst drops"], unstable))
        out.append("")

    # Cost per request at rates every mode serves. Modes measured at their own
    # operating points are not comparable; these are.
    if matched:
        grouped = defaultdict(list)
        for r in matched:
            per = num(r["cores_per_mrps"])
            if per:
                grouped[(r["mode"], r["offered_rps"])].append(per)
        rates = sorted({r for _, r in grouped}, key=int)
        rows = []
        for m in order({k[0] for k in grouped}):
            row = [m]
            for rate in rates:
                vals = sorted(grouped.get((m, rate), []))
                if not vals:
                    row.append("-")
                    continue
                med = vals[len(vals) // 2]
                row.append(f"{med:.2f}" + (f" ({vals[0]:.1f}-{vals[-1]:.1f})"
                                           if len(vals) > 1 else ""))
            rows.append(row)
        out.append("ARM cores per delivered Mrps, at matched offered rates")
        out.append(table(["mode"] + [f"{int(r):,}/s" for r in rates], rows))
        out.append("  median over repetitions, with the range in brackets")
        out.append("")

    if cores:
        rows = []
        for m in order({c["mode"] for c in cores}):
            for c in cores:
                if c["mode"] != m:
                    continue
                mrps = num(c["achieved_rps"])
                pct = num(c["arm_cores_pct"])
                per = (pct / 100.0) / mrps if mrps and pct else None
                rows.append([m, c["frame"], fmt(mrps), fmt(pct, "{:.1f}") + "%",
                             fmt(per, "{:.2f}"), fmt(num(c["reorder"], 0), "{:.0f}")])
        out.append("ARM cores")
        out.append(table(["mode", "frame", "Mrps", "cores", "cores/Mrps", "reorder"], rows))
        out.append("")

    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
