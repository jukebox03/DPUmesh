#!/usr/bin/env python3
"""Inline IPsec cost figure for the RDMA meeting.

Three panels: what encryption costs a round trip, what it costs goodput, and how
that cost compares with doing AES-GCM on an Arm core. Numbers are the closed-loop
ping-pong measurements in OWNER_RESULTS.md (owner vs stub adapter, same path).

Output is deterministic: rerunning with unchanged inputs reproduces both files.
"""
import matplotlib

matplotlib.use("Agg")
matplotlib.rcParams["svg.hashsalt"] = "dpumesh-ipsec-cost"
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path

OUT = Path(__file__).resolve().parent
PLAIN, CRYPT = "#989892", "#287de1"
SOFT, INLINE = "#c84b43", "#12ad76"

sizes = ["64 B", "8 KiB", "64 KiB"]
nbytes = np.array([64, 8192, 65536], dtype=float)
rtt_plain, rtt_crypt = np.array([31, 37, 74.0]), np.array([36, 43, 81.0])
bw_plain, bw_crypt = np.array([10.0, 744, 1440.0]), np.array([8.7, 673, 1362.0])
sw_us, inline_us = np.array([0.22, 15.0, 119.0]), np.array([5.7, 6.3, 7.2])

fig, (ax1, ax2, ax3) = plt.subplots(1, 3, figsize=(13.5, 4.1), constrained_layout=True)
x, w = np.arange(3), 0.36

# Panel 1 - latency: a fixed additive cost.
ax1.bar(x - w / 2, rtt_plain, w, color=PLAIN, label="plaintext")
ax1.bar(x + w / 2, rtt_crypt, w, color=CRYPT, label="encrypted")
for xi, p, c in zip(x, rtt_plain, rtt_crypt):
    ax1.annotate(f"+{c - p:.0f} µs", (xi + w / 2, c), xytext=(0, 4),
                 textcoords="offset points", ha="center", fontsize=9, color=CRYPT)
ax1.set_title("Round-trip latency p50", fontsize=11)
ax1.set_ylabel("µs")
ax1.set_ylim(0, 95)
ax1.legend(frameon=False, fontsize=9)

# Panel 2 - goodput: the same cost as a shrinking fraction.
ax2.bar(x - w / 2, bw_plain, w, color=PLAIN, label="plaintext")
ax2.bar(x + w / 2, bw_crypt, w, color=CRYPT, label="encrypted")
for xi, p, c in zip(x, bw_plain, bw_crypt):
    ax2.annotate(f"{100 * (c - p) / p:+.1f}%", (xi + w / 2, c), xytext=(0, 4),
                 textcoords="offset points", ha="center", fontsize=9, color=CRYPT)
ax2.set_yscale("log")
ax2.set_ylim(4, 4000)
ax2.set_title("Goodput (log scale)", fontsize=11)
ax2.set_ylabel("MB/s")
ax2.legend(frameon=False, fontsize=9, loc="upper left")

# Panel 3 - shape of the cost: O(n) software against O(1) inline.
ax3.plot(nbytes, sw_us, "o-", color=SOFT, lw=2, ms=6,
         label="software (AES-GCM, 1 Arm core)")
ax3.plot(nbytes, inline_us, "s-", color=INLINE, lw=2, ms=6, label="inline (NIC)")
ax3.annotate(f"{sw_us[1]:.0f}", (nbytes[1], sw_us[1]), xytext=(-4, 7),
             textcoords="offset points", ha="right", fontsize=8, color=SOFT)
ax3.annotate(f"{sw_us[2]:.0f} \u00b5s", (nbytes[2], sw_us[2]), xytext=(-6, 2),
             textcoords="offset points", ha="right", fontsize=9, color=SOFT)
ax3.annotate(f"{inline_us[0]:.1f} \u2192 {inline_us[2]:.1f} \u00b5s",
             (nbytes[2], inline_us[2]), xytext=(-6, 8), textcoords="offset points",
             ha="right", fontsize=9, color=INLINE)
ax3.set_xscale("log")
ax3.set_ylim(0, 132)
ax3.set_title("Crypto cost per round trip", fontsize=11)
ax3.set_ylabel("\u00b5s")
ax3.legend(frameon=False, fontsize=9, loc="upper left")

for ax in (ax1, ax2):
    ax.set_xticks(x, sizes)
ax3.set_xticks(nbytes, sizes)
ax3.set_xticks([], minor=True)
for ax in (ax1, ax2, ax3):
    ax.grid(axis="y", alpha=0.2)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)

fig.savefig(OUT / "ipsec_cost.png", dpi=180)
fig.savefig(OUT / "ipsec_cost.svg")
