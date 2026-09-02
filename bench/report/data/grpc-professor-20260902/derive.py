#!/usr/bin/env python3
"""Derive the report's secondary tables from the committed receipts.

Every number quoted in FINAL.md that is not a raw median comes from here, so a
reader can regenerate it:  python3 derive.py  (writes derived-*.csv).
"""

from pathlib import Path

import pandas as pd


ROOT = Path(__file__).resolve().parent
SLO_P99_US = 5000          # tail SLO used beside the delivery definition
WORKERS_DEFAULT = 8


def fmt(df: pd.DataFrame, name: str) -> None:
    df.to_csv(ROOT / name, index=False, float_format="%.4f")


open_df = pd.read_csv(ROOT / "open-summary.csv")
knee = pd.read_csv(ROOT / "knee-followup-summary.csv")
low = pd.read_csv(ROOT / "dpu-low-load-summary.csv")
conc = pd.read_csv(ROOT / "concurrency-summary.csv")
closed = pd.read_csv(ROOT / "closed-summary.csv")
mesh_closed = pd.read_csv(ROOT / "mesh-closed-summary.csv")
mesh_cpu = pd.read_csv(ROOT / "mesh-cpu-summary.csv")
scale = pd.read_csv(ROOT / "worker-scale-summary.csv")

# ---- 1. DPU worker CPU against the number of requests in flight -------------
# Open loop: mean in flight = achieved rate x latency (Little's law, p50 as the
# latency proxy).  Closed loop: the window itself is the in-flight count.
rows = []
for r in low.itertuples():
    rows.append(("open-low", r.frame, WORKERS_DEFAULT, r.achieved_med, r.p50_med_us,
                 r.achieved_med * r.p50_med_us / 1e6, r.dpu_worker_med_cores))
for r in open_df[open_df.clean.eq("yes")].itertuples():
    rows.append(("open", r.frame, WORKERS_DEFAULT, r.achieved_med, r.p50_med_us,
                 r.achieved_med * r.p50_med_us / 1e6, r.dpu_worker_med_cores))
for r in conc.itertuples():
    rows.append(("closed", r.frame, WORKERS_DEFAULT, r.achieved_med, r.p50_med_us,
                 float(r.total_concurrency), r.dpu_worker_med_cores))
for r in scale[scale.clean.eq("yes")].itertuples():
    rows.append((f"scale-A{r.workers}", 64, r.workers, r.achieved_med, r.p50_med_us,
                 r.achieved_med * r.p50_med_us / 1e6, r.dpu_worker_med_cores))
inflight = pd.DataFrame(rows, columns=[
    "series", "frame", "workers", "rps", "p50_us", "mean_inflight", "dpu_worker_cores"])
inflight["arm_us_per_rpc"] = inflight.dpu_worker_cores / inflight.rps * 1e6
inflight["cores_per_inflight"] = inflight.dpu_worker_cores / inflight.mean_inflight
inflight["worker_busy_fraction"] = inflight.dpu_worker_cores / inflight.workers
inflight = inflight.sort_values(["series", "frame", "rps"]).reset_index(drop=True)
fmt(inflight, "derived-inflight-cpu.csv")

# The fitted occupancy: below one request per worker the ratio is a constant.
under = inflight[inflight.mean_inflight < inflight.workers]
occupancy = under.cores_per_inflight.median()
knee_cost = inflight[inflight.series.eq("open") & inflight.frame.eq(64)].arm_us_per_rpc.min()

# ---- 2. Capacity under the delivery definition and under a tail SLO ---------
clean_curve = pd.concat([
    open_df[open_df.clean.eq("yes")][["frame", "offered", "p99_med_us"]],
    knee[knee.classification.eq("clean")][["frame", "offered", "p99_med_us"]],
]).drop_duplicates(["frame", "offered"]).sort_values(["frame", "offered"])
cap_rows = []
for frame, g in clean_curve.groupby("frame"):
    # A rate is only capacity if no fresh-redeploy repetition at or below it
    # was mixed or bad; the single-campaign clean run above that is an envelope.
    unstable = knee[knee.frame.eq(frame) & knee.classification.ne("clean")].offered.min()
    if pd.notna(unstable):
        g = g[g.offered.lt(unstable)]
    delivery = g.offered.max()
    p99_at_delivery = g[g.offered.eq(delivery)].p99_med_us.iloc[0]
    slo = g[g.p99_med_us.le(SLO_P99_US)]
    slo_rate = slo.offered.max() if not slo.empty else 0
    p99_at_slo = slo[slo.offered.eq(slo_rate)].p99_med_us.iloc[0] if slo_rate else float("nan")
    unloaded = low[low.frame.eq(frame) & low.offered.eq(10000)]
    cap_rows.append((frame, delivery, p99_at_delivery, slo_rate, p99_at_slo,
                     unloaded.p50_med_us.iloc[0]))
