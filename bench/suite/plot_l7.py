#!/usr/bin/env python3
"""Render the L7-mode figures from an l7_modes.sh dataset.

Three figures, each answering one question the tables cannot.

`l7_delivery` plots the fraction of the offered rate that arrived, against the
offered rate. A capacity quoted as a single number is the first point this curve
leaves 1.0, which says nothing about what happens past it; the curve shows
whether a path holds its rate, degrades, or recovers.

`l7_band` is the same quantity measured densely over one such excursion, beside
the tail it costs, on the plain data path. It is the evidence that the first
failure is a lower bound and not a ceiling.

`l7_arm_cores` is what the DPU spends per request at rates every mode serves.
Cost per request depends on the load it is measured at, so the modes are only
comparable where the load is the same.

  usage: plot_l7.py DATASET_DIR OUT_DIR
"""

import csv
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, LogLocator

# Okabe-Ito, matching the other figures. The instrument is the neutral grey: it
# is the reference the modes are read against, not one of the compared paths.
COLORS = {
    "instrument": "#5B6573",
    "dataplane": "#0072B2",
    "decision": "#009E73",
    "opaque": "#56B4E9",
    "l7-conn": "#E69F00",
    "l7-message": "#CC79A7",
}
LABELS = {
    "instrument": "TCP instrument",
    "dataplane": "L4 data path",
    "decision": "decision",
    "opaque": "opaque",
    "l7-conn": "L7 per connection",
    "l7-message": "L7 per message",
}
ORDER = ["instrument", "dataplane", "decision", "opaque", "l7-conn", "l7-message"]
FRAME_LABEL = {"64": "64 B", "1024": "1 KiB", "8192": "8 KiB"}
DELIVERED = 0.98          # the ratio the collector calls delivered


def load(path):
    if not path.exists():
        return []
    with path.open() as f:
        return list(csv.DictReader(f))


def num(x, default=None):
    try:
        return float(x)
    except (TypeError, ValueError):
        return default


def style(ax):
    ax.grid(alpha=0.25, linewidth=0.5)
    ax.tick_params(labelsize=8)


def log_ticks(ax):
    """Plain decimals on a log axis.

    The default log formatter writes minor ticks as 2x10^-1, which collide on a
    panel spanning less than a decade; ScalarFormatter rounds 0.05 to 0. `%g`
    keeps every tick short and exact."""
    fmt = FuncFormatter(lambda v, _: f"{v:g}" if v > 0 else "")
    ax.xaxis.set_major_locator(LogLocator(base=10, numticks=6))
    ax.xaxis.set_minor_locator(LogLocator(base=10, subs=(2.0, 5.0), numticks=8))
    ax.xaxis.set_major_formatter(fmt)
    ax.xaxis.set_minor_formatter(fmt)


def save(fig, out_dir, stem):
    out_dir.mkdir(parents=True, exist_ok=True)
    for ext in ("png", "pdf"):
        fig.savefig(out_dir / f"{stem}.{ext}", dpi=200, bbox_inches="tight")
    print(f"wrote {out_dir}/{stem}.png and .pdf")
    plt.close(fig)


def series(rows, keep=None):
    """(mode, frame) -> [(offered, achieved, p99)], sorted by offered."""
    out = defaultdict(list)
    for r in rows:
        mode = r["mode"]
        if keep and mode not in keep:
            continue
        o, a = num(r["offered_rps"]), num(r["achieved_rps"])
        if not o or a is None:
            continue
        out[(mode, r["frame"])].append((o, a, num(r.get("p99_us"))))
    return {k: sorted(v) for k, v in out.items()}


def delivery_figure(sat, out_dir):
    curves = series(sat, keep=set(ORDER))
    frames = sorted({f for _, f in curves}, key=int)
    if not frames:
        return
    fig, axes = plt.subplots(1, len(frames), figsize=(4.1 * len(frames), 3.4),
                             squeeze=False, sharey=True)
    for ax, frame in zip(axes[0], frames):
        for m in [m for m in ORDER if (m, frame) in curves]:
            pts = curves[(m, frame)]
            ax.plot([p[0] / 1e6 for p in pts], [p[1] / p[0] for p in pts],
                    marker="o", markersize=3, linewidth=1.4,
                    color=COLORS[m], label=LABELS[m],
                    linestyle="--" if m == "instrument" else "-",
                    zorder=2 if m == "instrument" else 3)
        ax.axhline(DELIVERED, color="#BBBBBB", linewidth=0.9, zorder=1)
        ax.set_xscale("log")
        ax.set_ylim(0, 1.12)
        ax.set_title(FRAME_LABEL.get(frame, f"{frame} B"), fontsize=10)
        ax.set_xlabel("offered (Mrps, log)", fontsize=9)
        style(ax)
        log_ticks(ax)
    axes[0][0].set_ylabel("delivered / offered", fontsize=9)
    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=len(labels),
               frameon=False, fontsize=8.5, bbox_to_anchor=(0.5, -0.06))
    fig.tight_layout(rect=(0, 0.07, 1, 1))
    save(fig, out_dir, "l7_delivery")


