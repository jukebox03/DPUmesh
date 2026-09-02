#!/usr/bin/env python3
"""Render the report figures from the committed receipts.

Run derive.py first; figures 09 and 10 read its derived-*.csv outputs.
"""

from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from matplotlib.lines import Line2D


ROOT = Path(__file__).resolve().parent
GRAPH = ROOT / "graphs"
GRAPH.mkdir(exist_ok=True)

# Categorical slots validated for adjacent and all-pairs separation.
FRAME_COLOR = {64: "#2a78d6", 1024: "#eb6834", 8192: "#1baf7a"}
FRAME_LABEL = {64: "64 B", 1024: "1 KiB", 8192: "8 KiB"}
TRANSPORT_COLOR = {"direct-tcp": "#8a8984", "DPUmesh": "#2a78d6", "Linkerd": "#eb6834"}
ROLE_COLOR = {"client": "#eb6834", "server": "#1baf7a", "dpu": "#0b0b0b"}
WORKER_COLOR = {4: "#86b6ef", 6: "#5598e7", 8: "#2a78d6", 12: "#184f95"}   # one-hue ordinal ramp
INK = "#0b0b0b"
INK2 = "#52514e"
GRID = "#e6e5e1"
REF = "#a8a7a1"

plt.rcParams.update({
    "font.size": 10, "axes.titlesize": 11.5, "axes.labelsize": 10,
    "legend.fontsize": 9, "figure.titlesize": 13,
    "text.color": INK, "axes.labelcolor": INK, "xtick.color": INK2, "ytick.color": INK2,
    "axes.edgecolor": INK2, "axes.spines.top": False, "axes.spines.right": False,
    "lines.linewidth": 1.6, "lines.markersize": 5.5, "legend.frameon": False,
    "savefig.facecolor": "white",
    "svg.hashsalt": "grpc-professor-20260902",   # deterministic SVG element ids
})


def style(ax):
    ax.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)


def finish(fig, name):
    # No timestamp metadata, so a regenerated file is byte-identical.
    fig.savefig(GRAPH / f"{name}.svg", bbox_inches="tight", metadata={"Date": None})
    fig.savefig(GRAPH / f"{name}.png", dpi=180, bbox_inches="tight")
    plt.close(fig)


def hollow(ax, x, y, color):
    ax.scatter(x, y, marker="o", s=34, facecolors="white", edgecolors=color,
               linewidths=1.3, zorder=4)


open_df = pd.read_csv(ROOT / "open-summary.csv")
knee = pd.read_csv(ROOT / "knee-followup-summary.csv")
low = pd.read_csv(ROOT / "dpu-low-load-summary.csv")
conc = pd.read_csv(ROOT / "concurrency-summary.csv")
closed = pd.read_csv(ROOT / "closed-summary.csv")
mesh_closed = pd.read_csv(ROOT / "mesh-closed-summary.csv")
mesh_cpu = pd.read_csv(ROOT / "mesh-cpu-summary.csv")
scale = pd.read_csv(ROOT / "worker-scale-summary.csv")
inflight = pd.read_csv(ROOT / "derived-inflight-cpu.csv")
comparison = pd.read_csv(ROOT / "derived-comparison.csv")
exchange = pd.read_csv(ROOT / "derived-exchange-10k.csv")
payload = pd.read_csv(ROOT / "derived-payload-scaling.csv")
knee_cost = pd.read_csv(ROOT / "derived-knee-cost.csv")
SLO_US = 5000

# The connected open-loop curve: the campaign grid plus the 3/3-clean 1 KiB and
# 8 KiB refinements. Mixed/bad refinements stay individual hollow circles.
knee_curve = knee[knee.series.isin(["fresh-1k", "fresh-8k"]) & knee.classification.eq("clean")].copy()
knee_curve["clean"] = "yes"
curve = pd.concat([open_df, knee_curve[open_df.columns.intersection(knee_curve.columns)]],
                  ignore_index=True, sort=False)

repeat_dirs = ["fresh-64-bracket", "fresh-64-98", "fresh-64-99", "fresh-1k-edge",
               "fresh-1k-76", "fresh-8k-30-clean", "fresh-8k-30-repeat"]
fresh = pd.concat([pd.read_csv(ROOT / "knee-followup-raw" / d / "points.csv") for d in repeat_dirs],
                  ignore_index=True)
fresh = fresh[~(fresh.frame.eq(64) & fresh.offered.eq(90000))]

