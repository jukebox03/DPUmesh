#!/usr/bin/env python3
"""Generate the gRPC adapter thread diagrams in PNG and PDF form."""

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
    number=None,
    count=None,
    title_size=11,
    body_size=8.2,
):
    patch = FancyBboxPatch(
        (x, y),
        w,
        h,
        boxstyle="round,pad=0.02,rounding_size=0.08",
        linewidth=1.45,
        edgecolor=edge,
        facecolor=face,
    )
    ax.add_patch(patch)
    title_x = x + 0.20
    if number is not None:
        ax.text(
            x + 0.30,
            y + h - 0.30,
            str(number),
            color="white",
            fontsize=7.5,
            ha="center",
            va="center",
            bbox=dict(boxstyle="circle,pad=0.23", fc=BLACK, ec=BLACK),
        )
        title_x = x + 0.72
    ax.text(
        title_x,
        y + h - 0.30,
        title,
        color="#202020",
        fontsize=title_size,
        ha="left",
        va="center",
    )
    if count:
        ax.text(
            x + w - 0.18,
            y + h - 0.30,
            count,
            color="#555555",
            fontsize=8,
            ha="right",
            va="center",
        )
    for i, line in enumerate(lines):
        ax.text(
            x + 0.20,
            y + h - 0.64 - i * 0.27,
            line,
            color="#555555",
            fontsize=body_size,
            family="DejaVu Sans Mono",
            ha="left",
            va="center",
        )
    return patch


def arrow(ax, start, end, *, label=None, color=BLACK, dashed=False, lw=1.45):
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
    )
    ax.add_patch(patch)
    if label:
        ax.text(
            (start[0] + end[0]) / 2,
            (start[1] + end[1]) / 2 + 0.14,
            label,
            color="#666666",
            fontsize=7.7,
            ha="center",
            va="center",
        )


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


