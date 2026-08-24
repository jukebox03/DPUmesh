"""Drawing primitives shared by the DPUmesh architecture diagrams.

Every figure in this directory is a hand-laid matplotlib drawing built from the
same five shapes: a rounded box, an arrow, a dashed container, a plain polyline
and a legend strip. The metrics that differ between figures — where a box's
title sits, how far apart its body lines are, how an arrow's label is drawn —
are a `Style`, and `styled()` binds one to the three primitives that read it.
The remaining primitives take no style, so they are used directly.

Output is deterministic: `save()` writes both formats with no timestamp, so
regenerating a figure whose inputs have not changed reproduces it byte for byte.
"""

from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

OUT = Path(__file__).resolve().parent

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


@dataclass(frozen=True)
class Style:
    """Per-figure metrics. The defaults are the architecture-diagram set."""

    title_dy: float = 0.31
    body_dy: float = 0.68
    body_step: float = 0.29
    label_dy: float = 0.15
    label_color: str = "#626262"
    label_size: float = 7.8
    label_boxed: bool = True
    meaning_dx: float = 2.05


def setup_figure(width: float, height: float, xlim, ylim):
    fig, ax = plt.subplots(figsize=(width, height))
    fig.patch.set_facecolor("white")
    ax.set_xlim(*xlim)
    ax.set_ylim(*ylim)
    ax.axis("off")
    return fig, ax


def line(ax, points, *, color=GRAY, dashed=False, lw=1.25):
    xs, ys = zip(*points)
    ax.plot(xs, ys, color=color, lw=lw, ls="--" if dashed else "-")


def styled(style: Style = Style()):
    """The three style-reading primitives, bound to one figure's metrics.

    Returns `box`, `arrow` and `terms`. `container` is a dashed `box`, so it
    comes back bound to the same style rather than as a separate import.
    """

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
        title_x = x + 0.20
        if number is not None:
            ax.text(
                x + 0.30,
                y + h - style.title_dy,
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
            y + h - style.title_dy,
            title,
            color="#202020",
            fontsize=title_size,
            ha="left",
            va="center",
        )
        if count:
            ax.text(
                x + w - 0.18,
                y + h - style.title_dy,
                count,
                color="#555555",
                fontsize=8,
                ha="right",
                va="center",
            )
        for i, body_line in enumerate(lines):
            ax.text(
                x + 0.20,
                y + h - style.body_dy - i * style.body_step,
                body_line,
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
        label_dy=None,
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
                (start[1] + end[1]) / 2
                + (style.label_dy if label_dy is None else label_dy),
                label,
                color=style.label_color,
                fontsize=style.label_size,
                ha="center",
                va="center",
                bbox=(
                    dict(fc="white", ec="none", pad=0.8) if style.label_boxed else None
                ),
            )

    def terms(ax, x, y, w, h, entries, *, columns=2, title="How to read this figure"):
        """Legend strip defining the vocabulary the boxes use."""
        patch = FancyBboxPatch(
            (x, y),
            w,
            h,
            boxstyle="round,pad=0.06,rounding_size=0.10",
            linewidth=1.1,
            edgecolor="#d9d9d4",
            facecolor="#fafaf8",
            mutation_aspect=1.0,
        )
        ax.add_patch(patch)
        ax.text(x + 0.22, y + h - 0.32, title, fontsize=9.6, color="#444444", ha="left")
        rows = (len(entries) + columns - 1) // columns
        col_w = (w - 0.44) / columns
        for i, (term, meaning) in enumerate(entries):
            col, row = divmod(i, rows)
            tx = x + 0.22 + col * col_w
            ty = y + h - 0.70 - row * 0.34
            ax.text(tx, ty, term, fontsize=8.3, color="#151515", ha="left")
            ax.text(
                tx + style.meaning_dx,
                ty,
                meaning,
                fontsize=8.3,
                color="#555555",
                ha="left",
            )

    return box, container, arrow, terms


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
