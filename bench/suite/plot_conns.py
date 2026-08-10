#!/usr/bin/env python3
"""Render the gRPC channel-count sweep figures from grpc_conns_sweep.sh output.

  conns_delivery   served fraction of the offered rate against offered rate,
                   one line per channel count
  conns_workers    the DPU's per-ARM-worker cores at the highest rate every
                   channel count still serves
  conns_latency    median latency against offered rate

A channel is one native QP, and a QP is pinned to one ARM worker, so the
per-worker panel says whether a channel count leaves workers idle.
"""
from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path
from statistics import median

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# Channel count is ordered, so its hues are one ramp light to dark rather than
# four unrelated colours.
CHANNEL_COLOR = {1: "#9dc3ea", 2: "#5a9bd9", 4: "#2a78d6", 8: "#14508f"}
WORKER_COLOR = "#2a78d6"
IDLE_COLOR = "#c9c6bd"
RULE_COLOR = "#e34948"
# A point counts as served when it delivers this fraction of what was offered.
SERVED = 0.98
# Serving a rate is not the same as serving it well: a saturated path still
# delivers every request, out of a queue that has grown milliseconds deep. A
# point is healthy when it is served and its median stays under this bound.
HEALTHY_P50_US = 1000.0

plt.rcParams.update({
    "figure.dpi": 120, "font.size": 9.5, "axes.titlesize": 10.5,
    "axes.labelsize": 9.5, "legend.fontsize": 9,
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "axes.axisbelow": True,
    "grid.alpha": 0.18, "grid.linewidth": 0.6,
})


def read(path):
    with open(path) as fh:
        return list(csv.DictReader(fh))


def num(row, key, default=float("nan")):
    try:
        return float(row[key])
    except (KeyError, TypeError, ValueError):
        return default


def collapse(rows):
    """Median over repetitions, keyed by (channels, offered)."""
    grouped = defaultdict(list)
    for row in rows:
        grouped[(int(float(row["channels"])), int(float(row["offered"])))].append(row)
    out = {}
    for key, reps in grouped.items():
        point = {"reps": len(reps)}
        for field in ("achieved", "ratio", "p50_us", "p99_us", "client_core",
                      "server_core", "dpu_core_workers"):
            values = [num(r, field) for r in reps]
            values = [v for v in values if v == v]
            point[field] = median(values) if values else float("nan")
        # Port allocation walks a bump cursor, so a channel lands on a different
        # worker each run. Which worker is busy carries no information; how many
        # are does. Rank each run's workers by load, then take each rank's
        # median across runs.
        ranked = []
        for r in reps:
            ranked.append(sorted((num(r, f"w{w}", 0.0) for w in range(8)),
                                 reverse=True))
        for rank in range(8):
            point[f"w{rank}"] = median(run[rank] for run in ranked)
        out[key] = point
    return out


def channel_counts(points):
    return sorted({c for c, _ in points})


def rates_for(points, channels):
    return sorted(r for c, r in points if c == channels)


def knee(points, channels):
    """Highest offered rate this channel count still serves in full."""
    served = [r for r in rates_for(points, channels)
              if points[(channels, r)]["ratio"] >= SERVED]
    return max(served) if served else None


def healthy_knee(points, channels):
    """Highest offered rate served with the median still bounded."""
    ok = [r for r in rates_for(points, channels)
          if points[(channels, r)]["ratio"] >= SERVED
          and points[(channels, r)]["p50_us"] <= HEALTHY_P50_US]
    return max(ok) if ok else None


