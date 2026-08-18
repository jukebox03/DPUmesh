#!/usr/bin/env python3
"""Generate the DPUmesh L7 thread-model diagrams in PNG and PDF form."""

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch


OUT = Path(__file__).resolve().parent / "figures"
OUT.mkdir(exist_ok=True)

BLUE = "#287de1"
BLUE_BG = "#edf4ff"
GREEN = "#12ad76"
GREEN_BG = "#ecf9f4"
ORANGE = "#d48a00"
ORANGE_BG = "#fff7e8"
PURPLE = "#5541bd"
PURPLE_BG = "#f1effc"
RED = "#c84b43"
RED_BG = "#fff0ef"
GRAY = "#989892"
GRAY_BG = "#f7f7f5"
BLACK = "#151515"


def setup_figure(width: float, height: float, xlim, ylim):
    fig, ax = plt.subplots(figsize=(width, height))
    fig.patch.set_facecolor("white")
    ax.set_xlim(*xlim)
    ax.set_ylim(*ylim)
    ax.axis("off")
    return fig, ax


def box(
    ax,
    x,
    y,
    w,
    h,
    title,
    lines=(),
    *,
    edge=GRAY,
    face=GRAY_BG,
    count=None,
    title_size=11,
    body_size=8.2,
    linewidth=1.45,
    dashed=False,
):
    patch = FancyBboxPatch(
        (x, y),
        w,
        h,
        boxstyle="round,pad=0.02,rounding_size=0.08",
        linewidth=linewidth,
        edgecolor=edge,
        facecolor=face,
        linestyle="--" if dashed else "-",
    )
    ax.add_patch(patch)
    ax.text(
        x + 0.20,
        y + h - 0.31,
        title,
        color="#202020",
        fontsize=title_size,
        ha="left",
        va="center",
    )
    if count:
        ax.text(
            x + w - 0.18,
            y + h - 0.31,
            count,
            color="#555555",
            fontsize=8,
            ha="right",
            va="center",
        )
    for i, line in enumerate(lines):
        ax.text(
            x + 0.20,
            y + h - 0.68 - i * 0.29,
            line,
            color="#555555",
            fontsize=body_size,
            family="DejaVu Sans Mono",
            ha="left",
            va="center",
        )
    return patch


def container(ax, x, y, w, h, title, *, edge=GRAY, face="white", count=None):
    return box(
        ax,
        x,
        y,
        w,
        h,
        title,
        edge=edge,
        face=face,
        count=count,
        title_size=10.5,
        linewidth=1.25,
        dashed=True,
    )


def arrow(
    ax,
    start,
    end,
    *,
    label=None,
    color=BLACK,
    dashed=False,
    lw=1.45,
    label_dx=0.0,
    label_dy=0.15,
):
    patch = FancyArrowPatch(
        start,
        end,
        arrowstyle="-|>",
        mutation_scale=11,
        linewidth=lw,
        color=color,
        linestyle="--" if dashed else "-",
        shrinkA=0,
        shrinkB=0,
        connectionstyle="arc3,rad=0",
    )
    ax.add_patch(patch)
    if label:
        ax.text(
            (start[0] + end[0]) / 2 + label_dx,
            (start[1] + end[1]) / 2 + label_dy,
            label,
            color="#626262",
            fontsize=7.8,
            ha="center",
            va="center",
            bbox=dict(fc="white", ec="none", pad=0.8),
        )


def line(ax, points, *, color=GRAY, dashed=False, lw=1.25):
    xs, ys = zip(*points)
    ax.plot(xs, ys, color=color, lw=lw, ls="--" if dashed else "-")


def save(fig, stem: str):
    fig.savefig(
        OUT / f"{stem}.png",
        dpi=180,
        bbox_inches="tight",
        pad_inches=0.12,
        metadata={"Software": "DPUmesh diagram generator"},
    )
    fig.savefig(
        OUT / f"{stem}.pdf",
        bbox_inches="tight",
        pad_inches=0.12,
        metadata={
            "Title": stem,
            "Creator": "DPUmesh diagram generator",
            "CreationDate": None,
            "ModDate": None,
        },
    )
    plt.close(fig)


