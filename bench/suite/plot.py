#!/usr/bin/env python3
"""plot.py — render the evaluation figures from a summary.csv (see analyze.py).

Produces the core paper figures as PNGs. Each function is defensive: it plots only
the points present, so the SAME script renders the localhost demo (one transport,
one line) and the full DPU run (four transports, four lines) without change. A
transport that was never measured simply does not appear.

    python3 plot.py <summary.csv> <out_dir>

Figures emitted when the matching points exist:
    fig_rtt_vs_size.png        unloaded RTT (conc=1, p50) vs request size, per transport
    fig_lat_throughput.png     p50/p99/p99.9 vs offered load (open loop), per transport
    fig_tput_vs_conc.png       closed-loop throughput vs concurrency window, per transport
    fig_lang_confound.png      same transport+server, C client vs Go client (the confound)
"""
import csv, sys, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# Colour-blind-safe, fixed per transport so figures across the paper agree.
COLORS = {
    "dpumesh-native":  "#0072B2", "dpumesh-preload": "#56B4E9",
    "tcp-envoy":       "#D55E00", "tcp-direct":      "#E69F00",
    "tcp-loopback-c":  "#009E73", "tcp-loopback-go": "#CC79A7",
}
def color(t): return COLORS.get(t, "#444444")

def load(path):
    return list(csv.DictReader(open(path)))

def f(r, k):
    try: return float(r[k])
    except (KeyError, ValueError): return None

def by_transport(rows, pred):
    d = {}
    for r in rows:
        if pred(r): d.setdefault(r["transport"], []).append(r)
    return d

def fig_rtt_vs_size(rows, out):
    sel = [r for r in rows if r["mode"] == "closed" and r["conc"] == "1"
           and r["threads"] == "1" and r["stage"] == "rtt"]
    if not sel: return None
    ts = by_transport(sel, lambda r: True)
    plt.figure(figsize=(6, 4))
    for t in sorted(ts):
        pts = sorted(ts[t], key=lambda r: int(r["req_size"]))
        xs = [int(r["req_size"]) for r in pts]
        ys = [f(r, "p50_med") for r in pts]
        lo = [f(r, "p50_lo") for r in pts]; hi = [f(r, "p50_hi") for r in pts]
        plt.plot(xs, ys, marker="o", label=t, color=color(t))
        plt.fill_between(xs, lo, hi, alpha=0.15, color=color(t))
    plt.xscale("log", base=2); plt.xlabel("request size (bytes)")
    plt.ylabel("unloaded p50 RTT (µs)"); plt.title("Unloaded RTT vs message size (conc=1)")
    plt.grid(True, which="both", alpha=0.3); plt.legend(); plt.tight_layout()
    p = os.path.join(out, "fig_rtt_vs_size.png"); plt.savefig(p, dpi=130); plt.close()
    return p

def fig_lat_throughput(rows, out):
    sel = [r for r in rows if r["mode"] == "open" and r["stage"] == "curve"]
    if not sel: return None
    # Group by (transport, workload) so different request sizes are never joined
    # into one line (64B and 1KB are separate curves, not one mixed series).
    ts = {}
    for r in sel: ts.setdefault((r["transport"], r["workload"]), []).append(r)
    plt.figure(figsize=(6.5, 4.2))
    styles = {"p50_med": "-", "p99_med": "--", "p999_med": ":"}
    labels = {"p50_med": "p50", "p99_med": "p99", "p999_med": "p99.9"}
    for (t, wl) in sorted(ts):
        pts = sorted(ts[(t, wl)], key=lambda r: (f(r, "mrps_med") or 0.0))
        for m, ls in styles.items():
            xy = [(f(r, "mrps_med"), f(r, m)) for r in pts]
            xy = [(x, y) for x, y in xy if x is not None and y is not None]
            if not xy: continue
            plt.plot([x for x, _ in xy], [y for _, y in xy], ls, marker=".",
                     color=color(t), label=f"{t} {wl} {labels[m]}")
    plt.xlabel("achieved throughput (Mrps)"); plt.ylabel("latency (µs)")
    plt.yscale("log"); plt.title("Latency vs offered load (open loop)")
    plt.grid(True, which="both", alpha=0.3); plt.legend(fontsize=7, ncol=2); plt.tight_layout()
    p = os.path.join(out, "fig_lat_throughput.png"); plt.savefig(p, dpi=130); plt.close()
    return p

