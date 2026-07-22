#!/usr/bin/env python3
"""Render simple DPUmesh-vs-Envoy comparisons across message sizes."""
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


DPU = "#0072B2"
ENVOY = "#D55E00"
GOOD = "#009E73"


def load(path):
    with open(path, newline="") as source:
        return list(csv.DictReader(source))


def size_label(size):
    if size >= 1024 * 1024:
        return f"{size // (1024 * 1024)} MiB"
    if size >= 1024:
        return f"{size // 1024} KiB"
    return f"{size} B"


def setup_axis(axis, sizes, ylabel, title):
    axis.set_xscale("log", base=2)
    axis.set_yscale("log")
    axis.set_xticks(sizes)
    axis.set_xticklabels([size_label(size) for size in sizes], rotation=28, ha="right")
    axis.set_xlabel("request size")
    axis.set_ylabel(ylabel)
    axis.set_title(title)
    axis.grid(True, which="both", alpha=.25)


def plot_latency(summary_rows, axis, sizes):
    points = {}
    for row in summary_rows:
        if row["stage"] != "rtt" or row["transport"] not in ("tcp-envoy", "dpumesh-native"):
            continue
        points[(row["transport"], int(row["req_size"]))] = float(row["p50_med"])
    sizes = [size for size in sizes
             if ("tcp-envoy", size) in points and ("dpumesh-native", size) in points]

    axis.plot(sizes, [points[("tcp-envoy", size)] for size in sizes],
              "-o", color=ENVOY, label="Envoy L4")
    axis.plot(sizes, [points[("dpumesh-native", size)] for size in sizes],
              "-o", color=DPU, label="DPUmesh")
    setup_axis(axis, sizes, "p50 RTT (µs, log scale)", "Latency")
    axis.set_xlabel("request size\n(concurrency=1)")


def cpu_points(cpu_rows, transport, metric):
    return {
        int(row["req"]): float(row[metric])
        for row in cpu_rows
        if row["transport"] == transport
        and int(row["reply"]) == 8
        and int(row["conc"]) == 32
    }


def plot_throughput(cpu_rows, axis, sizes):
    envoy = cpu_points(cpu_rows, "tcp", "mrps_med")
    dpu = cpu_points(cpu_rows, "dpumesh", "mrps_med")

    axis.plot(sizes, [envoy[size] for size in sizes], "-o", color=ENVOY, label="Envoy L4")
    axis.plot(sizes, [dpu[size] for size in sizes], "-o", color=DPU, label="DPUmesh")
    setup_axis(axis, sizes, "throughput (Mrps, log scale)", "Throughput")
    axis.set_xlabel("request size\n(requested concurrency=32)")


def plot_cpu(cpu_rows, axis, sizes):
    envoy = cpu_points(cpu_rows, "tcp", "host_us_per_req_med")
    dpu = cpu_points(cpu_rows, "dpumesh", "host_us_per_req_med")

    axis.plot(sizes, [envoy[size] for size in sizes], "-o", color=ENVOY, label="Envoy L4")
    axis.plot(sizes, [dpu[size] for size in sizes], "-o", color=DPU, label="DPUmesh")
    for size in sizes:
        saved = 100.0 * (1.0 - dpu[size] / envoy[size])
        axis.annotate(
            f"−{saved:.0f}%", (size, dpu[size]), xytext=(0, -16),
            textcoords="offset points", ha="center", color=GOOD,
            fontsize=8, fontweight="bold",
        )
    setup_axis(axis, sizes, "host CPU time / request (µs, log scale)",
               "Host CPU work / request")
    axis.set_xlabel("request size\n(requested concurrency=32)")


def main():
    if len(sys.argv) != 4:
        print("usage: plot_size_comparison.py <summary.csv> <cpu_summary.csv> <out_dir>",
              file=sys.stderr)
        raise SystemExit(2)
    os.makedirs(sys.argv[3], exist_ok=True)
    summary_rows = load(sys.argv[1])
    cpu_rows = load(sys.argv[2])
    envoy_sizes = set(cpu_points(cpu_rows, "tcp", "mrps_med"))
    dpu_sizes = set(cpu_points(cpu_rows, "dpumesh", "mrps_med"))
    sizes = sorted(envoy_sizes & dpu_sizes)
    if not sizes:
        raise ValueError("no common DPUmesh/Envoy size points")

    fig, axes = plt.subplots(1, 3, figsize=(14.8, 4.5))
    plot_latency(summary_rows, axes[0], sizes)
    plot_throughput(cpu_rows, axes[1], sizes)
    plot_cpu(cpu_rows, axes[2], sizes)

    handles, labels = axes[0].get_legend_handles_labels()
    fig.suptitle("DPUmesh vs Envoy across message sizes", fontsize=15, y=.99)
    fig.legend(handles, labels, loc="upper center", ncol=2,
               bbox_to_anchor=(.5, .925), frameon=False)
    fig.text(
        .5, .018,
        "8 B reply; five-run medians. CPU = client + server, normalized by achieved throughput; runs are not rate-matched.",
        ha="center", fontsize=8, color="#555555",
    )
    fig.tight_layout(rect=[0, .075, 1, .86])
    path = os.path.join(sys.argv[3], "fig_size_comparison.png")
    fig.savefig(path, dpi=180)
    plt.close(fig)
    print("wrote", path, file=sys.stderr)


if __name__ == "__main__":
    main()
