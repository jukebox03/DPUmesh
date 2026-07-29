#!/usr/bin/env python3
"""Render the L4 host-CPU and fixed-budget throughput figures from a collector dataset."""

import argparse
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

CONFIGS = ["envoy-permissive", "envoy-strict", "dpumesh-preload", "dpumesh-native"]
LABELS = {
    "envoy-permissive": "Envoy permissive",
    "envoy-strict": "Envoy strict",
    "dpumesh-preload": "DPUmesh preload",
    "dpumesh-native": "DPUmesh native",
}
SHORT = {
    "envoy-permissive": "Envoy\npermissive",
    "envoy-strict": "Envoy\nstrict",
    "dpumesh-preload": "DPUmesh\npreload",
    "dpumesh-native": "DPUmesh\nnative",
}
COLORS = {
    "envoy-permissive": "#5B6573",
    "envoy-strict": "#D55E00",
    "dpumesh-preload": "#0072B2",
    "dpumesh-native": "#009E73",
}
MARKERS = {
    "envoy-permissive": "o",
    "envoy-strict": "s",
    "dpumesh-preload": "^",
    "dpumesh-native": "D",
}
FRAMES = [64, 1024, 8192]
FRAME_LABEL = {64: "64 B", 1024: "1 KiB", 8192: "8 KiB"}
HOST_BUDGET = 2.0

plt.rcParams.update({
    "figure.dpi": 120,
    "font.size": 9.5,
    "axes.titlesize": 10.5,
    "axes.labelsize": 9.5,
    "legend.fontsize": 9,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.grid": True,
    "grid.alpha": 0.18,
    "grid.linewidth": 0.6,
})


def number(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return float("nan")


def human(rate):
    if rate >= 1e6:
        return f"{rate / 1e6:.2f} M".rstrip("0").rstrip(".") + " M" if False else f"{rate / 1e6:.2f}M"
    if rate >= 1e3:
        return f"{rate / 1e3:.0f}k"
    return f"{rate:.0f}"


def load(dataset):
    """Group retained load rows by config, frame and offered rate."""
    rows = defaultdict(list)
    with (dataset / "results.csv").open() as handle:
        for row in csv.DictReader(handle):
            if row["phase"] != "load":
                continue
            key = (row["config"], int(row["frame_bytes"]), int(float(row["offered_rps"])))
            rows[key].append(row)
    return rows


def sources(dataset):
    src = {}
    with (dataset / "rates.csv").open() as handle:
        for row in csv.DictReader(handle):
            src[(row["config"], int(row["frame_bytes"]), int(float(row["offered_rps"])))] = row["source"]
    return src


def point(rows):
    """Mean host cores, mean achieved rate and the clean-vote tally for one point."""
    n = len(rows)
    client = sum(number(r["client_core_busy_cores"]) for r in rows) / n
    server = sum(number(r["server_core_busy_cores"]) for r in rows) / n
    achieved = sum(number(r["achieved_rps"]) for r in rows) / n
    clean = sum(int(r["sla_clean"]) for r in rows)
    dpu = [number(r["dpu_arm_cores"]) for r in rows if r["dpu_arm_cores"] not in ("NA", "")]
    return {
        "client": client,
        "server": server,
        "host": client + server,
        "achieved": achieved,
        "clean": clean,
        "runs": n,
        "dpu": sum(dpu) / len(dpu) if dpu else float("nan"),
        "p99": max(number(r["p99_us"]) for r in rows),
    }


def curves(rows, src):
    """Clean points per config and frame, ordered by offered rate."""
    out = defaultdict(list)
    for (config, frame, rate), group in rows.items():
        stat = point(group)
        if stat["clean"] * 2 < stat["runs"]:          # majority-clean only
            continue
        out[(config, frame)].append((rate, stat, src.get((config, frame, rate), "")))
    for key in out:
        out[key].sort()
    return out


def capacity(curve):
    """Highest majority-clean point: the throughput a saturated host core sustains."""
    best = None
    for rate, stat, _ in curve:
        if best is None or stat["achieved"] > best[1]["achieved"]:
            best = (rate, stat)
    return best


def save(fig, output, stem):
    output.mkdir(parents=True, exist_ok=True)
    fig.savefig(output / f"{stem}.png", dpi=220, bbox_inches="tight")
    fig.savefig(output / f"{stem}.pdf", bbox_inches="tight")
    plt.close(fig)


def figure_cpu(data, output):
    """Host cores consumed against offered load, one panel per frame size."""
    fig, axes = plt.subplots(1, 3, figsize=(13.0, 4.2))
    for ax, frame in zip(axes, FRAMES):
        for config in CONFIGS:
            curve = data.get((config, frame), [])
            if not curve:
                continue
            x = [rate for rate, _, _ in curve]
            y = [stat["host"] for _, stat, _ in curve]
            ax.plot(x, y, marker=MARKERS[config], markersize=4.5, linewidth=1.6,
                    color=COLORS[config], label=LABELS[config] if frame == FRAMES[0] else None)
        ax.axhline(HOST_BUDGET, color="#333333", linestyle="--", linewidth=0.9)
        ax.text(0.985, HOST_BUDGET - 0.055, "2-core budget", transform=ax.get_yaxis_transform(),
                ha="right", va="top", fontsize=8, color="#333333")
        ax.set_xscale("log")
        ax.set_ylim(0, HOST_BUDGET * 1.08)
        ax.set_title(FRAME_LABEL[frame])
        ax.set_xlabel("Offered load (RPC/s)")
    axes[0].set_ylabel("Host cores consumed\n(client + server)")
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, ncol=4, frameon=False, loc="upper center",
               bbox_to_anchor=(0.5, 1.04))
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    save(fig, output, "01_host_cpu_vs_load")