def generate_dpumesh_threads():
    fig, ax = setup_figure(18.0, 11.5, (0, 18.0), (0, 11.5))
    ax.text(0.1, 11.1, "DPUmesh with embedded Linkerd — thread model", fontsize=17, ha="left")
    ax.text(2.3, 10.55, "HOST", fontsize=11, color="#555555", ha="center")
    ax.text(10.0, 10.55, "BLUEFIELD DPU", fontsize=11, color="#555555", ha="center")
    ax.text(16.0, 10.55, "BACKEND HOST", fontsize=11, color="#555555", ha="center")
    ax.plot([4.65, 4.65], [0.75, 10.45], color="#ddddda", lw=1, ls=(0, (2, 3)))
    ax.plot([14.15, 14.15], [0.75, 10.45], color="#ddddda", lw=1, ls=(0, (2, 3)))

    box(
        ax,
        0.35,
        8.65,
        3.95,
        1.25,
        "Application / gRPC threads",
        ("DPUmesh stream API", "request and response bytes"),
        edge=BLUE,
        face=BLUE_BG,
    )
    box(
        ax,
        0.35,
        5.95,
        3.95,
        1.35,
        "Host PE progress thread",
        ("reverse rings: REV_DONE / TX_ACK", "EQ readiness and callbacks"),
        edge=BLUE,
        face=BLUE_BG,
        count="x 1 / context",
    )
    box(
        ax,
        0.35,
        3.55,
        3.95,
        1.25,
        "Tail timer thread",
        ("retained partial-unit deadlines", "signals the owning EQ fd"),
        edge=BLUE,
        face=BLUE_BG,
        count="x 1 / context",
    )
    box(
        ax,
        0.35,
        1.15,
        3.95,
        1.25,
        "Control caller",
        ("registration and teardown", "Host↔DPU control protocol"),
        edge=PURPLE,
        face=PURPLE_BG,
    )

    box(
        ax,
        5.15,
        8.45,
        3.25,
        1.45,
        "DPA execution units",
        ("consume forward rings", "stage request bytes"),
        edge=ORANGE,
        face=ORANGE_BG,
        count="x N",
    )
    box(
        ax,
        5.15,
        1.15,
        3.25,
        1.45,
        "ARM control thread",
        ("registration / teardown", "doorbells / readiness"),
        edge=PURPLE,
        face=PURPLE_BG,
        count="x 1",
    )

    container(
        ax,
        8.85,
        5.45,
        4.75,
        4.45,
        "Pinned ARM data worker s",
        edge=GREEN,
        face="#fbfffd",
        count="selected id; x A with all",
    )
    box(
        ax,
        9.15,
        8.15,
        4.15,
        1.15,
        "Tokio current_thread runtime",
        ("created by l7_worker_run(s, state[s])",),
        edge=GREEN,
        face=GREEN_BG,
        title_size=10.2,
    )
    box(
        ax,
        9.15,
        6.75,
        4.15,
        1.05,
        "Persistent runtime driver",
        ("DPUmesh drain + arm/recheck/wait",),
        edge=GREEN,
        face=GREEN_BG,
        title_size=10.2,
    )
    box(
        ax,
        9.15,
        5.75,
        1.95,
        0.70,
        "DPUmesh C backend",
        edge=ORANGE,
        face=ORANGE_BG,
        title_size=8.8,
    )
    box(
        ax,
        11.35,
        5.75,
        1.95,
        0.70,
        "Linkerd tasks",
        edge=GREEN,
        face=GREEN_BG,
        title_size=9.0,
    )

    container(
        ax,
        8.85,
        1.15,
        4.75,
        3.75,
        "Other pinned ARM data workers i ≠ s",
        edge=GRAY,
        face="#fcfcfb",
        count="when A > 1",
    )
    box(
        ax,
        9.15,
        3.25,
        4.15,
        1.10,
        "Tokio current_thread runtime",
        ("one runtime on each pinned pthread",),
        edge=GRAY,
        face=GRAY_BG,
        title_size=10.2,
    )
    box(
        ax,
        9.15,
        1.60,
        4.15,
        1.25,
        "Persistent DPUmesh driver",
        ("owns its completion PE and DMA lanes", "Linkerd state too when selector = all"),
        edge=GRAY,
        face=GRAY_BG,
        title_size=10.2,
    )

    box(
        ax,
        14.65,
        6.55,
        2.95,
        1.55,
        "Backend pod",
        ("service socket / gRPC", "request → reply"),
        edge=RED,
        face=RED_BG,
    )

    arrow(ax, (4.30, 9.15), (5.15, 9.15), label="forward rings", color=BLUE, label_dy=0.20)
    arrow(ax, (8.40, 9.15), (8.85, 8.90), label="completion", color=ORANGE, label_dy=0.28)
    arrow(ax, (8.40, 8.80), (8.85, 4.00), color=ORANGE, dashed=True)
    arrow(ax, (13.60, 7.25), (14.65, 7.25), label="SG-DMA", color=GREEN)
    arrow(ax, (14.65, 6.85), (13.60, 6.85), label="reply", color=RED, label_dy=-0.18)
    arrow(ax, (8.85, 6.00), (4.30, 6.55), label="reverse rings", color=BLUE, label_dy=0.22)
    arrow(ax, (8.85, 2.00), (4.30, 6.15), color=BLUE, dashed=True)
    arrow(ax, (2.30, 7.30), (2.30, 8.65), label="events", color=BLUE, label_dx=0.36, label_dy=0)
    arrow(ax, (2.30, 4.80), (2.30, 5.95), label="deadline wake", color=BLUE, label_dx=0.55, label_dy=0)
    arrow(ax, (4.30, 1.80), (5.15, 1.80), label="control", color=PURPLE)
    arrow(ax, (6.78, 2.60), (6.78, 8.45), label="RING_ADD / RING_DEL", color=PURPLE, dashed=True, label_dx=0.83, label_dy=0)

    ax.text(
        14.65,
        5.92,
        "Worker ownership",
        fontsize=9.2,
        color="#555555",
        ha="left",
    )
    ax.text(
        14.65,
        5.50,
        "• pthread + affinity: DPUmesh",
        fontsize=8.5,
        color="#555555",
        ha="left",
    )
    ax.text(
        14.65,
        5.12,
        "• runtime + driver: Linkerd port",
        fontsize=8.5,
        color="#555555",
        ha="left",
    )
    ax.text(
        14.65,
        4.74,
        "• DOCA/DPA/SG-DMA: DPUmesh",
        fontsize=8.5,
        color="#555555",
        ha="left",
    )
    ax.text(
        14.65,
        3.85,
        "One connection remains on one ARM worker.",
        fontsize=8.6,
        color="#444444",
        ha="left",
    )
    ax.text(
        14.65,
        3.47,
        "one id routes L7 flows to worker s;",
        fontsize=8.6,
        color="#444444",
        ha="left",
    )
    ax.text(
        14.65,
        3.09,
        "all gives every worker a proxy + port policy.",
        fontsize=8.6,
        color="#444444",
        ha="left",
    )

    save(fig, "dpumesh_threads")