def fig_tput_vs_conc(rows, out):
    sel = [r for r in rows if r["mode"] == "closed" and r["stage"] == "conc"]
    if not sel: return None
    ts = by_transport(sel, lambda r: True)
    plt.figure(figsize=(6, 4))
    for t in sorted(ts):
        pts = sorted(ts[t], key=lambda r: int(r["conc"]))
        xs = [int(r["conc"]) for r in pts]
        ys = [f(r, "mrps_med") for r in pts]
        lo = [f(r, "mrps_lo") for r in pts]; hi = [f(r, "mrps_hi") for r in pts]
        plt.plot(xs, ys, marker="o", label=t, color=color(t))
        plt.fill_between(xs, lo, hi, alpha=0.15, color=color(t))
    plt.xscale("log", base=2); plt.xlabel("concurrency window (outstanding / conn)")
    plt.ylabel("throughput (Mrps)"); plt.title("Closed-loop throughput vs concurrency")
    plt.grid(True, which="both", alpha=0.3); plt.legend(); plt.tight_layout()
    p = os.path.join(out, "fig_tput_vs_conc.png"); plt.savefig(p, dpi=130); plt.close()
    return p

def fig_goodput_vs_size(rows, out):
    sel = [r for r in rows if r["mode"] == "closed" and r["stage"] == "bw"]
    if not sel: return None
    ts = by_transport(sel, lambda r: True)
    plt.figure(figsize=(6, 4))
    for t in sorted(ts):
        pts = sorted(ts[t], key=lambda r: int(r["req_size"]))
        xs = [int(r["req_size"]) for r in pts]
        ys = [f(r, "gbps_med") for r in pts]
        lo = [f(r, "gbps_lo") for r in pts]; hi = [f(r, "gbps_hi") for r in pts]
        plt.plot(xs, ys, marker="o", label=t, color=color(t))
        plt.fill_between(xs, lo, hi, alpha=0.15, color=color(t))
        # Annotate the ACHIEVED outstanding window N ≈ throughput × avg-latency
        # (Little's law) wherever the requested conc=32 is NOT actually held, so the
        # large-message region is not mistaken for a like-for-like comparison.
        for r in pts:
            sz = int(r["req_size"]); g = f(r, "gbps_med")
            mr = f(r, "mrps_med"); av = f(r, "avg_med")
            if g is None or mr is None or av is None: continue
            N = mr * av
            if N < 28:  # meaningfully short of the requested 32
                dy = -12 if t == "dpumesh-native" else 8
                plt.annotate(f"N≈{N:.1f}", (sz, g), textcoords="offset points",
                             xytext=(0, dy), fontsize=6, color=color(t), ha="center")
    plt.xscale("log", base=2); plt.xlabel("request size (bytes)")
    plt.ylabel("request goodput (Gb/s)")
    plt.title("Goodput vs message size  (N = achieved concurrency)")
    plt.grid(True, which="both", alpha=0.3); plt.legend(); plt.tight_layout()
    p = os.path.join(out, "fig_goodput_vs_size.png"); plt.savefig(p, dpi=130); plt.close()
    return p

def fig_lang_confound(rows, out):
    sel = [r for r in rows if r["stage"] == "conc"
           and r["transport"] in ("tcp-loopback-c", "tcp-loopback-go")]
    if len({r["transport"] for r in sel}) < 2: return None
    ts = by_transport(sel, lambda r: True)
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 4))
    for t in sorted(ts):
        pts = sorted(ts[t], key=lambda r: int(r["conc"]))
        xs = [int(r["conc"]) for r in pts]
        a1.plot(xs, [f(r, "mrps_med") for r in pts], marker="o", label=t, color=color(t))
        a2.plot(xs, [f(r, "p50_med") for r in pts], marker="o", label=t, color=color(t))
    for a in (a1, a2): a.set_xscale("log", base=2); a.grid(True, which="both", alpha=0.3); a.legend()
    a1.set_xlabel("concurrency"); a1.set_ylabel("throughput (Mrps)"); a1.set_title("Throughput")
    a2.set_xlabel("concurrency"); a2.set_ylabel("p50 (µs)"); a2.set_title("Latency")
    fig.suptitle("Client-language confound: same transport + server, C vs Go client")
    fig.tight_layout()
    p = os.path.join(out, "fig_lang_confound.png"); fig.savefig(p, dpi=130); plt.close()
    return p

