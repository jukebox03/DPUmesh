#!/usr/bin/env python3
"""Render the host-CPU and fixed-budget throughput figures for the gRPC paths.

Same layout and metric definitions as plot_final.py; the configuration set and
the bar centring differ because three L7 paths are compared rather than four.
"""

import csv
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

CONFIGS = ["grpc-envoy-permissive", "grpc-tcp", "grpc-dpumesh"]
LABELS = {
    "grpc-envoy-permissive": "gRPC via Envoy",
    "grpc-tcp": "gRPC direct TCP",
    "grpc-dpumesh": "gRPC via DPUmesh",
}
SHORT = {
    "grpc-envoy-permissive": "gRPC\nvia Envoy",
    "grpc-tcp": "gRPC\ndirect TCP",
    "grpc-dpumesh": "gRPC\nvia DPUmesh",
}
COLORS = {
    "grpc-envoy-permissive": "#5B6573",
    "grpc-tcp": "#D55E00",
    "grpc-dpumesh": "#009E73",
}
FRAMES = [64, 1024, 8192]
FRAME_LABEL = {64: "64 B", 1024: "1 KiB", 8192: "8 KiB"}
BUDGET = 2.0

plt.rcParams.update({
    "figure.dpi": 120, "font.size": 9.5, "axes.titlesize": 10.5,
    "axes.labelsize": 9.5, "legend.fontsize": 9,
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.alpha": 0.18, "grid.linewidth": 0.6,
})


def human(v):
    return f"{v/1e6:.2f}M" if v >= 1e6 else f"{v/1e3:.0f}k"


def load(path):
    rows = []
    with open(path) as handle:
        for r in csv.DictReader(handle):
            rows.append({
                "config": r["config"], "frame": int(r["frame"]), "kind": r["kind"],
                "offered": float(r["offered"]), "achieved": float(r["achieved"]),
                "client": float(r["client"]), "server": float(r["server"]),
                "p99": float(r["p99"]), "drop": float(r["drop"]), "clean": int(r["clean"]),
            })
    return rows


def save(fig, out, stem):
    out.mkdir(parents=True, exist_ok=True)
    fig.savefig(out / f"{stem}.png", dpi=220, bbox_inches="tight")
    fig.savefig(out / f"{stem}.pdf", bbox_inches="tight")
    plt.close(fig)


def figure_cpu(rows, out):
    fig, axes = plt.subplots(1, 3, figsize=(13.0, 4.2), sharey=True)
    width = 0.26
    centre = (len(CONFIGS) - 1) / 2.0
    for ax, frame in zip(axes, FRAMES):
        loads = sorted({r["offered"] for r in rows
                        if r["frame"] == frame and r["kind"] == "common"})
        idx = np.arange(len(loads))
        for off, config in enumerate(CONFIGS):
            y = []
            for load_rate in loads:
                m = [r for r in rows if r["frame"] == frame and r["config"] == config
                     and r["kind"] == "common" and r["offered"] == load_rate]
                y.append(m[0]["client"] + m[0]["server"] if m else 0.0)
            ax.bar(idx + (off - centre) * width, y, width, color=COLORS[config],
                   label=LABELS[config] if frame == FRAMES[0] else None)
        ax.axhline(BUDGET, color="#333333", linestyle="--", linewidth=0.9)
        ax.set_xticks(idx, [human(v) + "/s" for v in loads])
        ax.set_ylim(0, BUDGET * 1.08)
        ax.set_title(FRAME_LABEL[frame])
        ax.set_xlabel("Offered load")
    axes[0].set_ylabel("Host cores consumed\n(client + server)")
    h, l = axes[0].get_legend_handles_labels()
    fig.legend(h, l, ncol=3, frameon=False, loc="upper center", bbox_to_anchor=(0.5, 1.04))
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    save(fig, out, "01_grpc_host_cpu_by_load")


def capacities(rows):
    best = {}
    for r in rows:
        if not r["clean"]:
            continue
        key = (r["frame"], r["config"])
        if key not in best or r["achieved"] > best[key]["achieved"]:
            best[key] = r
    return best


def figure_capacity(rows, out):
    best = capacities(rows)
    fig, axes = plt.subplots(1, 3, figsize=(13.0, 4.2))
    for ax, frame in zip(axes, FRAMES):
        vals, labels, colors = [], [], []
        for config in CONFIGS:
            r = best.get((frame, config))
            vals.append(r["achieved"] if r else 0.0)
            labels.append(SHORT[config])
            colors.append(COLORS[config])
        idx = np.arange(len(vals))
        ax.bar(idx, vals, 0.62, color=colors)
        base = vals[0] or 1.0
        for i, v in enumerate(vals):
            ax.text(i, v, (f"{human(v)}\n{v/base:.2f}×" if v else "no clean\nboundary"),
                    ha="center", va="bottom", fontsize=8.5)
        ax.set_xticks(idx, labels)
        ax.set_ylim(0, (max(vals) or 1) * 1.26)
        ax.set_title(FRAME_LABEL[frame])
    axes[0].set_ylabel("Sustained throughput (RPC/s)\nat host-core saturation")
    fig.tight_layout()
    save(fig, out, "02_grpc_fixed_budget_throughput")


def main():
    rows = load(sys.argv[1])
    out = Path(sys.argv[2] if len(sys.argv) > 2 else "bench/report/figures")
    figure_cpu(rows, out)
    figure_capacity(rows, out)
    best = capacities(rows)
    print("=== capacity (highest clean) ===")
    for frame in FRAMES:
        base = best.get((frame, "grpc-envoy-permissive"))
        for config in CONFIGS:
            r = best.get((frame, config))
            if not r:
                print(f"{frame:>5}B {config:22} no clean boundary")
                continue
            ratio = r["achieved"] / base["achieved"] if base else float("nan")
            print(f"{frame:>5}B {config:22}{r['achieved']:>11,.0f}  {ratio:5.2f}x"
                  f"  cli={r['client']:.3f} srv={r['server']:.3f} p99={r['p99']/1000:5.2f}ms")
    print("\n=== host cores at matched loads ===")
    for frame in FRAMES:
        for load_rate in sorted({r["offered"] for r in rows
                                 if r["frame"] == frame and r["kind"] == "common"}):
            cells = []
            for config in CONFIGS:
                m = [r for r in rows if r["frame"] == frame and r["config"] == config
                     and r["kind"] == "common" and r["offered"] == load_rate]
                cells.append(f"{config.replace('grpc-','')[:6]}={m[0]['client']+m[0]['server']:.3f}"
                             if m else "-")
            print(f"{frame:>5}B {load_rate:>10,.0f}/s  " + "  ".join(cells))


if __name__ == "__main__":
    main()