# Low-load open-loop points (500 RPS upward) extend the latency and CPU curves
# to the left of the campaign grid.
low_ext = low.rename(columns={"achieved_med": "achieved_med"}).copy()

REPEAT_HANDLES = [
    Line2D([0], [0], color=INK2, marker="o", linestyle="--", label="first bad / overload"),
    Line2D([0], [0], color=INK2, marker="o", markerfacecolor="white", linestyle="none",
           label="fresh-redeploy repetition"),
]


def plot_curve(ax, frame, ycol, ybad=None, scale_y=1.0):
    g = curve[curve.frame.eq(frame)].sort_values("offered")
    clean, bad = g[g.clean.eq("yes")], g[g.clean.ne("yes")]
    ax.plot(clean.offered / 1e3, clean[ycol] * scale_y, "o-", color=FRAME_COLOR[frame],
            label=FRAME_LABEL[frame])
    if not bad.empty:
        bridge = pd.concat([clean.tail(1), bad])
        ax.plot(bridge.offered / 1e3, bridge[ycol] * scale_y, "o--", color=FRAME_COLOR[frame])
    reps = fresh[fresh.frame.eq(frame)]
    hollow(ax, reps.offered / 1e3, reps[ybad or ycol] * scale_y, FRAME_COLOR[frame])


# 00: the summary figure for FINAL.md. Highest closed-loop throughput of the two
# meshes on the same application, and their latency at the same 10k RPS.
frames = [64, 1024, 8192]
x = np.arange(3)
w = 0.34
fig, axes = plt.subplots(1, 3, figsize=(13.2, 4.3))
panels = [
    (axes[0], mesh_closed, "achieved_med", 1e-3, "Throughput (kRPC/s)",
     "Max RPS: closed loop, 1,024 in flight", "{:.1f}k"),
    (axes[1], mesh_cpu, "p50_med_us", 1.0, "p50 latency (µs)",
     "p50 at 10k RPS, open loop", "{:.0f}"),
    (axes[2], mesh_cpu, "p99_med_us", 1.0, "p99 latency (µs)",
     "p99 at 10k RPS, open loop", "{:.0f}"),
]
for ax, table, col, scale_y, ylabel, title, fmt in panels:
    for i, t in enumerate(["DPUmesh", "Linkerd"]):
        g = table[table.transport.eq(t)].set_index("frame").loc[frames]
        bars = ax.bar(x + (i - 0.5) * (w + 0.03), g[col] * scale_y, w,
                      color=TRANSPORT_COLOR[t], label=t)
        ax.bar_label(bars, labels=[fmt.format(v * scale_y) for v in g[col]],
                     color=INK2, fontsize=8.5, padding=2)
    ax.set_xticks(x, [FRAME_LABEL[f] for f in frames])
    ax.set(ylabel=ylabel, title=title)
    style(ax)
axes[0].set_ylim(0, 125)
for ax in axes[1:]:
    ax.set_ylim(0, 2100)          # one scale for p50 and p99
axes[0].legend(loc="upper right")
fig.suptitle("Same gRPC application on one node: DPUmesh (8 ARM workers) against "
             "per-Pod Linkerd sidecars (1 core each)")
finish(fig, "00_summary")

# 01: offered against achieved.
fig, ax = plt.subplots(figsize=(7.6, 4.9))
ax.plot([0, 120], [0, 120], color=REF, linewidth=1.0, label="y = x")
for frame in (64, 1024, 8192):
    plot_curve(ax, frame, "achieved_med", "achieved", 1e-3)
ax.set(xlabel="Offered load (kRPC/s)", ylabel="Achieved (kRPC/s)", xlim=(0, 120), ylim=(0, 120),
       title="Open loop: achieved throughput against offered load")
style(ax)
h, l = ax.get_legend_handles_labels()
ax.legend(h + REPEAT_HANDLES, l + [x.get_label() for x in REPEAT_HANDLES], loc="upper left")
finish(fig, "01_offered_achieved")