def generate_l7_interaction():
    fig, ax = setup_figure(15.0, 12.4, (0, 15.0), (0, 12.4))
    ax.text(0.1, 12.0, "Linkerd-enabled ARM worker — persistent runtime loop", fontsize=17, ha="left")
    ax.text(
        0.1,
        11.53,
        "One pinned pthread hosts one Tokio current_thread runtime for the worker lifetime.",
        fontsize=9.5,
        color="#555555",
        ha="left",
    )

    container(ax, 0.45, 0.70, 9.15, 10.40, "Pinned ARM data-worker thread", edge=GREEN, face="#fbfffd")
    box(
        ax,
        0.85,
        9.55,
        8.35,
        1.00,
        "l7_worker_run(worker_id, worker_state)",
        ("build current_thread runtime; install worker-local adapter state",),
        edge=GREEN,
        face=GREEN_BG,
    )
    arrow(ax, (5.02, 9.55), (5.02, 9.05), color=GREEN)
    box(
        ax,
        0.85,
        8.00,
        8.35,
        1.05,
        "dmesh_doca::runtime::run(ExternalBackend)",
        ("maintenance deadline due → dpu_send_wake_worker",),
        edge=GREEN,
        face=GREEN_BG,
    )
    arrow(ax, (5.02, 8.00), (5.02, 7.55), color=GREEN)
    box(
        ax,
        0.85,
        6.35,
        8.35,
        1.20,
        "Drain with budget 64",
        ("DPU completion PE + cross-worker queues + SG-DMA", "registration events + Linkerd DmeshIo output"),
        edge=ORANGE,
        face=ORANGE_BG,
    )
    arrow(ax, (5.02, 6.35), (5.02, 5.90), color=ORANGE)
    box(
        ax,
        0.85,
        4.85,
        3.70,
        1.05,
        "Progressed",
        ("yield to runtime tasks", "then drain again"),
        edge=BLUE,
        face=BLUE_BG,
    )
    box(
        ax,
        5.50,
        4.85,
        3.70,
        1.05,
        "Idle / pending",
        ("arm PE notifications", "mark worker parked"),
        edge=PURPLE,
        face=PURPLE_BG,
    )
    arrow(ax, (4.30, 6.35), (2.70, 5.90), color=BLUE)
    arrow(ax, (5.75, 6.35), (7.35, 5.90), color=PURPLE)
    line(ax, [(2.70, 4.85), (2.70, 4.45), (0.65, 4.45), (0.65, 6.95)], color=BLUE)
    arrow(ax, (0.65, 6.95), (0.85, 6.95), color=BLUE)
    arrow(ax, (7.35, 4.85), (7.35, 4.35), color=PURPLE)
    box(
        ax,
        5.50,
        3.15,
        3.70,
        1.20,
        "Drain again after arm",
        ("close arm-to-sleep race", "clear if work appeared"),
        edge=PURPLE,
        face=PURPLE_BG,
    )
    arrow(ax, (7.35, 3.15), (7.35, 2.70), label="still idle", color=PURPLE, label_dx=0.55, label_dy=0)
    box(
        ax,
        0.85,
        1.25,
        8.35,
        1.45,
        "tokio::select!",
        (
            "completion fd  |  optional SG-DMA fd  |  cross-worker wake fd",
            "Linkerd output waker  |  1 ms maintenance deadline",
            "wake → clear notifications → drain again",
        ),
        edge=GREEN,
        face=GREEN_BG,
    )
    line(ax, [(9.20, 1.92), (9.38, 1.92), (9.38, 6.95)], color=GREEN)
    line(ax, [(9.20, 3.72), (9.38, 3.72)], color=GREEN)
    arrow(ax, (9.38, 6.95), (9.20, 6.95), color=GREEN)
    ax.text(
        9.47,
        4.80,
        "clear notifications",
        fontsize=7.7,
        color="#626262",
        rotation=90,
        ha="center",
        va="center",
        bbox=dict(fc="white", ec="none", pad=0.7),
    )

    box(
        ax,
        10.15,
        8.75,
        4.40,
        2.35,
        "DPUmesh C backend",
        (
            "notification_fds / arm / drain",
            "clear_notifications / maintenance",
            "stopped / ready / failed",
            "DOCA, rings, custody, SG-DMA",
        ),
        edge=ORANGE,
        face=ORANGE_BG,
        title_size=10.5,
    )
    box(
        ax,
        10.15,
        5.65,
        4.40,
        2.35,
        "Worker-local Linkerd state",
        (
            "outbound stack tasks",
            "DmeshIo endpoint pairs",
            "backend registry + sessions (per worker)",
            "tx wakers",
        ),
        edge=GREEN,
        face=GREEN_BG,
        title_size=10.5,
    )
    box(
        ax,
        10.15,
        2.55,
        4.40,
        2.35,
        "Connection ABI on this thread",
        (
            "l7_conn_open / segment / eof / close",
            "dmesh_l7_tx_reserve / commit / release",
            "dmesh_l7_backends",
            "thread-local lookup by worker_id",
        ),
        edge=BLUE,
        face=BLUE_BG,
        title_size=10.2,
        body_size=7.7,
    )
    arrow(ax, (9.20, 8.50), (10.15, 9.35), label="RuntimeBackend", color=ORANGE, label_dy=0.20)
    arrow(ax, (9.20, 6.80), (10.15, 6.80), label="poll_internal", color=GREEN)
    arrow(ax, (9.20, 4.10), (10.15, 3.72), color=BLUE)
    ax.text(
        10.15,
        1.62,
        "Exit condition",
        fontsize=9.5,
        color="#555555",
        ha="left",
    )
    ax.text(
        10.15,
        1.18,
        "worker_state->stop → detach state → return",
        fontsize=8.3,
        family="DejaVu Sans Mono",
        color="#555555",
        ha="left",
    )

    save(fig, "l7_interaction")