def fig_cpu_accounting(cpu_rows, out):
    """Host CPU (RQ3): where the cores go. Left = host %core vs load, with the
    DPUmesh DPU-ARM cost shown as the offloaded cycles; right = host efficiency
    (%core per Krps) — lower is better."""
    if not cpu_rows: return None
    ts = {}
    for r in cpu_rows: ts.setdefault(r["transport"], []).append(r)
    tname = {"dpumesh": "dpumesh-native", "tcp": "tcp-envoy"}
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 4.2))
    for t in sorted(ts):
        pts = sorted(ts[t], key=lambda r: int(r["conc"]))
        xs = [float(r["mrps"]) for r in pts]
        c = color(tname.get(t, t))
        a1.plot(xs, [float(r["host_total_pct"]) for r in pts], "-o", color=c,
                label=f"{tname.get(t,t)} host")
        if t == "dpumesh":
            a1.plot(xs, [float(r["dpu_arm_pct"]) for r in pts], "--s", color=c, alpha=0.6,
                    label="dpumesh DPU-ARM (offloaded)")
        a2.plot([float(r["mrps"]) for r in pts], [float(r["host_pct_per_krps"]) for r in pts],
                "-o", color=c, label=tname.get(t, t))
    a1.axhline(100, color="#888", ls=":", lw=1); a1.text(a1.get_xlim()[1], 103, "1 host core", ha="right", fontsize=7, color="#888")
    a1.set_xlabel("achieved throughput (Mrps)"); a1.set_ylabel("% of one core")
    a1.set_title("CPU vs throughput — host (solid) & DPU-ARM (dashed)")
    a1.grid(True, alpha=0.3); a1.legend(fontsize=7)
    a2.set_xlabel("achieved throughput (Mrps)"); a2.set_ylabel("host %core per Krps")
    a2.set_title("Host CPU per request vs load (lower = better)")
    a2.grid(True, which="both", alpha=0.3); a2.legend(fontsize=8)
    fig.suptitle("CPU cost per transport — host + DPU ARM (single pod, L4, 1KB)")
    fig.tight_layout()
    p = os.path.join(out, "fig_cpu_accounting.png"); fig.savefig(p, dpi=130); plt.close()
    return p

def tmap(t):  # busy/npod use short transport names; map to the fixed colour keys
    return {"dpumesh": "dpumesh-native", "tcp": "tcp-envoy", "direct": "tcp-direct"}.get(t, t)

def fig_busyapp(path, out):
    """A3: throughput & latency vs injected app-work. If freeing the host core matters,
    tcp-envoy (echo shares its core with an active sidecar) should fall off faster than
    dpumesh / tcp-direct (echo owns its core)."""
    rows = load(path)
    if not rows: return None
    ts = {}
    for r in rows: ts.setdefault(r["transport"], []).append(r)
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 4.2))
    for t in sorted(ts):
        pts = sorted(ts[t], key=lambda r: int(r["work_us"]))
        xs = [int(r["work_us"]) for r in pts]
        a1.plot(xs, [f(r, "mrps") for r in pts], "-o", color=color(tmap(t)), label=tmap(t))
        a2.plot(xs, [f(r, "p50") for r in pts], "-o", color=color(tmap(t)), label=tmap(t))
    a1.set_xlabel("injected app-work per request (µs)"); a1.set_ylabel("throughput (Mrps, log)")
    a1.set_yscale("log")   # the ≥5µs regime (where dpumesh overtakes tcp) is otherwise hidden
    a1.set_title("Throughput vs app-work (conc=32)"); a1.grid(True, which="both", alpha=.3); a1.legend()
    a2.set_xlabel("injected app-work per request (µs)"); a2.set_ylabel("p50 latency (µs)")
    a2.set_title("Latency vs app-work"); a2.grid(True, alpha=.3); a2.legend()
    fig.suptitle("Busy-app: does moving the transport off the host core help under real app-work?")
    fig.tight_layout(); p = os.path.join(out, "fig_busyapp.png"); fig.savefig(p, dpi=130); plt.close()
    return p