# 02: p50 from 500 RPS to overload. Log x shows the low-load floor and the knee
# on one axis; log y keeps overload visible without hiding the floor.
fig, ax = plt.subplots(figsize=(7.6, 4.9))
for frame in (64, 1024, 8192):
    lo = low[low.frame.eq(frame)].sort_values("offered")
    g = curve[curve.frame.eq(frame) & curve.clean.eq("yes")].sort_values("offered")
    joined = pd.concat([lo[["offered", "p50_med_us"]], g[["offered", "p50_med_us"]]]).drop_duplicates("offered")
    ax.plot(joined.offered / 1e3, joined.p50_med_us / 1e3, "o-", color=FRAME_COLOR[frame],
            label=FRAME_LABEL[frame])
    bad = curve[curve.frame.eq(frame) & curve.clean.ne("yes")].sort_values("offered")
    if not bad.empty:
        bridge = pd.concat([g.tail(1), bad])
        ax.plot(bridge.offered / 1e3, bridge.p50_med_us / 1e3, "o--", color=FRAME_COLOR[frame])
    reps = fresh[fresh.frame.eq(frame)]
    hollow(ax, reps.offered / 1e3, reps.p50_us / 1e3, FRAME_COLOR[frame])
ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xticks([0.5, 1, 2.5, 5, 10, 20, 50, 100], ["0.5", "1", "2.5", "5", "10", "20", "50", "100"])
ax.set(xlabel="Offered load (kRPC/s)", ylabel="p50 latency (ms)",
       title="Open loop: p50 falls from 0.98 ms at 500 RPS to 0.61 ms at 10k, then rises")
style(ax)
h, l = ax.get_legend_handles_labels()
ax.legend(h + REPEAT_HANDLES, l + [x.get_label() for x in REPEAT_HANDLES], loc="upper left")
finish(fig, "02_p50_latency")

# 03: p99 per payload with the tail SLO used for the second capacity definition.
fig, axes = plt.subplots(1, 3, figsize=(13.5, 4.4), sharey=True)
for ax, frame in zip(axes, (64, 1024, 8192)):
    plot_curve(ax, frame, "p99_med_us", "p99_us", 1e-3)
    ax.axhline(SLO_US / 1e3, color=REF, linewidth=1.0)
    ax.text(0.02, SLO_US / 1e3 * 1.15, "p99 = 5 ms", color=INK2, fontsize=8.5,
            transform=ax.get_yaxis_transform())
    ax.set_yscale("log")
    ax.set(xlabel="Offered load (kRPC/s)", title=FRAME_LABEL[frame], ylim=(0.8, 8000))
    ax.set_xlim(left=0)
    style(ax)
axes[0].set_ylabel("p99 latency (ms)")
fig.legend(handles=[Line2D([0], [0], color=INK2, marker="o", label="3/3 clean")] + REPEAT_HANDLES,
           loc="lower center", ncol=3, bbox_to_anchor=(0.5, -0.04))
fig.suptitle("Open loop: p99 by payload, no smoothing")
fig.subplots_adjust(bottom=0.24)
finish(fig, "03_p99_latency")

# 04: closed loop, throughput and p50 against the in-flight window.
fig, axes = plt.subplots(1, 2, figsize=(11.5, 4.5))
for frame, g in conc.groupby("frame"):
    g = g.sort_values("total_concurrency")
    axes[0].plot(g.total_concurrency, g.achieved_med / 1e3, "o-", color=FRAME_COLOR[frame],
                 label=FRAME_LABEL[frame])
    axes[1].plot(g.total_concurrency, g.p50_med_us / 1e3, "o-", color=FRAME_COLOR[frame],
                 label=FRAME_LABEL[frame])
for ax in axes:
    ax.set_xscale("log", base=2)
    ax.set_xticks([8, 32, 128, 512, 2048, 8192], ["8", "32", "128", "512", "2,048", "8,192"])
    ax.set_xlabel("Total in-flight requests")
    style(ax)
axes[0].set(ylabel="Throughput (kRPC/s)", ylim=(0, 130),
            title="Throughput peaks at 2,048 (64 B) / 1,024 (1 KiB), then falls")
axes[1].set_yscale("log")
axes[1].set(ylabel="p50 latency (ms)", title="Latency keeps rising past the peak")
axes[0].legend(loc="upper left")
fig.suptitle("Closed loop, 8 threads / 8 channels, 10 s, median of 3")
finish(fig, "04_inflight")

