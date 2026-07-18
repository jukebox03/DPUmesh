#!/usr/bin/env python3
"""plot_batch.py — figures for the matched-batching evaluation.

Reads the fresh (4,4) receipts and renders the batching-aware figures the report is
built on. Kept separate from plot.py (which renders the frozen summary.csv figures)
so each figure's data source is unambiguous.

    python3 plot_batch.py <data_dir> <out_dir>

Inputs  (in <data_dir>):
    batch_ablation.csv  one row/rep: transport,conc,batch,reply_batch,rep,mrps,gbps,
                        p50,p95,p99,avg,fail,reorder,rcnt,dist
    batch_cpu.csv       cell,conc,batch,reply_batch,mrps,p50_us,dpu_arm_pct,
                        arm_us_per_rpc,arm_pct_per_mrps
Outputs (PNG, in <out_dir>):
    fig_tput_vs_conc.png     throughput vs concurrency: direct / envoy / dpumesh
                             unbatched / dpumesh matched-batched
    fig_p50_vs_conc.png      p50 latency vs concurrency, same four series
    fig_batch_ablation.png   4-way ablation bars (none/req/reply/both) at conc 8/32/64
    fig_arm_cpu.png          DPU-ARM µs/RPC and %core, unbatched vs matched-batched
"""
import csv, sys, os, statistics as st
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

C_DIRECT = "#E69F00"; C_ENVOY = "#D55E00"; C_UNB = "#0072B2"; C_BOTH = "#009E73"
C_REQ = "#56B4E9"; C_REPLY = "#CC79A7"; C_NONE = "#999999"

def load(path):
    with open(path) as fh:
        return list(csv.DictReader(fh))

def med_cells(rows):
    """median of every metric over reps, keyed by (transport,conc,batch,reply_batch)."""
    g = defaultdict(list)
    for r in rows:
        try:
            g[(r["transport"], int(r["conc"]), int(r["batch"]), int(r["reply_batch"]))].append(r)
        except (KeyError, ValueError):
            continue
    out = {}
    for k, rs in g.items():
        def m(col): return st.median([float(x[col]) for x in rs])
        out[k] = dict(mrps=m("mrps"), p50=m("p50"), p99=m("p99"),
                      mrps_lo=min(float(x["mrps"]) for x in rs),
                      mrps_hi=max(float(x["mrps"]) for x in rs), n=len(rs))
    return out

# The four headline series and how to pick their cell for a given conc.
SERIES = [
    ("tcp-direct",           C_DIRECT, "-o", lambda C, c: C.get(("direct", c, 0, 0))),
    ("tcp-envoy",            C_ENVOY,  "-o", lambda C, c: C.get(("envoy",  c, 0, 0))),
    ("dpumesh (per-RPC)",    C_UNB,    "-o", lambda C, c: C.get(("dpumesh", c, 0, 0))),
    ("dpumesh (batched)",    C_BOTH,   "-s", lambda C, c: C.get(("dpumesh", c, 1, 1))),
]

def conc_axis(C):
    return sorted({c for (_, c, _, _) in C})

def fig_tput(C, out):
    concs = conc_axis(C)
    plt.figure(figsize=(6.2, 4.2))
    for name, col, style, pick in SERIES:
        xy = [(c, pick(C, c)) for c in concs]
        xy = [(c, d) for c, d in xy if d]
        if not xy: continue
        xs = [c for c, _ in xy]; ys = [d["mrps"] for _, d in xy]
        lo = [d["mrps_lo"] for _, d in xy]; hi = [d["mrps_hi"] for _, d in xy]
        plt.plot(xs, ys, style, color=col, label=name)
        plt.fill_between(xs, lo, hi, color=col, alpha=0.15)
    plt.xscale("log", base=2); plt.xlabel("concurrency window (outstanding / connection)")
    plt.ylabel("throughput (Mrps)")
    plt.title("Closed-loop throughput vs concurrency (1 KB, one connection)")
    plt.grid(True, which="both", alpha=0.3); plt.legend(); plt.tight_layout()
    p = os.path.join(out, "fig_tput_vs_conc.png"); plt.savefig(p, dpi=140); plt.close()
    return p

def fig_p50(C, out):
    concs = conc_axis(C)
    plt.figure(figsize=(6.2, 4.2))
    for name, col, style, pick in SERIES:
        xy = [(c, pick(C, c)) for c in concs]
        xy = [(c, d) for c, d in xy if d]
        if not xy: continue
        plt.plot([c for c, _ in xy], [d["p50"] for _, d in xy], style, color=col, label=name)
    plt.xscale("log", base=2); plt.xlabel("concurrency window (outstanding / connection)")
    plt.ylabel("median RTT p50 (µs)")
    plt.title("Closed-loop p50 latency vs concurrency (1 KB)")
    plt.grid(True, which="both", alpha=0.3); plt.legend(); plt.tight_layout()
    p = os.path.join(out, "fig_p50_vs_conc.png"); plt.savefig(p, dpi=140); plt.close()
    return p