def band_figure(band, out_dir):
    curves = series(band, keep=set(ORDER))
    if not curves:
        return
    # One line per frame of the same path, so the panel compares a frame that
    # has an excursion against one that does not.
    keys = sorted(curves, key=lambda k: (ORDER.index(k[0]), int(k[1])))
    # The frames were scanned over different rate ranges, so the series do not
    # overlap on the x axis. Marker and dash keep them from reading as one line.
    looks = [("#0072B2", "o", "-"), ("#D55E00", "s", "--"), ("#009E73", "^", ":")]
    fig, axes = plt.subplots(1, 2, figsize=(8.8, 3.4))
    for i, (m, frame) in enumerate(keys):
        pts = curves[(m, frame)]
        label = f"{LABELS[m]}, {FRAME_LABEL.get(frame, frame)}"
        color, mark, dash = looks[i % len(looks)]
        for ax, ys in ((axes[0], [p[1] / p[0] for p in pts]),
                       (axes[1], [(p[2] or 0) / 1000 for p in pts])):
            ax.plot([p[0] / 1e6 for p in pts], ys, marker=mark, markersize=4,
                    linewidth=1.5, color=color, linestyle=dash, label=label)
    axes[0].axhline(DELIVERED, color="#BBBBBB", linewidth=0.9, zorder=1)
    axes[0].set_ylim(0, 1.12)
    axes[0].set_ylabel("delivered / offered", fontsize=9)
    axes[1].set_yscale("log")
    axes[1].set_ylabel("p99 (ms, log)", fontsize=9)
    for ax in axes:
        ax.set_xlabel("offered (Mrps)", fontsize=9)
        style(ax)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=len(labels),
               frameon=False, fontsize=8.5, bbox_to_anchor=(0.5, -0.06))
    fig.tight_layout(rect=(0, 0.07, 1, 1))
    save(fig, out_dir, "l7_band")


def cores_figure(matched, out_dir):
    """ARM cores per delivered Mrps, at rates every mode serves.

    Repetitions are drawn as a band. One measurement moves by about a tenth
    between runs, and the differences between modes are of that size, so a
    single line per mode would assert a separation the data does not carry.
    """
    reps = defaultdict(list)
    for r in matched:
        rate, per = num(r["offered_rps"]), num(r["cores_per_mrps"])
        if rate and per:
            reps[(r["mode"], rate)].append(per)
    if not reps:
        return
    by = defaultdict(list)
    for (mode, rate), vals in reps.items():
        vals.sort()
        by[mode].append((rate, vals[len(vals) // 2], vals[0], vals[-1]))
    flat = [v for vals in reps.values() for v in vals]
    lo, hi = min(flat), max(flat)
    fig, ax = plt.subplots(figsize=(6.4, 3.8))
    for m in [m for m in ORDER if m in by]:
        pts = sorted(by[m])
        xs = [p[0] / 1000 for p in pts]
        ax.fill_between(xs, [p[2] for p in pts], [p[3] for p in pts],
                        color=COLORS[m], alpha=0.15, linewidth=0)
        ax.plot(xs, [p[1] for p in pts], marker="o", markersize=4,
                linewidth=1.6, color=COLORS[m], label=LABELS[m])
    ax.set_xlabel("offered (thousand requests/s)", fontsize=9)
    ax.set_ylabel("ARM cores per delivered Mrps (log)", fontsize=9)
    # The lowest rate costs five times the others, which on a linear axis
    # flattens the range where the modes actually separate.
    ax.set_yscale("log")
    # A decade locator puts no tick where the data sits, so name them.
    ticks = [t for t in (10, 12, 15, 20, 30, 50, 70, 100)
             if lo / 1.1 <= t <= hi * 1.1]
    ax.set_yticks(ticks)
    ax.set_yticks([], minor=True)
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}"))
    style(ax)
    ax.legend(frameon=False, fontsize=8.5)
    fig.tight_layout()
    save(fig, out_dir, "l7_arm_cores")


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    root, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    delivery_figure(load(root / "saturation.csv"), out_dir)
    band_figure(load(root / "band.csv"), out_dir)
    cores_figure(load(root / "cores_matched.csv"), out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
