#!/usr/bin/env python3
"""Render the host-core attribution figures from core_layers.py output.

  <stem>_cost_split   one exclusive core per bar at the highest measured rate,
                      split into the seven things that spend it
  <stem>_per_rpc      the same split divided by the requests served
  <stem>_load_curve   cost per request and cores consumed against offered rate

User-space time goes to its owner and kernel time to the subsystem its leaf
names.
"""
from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

CONFIG_ORDER = [
    "envoy-permissive", "envoy-strict", "dpumesh-preload", "dpumesh-native",
    "grpc-envoy-permissive", "grpc-envoy-strict", "grpc-tcp", "grpc-dpumesh",
]
CONFIG_LABEL = {
    "envoy-permissive": "Envoy\npermissive",
    "envoy-strict": "Envoy\nstrict",
    "dpumesh-preload": "DPUmesh\npreload",
    "dpumesh-native": "DPUmesh\nnative",
    "grpc-envoy-permissive": "gRPC\nEnvoy",
    "grpc-envoy-strict": "gRPC\nEnvoy mTLS",
    "grpc-tcp": "gRPC\ndirect TCP",
    "grpc-dpumesh": "gRPC\nDPUmesh",
}
CONFIG_LINE = {k: v.replace("\n", " ") for k, v in CONFIG_LABEL.items()}

# Buckets, in stacking order. Hues are assigned in this order and never cycled.
BUCKETS = ["app", "transport", "grpc", "envoy", "net", "syscall", "sched", "other"]
BUCKET_LABEL = {
    "app": "application code",
    "transport": "transport library + verbs driver",
    "grpc": "gRPC runtime",
    "envoy": "Envoy sidecar",
    "net": "kernel sockets",
    "syscall": "kernel syscall + poll",
    "sched": "scheduler + wake-up",
    "other": "unattributed",
}
BUCKET_COLOR = {
    "app": "#2a78d6", "transport": "#1baf7a", "grpc": "#eda100",
    "envoy": "#e87ba4", "net": "#008300", "syscall": "#4a3aa7",
    "sched": "#e34948", "other": "#c9c6bd",
}
ROLLUP = ["app", "transport", "grpc", "sched"]
ROLLUP_LABEL = {
    "app": "application code",
    "transport": "transport (library, sidecar, kernel stack, driver)",
    "grpc": "gRPC runtime",
    "sched": "scheduler + wake-up",
}
ROLLUP_OF = {
    "app": "app", "transport": "transport", "grpc": "grpc", "envoy": "transport",
    "net": "transport", "syscall": "transport", "sched": "sched", "other": None,
}
# One hue per configuration for the load curve. These are the colours the other
# reports give the same configurations, and they are deliberately distinct from
# the layer colours above, which appear in the split figures of this report.
LINE_COLOR = {
    "envoy-permissive": "#5B6573", "envoy-strict": "#D55E00",
    "dpumesh-preload": "#0072B2", "dpumesh-native": "#009E73",
    "grpc-envoy-permissive": "#5B6573", "grpc-envoy-strict": "#D55E00",
    "grpc-tcp": "#0072B2", "grpc-dpumesh": "#009E73",
}
MARKERS = ["o", "s", "^", "D"]

FRAME_LABEL = {48: "64 B frame", 1008: "1 KiB frame", 8176: "8 KiB frame"}

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


def bucket_of(layer: str, site: str) -> str:
    if site == "unresolved" or layer == "noise":
        return "other"
    if site == "kernel-sched":
        return "sched"
    if site == "kernel-net":
        return "net"
    if site == "driver-code":
        return "transport"
    if site in ("kernel-syscall", "kernel-poll", "kernel-other"):
        return "syscall"
    if layer in ("app", "transport", "grpc", "envoy"):
        return layer
    return "other"


def frame_title(frame, rate):
    return (f"{FRAME_LABEL.get(frame, str(frame + 16) + ' B frame')}, "
            f"{rate/1000:.0f}k/s offered")


def draw(ax, cells, keys, colors, labels, configs, title, fmt, ymax):
    """Two bars per configuration — client core, then server core."""
    width = 0.36
    idx = np.arange(len(configs))
    offs = {"client": -width / 2 - 0.012, "server": width / 2 + 0.012}
    for role in ("client", "server"):
        bottom = np.zeros(len(configs))
        for key in keys:
            vals = np.array([cells.get((c, role, key), 0.0) for c in configs])
            if vals.sum() <= 0:
                continue
            ax.bar(idx + offs[role], vals, width, bottom=bottom, color=colors[key],
                   label=labels[key], linewidth=1.0, edgecolor="white")
            bottom += vals
        for i, total in enumerate(bottom):
            if total > 0:
                ax.text(idx[i] + offs[role], total + ymax * 0.012, fmt(total),
                        ha="center", va="bottom", fontsize=8, color="#3a3a38")
    ticks, names = [], []
    for i, _ in enumerate(configs):
        ticks += [idx[i] + offs["client"], idx[i] + offs["server"]]
        names += ["C", "S"]
    ax.set_xticks(ticks)
    ax.set_xticklabels(names, fontsize=8, color="#6b6b66")
    ax.tick_params(axis="x", length=0)
    for i, config in enumerate(configs):
        ax.text(idx[i], -0.055, CONFIG_LABEL.get(config, config),
                transform=ax.get_xaxis_transform(), ha="center", va="top")
    ax.set_xlim(-0.6, len(configs) - 0.4)
    ax.set_title(title, pad=14)
    ax.set_ylim(0, ymax)