def fig_npod(path, out):
    """B1: N-pod amortization. Aggregate throughput and total CPU-per-request as N pods share
    the one DPU. TCP (independent pairs) is linear; DPUmesh's fixed DPU cost should amortize."""
    rows = load(path)
    if not rows: return None
    cfg = rows[0]["config"]; rows = [r for r in rows if r["config"] == cfg]
    ts = {}
    for r in rows: ts.setdefault(r["transport"], []).append(r)
    fig, (a1, a2, a3) = plt.subplots(1, 3, figsize=(15, 4.2))
    for t in sorted(ts):
        pts = sorted(ts[t], key=lambda r: int(r["N"]))
        ns = [int(r["N"]) for r in pts]
        c = color("dpumesh-native" if t == "dpumesh" else "tcp-envoy")
        a1.plot(ns, [f(r, "agg_mrps") for r in pts], "-o", color=c, label=t)
        a2.plot(ns, [f(r, "host_total_pct") for r in pts], "-o", color=c, label=f"{t} host")
        if t == "dpumesh":
            a2.plot(ns, [f(r, "host_total_pct") + f(r, "dpu_arm_pct") for r in pts], "--s",
                    color=c, alpha=.6, label="dpumesh host+DPU")
        # total CPU (host + DPU for dpumesh) per aggregate Krps — the amortization metric
        teff = []
        for r in pts:
            agg = f(r, "agg_mrps") or 0.0
            cpu = f(r, "host_total_pct") + (f(r, "dpu_arm_pct") if t == "dpumesh" else 0.0)
            teff.append(cpu / (agg * 1000) if agg > 0 else None)
        a3.plot(ns, teff, "-o", color=c, label=("dpumesh host+DPU" if t == "dpumesh" else "tcp host"))
    a1.set_xlabel("N pods"); a1.set_ylabel("aggregate throughput (Mrps)"); a1.set_title(f"Aggregate throughput vs N ({cfg})")
    a2.set_xlabel("N pods"); a2.set_ylabel("% of one core"); a2.set_title("Total host cores (dashed = + DPU)")
    a3.set_xlabel("N pods"); a3.set_ylabel("%core per Krps"); a3.set_title("Total CPU per request (host+DPU) vs N")
    for a in (a1, a2, a3): a.grid(True, alpha=.3); a.legend(fontsize=8)
    fig.suptitle(f"N-pod amortization ({cfg}) — TCP N>1 is linear (independent pairs)")
    fig.tight_layout(); p = os.path.join(out, "fig_npod.png"); fig.savefig(p, dpi=130); plt.close()
    return p

def fig_frontier(path, out):
    """DPU-config frontier: total CPU (host+DPU) per aggregate Krps vs N, one line per DPU
    config. Leaner configs lower the per-RPC DPU cost so amortization improves — but even the
    leanest stays well above TCP's flat line."""
    allrows = load(path)
    rows = [r for r in allrows if r["transport"] == "dpumesh"]
    if not rows: return None
    cmap = {"1-1": "#009E73", "2-2": "#0072B2", "4-4": "#CC79A7"}
    plt.figure(figsize=(6.5, 4.2))
    for cfg in sorted({r["config"] for r in rows}):
        pts = sorted([r for r in rows if r["config"] == cfg], key=lambda r: int(r["N"]))
        ns = [int(r["N"]) for r in pts]
        eff = [((f(r, "host_total_pct") + f(r, "dpu_arm_pct")) / (f(r, "agg_mrps") * 1000))
               if f(r, "agg_mrps") else None for r in pts]
        plt.plot(ns, eff, "-o", label=f"dpumesh ({cfg})", color=cmap.get(cfg, "#444"))
    tcp = [r for r in allrows if r["transport"] == "tcp-envoy"]
    if tcp:
        teff = f(tcp[0], "host_per_krps")
        plt.axhline(teff, color="#D55E00", ls="--", label=f"tcp-envoy (~{teff:.2f}, no DPU)")
    plt.xlabel("N pods"); plt.ylabel("total CPU per Krps (host+DPU, %core)")
    plt.title("DPU-config frontier: total CPU per request vs N")
    plt.grid(True, alpha=.3); plt.legend(); plt.tight_layout()
    p = os.path.join(out, "fig_frontier.png"); plt.savefig(p, dpi=130); plt.close()
    return p

def main():
    if len(sys.argv) < 3:
        print("usage: plot.py <summary.csv> <out_dir> [cpu.csv] [busy.csv] [npod.csv]", file=sys.stderr); sys.exit(2)
    rows = load(sys.argv[1]); out = sys.argv[2]; os.makedirs(out, exist_ok=True)
    made = []
    for fn in (fig_rtt_vs_size, fig_lat_throughput, fig_tput_vs_conc,
               fig_goodput_vs_size, fig_lang_confound):
        p = fn(rows, out)
        if p: made.append(p)
    for extra in sys.argv[3:]:                       # dispatch extra CSVs by filename
        if not os.path.exists(extra): continue
        base = os.path.basename(extra)
        if   "cpu"  in base: p = fig_cpu_accounting(load(extra), out)
        elif "busy" in base: p = fig_busyapp(extra, out)
        elif "npod" in base:
            p = fig_npod(extra, out);  made += [x for x in [p] if x]
            p = fig_frontier(extra, out)
        else: p = None
        if p: made.append(p)
    for p in made: print("wrote", p, file=sys.stderr)
    if not made: print("plot: no matching points for any figure", file=sys.stderr)

if __name__ == "__main__":
    main()