def fig_ablation(C, out):
    concs = [c for c in (8, 32, 64) if ("dpumesh", c, 0, 0) in C]
    cells = [("none",      C_NONE,  lambda c: C.get(("dpumesh", c, 0, 0))),
             ("req-only",  C_REQ,   lambda c: C.get(("dpumesh", c, 1, 0))),
             ("reply-only",C_REPLY, lambda c: C.get(("dpumesh", c, 0, 1))),
             ("both",      C_BOTH,  lambda c: C.get(("dpumesh", c, 1, 1)))]
    fig, ax = plt.subplots(figsize=(7.2, 4.2))
    nb = len(cells); w = 0.8 / nb
    for i, (lab, col, pick) in enumerate(cells):
        xs = [j + (i - (nb - 1) / 2) * w for j in range(len(concs))]
        ys = [(pick(c) or {}).get("mrps", 0) for c in concs]
        ax.bar(xs, ys, width=w, color=col, label=lab)
    # envoy reference marks per conc
    for j, c in enumerate(concs):
        e = C.get(("envoy", c, 0, 0))
        if e:
            ax.hlines(e["mrps"], j - 0.45, j + 0.45, color=C_ENVOY, ls="--", lw=1.5,
                      label="tcp-envoy" if j == 0 else None)
    ax.set_xticks(range(len(concs))); ax.set_xticklabels([f"conc={c}" for c in concs])
    ax.set_ylabel("throughput (Mrps)")
    ax.set_title("Matched-batching ablation: coalescing must be symmetric to help")
    ax.grid(True, axis="y", alpha=0.3); ax.legend(ncol=3, fontsize=8); fig.tight_layout()
    p = os.path.join(out, "fig_batch_ablation.png"); fig.savefig(p, dpi=140); plt.close()
    return p

def fig_arm_cpu(cpu_rows, out):
    concs = sorted({int(r["conc"]) for r in cpu_rows})
    def pick(c, cell):
        for r in cpu_rows:
            if int(r["conc"]) == c and r["cell"] == cell: return r
        return None
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 4.2))
    x = range(len(concs)); w = 0.35
    ub = [float(pick(c, "unbatched")["arm_us_per_rpc"]) for c in concs]
    bo = [float(pick(c, "both")["arm_us_per_rpc"]) for c in concs]
    a1.bar([i - w/2 for i in x], ub, w, color=C_UNB, label="per-RPC (unbatched)")
    a1.bar([i + w/2 for i in x], bo, w, color=C_BOTH, label="matched-batched")
    for i, c in enumerate(concs):
        a1.text(i + w/2, bo[i], f"-{(1-bo[i]/ub[i])*100:.0f}%", ha="center", va="bottom", fontsize=9)
    a1.set_xticks(list(x)); a1.set_xticklabels([f"conc={c}" for c in concs])
    a1.set_ylabel("DPU-ARM CPU per request (µs)")
    a1.set_title("Batching halves ARM cost/RPC → cost is per-RPC, not core-bound")
    a1.grid(True, axis="y", alpha=0.3); a1.legend()
    # right: %core occupancy vs throughput
    ubm = [float(pick(c, "unbatched")["mrps"]) for c in concs]
    bom = [float(pick(c, "both")["mrps"]) for c in concs]
    uba = [float(pick(c, "unbatched")["dpu_arm_pct"]) for c in concs]
    boa = [float(pick(c, "both")["dpu_arm_pct"]) for c in concs]
    a2.plot(ubm, uba, "-o", color=C_UNB, label="per-RPC (unbatched)")
    a2.plot(bom, boa, "-s", color=C_BOTH, label="matched-batched")
    for c, m, a in list(zip(concs, ubm, uba)) + list(zip(concs, bom, boa)):
        a2.annotate(f"c{c}", (m, a), textcoords="offset points", xytext=(4, 4), fontsize=7)
    a2.set_xlabel("achieved throughput (Mrps)"); a2.set_ylabel("DPU-ARM occupancy (% of one core)")
    a2.set_title("ARM occupancy vs throughput")
    a2.grid(True, alpha=0.3); a2.legend()
    fig.suptitle("DPU-ARM cost: unbatched vs matched-batched (1 KB, one connection, (4,4))")
    fig.tight_layout(); p = os.path.join(out, "fig_arm_cpu.png"); fig.savefig(p, dpi=140); plt.close()
    return p

def main():
    if len(sys.argv) != 3:
        print("usage: plot_batch.py <data_dir> <out_dir>", file=sys.stderr); sys.exit(2)
    data, out = sys.argv[1], sys.argv[2]; os.makedirs(out, exist_ok=True)
    C = med_cells(load(os.path.join(data, "batch_ablation.csv")))
    cpu = load(os.path.join(data, "batch_cpu.csv"))
    for p in (fig_tput(C, out), fig_p50(C, out), fig_ablation(C, out), fig_arm_cpu(cpu, out)):
        print("wrote", p, file=sys.stderr)

if __name__ == "__main__":
    main()