capacity = pd.DataFrame(cap_rows, columns=[
    "frame", "delivery_capacity_rps", "p99_us_at_delivery",
    f"slo_capacity_rps_p99_le_{SLO_P99_US}us", "p99_us_at_slo", "p50_us_at_10k"])
fmt(capacity, "derived-capacity.csv")

# ---- 3. Three transports on the same application ---------------------------
PROXY_CORES = {"direct-tcp": 0, "DPUmesh": 8, "Linkerd": 2}   # configured, not measured
cmp_rows = []
for frame in (64, 1024, 8192):
    direct = closed[closed.transport.eq("direct-tcp") & closed.frame.eq(frame)].iloc[0]
    dpum = mesh_closed[mesh_closed.transport.eq("DPUmesh") & mesh_closed.frame.eq(frame)].iloc[0]
    lkd = mesh_closed[mesh_closed.transport.eq("Linkerd") & mesh_closed.frame.eq(frame)].iloc[0]
    for name, row in (("direct-tcp", direct), ("DPUmesh", dpum), ("Linkerd", lkd)):
        cores = PROXY_CORES[name]
        cmp_rows.append((frame, name, row.achieved_med, row.achieved_med / direct.achieved_med,
                         row.p50_med_us, cores,
                         row.achieved_med / cores if cores else float("nan")))
comparison = pd.DataFrame(cmp_rows, columns=[
    "frame", "transport", "closed_1024_rps", "ratio_to_direct", "p50_us",
    "configured_proxy_cores", "rps_per_proxy_core"])
fmt(comparison, "derived-comparison.csv")

# Matched 10k: what the Host gives back and what the DPU spends for it.
ex_rows = []
for frame in (64, 1024, 8192):
    d = mesh_cpu[mesh_cpu.transport.eq("DPUmesh") & mesh_cpu.frame.eq(frame)].iloc[0]
    l = mesh_cpu[mesh_cpu.transport.eq("Linkerd") & mesh_cpu.frame.eq(frame)].iloc[0]
    ex_rows.append((frame, l.host_total_core, d.host_total_core,
                    l.host_total_core - d.host_total_core, d.dpu_worker_core,
                    d.dpu_worker_core / (l.host_total_core - d.host_total_core),
                    l.p50_med_us, d.p50_med_us, d.p50_med_us / l.p50_med_us))
exchange = pd.DataFrame(ex_rows, columns=[
    "frame", "linkerd_host_cores", "dpumesh_host_cores", "host_cores_saved",
    "arm_worker_cores_spent", "arm_per_host_core_saved",
    "linkerd_p50_us", "dpumesh_p50_us", "p50_ratio_dpumesh_over_linkerd"])
fmt(exchange, "derived-exchange-10k.csv")

# ---- 4. Payload scaling -----------------------------------------------------
pay_rows = []
for name, table in (("direct-tcp", closed[closed.transport.eq("direct-tcp")]),
                    ("DPUmesh", mesh_closed[mesh_closed.transport.eq("DPUmesh")]),
                    ("Linkerd", mesh_closed[mesh_closed.transport.eq("Linkerd")])):
    base = table[table.frame.eq(64)].achieved_med.iloc[0]
    for frame in (64, 1024, 8192):
        v = table[table.frame.eq(frame)].achieved_med.iloc[0]
        pay_rows.append((name, frame, v, v / base))
payload = pd.DataFrame(pay_rows, columns=["transport", "frame", "closed_1024_rps", "relative_to_64B"])
fmt(payload, "derived-payload-scaling.csv")

# ARM cost per RPC at each payload's highest 3/3-clean open-loop point.
knee_rows = []
for frame, g in open_df[open_df.clean.eq("yes")].groupby("frame"):
    top = g.sort_values("offered").iloc[-1]
    knee_rows.append((frame, top.offered, top.achieved_med, top.dpu_worker_med_cores,
                      top.dpu_worker_med_cores / top.achieved_med * 1e6))
knee_cost_df = pd.DataFrame(knee_rows, columns=[
    "frame", "offered", "achieved", "dpu_worker_cores", "arm_us_per_rpc"])
c64 = knee_cost_df[knee_cost_df.frame.eq(64)].arm_us_per_rpc.iloc[0]
c8k = knee_cost_df[knee_cost_df.frame.eq(8192)].arm_us_per_rpc.iloc[0]
knee_cost_df["ns_per_payload_byte_vs_64B"] = (
    (knee_cost_df.arm_us_per_rpc - c64) * 1e3 / (2 * (knee_cost_df.frame - 64))
).where(knee_cost_df.frame.ne(64))
fmt(knee_cost_df, "derived-knee-cost.csv")

print(f"occupancy (worker cores per request in flight, below 1/worker): {occupancy:.3f}")
print(f"64 B ARM us/RPC at the open-loop knee: {knee_cost:.1f}")
print(f"64 B -> 8 KiB ARM cost per payload byte: {(c8k - c64) * 1e3 / (2 * 8128):.1f} ns")
print(capacity.to_string(index=False))
print(comparison.to_string(index=False))
print(exchange.to_string(index=False))