# 05: CPU against achieved throughput from 500 RPS, all three payloads.
fig, axes = plt.subplots(1, 3, figsize=(13.5, 4.3), sharey=True)
for ax, frame in zip(axes, (64, 1024, 8192)):
    lo = low[low.frame.eq(frame)]
    g = open_df[open_df.frame.eq(frame) & open_df.clean.eq("yes")]
    both = pd.concat([lo, g]).drop_duplicates("offered").sort_values("achieved_med")
    x = both.achieved_med / 1e3
    ax.plot(x, both.client_med_cores, "o-", color=ROLE_COLOR["client"], label="client Pod")
    ax.plot(x, both.server_med_cores, "o-", color=ROLE_COLOR["server"], label="server Pod")
    ax.plot(x, both.dpu_worker_med_cores, "o-", color=ROLE_COLOR["dpu"], label="DPU workers")
    ax.axhline(8, color=REF, linewidth=1.0)
    ax.text(0.02, 8.12, "8 ARM workers", color=INK2, fontsize=8.5, transform=ax.get_yaxis_transform())
    ax.set(xlabel="Achieved throughput (kRPC/s)", title=FRAME_LABEL[frame])
    ax.set_xlim(left=0)
    style(ax)
axes[0].set(ylabel="CPU cores", ylim=(0, 9.2))
axes[2].legend(loc="lower left", bbox_to_anchor=(0.0, 0.27))
fig.suptitle("CPU against load: DPU workers reach 4.1 cores at 10k RPS and 7.3 at 40k")
finish(fig, "05_cpu_attribution")

# 06: three transports on the same application.
frames = [64, 1024, 8192]
x = np.arange(3)
fig, axes = plt.subplots(1, 3, figsize=(13.8, 4.5))
w = 0.26
for i, t in enumerate(["direct-tcp", "DPUmesh", "Linkerd"]):
    g = comparison[comparison.transport.eq(t)].set_index("frame").loc[frames]
    axes[0].bar(x + (i - 1) * (w + 0.02), g.closed_1024_rps / 1e3, w, color=TRANSPORT_COLOR[t], label=t)
for i, t in enumerate(["DPUmesh", "Linkerd"]):
    g = comparison[comparison.transport.eq(t)].set_index("frame").loc[frames]
    axes[1].bar(x + (i - 0.5) * (w + 0.02), g.rps_per_proxy_core / 1e3, w, color=TRANSPORT_COLOR[t],
                label=f"{t} ({int(g.configured_proxy_cores.iloc[0])} proxy cores)")
ex = exchange.set_index("frame").loc[frames]
axes[2].bar(x - (w + 0.02), ex.linkerd_host_cores, w, color=TRANSPORT_COLOR["Linkerd"], label="Linkerd: Host (app + 2 sidecars)")
axes[2].bar(x, ex.dpumesh_host_cores, w, color=TRANSPORT_COLOR["DPUmesh"], label="DPUmesh: Host (app + broker)")
axes[2].bar(x + (w + 0.02), ex.arm_worker_cores_spent, w, color="#86b6ef", label="DPUmesh: DPU ARM workers")
for ax in axes:
    ax.set_xticks(x, [FRAME_LABEL[f] for f in frames])
    ax.set_ylim(bottom=0)
    style(ax)
axes[0].set(ylabel="Throughput (kRPC/s)", title="Closed loop, 1,024 in flight")
axes[1].set(ylabel="kRPC/s per configured proxy core", title="Same, per proxy core")
axes[2].set(ylabel="CPU cores", title="Matched 10k RPS: cores used", ylim=(0, 7.2))
axes[0].legend(loc="upper right", fontsize=8.2)
axes[1].legend(loc="upper right", fontsize=8.2)
axes[2].legend(loc="upper left", fontsize=8.2)
fig.suptitle("Same gRPC application, Host 9+9 cores; Linkerd runs LINKERD2_PROXY_CORES=1 per sidecar")
finish(fig, "06_comparison")

# 07: exclusive DPU profile and the two idle/loaded process readings.
self_df = pd.read_csv(ROOT / "perf-self.csv").sort_values("self_percent")
stat = pd.read_csv(ROOT / "perf-stat.csv")
fig, axes = plt.subplots(1, 2, figsize=(11.8, 4.8), gridspec_kw={"width_ratios": [2.3, 1]})
labels = [f"{s}  ({c})" for s, c in zip(self_df.symbol, self_df.category)]
axes[0].barh(labels, self_df.self_percent, color=TRANSPORT_COLOR["DPUmesh"], height=0.62)
axes[0].set(xlabel="Self cycles (%)", title="perf record -F 49, 64 B at 50k RPS; top 13 symbols sum to 20%")
axes[0].set_xlim(0, 4)
style(axes[0])
bars = axes[1].bar(["64 B, 50k RPS", "no sessions"], stat.cores, color=[TRANSPORT_COLOR["DPUmesh"], "#86b6ef"], width=0.55)
axes[1].bar_label(bars, fmt="%.3f cores", color=INK2, fontsize=8.5)
axes[1].set(ylabel="DPU process cores", title="perf stat, 8 s windows", ylim=(0, 8.6))
style(axes[1])
fig.suptitle("DPU profile is flat; no symbol above 3.5% self time")
finish(fig, "07_perf")