def legend_of(ax, fig, ncol, y=-0.125):
    handles, names = ax.get_legend_handles_labels()
    seen, h, n = set(), [], []
    for handle, name in zip(handles, names):
        if name not in seen:
            seen.add(name)
            h.append(handle)
            n.append(name)
    fig.legend(h, n, loc="lower center", ncol=min(len(n), ncol), frameon=False,
               bbox_to_anchor=(0.5, y))


def save(fig, out: Path, stem: str):
    out.mkdir(parents=True, exist_ok=True)
    fig.savefig(out / f"{stem}.png", dpi=220, bbox_inches="tight")
    fig.savefig(out / f"{stem}.pdf", bbox_inches="tight")
    plt.close(fig)


def headroom(table):
    totals = defaultdict(float)
    for f in table:
        for (config, role, _), v in table[f].items():
            totals[(f, config, role)] += v
    return max(totals.values(), default=1.0) * 1.16


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data", help="directory holding cross.csv and points.csv")
    ap.add_argument("--out", required=True)
    ap.add_argument("--stem", default="core")
    args = ap.parse_args()
    data, out = Path(args.data), Path(args.out)

    cross = read(data / "cross.csv")
    configs = [c for c in CONFIG_ORDER if c in {r["config"] for r in cross}]
    frames = sorted({int(r["body"]) for r in cross})
    top = {f: max(int(r["rate"]) for r in cross if int(r["body"]) == f) for f in frames}

    cells = defaultdict(lambda: defaultdict(float))
    for r in cross:
        cells[(int(r["body"]), int(r["rate"]))][
            (r["config"], r["role"], bucket_of(r["layer"], r["site"]))] += float(r["cores"])
    peak = {f: cells[(f, top[f])] for f in frames}

    ymax = headroom(peak)
    fig, axes = plt.subplots(1, len(frames), figsize=(6.6 * len(frames), 4.8),
                             sharey=True, squeeze=False)
    for ax, frame in zip(axes[0], frames):
        draw(ax, peak[frame], BUCKETS, BUCKET_COLOR, BUCKET_LABEL, configs,
             frame_title(frame, top[frame]), lambda v: f"{v:.2f}", ymax)
    axes[0][0].set_ylabel("host cores")
    legend_of(axes[0][0], fig, 4)
    fig.text(0.5, -0.20, "C = client core, S = server core; each bar is one "
             "exclusive physical core", ha="center", fontsize=8.5, color="#6b6b66")
    fig.tight_layout()
    save(fig, out, f"{args.stem}_cost_split")

    per = {f: defaultdict(float) for f in frames}
    for frame in frames:
        for (config, role, b), v in peak[frame].items():
            key = ROLLUP_OF.get(b)
            if key:
                per[frame][(config, role, key)] += v * 1e9 / top[frame]
    ymax = headroom(per)
    fig, axes = plt.subplots(1, len(frames), figsize=(6.6 * len(frames), 4.8),
                             sharey=True, squeeze=False)
    for ax, frame in zip(axes[0], frames):
        draw(ax, per[frame], ROLLUP, BUCKET_COLOR, ROLLUP_LABEL, configs,
             frame_title(frame, top[frame]), lambda v: f"{v:.0f}", ymax)
    axes[0][0].set_ylabel("host core nanoseconds per request")
    legend_of(axes[0][0], fig, 4)
    fig.text(0.5, -0.20, "C = client core, S = server core; a saturated endpoint's "
             "bar is its whole core divided by the rate it served",
             ha="center", fontsize=8.5, color="#6b6b66")
    fig.tight_layout()
    save(fig, out, f"{args.stem}_per_rpc")

    curve_frame = min(frames)
    rates = sorted({int(r["rate"]) for r in cross if int(r["body"]) == curve_frame})
    if len(rates) < 2:
        print(f"wrote {out}/{args.stem}_{{cost_split,per_rpc}}.png")
        return
    host = defaultdict(float)
    for r in read(data / "points.csv"):
        if int(r["body"]) == curve_frame:
            host[(r["config"], int(r["rate"]))] += float(r["cores"])
    fig, axes = plt.subplots(1, 2, figsize=(12.4, 4.4), sharex=True)
    for i, config in enumerate(configs):
        xs = [x for x in rates if (config, x) in host]
        style = dict(marker=MARKERS[i % len(MARKERS)], markersize=5, lw=2,
                     color=LINE_COLOR.get(config, "#6b6b66"),
                     label=CONFIG_LINE.get(config, config))
        axes[0].plot(xs, [host[(config, x)] * 1e9 / x for x in xs], **style)
        axes[1].plot(xs, [host[(config, x)] for x in xs], **style)
    axes[0].set_ylabel("host core nanoseconds per request")
    axes[0].set_title("cost per request")
    axes[1].set_ylabel("host cores, client + server")
    axes[1].set_title("cores consumed")
    axes[1].axhline(2.0, color="#8a8a84", lw=1.0, ls=(0, (4, 3)), zorder=1)
    for ax in axes:
        ax.set_xlabel("offered requests/s")
        ax.set_xlim(0, max(rates) * 1.06)
        ax.set_ylim(0, None)
    legend_of(axes[0], fig, 4, y=-0.10)
    fig.text(0.5, -0.17, f"{FRAME_LABEL.get(curve_frame)}; dashed line = the "
             "two-core budget", ha="center", fontsize=8.5, color="#6b6b66")
    fig.tight_layout()
    save(fig, out, f"{args.stem}_load_curve")
    print(f"wrote {out}/{args.stem}_{{cost_split,per_rpc,load_curve}}.png")


if __name__ == "__main__":
    main()