def generate_grpc_threads():
    fig, ax = setup_figure(18.5, 18.0, (0, 18.5), (0, 18.0))
    ax.text(
        0.1,
        17.55,
        "gRPC adapter — the threads a connection and then one RPC cross",
        fontsize=17,
        ha="left",
    )
    ax.text(2.8, 16.95, "CLIENT process", fontsize=11, color="#555555", ha="center")
    ax.text(9.25, 16.95, "DPU", fontsize=11, color="#555555", ha="center")
    ax.text(15.65, 16.95, "SERVER process", fontsize=11, color="#555555", ha="center")
    ax.plot([6.15, 6.15], [0.75, 16.9], color="#ddddda", lw=1, ls=(0, (2, 3)))
    ax.plot([11.65, 11.65], [0.75, 16.9], color="#ddddda", lw=1, ls=(0, (2, 3)))

    ax.text(0.1, 16.45, "ESTABLISH", fontsize=10, color=PURPLE)
    box(
        ax,
        0.45,
        14.70,
        5.15,
        1.35,
        "gRPC / Endpoint caller",
        ("EventEngine::Connect(...) -> Runtime::Connect", "enqueue reactor command; signal command fd"),
        edge=BLUE,
        face=BLUE_BG,
        number=1,
        count="gRPC-managed",
    )
    box(
        ax,
        0.45,
        12.80,
        5.15,
        1.35,
        "EQ owner thread",
        ("command fd wakes; dmesh_create_qp(service)", "DeliverConnect -> deferred callback queue"),
        edge=BLUE,
        face=BLUE_BG,
        number=2,
        count="x 1 per shard",
    )
    box(
        ax,
        0.45,
        10.90,
        5.15,
        1.35,
        "callback dispatcher thread",
        ("FinishConnect: construct DmeshEndpoint", "invoke gRPC OnConnectCallback"),
        edge=BLUE,
        face=BLUE_BG,
        number=3,
        count="x 1 default/runtime",
        title_size=10.2,
    )
    arrow(ax, (3.02, 14.70), (3.02, 14.17), label="reactor command queue")
    arrow(ax, (3.02, 12.80), (3.02, 12.27), label="deferred callback queue")

    box(
        ax,
        7.05,
        13.45,
        4.35,
        1.35,
        "DPU connection setup",
        ("control message creates the remote QP", "and binds it to a backend pod"),
        edge=ORANGE,
        face=ORANGE_BG,
    )
    arrow(ax, (5.60, 13.48), (7.05, 13.48), label="create QP")
    box(
        ax,
        12.40,
        13.75,
        5.25,
        1.25,
        "EQ owner thread",
        ("DMESH_EVENT_CONN_REQ -> ConnectedTransport", "DeliverAccept -> deferred callback queue"),
        edge=GREEN,
        face=GREEN_BG,
        number=4,
        count="x 1 per shard",
    )
    box(
        ax,
        12.40,
        11.85,
        5.25,
        1.35,
        "callback dispatcher thread",
        ("construct DmeshEndpoint with allocator", "PassiveListener::AcceptConnectedEndpoint"),
        edge=GREEN,
        face=GREEN_BG,
        number=5,
        count="x 1 default/runtime",
        title_size=10.2,
    )
    arrow(ax, (11.40, 14.12), (12.40, 14.12), label="CONN_REQ")
    arrow(ax, (15.02, 13.75), (15.02, 13.20), label="deferred callback queue")
    ax.text(
        9.25,
        11.75,
        "The dispatcher handles control and deferred terminal callbacks;\nnormal RX and TX_READY do not take this hop.",
        fontsize=9.2,
        color=PURPLE,
        ha="center",
    )

    ax.text(0.1, 10.35, "REQUEST", fontsize=10, color=BLUE)
    box(
        ax,
        0.45,
        7.95,
        5.15,
        1.75,
        "Endpoint::Write caller",
        ("stub -> chttp2 -> Endpoint::Write", "TX pump: alloc, copy, post_send, flush", "serialized by the per-connection TX lock"),
        edge=BLUE,
        face=BLUE_BG,
        number=6,
        count="gRPC-managed",
    )
    box(
        ax,
        7.05,
        7.65,
        4.35,
        2.05,
        "DPA EU -> ARM worker",
        ("drain forward ring; DMA to pod staging", "route and SG-DMA", "publish reverse-ring completion"),
        edge=ORANGE,
        face=ORANGE_BG,
    )
    arrow(ax, (5.60, 8.80), (7.05, 8.80), label="forward ring")
    box(
        ax,
        12.40,
        8.75,
        5.25,
        0.95,
        "PE progress",
        ("reverse drain -> REV_DONE; signal EQ readiness",),
        edge=GREEN,
        face=GREEN_BG,
        number=7,
        count="x 1",
    )
    box(
        ax,
        12.40,
        6.25,
        5.25,
        1.90,
        "EQ owner thread",
        ("ppoll(command fd, EQ fd) -> dmesh_poll_eq", "copy one slice per receive run; release credit", "TX_READY -> resume a parked write pump"),
        edge=GREEN,
        face=GREEN_BG,
        number=8,
        count="x 1 per shard",
    )
    box(
        ax,
        12.40,
        4.55,
        5.25,
        1.05,
        "gRPC worker / handler",
        ("inline Endpoint completion -> chttp2 progress",),
        edge=GREEN,
        face=GREEN_BG,
        number=9,
        count="gRPC-managed",
    )
    arrow(ax, (11.40, 9.10), (12.40, 9.10), label="reverse ring")
    arrow(ax, (15.02, 8.75), (15.02, 8.15), label="EQ ready edge", color=GRAY, dashed=True)
    arrow(ax, (15.02, 6.25), (15.02, 5.60), label="inline chttp2 callback")

    ax.text(0.1, 4.15, "RESPONSE", fontsize=10, color=GREEN)
    box(
        ax,
        12.40,
        2.85,
        5.25,
        0.95,
        "handler / Endpoint::Write caller",
        ("Finish -> chttp2 -> Endpoint::Write; TX pump here",),
        edge=GREEN,
        face=GREEN_BG,
        number=10,
        count="gRPC-managed",
        title_size=10.3,
    )
    box(
        ax,
        7.05,
        2.55,
        4.35,
        1.25,
        "ARM worker -> DPA EU",
        ("route and SG-DMA the reply", "publish reverse-ring completion"),
        edge=ORANGE,
        face=ORANGE_BG,
    )
    arrow(ax, (12.40, 3.25), (11.40, 3.25), label="forward ring")
    box(
        ax,
        0.45,
        2.85,
        5.15,
        0.95,
        "PE progress",
        ("reverse drain -> REV_DONE; signal EQ readiness",),
        edge=BLUE,
        face=BLUE_BG,
        number=11,
        count="x 1",
    )
    box(
        ax,
        0.45,
        1.25,
        5.15,
        1.05,
        "EQ owner thread",
        ("poll EQ; copy receive run; release RX credit",),
        edge=BLUE,
        face=BLUE_BG,
        number=12,
        count="x 1 per shard",
    )
    arrow(ax, (7.05, 3.25), (5.60, 3.25), label="reverse ring")
    arrow(ax, (3.02, 2.85), (3.02, 2.30), label="EQ ready edge", color=GRAY, dashed=True)
    ax.text(
        7.10,
        0.65,
        "callback dispatcher reappears only for connect/accept, FIN, transport error,\nEndpoint destruction, and Read/Write on an already-terminal Endpoint.",
        fontsize=8.5,
        color="#666666",
        ha="left",
        va="bottom",
        bbox=dict(boxstyle="round,pad=0.45", fc=GRAY_BG, ec="#b0b0aa"),
    )
    save(fig, "grpc_threads")