def delivery(points, out: Path, stem: str):
    fig, ax = plt.subplots(figsize=(7.4, 4.6))
    for channels in channel_counts(points):
        rates = rates_for(points, channels)
        color = CHANNEL_COLOR.get(channels, "#666666")
        ax.plot([r / 1000 for r in rates],
                [points[(channels, r)]["ratio"] for r in rates],
                linewidth=2, color=color, zorder=2,
                label=f"{channels} channel" + ("s" if channels != 1 else ""))
        # A hollow marker still delivered the rate, but out of a queue deeper
        # than the median bound: served is not the same as served well.
        for r in rates:
            healthy = points[(channels, r)]["p50_us"] <= HEALTHY_P50_US
            ax.plot(r / 1000, points[(channels, r)]["ratio"], marker="o",
                    markersize=6, zorder=3, color=color,
                    markerfacecolor=color if healthy else "white",
                    markeredgecolor=color, markeredgewidth=1.6)
    ax.axhline(SERVED, color=RULE_COLOR, linewidth=1.2, linestyle="--", alpha=0.8)
    ax.text(ax.get_xlim()[1], SERVED, f" served ≥ {SERVED:.2f} ",
            color=RULE_COLOR, va="bottom", ha="right", fontsize=8.5)
    ax.set_xlabel("offered rate (thousand RPC/s)")
    ax.set_ylabel("achieved / offered")
    ax.set_ylim(0, 1.08)
    ax.set_title("What each channel count delivers\n"
                 f"(hollow marker: median above {HEALTHY_P50_US:.0f} µs)")
    ax.legend(frameon=False, loc="lower left")
    fig.tight_layout()
    fig.savefig(out / f"{stem}_delivery.png", dpi=220, bbox_inches="tight")
    fig.savefig(out / f"{stem}_delivery.pdf", bbox_inches="tight")
    plt.close(fig)


def workers(points, out: Path, stem: str):
    counts = channel_counts(points)
    fig, axes = plt.subplots(1, len(counts), figsize=(3.2 * len(counts), 3.9),
                             sharey=True)
    if len(counts) == 1:
        axes = [axes]
    for ax, channels in zip(axes, counts):
        rate = healthy_knee(points, channels) or knee(points, channels)
        if rate is None:
            rate = max(rates_for(points, channels))
        point = points[(channels, rate)]
        values = [point[f"w{w}"] for w in range(8)]
        colors = [WORKER_COLOR if v >= 0.02 else IDLE_COLOR for v in values]
        ax.bar(range(8), values, color=colors, width=0.72)
        ax.axhline(1.0, color=RULE_COLOR, linewidth=1.2, linestyle="--", alpha=0.8)
        ax.set_xticks(range(8))
        ax.set_xticklabels([str(w + 1) for w in range(8)])
        ax.set_xlabel("ARM worker, busiest first")
        busy = sum(1 for v in values if v >= 0.02)
        ax.set_title(f"{channels} channel" + ("s" if channels != 1 else "") +
                     f"\n{rate / 1000:g}K/s · {busy} of 8 busy")
    axes[0].set_ylabel("ARM cores")
    axes[0].text(0.1, 1.02, "one full core", color=RULE_COLOR, fontsize=8.5,
                 va="bottom")
    fig.suptitle("How the DPU's ARM workers share the load", y=1.0)
    fig.tight_layout()
    fig.savefig(out / f"{stem}_workers.png", dpi=220, bbox_inches="tight")
    fig.savefig(out / f"{stem}_workers.pdf", bbox_inches="tight")
    plt.close(fig)


def latency(points, out: Path, stem: str):
    fig, axes = plt.subplots(1, 2, figsize=(12.0, 4.2), sharex=True)
    for channels in channel_counts(points):
        rates = rates_for(points, channels)
        served = [r for r in rates if points[(channels, r)]["ratio"] >= SERVED]
        color = CHANNEL_COLOR.get(channels, "#666666")
        label = f"{channels} channel" + ("s" if channels != 1 else "")
        axes[0].plot([r / 1000 for r in served],
                     [points[(channels, r)]["p50_us"] for r in served],
                     marker="o", markersize=5, linewidth=2, color=color,
                     label=label)
        axes[1].plot([r / 1000 for r in rates],
                     [points[(channels, r)]["client_core"] +
                      points[(channels, r)]["server_core"] for r in rates],
                     marker="o", markersize=5, linewidth=2, color=color,
                     label=label)
    axes[0].axhline(HEALTHY_P50_US, color=RULE_COLOR, linewidth=1.2,
                    linestyle="--", alpha=0.8)
    axes[0].set_yscale("log")
    axes[0].set_ylabel("median latency (µs)")
    axes[0].set_title("Median latency where the rate is served")
    axes[1].set_ylabel("host cores (client + server)")
    axes[1].set_title("Host cores consumed, out of twelve available")
    for ax in axes:
        ax.set_xlabel("offered rate (thousand RPC/s)")
        ax.legend(frameon=False, loc="upper left")
    fig.tight_layout()
    fig.savefig(out / f"{stem}_latency.png", dpi=220, bbox_inches="tight")
    fig.savefig(out / f"{stem}_latency.pdf", bbox_inches="tight")
    plt.close(fig)


