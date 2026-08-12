#!/usr/bin/env python3
"""Host-core attribution at one matched load, as one paper figure.

Two panels, one exclusive client core and one exclusive server core each. Envoy's
own binary and libdpumesh share a bucket: both are the mesh's own instructions on
the host, and that is the comparison the split is for. DPU ARM cores are not
drawn — they are a different resource on a different budget, and a bar that mixed
them would invite the sum the report refuses to make.

Rows are the median-repetition figures of bench/report/REPORT_CORE.md.

  python3 bench/suite/plot_cost_split.py bench/report/figures
"""
from __future__ import annotations

import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# Buckets in stacking order; hues follow plot_core.py so the paper's figures
# name the same layer with the same colour.
BUCKETS = ["app", "grpc", "mesh", "net", "syscall", "sched"]
LABEL = {
    "app": "application code",
    "grpc": "gRPC runtime",
    "mesh": "mesh code (Envoy / libdpumesh)",
    "net": "kernel sockets",
    "syscall": "kernel syscall + poll",
    "sched": "scheduler + wake-up",
}
COLOR = {
    "app": "#2a78d6", "grpc": "#eda100", "mesh": "#1baf7a",
    "net": "#008300", "syscall": "#4a3aa7", "sched": "#e34948",
}

#                         app    grpc   mesh   net    syscall sched   total
L4 = [
    ("Envoy permissive", [0.053, 0.000, 0.272, 0.553, 0.576, 0.486], 1.940),
    ("Envoy strict",     [0.049, 0.000, 0.360, 0.529, 0.526, 0.468], 1.931),
    ("DPUmesh preload",  [0.058, 0.000, 0.136, 0.027, 0.194, 0.293], 0.707),
    ("DPUmesh native",   [0.071, 0.000, 0.073, 0.000, 0.119, 0.236], 0.498),
]
L7 = [
    ("gRPC via Envoy",      [0.011, 0.885, 0.144, 0.278, 0.349, 0.331], 1.999),
    ("gRPC via Envoy mTLS", [0.012, 0.885, 0.187, 0.256, 0.332, 0.321], 1.994),
    ("gRPC direct TCP",     [0.012, 0.909, 0.000, 0.244, 0.256, 0.275], 1.697),
    ("gRPC via DPUmesh",    [0.010, 0.816, 0.084, 0.000, 0.153, 0.227], 1.291),
]

PANELS = [
    ("(a) L4 byte stream, 64 B at 400 k/s", L4),
    ("(b) L7 unary gRPC, 64 B at 24 k/s", L7),
]

plt.rcParams.update({
    "figure.dpi": 120, "font.size": 9.5, "axes.titlesize": 9.5,
    "axes.labelsize": 9.5, "legend.fontsize": 8.5,
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.spines.left": False,
    "axes.grid": True, "axes.axisbelow": True,
    "grid.alpha": 0.18, "grid.linewidth": 0.6,
    "axes.edgecolor": "#9a9a95", "axes.linewidth": 0.8,
    "xtick.color": "#3a3a38", "ytick.color": "#3a3a38",
})

XMAX = 2.20
BAR_H = 0.62


def draw(ax, rows, title):
    y = np.arange(len(rows))[::-1]
    for yi, (_, vals, total) in zip(y, rows):
        left = 0.0
        for key, v in zip(BUCKETS, vals):
            if v <= 0:
                continue
            ax.barh(yi, v, left=left, height=BAR_H, color=COLOR[key],
                    label=LABEL[key], edgecolor="white", linewidth=1.0)
            left += v
        ax.text(left + 0.025, yi, f"{total:.3f}", ha="left", va="center",
                fontsize=8, color="#3a3a38")
    ax.set_yticks(y)
    ax.set_yticklabels([r[0] for r in rows])
    ax.tick_params(axis="y", length=0)
    ax.set_xlim(0, XMAX)
    ax.set_ylim(-0.6, len(rows) - 0.4)
    ax.set_xticks([0, 0.5, 1.0, 1.5, 2.0])
    ax.xaxis.grid(True)
    ax.yaxis.grid(False)
    ax.set_title(title, loc="left", pad=6)


fig, axes = plt.subplots(2, 1, figsize=(7.0, 4.5), sharex=True,
                         gridspec_kw={"hspace": 0.34})
for ax, (title, rows) in zip(axes, PANELS):
    draw(ax, rows, title)
axes[0].spines["bottom"].set_visible(False)   # the shared rule belongs to (b)
axes[-1].set_xlabel("host cores (client + server)")

handles, names = axes[1].get_legend_handles_labels()
seen, by_name = set(), {}
for handle, name in zip(handles, names):
    if name not in seen:
        seen.add(name)
        by_name[name] = handle
# fig.legend fills column-major, so interleave the stacking order into the
# columns it will lay out: user-space layers on the first row, kernel on the second.
COLUMN_MAJOR = ["app", "net", "grpc", "syscall", "mesh", "sched"]
keys = [b for b in COLUMN_MAJOR if LABEL[b] in by_name]
fig.legend([by_name[LABEL[b]] for b in keys], [LABEL[b] for b in keys],
           loc="lower center", ncol=3, frameon=False,
           bbox_to_anchor=(0.5, -0.10), handlelength=1.1, handleheight=1.1,
           columnspacing=1.6)

out = Path(sys.argv[1] if len(sys.argv) > 1 else "bench/report/figures")
out.mkdir(parents=True, exist_ok=True)
fig.savefig(out / "core_cost_split.png", dpi=220, bbox_inches="tight")
fig.savefig(out / "core_cost_split.pdf", bbox_inches="tight")
print(f"wrote {out}/core_cost_split.{{png,pdf}}")