def generate_grpc_vs_stock():
    fig, ax = setup_figure(20.0, 13.7, (0, 20.0), (0, 13.7))
    ax.text(0.15, 13.25, "Where DPUmesh enters gRPC — both directions", fontsize=17, ha="left")
    ax.text(
        0.15,
        12.80,
        "Everything above the seam is stock gRPC. DPUmesh replaces only the transport below it.",
        fontsize=9.5,
        color="#555555",
    )
    box(
        ax,
        0.55,
        11.55,
        18.90,
        0.80,
        "application",
        ("generated stub continuation or service handler",),
        count="gRPC worker / completion-queue thread",
        body_size=7.4,
    )
    box(
        ax,
        0.55,
        10.45,
        18.90,
        0.80,
        "chttp2",
        ("HTTP/2 framing, flow control, HPACK, promise/filter and stream dispatch",),
        count="gRPC worker threads",
        body_size=7.4,
    )
    ax.plot([0.55, 19.45], [9.85, 9.85], color=BLACK, lw=1.7)
    ax.text(0.55, 10.03, "EventEngine::Endpoint", fontsize=10)
    ax.text(5.0, 10.03, "Read(SliceBuffer*, on_read)", fontsize=9, ha="center")
    ax.text(15.0, 10.03, "Write(SliceBuffer*, on_writable)", fontsize=9, ha="center")
    ax.plot([10.0, 10.0], [1.15, 9.85], color="#c7c7c3", lw=1)

    ax.text(0.55, 9.40, "RECEIVE", fontsize=11)
    ax.text(1.0, 9.05, "stock gRPC", fontsize=9, color=PURPLE)
    ax.text(5.35, 9.05, "gRPC on DPUmesh", fontsize=9, color=GREEN)
    box(
        ax,
        0.55,
        7.35,
        4.55,
        1.35,
        "EventEngine callback thread",
        ("PosixEndpoint::HandleRead", "recvmsg copies into a gRPC slice", "invoke on_read -> chttp2"),
        edge=PURPLE,
        face=PURPLE_BG,
        title_size=9.4,
        body_size=7.3,
    )
    box(
        ax,
        0.55,
        5.60,
        4.55,
        1.20,
        "EventEngine poller thread",
        ("epoll_wait notices readiness", "queue the read closure"),
        edge=PURPLE,
        face=PURPLE_BG,
        title_size=9.4,
        body_size=7.3,
    )
    box(
        ax,
        0.55,
        3.95,
        4.55,
        1.10,
        "kernel + socket receive buffer",
        ("softirq, TCP/IP, netfilter",),
        edge=PURPLE,
        face="white",
        title_size=9.0,
        body_size=7.3,
    )
    box(
        ax,
        0.55,
        2.55,
        4.55,
        0.75,
        "NIC",
        ("a packet arrives",),
        edge=PURPLE,
        face="white",
        body_size=7.3,
    )
    arrow(ax, (2.82, 3.30), (2.82, 3.95))
    arrow(ax, (2.82, 5.05), (2.82, 5.60), color=GRAY, dashed=True, label="readiness")
    arrow(ax, (2.82, 6.80), (2.82, 7.35), label="scheduled callback")
    arrow(ax, (2.82, 8.70), (2.82, 9.85))

    box(
        ax,
        5.35,
        6.25,
        4.20,
        2.45,
        "DPUmesh EQ owner thread",
        (
            "ppoll(command fd, EQ fd)",
            "dmesh_poll_eq(events)",
            "coalesce one QP receive run",
            "copy into one allocator slice",
            "release or retain RX credit",
            "invoke on_read inline",
        ),
        edge=GREEN,
        face=GREEN_BG,
        count="x 1 per shard",
        title_size=9.3,
        body_size=7.2,
    )
    box(
        ax,
        5.35,
        3.95,
        4.20,
        1.10,
        "pod RX mmap",
        ("registered host memory already written", "no kernel and no syscall"),
        edge=GREEN,
        face="white",
        title_size=9.0,
        body_size=7.2,
    )
    box(
        ax,
        5.35,
        2.55,
        4.20,
        0.75,
        "DPU",
        ("SG-DMA over PCIe",),
        edge=GREEN,
        face="white",
        body_size=7.2,
    )
    arrow(ax, (7.45, 3.30), (7.45, 3.95))
    arrow(ax, (7.45, 5.05), (7.45, 6.25), color=GRAY, dashed=True, label="EQ readiness")
    arrow(ax, (7.45, 8.70), (7.45, 9.85))
    ax.text(7.45, 5.55, "no callback-dispatch hop", fontsize=8.5, color=GREEN, ha="center")

    ax.text(10.40, 9.40, "TRANSMIT", fontsize=11)
    ax.text(10.40, 9.05, "stock gRPC", fontsize=9, color=PURPLE)
    ax.text(15.25, 9.05, "gRPC on DPUmesh", fontsize=9, color=GREEN)
    box(
        ax,
        10.40,
        7.35,
        4.35,
        1.35,
        "Endpoint::Write caller",
        ("sendmsg copies into socket buffer", "EAGAIN hands retry to poller"),
        edge=PURPLE,
        face=PURPLE_BG,
        count="gRPC worker",
        title_size=9.4,
        body_size=7.3,
    )
    box(
        ax,
        10.40,
        5.60,
        4.35,
        1.20,
        "poller, then pool thread",
        ("EPOLLOUT arms readiness", "a pool thread retries the write"),
        edge=PURPLE,
        face=PURPLE_BG,
        title_size=9.4,
        body_size=7.3,
    )
    box(
        ax,
        10.40,
        3.95,
        4.35,
        1.10,
        "kernel + socket send buffer",
        ("TCP/IP, netfilter and bridge",),
        edge=PURPLE,
        face="white",
        title_size=9.0,
        body_size=7.3,
    )
    box(
        ax,
        10.40,
        2.55,
        4.35,
        0.75,
        "NIC",
        ("the packet leaves",),
        edge=PURPLE,
        face="white",
        body_size=7.3,
    )
    arrow(ax, (12.58, 9.85), (12.58, 8.70))
    arrow(ax, (12.58, 7.35), (12.58, 6.80), label="blocked: hand retry off")
    arrow(ax, (12.58, 5.60), (12.58, 5.05))
    arrow(ax, (12.58, 3.95), (12.58, 3.30))

    box(
        ax,
        15.25,
        7.15,
        4.20,
        1.55,
        "Endpoint::Write caller",
        ("TX pump: alloc, copy, post_send, flush", "EAGAIN parks the exact cursor"),
        edge=GREEN,
        face=GREEN_BG,
        count="gRPC worker",
        title_size=9.4,
        body_size=7.3,
    )
    box(
        ax,
        15.25,
        5.20,
        4.20,
        1.30,
        "DPUmesh EQ owner thread",
        ("TX_READY arrives on the EQ", "the same owner resumes the parked pump"),
        edge=GREEN,
        face=GREEN_BG,
        count="x 1 per shard",
        title_size=9.2,
        body_size=7.2,
    )
    box(
        ax,
        15.25,
        3.95,
        4.20,
        0.85,
        "registered forward ring",
        ("publish the descriptor",),
        edge=GREEN,
        face="white",
        title_size=9.0,
        body_size=7.2,
    )
    box(
        ax,
        15.25,
        2.55,
        4.20,
        0.75,
        "DPU",
        ("a DPA EU drains the ring",),
        edge=GREEN,
        face="white",
        body_size=7.2,
    )
    arrow(ax, (17.35, 9.85), (17.35, 8.70))
    arrow(ax, (17.35, 7.15), (17.35, 6.50), color=GRAY, dashed=True, label="blocked: park")
    arrow(ax, (17.35, 5.20), (17.35, 4.80))
    arrow(ax, (17.35, 3.95), (17.35, 3.30))

    box(
        ax,
        0.55,
        0.55,
        18.90,
        1.35,
        "what actually changed",
        (
            "RX: one EQ owner reaches chttp2 directly after the DPU has DMA'd bytes into registered memory.",
            "TX: the caller pumps immediately; TX_READY returns to that QP's EQ owner without a pool hand-off.",
            "The callback dispatcher is a separate control/terminal path, not an extra normal-data-path hop.",
        ),
        body_size=7.7,
    )
    save(fig, "grpc_vs_stock")


if __name__ == "__main__":
    generate_grpc_threads()
    generate_grpc_vs_stock()
