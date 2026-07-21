#!/usr/bin/env python3
"""Render the two figures used for the current lab-meeting performance story."""
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


DPU = "#0072B2"
ENVOY = "#D55E00"
GREY = "#777777"


def load(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def val(row, key):
    return float(row[key])


def size_label(size):
    if size >= 1024 * 1024: return f"{size // (1024 * 1024)} MiB"
    if size >= 1024: return f"{size // 1024} KiB"
    return f"{size} B"


def size_axis(axis, sizes):
    sizes = sorted(set(sizes))
    axis.set_xscale("log", base=2)
    axis.set_xticks(sizes)
    axis.set_xticklabels([size_label(size) for size in sizes], rotation=25, ha="right")


def group(rows, stage, transport):
    return [row for row in rows if row["stage"] == stage and row["transport"] == transport]


def meeting_gap(rows, out_dir):
    fig, (lat, scale) = plt.subplots(1, 2, figsize=(11.5, 4.2))
    for transport, label, color in (
        ("dpumesh-native", "DPUmesh native", DPU),
        ("tcp-envoy", "two-sided Envoy L4", ENVOY),
    ):
        pts = sorted(group(rows, "rtt", transport), key=lambda row: int(row["req_size"]))
        sizes = [int(row["req_size"]) for row in pts]
        lat.plot(sizes, [val(row, "p50_med") for row in pts], "-o", color=color, label=label)
        lat.fill_between(sizes, [val(row, "p50_lo") for row in pts],
                        [val(row, "p50_hi") for row in pts], color=color, alpha=.15)

        pts = sorted(group(rows, "conc", transport), key=lambda row: int(row["conc"]))
        conc = [int(row["conc"]) for row in pts]
        scale.plot(conc, [val(row, "mrps_med") for row in pts], "-o", color=color, label=label)
        scale.fill_between(conc, [val(row, "mrps_lo") for row in pts],
                           [val(row, "mrps_hi") for row in pts], color=color, alpha=.15)

    sizes = [int(row["req_size"]) for row in group(rows, "rtt", "dpumesh-native")]
    size_axis(lat, sizes)
    lat.set_xlabel("request size (8 B reply, concurrency=1)")
    lat.set_ylabel("p50 RTT (µs)")
    lat.set_title("Small messages pay the DPU traversal cost")

    scale.set_xscale("log", base=2)
    scale.set_xticks([1, 2, 4, 8, 16, 32, 64])
    scale.set_xticklabels(["1", "2", "4", "8", "16", "32", "64"])
    scale.set_xlabel("outstanding requests / connection")
    scale.set_ylabel("throughput (Mrps)")
    scale.set_title("Concurrency hides most, but not all, of the gap")

    for axis in (lat, scale):
        axis.grid(True, which="both", alpha=.3)
        axis.legend(fontsize=8)
    fig.suptitle("Current position against a minimal two-sided Envoy L4 proxy")
    fig.tight_layout(rect=[0, 0, 1, .92])
    path = os.path.join(out_dir, "fig_meeting_current_gap.png")
    fig.savefig(path, dpi=180)
    plt.close(fig)
    return path


def meeting_targets(rows, cpu_rows, out_dir):
    fig, (window, cpu) = plt.subplots(1, 2, figsize=(11.5, 4.35))
    for transport, label, color in (
        ("dpumesh-native", "DPUmesh native", DPU),
        ("tcp-envoy", "two-sided Envoy L4", ENVOY),
    ):
        pts = sorted(group(rows, "bw", transport), key=lambda row: int(row["req_size"]))
        sizes = [int(row["req_size"]) for row in pts]
        achieved = [val(row, "mrps_med") * val(row, "avg_med") for row in pts]
        window.plot(sizes, achieved, "-o", color=color, label=label)
        if transport == "dpumesh-native":
            for size, n in zip(sizes, achieved):
                if size >= 65536:
                    window.annotate(f"{n:.1f}", (size, n), xytext=(0, 7),
                                    textcoords="offset points", ha="center", fontsize=8, color=color)
    window.axhline(32, color=GREY, linestyle=":", linewidth=1.5, label="requested window = 32")
    size_axis(window, [int(row["req_size"]) for row in group(rows, "bw", "dpumesh-native")])
    window.set_xlabel("request size (8 B reply)")
    window.set_ylabel("achieved outstanding requests")
    window.set_ylim(0, 36)
    window.set_title("Large-message window collapses in the native path")

    mapped = {"dpumesh": "DPUmesh host", "tcp": "Envoy host"}
    styles = {
        "DPUmesh host": (DPU, "-o"),
        "Envoy host": (ENVOY, "-o"),
    }
    for transport in ("dpumesh", "tcp"):
        pts = sorted([row for row in cpu_rows if row["transport"] == transport],
                     key=lambda row: int(row["req"]))
        sizes = [int(row["req"]) for row in pts]
        label = mapped[transport]
        color, style = styles[label]
        med = [val(row, "host_total_pct_med") / 100.0 for row in pts]
        lo = [val(row, "host_total_pct_lo") / 100.0 for row in pts]
        hi = [val(row, "host_total_pct_hi") / 100.0 for row in pts]
        cpu.plot(sizes, med, style, color=color, label=label)
        cpu.fill_between(sizes, lo, hi, color=color, alpha=.15)
        if transport == "dpumesh":
            arm = [val(row, "dpu_arm_pct_med") / 100.0 for row in pts]
            arm_lo = [val(row, "dpu_arm_pct_lo") / 100.0 for row in pts]
            arm_hi = [val(row, "dpu_arm_pct_hi") / 100.0 for row in pts]
            cpu.plot(sizes, arm, "--s", color=DPU, alpha=.75, label="DPUmesh DPU ARM")
            cpu.fill_between(sizes, arm_lo, arm_hi, color=DPU, alpha=.08)
    size_axis(cpu, [int(row["req"]) for row in cpu_rows])
    cpu.set_xlabel("request size (8 B reply, requested window=32)")
    cpu.set_ylabel("occupied cores")
    cpu.set_ylim(0, 4.7)
    cpu.set_title("Host work is offloaded, but ARM cost remains high")

    for axis in (window, cpu):
        axis.grid(True, which="both", alpha=.3)
        axis.legend(fontsize=8)
    fig.suptitle("Current optimization targets")
    fig.text(.5, .015, "Host and DPU ARM are different processor domains; their core counts are not summed.",
             ha="center", fontsize=8, color="#555555")
    fig.tight_layout(rect=[0, .05, 1, .92])
    path = os.path.join(out_dir, "fig_meeting_optimization_targets.png")
    fig.savefig(path, dpi=180)
    plt.close(fig)
    return path


def main():
    if len(sys.argv) != 4:
        print("usage: plot_meeting.py <summary.csv> <cpu_summary.csv> <out_dir>", file=sys.stderr)
        raise SystemExit(2)
    rows = load(sys.argv[1])
    cpu_rows = load(sys.argv[2])
    os.makedirs(sys.argv[3], exist_ok=True)
    for path in (meeting_gap(rows, sys.argv[3]), meeting_targets(rows, cpu_rows, sys.argv[3])):
        print("wrote", path, file=sys.stderr)


if __name__ == "__main__":
    main()
