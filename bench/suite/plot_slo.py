#!/usr/bin/env python3
"""Render the latency-budget curve: sustained rate under a p99 ceiling.

A single "maximum throughput" number rewards whichever path is willing to queue
deepest, because a deeper queue amortises per-wake cost and the offered rate is
still met, just later. The number is only meaningful once a latency ceiling is
attached, and any single ceiling is an arbitrary choice that decides the ranking.
This plots the whole family instead: for each p99 budget on the x axis, the
highest rate a path delivered while staying under it. The reader picks the
budget their service actually has.

  usage: plot_slo.py OUT_DIR STEM SOURCE [SOURCE ...]

SOURCE is either a collector run directory (results.csv + scout.csv, open loop)
or a closed-loop points.csv. Every delivered point in them contributes.
"""

import csv
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# Okabe-Ito, matching the other figures. The baseline path is deliberately the
# neutral grey; the three compared paths carry the chromatic slots, which clear
# the CVD and chroma checks among themselves.
COLORS = {
    "envoy-permissive": "#5B6573", "grpc-envoy-permissive": "#5B6573",
    "envoy-strict": "#D55E00", "grpc-envoy-strict": "#D55E00",
    "dpumesh-preload": "#0072B2", "grpc-tcp": "#0072B2",
    "dpumesh-native": "#009E73", "grpc-dpumesh": "#009E73",
}
LABELS = {
    "envoy-permissive": "Envoy permissive", "grpc-envoy-permissive": "Envoy permissive",
    "envoy-strict": "Envoy strict", "grpc-envoy-strict": "Envoy strict",
    "dpumesh-preload": "DPUmesh preload", "grpc-tcp": "direct TCP",
    "dpumesh-native": "DPUmesh native", "grpc-dpumesh": "DPUmesh",
}
FRAME_LABEL = {"64": "64 B", "1024": "1 KiB", "8192": "8 KiB"}
# Budgets span the range the data actually covers; log-spaced because latency is.
BUDGETS_US = [500, 1000, 2000, 5000, 10000, 30000, 100000]
MIN_DELIVERED = 0.98


def add(points, config, frame, achieved, p99):
    if achieved > 0 and p99 > 0:
        points[(config, str(frame))].append((achieved, p99))


def read_open(run_dir, points):
    for name in ("results.csv", "scout.csv"):
        path = run_dir / name
        if not path.exists():
            continue
        for r in csv.DictReader(open(path)):
            if name == "results.csv" and r.get("phase") != "load":
                continue
            try:
                off = float(r["offered_rps"]); ach = float(r["achieved_rps"])
                p99 = float(r["p99_us"])
            except (KeyError, ValueError):
                continue
            if off <= 0 or ach / off < MIN_DELIVERED:
                continue
            add(points, r["config"], r["frame_bytes"], ach, p99)


def read_closed(path, points):
    for r in csv.DictReader(open(path)):
        try:
            ach = float(r["achieved"]); p99 = float(r["p99_us"])
        except (KeyError, ValueError):
            continue
        add(points, r["config"], r["frame"], ach, p99)


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    out_dir, stem, sources = Path(sys.argv[1]), sys.argv[2], sys.argv[3:]

    points = defaultdict(list)
    for src in sources:
        p = Path(src)
        if p.is_dir():
            read_open(p, points)
        else:
            read_closed(p, points)
    if not points:
        raise SystemExit("no delivered points found")

    frames = [f for f in ("64", "1024", "8192")
              if any(k[1] == f for k in points)]
    configs = [c for c in ("envoy-permissive", "grpc-envoy-permissive",
                           "envoy-strict", "grpc-envoy-strict",
                           "dpumesh-preload", "grpc-tcp",
                           "dpumesh-native", "grpc-dpumesh")
               if any(k[0] == c for k in points)]

    fig, axes = plt.subplots(1, len(frames), figsize=(4.6 * len(frames), 4.0),
                             squeeze=False)
    axes = axes[0]
    for ax, frame in zip(axes, frames):
        ends = []
        for config in configs:
            pts = points.get((config, frame))
            if not pts:
                continue
            # A budget no measured point met is absent, not zero: drawing it on
            # the axis would read as "this path delivered nothing" when it only
            # means the rate grid has no point that tight.
            ys = [max([a for a, p in pts if p <= b], default=np.nan)
                  for b in BUDGETS_US]
            xs = [b / 1000.0 for b in BUDGETS_US]
            if all(np.isnan(y) for y in ys):
                continue
            ax.plot(xs, ys, drawstyle="steps-post", linewidth=2.0,
                    marker="o", markersize=5, color=COLORS[config],
                    label=LABELS[config], zorder=3,
                    solid_capstyle="round")
            last = next((i for i in range(len(ys) - 1, -1, -1)
                         if not np.isnan(ys[i])), None)
            if last is not None:
                ends.append([ys[last], xs[last], config])
        # Direct labels carry identity so it never rests on hue alone. Paths
        # that finish at the same rate would print on top of each other, so the
        # labels are pushed apart along y while the markers stay put.
        ends.sort(key=lambda e: e[0])
        span = (max(e[0] for e in ends) - min(e[0] for e in ends)) if len(ends) > 1 else 0
        gap = span * 0.055
        for i in range(1, len(ends)):
            if ends[i][0] - ends[i - 1][0] < gap:
                ends[i][0] = ends[i - 1][0] + gap
        for y, x, config in ends:
            ax.annotate(LABELS[config], (x, y),
                        textcoords="offset points", xytext=(7, 0),
                        fontsize=7.5, color=COLORS[config],
                        va="center", zorder=4)
        ax.set_xscale("log")
        ax.set_xticks([b / 1000.0 for b in BUDGETS_US])
        ax.set_xticklabels([("%g" % (b / 1000.0)) for b in BUDGETS_US], fontsize=8)
        ax.set_xlabel("p99 budget (ms)", fontsize=9)
        ax.set_title(FRAME_LABEL.get(frame, frame), fontsize=10)
        ax.grid(True, which="major", axis="y", color="#e6e6e6", linewidth=0.8,
                zorder=0)
        ax.set_axisbelow(True)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            ax.spines[side].set_color("#bbbbbb")
        ax.tick_params(labelsize=8, color="#bbbbbb")
        ax.margins(x=0.28)
    axes[0].set_ylabel("sustained rate under the budget (RPC/s)", fontsize=9)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=len(labels),
               frameon=False, fontsize=8.5, bbox_to_anchor=(0.5, -0.02))
    fig.tight_layout(rect=(0, 0.06, 1, 1))

    out_dir.mkdir(parents=True, exist_ok=True)
    for ext in ("png", "pdf"):
        fig.savefig(out_dir / f"{stem}.{ext}", dpi=200, bbox_inches="tight")
    print(f"wrote {out_dir}/{stem}.png and .pdf")

    for frame in frames:
        print(f"\n{FRAME_LABEL.get(frame, frame)}:")
        for b in BUDGETS_US:
            row = f"  p99<={b/1000:>6g} ms | "
            for config in configs:
                pts = points.get((config, frame))
                if not pts:
                    continue
                v = max([a for a, p in pts if p <= b], default=None)
                cell = f"{v:>9,.0f}" if v else f"{'-':>9}"
                row += f"{LABELS[config]}={cell}  "
            print(row)


if __name__ == "__main__":
    main()