def figure_matched(data, src, output):
    """Host cores at the loads every configuration serves cleanly."""
    fig, axes = plt.subplots(1, 3, figsize=(13.0, 4.2), sharey=True)
    width = 0.2
    for ax, frame in zip(axes, FRAMES):
        common = sorted({rate for (config, f), curve in data.items() if f == frame
                         for rate, _, source in curve if source == "common"})
        common = [r for r in common
                  if all(any(rate == r for rate, _, _ in data.get((c, frame), []))
                         for c in CONFIGS)]
        if not common:
            continue
        idx = np.arange(len(common))
        for offset, config in enumerate(CONFIGS):
            lookup = {rate: stat for rate, stat, _ in data[(config, frame)]}
            y = [lookup[r]["host"] for r in common]
            ax.bar(idx + (offset - 1.5) * width, y, width, color=COLORS[config],
                   label=LABELS[config] if frame == FRAMES[0] else None)
        ax.axhline(HOST_BUDGET, color="#333333", linestyle="--", linewidth=0.9)
        ax.set_xticks(idx, [human(r) + "/s" for r in common])
        ax.set_ylim(0, HOST_BUDGET * 1.08)
        ax.set_title(FRAME_LABEL[frame])
        ax.set_xlabel("Offered load")
    axes[0].set_ylabel("Host cores consumed\n(client + server)")
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, ncol=4, frameon=False, loc="upper center",
               bbox_to_anchor=(0.5, 1.04))
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    save(fig, output, "02_host_cpu_matched_load")


def figure_capacity(data, output):
    """Throughput each configuration sustains once its host core saturates."""
    fig, axes = plt.subplots(1, 3, figsize=(13.0, 4.2))
    for ax, frame in zip(axes, FRAMES):
        values, labels, colors = [], [], []
        for config in CONFIGS:
            best = capacity(data.get((config, frame), []))
            if not best:
                continue
            values.append(best[1]["achieved"])
            labels.append(SHORT[config])
            colors.append(COLORS[config])
        if not values:
            continue
        idx = np.arange(len(values))
        ax.bar(idx, values, 0.62, color=colors)
        base = values[0]
        for i, v in enumerate(values):
            ax.text(i, v, f"{v / 1e6:.2f}M\n{v / base:.1f}×" if v >= 1e6
                    else f"{v / 1e3:.0f}k\n{v / base:.1f}×",
                    ha="center", va="bottom", fontsize=8.5)
        ax.set_xticks(idx, labels)
        ax.set_ylim(0, max(values) * 1.24)
        ax.set_title(FRAME_LABEL[frame])
    axes[0].set_ylabel("Sustained throughput (RPC/s)\nat host-core saturation")
    fig.tight_layout()
    save(fig, output, "03_fixed_budget_throughput")


def write_points(data, output):
    fields = ["figure", "config", "frame_bytes", "offered_rps", "achieved_rps", "source",
              "client_cores", "server_cores", "host_cores", "dpu_arm_cores",
              "p99_us", "clean_votes", "runs"]
    output.mkdir(parents=True, exist_ok=True)
    with (output / "figure_points.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for (config, frame), curve in sorted(data.items()):
            for rate, stat, source in curve:
                writer.writerow({
                    "figure": "cpu/capacity", "config": config, "frame_bytes": frame,
                    "offered_rps": rate, "achieved_rps": round(stat["achieved"], 1),
                    "source": source,
                    "client_cores": round(stat["client"], 4),
                    "server_cores": round(stat["server"], 4),
                    "host_cores": round(stat["host"], 4),
                    "dpu_arm_cores": "" if stat["dpu"] != stat["dpu"] else round(stat["dpu"], 3),
                    "p99_us": round(stat["p99"], 1),
                    "clean_votes": stat["clean"], "runs": stat["runs"],
                })


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dataset", type=Path)
    parser.add_argument("--out", type=Path, default=Path("bench/report/figures"))
    args = parser.parse_args()

    data = curves(load(args.dataset), sources(args.dataset))
    figure_cpu(data, args.out)
    figure_matched(data, sources(args.dataset), args.out)
    figure_capacity(data, args.out)
    write_points(data, args.out)
    print(f"plot_l4: figures={args.out} points={args.out / 'figure_points.csv'}")


if __name__ == "__main__":
    main()