# 08: ARM worker count against the highest repeated clean rate.
fig, axes = plt.subplots(1, 2, figsize=(11.8, 4.5))
axes[0].plot([0, 150], [0, 150], color=REF, linewidth=1.0, label="y = x")
for workers, g in scale.groupby("workers"):
    g = g.sort_values("offered")
    clean, bad = g[g.clean.eq("yes")], g[g.clean.ne("yes")]
    axes[0].plot(clean.offered / 1e3, clean.achieved_med / 1e3, "o-", color=WORKER_COLOR[workers],
                 label=f"A = {workers} workers")
    if not bad.empty:
        bridge = pd.concat([clean.tail(1), bad])
        axes[0].plot(bridge.offered / 1e3, bridge.achieved_med / 1e3, "o--", color=WORKER_COLOR[workers])
axes[0].set(xlabel="Offered load (kRPC/s)", ylabel="Achieved (kRPC/s)", xlim=(0, 150), ylim=(0, 150),
            title="64 B open loop, threads = channels = workers")
h, l = axes[0].get_legend_handles_labels()
axes[0].legend(h + [REPEAT_HANDLES[0]], l + ["first bad"], loc="upper left")
style(axes[0])
top = scale[scale.clean.eq("yes")].sort_values("offered").groupby("workers").tail(1).sort_values("workers")
per_core = top.iloc[0].achieved_med / top.iloc[0].workers / 1e3
axes[1].plot([0, 12.6], [0, 12.6 * per_core], color=REF, linewidth=1.0, label=f"linear from A = 4 ({per_core:.0f}k per worker)")
axes[1].plot(top.workers, top.achieved_med / 1e3, "o-", color=TRANSPORT_COLOR["DPUmesh"], markersize=7)
for r in top.itertuples():
    axes[1].annotate(f"{r.achieved_med / 1e3:.0f}k", (r.workers, r.achieved_med / 1e3),
                     textcoords="offset points", xytext=(0, 8), ha="center", color=INK2, fontsize=9)
axes[1].set_xticks([0, 4, 6, 8, 12])
axes[1].set(xlabel="DPU ARM data workers", ylabel="Highest 3/3 clean rate (kRPC/s)",
            xlim=(0, 12.6), ylim=(0, 145), title="Capacity against worker count")
axes[1].legend(loc="upper left")
style(axes[1])
fig.suptitle("DPU ARM worker scaling")
finish(fig, "08_worker_scaling")

# 09: DPU worker CPU against requests in flight, and ARM time per RPC against rate.
fig, axes = plt.subplots(1, 2, figsize=(11.8, 4.6))
series = [
    ("open-loop 64 B", inflight[inflight.series.isin(["open-low", "open"]) & inflight.frame.eq(64)], FRAME_COLOR[64], "o"),
    ("closed-loop 64 B", inflight[inflight.series.eq("closed") & inflight.frame.eq(64)], FRAME_COLOR[64], "s"),
    ("closed-loop 1 KiB", inflight[inflight.series.eq("closed") & inflight.frame.eq(1024)], FRAME_COLOR[1024], "s"),
    ("open-loop 8 KiB", inflight[inflight.series.isin(["open-low", "open"]) & inflight.frame.eq(8192)], FRAME_COLOR[8192], "o"),
]
occ = inflight[inflight.mean_inflight.lt(inflight.workers)].cores_per_inflight.median()
xs = np.logspace(np.log10(0.3), np.log10(9000), 200)
axes[0].plot(xs, np.minimum(occ * xs, 8), color=REF, linewidth=1.0,
             label=f"{occ:.2f} cores per request in flight, capped at 8")
for label, g, color, marker in series:
    g = g.sort_values("mean_inflight")
    axes[0].plot(g.mean_inflight, g.dpu_worker_cores, marker=marker, linestyle="-", color=color,
                 markerfacecolor="white" if marker == "s" else color, label=label)
    axes[1].plot(g.rps / 1e3, g.arm_us_per_rpc, marker=marker, linestyle="-", color=color,
                 markerfacecolor="white" if marker == "s" else color, label=label)