def generate_linkerd_driven():
    fig, ax = setup_figure(17.0, 10.4, (0, 17.0), (0, 10.4))
    ax.text(0.1, 10.0, "Embedded Linkerd — runtime and ownership boundary", fontsize=17, ha="left")
    ax.text(3.10, 9.45, "DPUMESH", fontsize=11, color="#555555", ha="center")
    ax.text(8.50, 9.45, "C ABI / RUNTIME BACKEND", fontsize=11, color="#555555", ha="center")
    ax.text(14.00, 9.45, "LINKERD PORT", fontsize=11, color="#555555", ha="center")
    ax.plot([5.75, 5.75], [0.80, 9.30], color="#ddddda", lw=1, ls=(0, (2, 3)))
    ax.plot([11.25, 11.25], [0.80, 9.30], color="#ddddda", lw=1, ls=(0, (2, 3)))

    box(
        ax,
        0.45,
        7.35,
        4.85,
        1.45,
        "ARM worker lifecycle",
        ("pthread create / CPU affinity", "worker_state lifetime / stop flag"),
        edge=BLUE,
        face=BLUE_BG,
    )
    box(
        ax,
        0.45,
        4.90,
        4.85,
        1.65,
        "Data-plane resources",
        ("DOCA device, DPA, progress engines", "rings, conntrack, staging custody", "egress arena and SG-DMA"),
        edge=ORANGE,
        face=ORANGE_BG,
    )
    box(
        ax,
        0.45,
        2.40,
        4.85,
        1.65,
        "Host transport",
        ("API and control protocol", "forward / reverse rings and credits", "gRPC adapter uses the same channel"),
        edge=BLUE,
        face=BLUE_BG,
    )

    box(
        ax,
        6.20,
        7.35,
        4.60,
        1.45,
        "Worker entry",
        ("l7_worker_run(worker_id, worker_state)", "blocking until worker stop"),
        edge=PURPLE,
        face=PURPLE_BG,
    )
    box(
        ax,
        6.20,
        4.90,
        4.60,
        1.65,
        "RuntimeBackend ABI",
        ("notification_fds / arm / drain / clear", "maintenance / stopped / ready / failed", "bounded progress: budget 64"),
        edge=PURPLE,
        face=PURPLE_BG,
        body_size=7.8,
    )
    box(
        ax,
        6.20,
        2.40,
        4.60,
        1.65,
        "Flow and byte ABI",
        ("l7_conn_open / segment / eof / close", "tx_reserve / commit / release / backends", "prefix acceptance and staging custody"),
        edge=PURPLE,
        face=PURPLE_BG,
        body_size=7.8,
    )

    box(
        ax,
        11.70,
        7.35,
        4.85,
        1.45,
        "Per-worker runtime",
        ("Tokio current_thread", "persistent async driver"),
        edge=GREEN,
        face=GREEN_BG,
    )
    box(
        ax,
        11.70,
        4.90,
        4.85,
        1.65,
        "Enabled worker state",
        ("DmeshIo endpoint pairs", "one outbound stack per session", "exact-token backend registry"),
        edge=GREEN,
        face=GREEN_BG,
    )
    box(
        ax,
        11.70,
        2.40,
        4.85,
        1.65,
        "Control-plane clients",
        ("deployed destination, identity and policy", "host-network TCP gateway", "service target → backend connector"),
        edge=GREEN,
        face=GREEN_BG,
    )

    arrow(ax, (5.30, 8.08), (6.20, 8.08), label="call", color=BLUE)
    arrow(ax, (10.80, 8.08), (11.70, 8.08), label="build + run", color=GREEN)
    arrow(ax, (11.70, 5.72), (10.80, 5.72), label="poll", color=GREEN)
    arrow(ax, (6.20, 5.72), (5.30, 5.72), label="progress", color=ORANGE)
    arrow(ax, (5.30, 3.22), (6.20, 3.22), label="input", color=BLUE)
    arrow(ax, (10.80, 3.22), (11.70, 5.20), label="DmeshIo", color=GREEN, label_dy=0.22)
    arrow(ax, (11.70, 5.02), (10.80, 2.85), label="output", color=GREEN, label_dy=0.18)
    arrow(ax, (6.20, 2.85), (5.30, 2.85), label="send / release", color=ORANGE, label_dy=-0.18)

    ax.text(
        0.45,
        1.35,
        "Thread invariant",
        fontsize=10,
        color="#444444",
        ha="left",
    )
    ax.text(
        2.55,
        1.35,
        "All adapter calls and runtime progress for a worker execute on that worker's pinned pthread.",
        fontsize=9.2,
        color="#444444",
        ha="left",
    )
    ax.text(
        0.45,
        0.92,
        "Session placement",
        fontsize=10,
        color="#444444",
        ha="left",
    )
    ax.text(
        2.55,
        0.92,
        "DPUMESH_L7_LINKERD_WORKER selects one worker (default: 0), or all workers with `all`.",
        fontsize=9.2,
        color="#444444",
        ha="left",
    )

    save(fig, "linkerd_driven")


if __name__ == "__main__":
    generate_dpumesh_threads()
    generate_l7_interaction()
    generate_linkerd_driven()