def scaling(points, out: Path, stem: str):
    """Healthy capacity against channel count, and what the DPU spends on it."""
    counts = channel_counts(points)
    top = {c: max(rates_for(points, c)) for c in counts}
    knees = {c: healthy_knee(points, c) for c in counts}
    if any(v is None for v in knees.values()):
        return

    fig, axes = plt.subplots(1, 2, figsize=(11.4, 4.2))
    x = np.arange(len(counts))
    values = [knees[c] / 1000 for c in counts]
    # A knee that equals the top of its grid is a lower bound, not a knee.
    capped = [knees[c] >= top[c] for c in counts]
    colors = [CHANNEL_COLOR.get(c, "#666666") for c in counts]
    bars = axes[0].bar(x, values, color=colors, width=0.62)
    for xi, bar, value, is_capped in zip(x, bars, values, capped):
        axes[0].text(xi, value + 1.0, f"≥{value:g}K" if is_capped else f"{value:g}K",
                     ha="center", fontsize=9,
                     color=RULE_COLOR if is_capped else "#333333")
        if is_capped:
            axes[0].annotate("", xy=(xi, value + 5.5), xytext=(xi, value + 0.5),
                             arrowprops=dict(arrowstyle="->", color=RULE_COLOR,
                                             linewidth=1.4))
    axes[0].set_xticks(x)
    axes[0].set_xticklabels([str(c) for c in counts])
    axes[0].set_xlabel("channels (= native QPs)")
    axes[0].set_ylabel("healthy rate (thousand RPC/s)")
    axes[0].set_title("Capacity served with the median bounded\n"
                      "(arrow: still rising at the top of the measured grid)")

    for channels in counts:
        rates = rates_for(points, channels)
        axes[1].plot([r / 1000 for r in rates],
                     [points[(channels, r)]["dpu_core_workers"] for r in rates],
                     marker="o", markersize=5, linewidth=2,
                     color=CHANNEL_COLOR.get(channels, "#666666"),
                     label=f"{channels} channel" + ("s" if channels != 1 else ""))
    axes[1].set_xlabel("offered rate (thousand RPC/s)")
    axes[1].set_ylabel("ARM cores (all workers)")
    axes[1].set_title("What the DPU spends")
    axes[1].legend(frameon=False, loc="upper left")
    fig.tight_layout()
    fig.savefig(out / f"{stem}_scaling.png", dpi=220, bbox_inches="tight")
    fig.savefig(out / f"{stem}_scaling.pdf", bbox_inches="tight")
    plt.close(fig)


def summary(points):
    counts = channel_counts(points)
    lines = ["channels  served_to  healthy_to    p50_us   host_cores  "
             "busy_workers  peak_worker"]
    for channels in counts:
        served = knee(points, channels)
        healthy = healthy_knee(points, channels)
        shown = healthy or served or max(rates_for(points, channels))
        point = points[(channels, shown)]
        values = [point[f"w{w}"] for w in range(8)]
        busy = sum(1 for v in values if v >= 0.02)
        lines.append(
            f"{channels:>8}  {served or 0:>9}  {healthy or 0:>10}  "
            f"{point['p50_us']:>8.0f}  "
            f"{point['client_core'] + point['server_core']:>10.3f}  "
            f"{busy:>12}  {max(values):>11.3f}")
    for name, fn in (("served", knee), ("healthy", healthy_knee)):
        base, top = fn(points, counts[0]), fn(points, counts[-1])
        if base and top:
            lines.append(
                f"{name} knee {counts[-1]}ch / {counts[0]}ch = {top / base:.2f}x")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--stem", default="conns")
    args = parser.parse_args()

    rows = read(args.csv)
    points = collapse(rows)
    args.out.mkdir(parents=True, exist_ok=True)
    delivery(points, args.out, args.stem)
    workers(points, args.out, args.stem)
    latency(points, args.out, args.stem)
    scaling(points, args.out, args.stem)
    print(summary(points))


if __name__ == "__main__":
    main()