axes[0].set_xscale("log")
axes[0].set(xlabel="Mean requests in flight (rate × p50, or the closed-loop window)",
            ylabel="DPU worker cores", ylim=(0, 8.6),
            title="Worker CPU follows the number of open requests")
axes[0].legend(loc="lower right", fontsize=8.5)
style(axes[0])
axes[1].set_xscale("log")
axes[1].set_xticks([0.5, 1, 2.5, 5, 10, 20, 50, 100], ["0.5", "1", "2.5", "5", "10", "20", "50", "100"])
axes[1].set(xlabel="Achieved rate (kRPC/s)", ylabel="ARM µs per RPC (worker cores ÷ rate)", ylim=(0, 750),
            title="ARM time per RPC: 692 µs at 500 RPS, 66–87 µs at the knee")
axes[1].legend(loc="upper right", fontsize=8.5)
style(axes[1])
fig.suptitle("DPU worker CPU is proportional to requests in flight, not to requests per second")
finish(fig, "09_inflight_cpu")

# 10: payload scaling across transports and ARM cost per RPC at the knee.
fig, axes = plt.subplots(1, 2, figsize=(11.8, 4.4))
for t in ("direct-tcp", "DPUmesh", "Linkerd"):
    g = payload[payload.transport.eq(t)].set_index("frame").loc[frames]
    axes[0].plot(x, g.relative_to_64B, "o-", color=TRANSPORT_COLOR[t], label=t)
axes[0].set_xticks(x, [FRAME_LABEL[f] for f in frames])
axes[0].set(ylabel="Throughput relative to 64 B", ylim=(0, 1.1),
            title="Closed loop, 1,024 in flight, relative to 64 B")
axes[0].legend(loc="lower left")
style(axes[0])
kc = knee_cost.set_index("frame").loc[frames]
bars = axes[1].bar([FRAME_LABEL[f] for f in frames], kc.arm_us_per_rpc, color=TRANSPORT_COLOR["DPUmesh"], width=0.55)
axes[1].bar_label(bars, fmt="%.0f µs", color=INK2, fontsize=9)
axes[1].set(ylabel="ARM µs per RPC", ylim=(0, 360),
            title="DPUmesh ARM cost at the highest clean open-loop rate")
style(axes[1])
fig.suptitle("Payload scaling: DPUmesh loses the most throughput; +14.6 ns of ARM time per payload byte")
finish(fig, "10_payload_scaling")

# 11: what one RPC costs a worker at three loads, from per-thread PMU counters.
pmu = pd.read_csv(ROOT / "lowload" / "pmu-per-rpc.csv")
fig, axes = plt.subplots(1, 2, figsize=(11.8, 4.4))
labels = [f"{int(r.rps_per_worker):,} RPS/worker\n({r.channels} ch, {int(r.rps):,} RPS)" for r in pmu.itertuples()]
x = np.arange(len(pmu))
w = 0.26
metrics = [("instructions_per_rpc", "instructions", "#2a78d6"),
           ("cycles_per_rpc", "cycles", "#86b6ef"),
           ("cache_misses_per_rpc", "cache misses", "#eb6834")]
ref = pmu.iloc[-1]
for i, (col, name, color) in enumerate(metrics):
    axes[0].bar(x + (i - 1) * (w + 0.02), pmu[col] / ref[col], w, color=color, label=f"{name} per RPC")
axes[0].axhline(1, color=REF, linewidth=1.0)
axes[0].set_xticks(x, labels)
axes[0].set(ylabel="relative to the 50k RPS point", ylim=(0, 5.4),
            title="Per-RPC cost of one worker, normalized to the knee")
axes[0].legend(loc="upper right")
style(axes[0])
axes[1].bar(x, pmu.us_per_rpc, 0.5, color="#2a78d6")
for xi, r in zip(x, pmu.itertuples()):
    axes[1].annotate(f"{r.us_per_rpc:.0f} µs\nIPC {r.ipc:.2f}", (xi, r.us_per_rpc),
                     textcoords="offset points", xytext=(0, 6), ha="center", color=INK2, fontsize=9)
axes[1].set_xticks(x, labels)
axes[1].set(ylabel="ARM µs per RPC (task-clock ÷ RPCs)", ylim=(0, 900),
            title="The same request costs 5x more CPU when the worker is cold")
style(axes[1])
fig.suptitle("64 B unary RPC on one DPU worker: 2.7x the instructions, 2.9x the cache misses, half the IPC at low load")
finish(fig, "11_per_rpc_pmu")

print(f"wrote 24 graph files under {GRAPH}")
