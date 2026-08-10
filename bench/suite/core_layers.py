#!/usr/bin/env python3
"""Attribute sampled host-core time to the software layer that asked for it.

Reads the run directories written by core_profile.sh and reports, per
configuration, frame size and offered rate:

  owner   the deepest user frame belonging to the application, the transport
          library, the gRPC runtime or Envoy. A syscall, a driver call or a
          context switch is charged to the caller that entered it.
  site    what the leaf frame is: that layer's own instructions, libc, the
          vDSO, the kernel network path, syscall and poll machinery, the
          scheduler, or the verbs driver.

Sample counts become cores through the endpoint's cgroup CPU usage over the
same window. Physical core busy time comes from /proc/stat.

Repetitions of one point are reduced to the run whose endpoint core is the
median, so every bucket comes from one observation and the buckets sum to the
reported total.
"""
from __future__ import annotations

import argparse
import csv
import os
import re
import statistics
import sys
from collections import Counter, defaultdict

# ---------------------------------------------------------------- taxonomy

APP_BINARIES = {
    "bench_dpumesh", "echo_dpumesh", "bench_sock", "echo_sock",
    "preload_runner", "tcp_client", "tcp_echo",
}
GRPC_BINARIES = {"bench_grpc", "echo_grpc"}
TRANSPORT_LIBS = ("libdpumesh.so", "libdmesh_preload.so")
DRIVER_LIBS = ("libdoca_", "libibverbs", "libmlx5", "libflexio", "libnl-", "librdmacm")
RUNTIME_LIBS = (
    "libc.so", "libc-", "libpthread", "libm.so", "libm-", "libstdc++", "libgcc_s",
    "ld-linux", "ld-2", "libdl.so", "librt.so", "libatomic", "libcrypto", "libssl",
    "libz.so",
)
DRIVER_MODULES = ("mlx5", "ib_", "rdma_")

# bench_grpc and echo_grpc link the application, the gRPC runtime and the
# DPUmesh EventEngine adapter into one ELF, so the split is by symbol.
ADAPTER_SYMBOL_RE = re.compile(r"dpumesh::grpc")
APP_SYMBOL_RE = re.compile(
    r"^main$|(?:^|::)(Issue|CompleterMain|SleepUntil|IssuerMain|SelfTest|"
    r"RunBench|CtrlListen|HandleControl|EchoService|BenchClient|"
    r"bench_[a-z_]+|hist_[a-z_]+|frame_[a-z_]+)\b"
)
GRPC_SYMBOL_RE = re.compile(
    r"^(grpc|gpr_|census_|upb_|_upb|tsi_|absl::|google::|std::|__gnu_cxx|"
    r"envoy::|xds::|udpa::|re2::|RE2|opencensus|OPENSSL_|SSL_|EVP_|CRYPTO_|"
    r"BIO_|X509|asn1_|ASN1_|bn_|BN_|ec_|EC_|rsa_|RSA_|aes|AES_|sha|SHA)"
)

IDLE_RE = re.compile(
    r"^(mwait_idle|cpuidle_|do_idle|poll_idle|intel_idle|cpu_startup_entry|acpi_idle)")
SCHED_RE = re.compile(
    r"^(__schedule|schedule|schedule_\w+|psi_|update_load_avg|__update_load_avg|"
    r"update_curr|__calc_delta|pick_next|put_prev|dequeue_|enqueue_|__switch_to|"
    r"switch_fpu|save_fpregs|restore_fpregs|fpregs_|fpu_|set_next_entity|"
    r"update_cfs|update_rq|sched_clock|native_sched_clock|read_tsc|finish_task|"
    r"prepare_task|activate_task|deactivate_task|ttwu|try_to_wake_up|wake_up_|"
    r"__wake_up|autoremove_wake|default_wake|select_task_rq|check_preempt|resched_|"
    r"__cond_resched|cpuacct|cgroup_rstat|hrtimer|__hrtimer|tick_|clockevents|"
    r"lapic_next|rcu_|newidle_balance|load_balance|available_idle|raw_spin_rq|"
    r"reweight_entity|timerqueue_|set_next_buddy|update_min_vruntime|iterate_groups|"
    r"__wrgsbase|__rdgsbase|native_load_tls|__rseq|rseq_|__perf_event|"
    r"perf_event_task_sched|place_entity|__enqueue_entity|__dequeue_entity|"
    r"migrate_task|sched_ttwu)")
KDRIVER_RE = re.compile(
    r"^(devx_|mlx5_|__mlx5|ib_uverbs|uverbs_|ib_umem|ib_device|rdma_|mlx4_)")
POLL_RE = re.compile(
    r"^(ep_|do_epoll|__x64_sys_epoll|__do_sys_epoll|eventfd|poll_freewait|"
    r"__pollwait|do_poll|do_sys_poll|futex|__futex|do_futex|timerfd|"
    r"wake_up_q|remove_wait_queue|add_wait_queue|init_wait|prepare_to_wait|"
    r"finish_wait|__ep_eventpoll_poll|select_estimate_accuracy|poll_select)")
NET_RE = re.compile(
    r"^(tcp_|__tcp_|ip_|__ip_|ip6_|inet_|__inet|udp_|skb_|__skb|kfree_skb|"
    r"napi_|__napi|netif_|__netif|dev_hard|dev_queue|__dev_queue|net_rx|net_tx|"
    r"sock_|__sock|lock_sock|release_sock|sk_|__sk_|__local_bh|do_softirq|"
    r"__do_softirq|loopback_xmit|process_backlog|nf_|__nf|iptable|ipt_|br_|veth|"
    r"csum_partial|packet_|security_socket|selinux_socket|move_addr|__sys_send|"
    r"nft_|nf_conntrack|__nf_conntrack|conntrack_|ipvs_|xt_|ebt_|"
    r"__sys_recv|____sys_send|____sys_recv|__x64_sys_send|__x64_sys_recv|"
    r"unix_|__unix|scm_)")
SYSCALL_RE = re.compile(
    r"^(entry_SYSCALL|do_syscall_64|syscall_|x64_sys_call|__x64_sys_|ksys_|vfs_|"
    r"__fget|fput|fdget|__fdget|_copy_to_user|_copy_from_user|copy_user_|"
    r"__check_object_size|rw_verify|new_sync|__audit|audit_|get_timespec64|"
    r"put_timespec64|ktime_get_ts64|__put_user|__get_user|exit_to_user_mode|"
    r"arch_exit_to_user|syscall_exit|apparmor_|security_file|__fsnotify|"
    r"__x64_sys_ioctl|do_vfs_ioctl)")
# Leaves that name no subsystem; the caller decides.
GENERIC_RE = re.compile(
    r"^(_raw_spin|native_queued_spin|queued_spin|__memcpy|memcpy|__memset|memset|"
    r"__memmove|memmove|copy_user_|clear_page|error_entry|error_return|"
    r"native_write_msr|native_read_msr|__sanitizer|osq_lock|osq_unlock|"
    r"mutex_|__mutex|down_read|up_read|preempt_count|rb_insert_color|rb_erase|"
    r"rb_next|rb_first|__rb_|_raw_read|_raw_write|__list_|list_)")

OWNER_ORDER = ["app", "transport", "grpc", "envoy", "kernel-only", "softirq", "noise"]

FRAME_COLORS = {
    "app":        (0x2A, 0x78, 0xD6),
    "transport":  (0x1B, 0xAF, 0x7A),
    "grpc":       (0xED, 0xA1, 0x00),
    "envoy":      (0xE8, 0x7B, 0xA4),
    "driver":     (0x00, 0x83, 0x00),
    "runtime":    (0x4A, 0x3A, 0xA7),
    "vdso":       (0x4A, 0x3A, 0xA7),
    "kernel":     (0x9A, 0x97, 0x8D),
    "noise":      (0xC9, 0xC6, 0xBD),
    "unresolved": (0xC9, 0xC6, 0xBD),
}

SAMPLE_RE = re.compile(
    r"^(?P<comm>.+?)\s+(?P<pid>\d+)(?:/(?P<tid>\d+))?\s+\[(?P<cpu>\d+)\]\s+"
    r"(?P<ts>[\d.]+):\s+(?P<period>\d+)\s+(?P<event>\S+):"
)
FRAME_RE = re.compile(r"^\s+(?P<addr>[0-9a-f]+)\s+(?P<sym>.*?)\s+\((?P<dso>.*)\)\s*$")


def base(path: str) -> str:
    return path.rsplit("/", 1)[-1]


def frame_layer(sym: str, dso: str) -> str:
    if dso == "[kernel.kallsyms]":
        return "kernel"
    if dso.startswith("[vdso"):
        return "vdso"
    if dso.startswith("[") and dso.endswith("]"):
        name = dso[1:-1]
        if any(name.startswith(p) for p in DRIVER_MODULES):
            return "driver"
        return "unresolved" if name == "unknown" else "kernel"
    d = base(dso)
    if d == "anon" or d.startswith("anon_inode"):
        return "unresolved"
    if ".ko" in d:
        return "driver" if any(d.startswith(p) for p in DRIVER_MODULES) else "kernel"
    if any(d.startswith(p) for p in TRANSPORT_LIBS):
        return "transport"
    if any(d.startswith(p) for p in DRIVER_LIBS):
        return "driver"
    if d in APP_BINARIES:
        return "app"
    if d in GRPC_BINARIES:
        if ADAPTER_SYMBOL_RE.search(sym):
            return "transport"
        if APP_SYMBOL_RE.search(sym):
            return "app"
        return "grpc"
    if d == "envoy":
        return "envoy"
    if any(d.startswith(p) for p in RUNTIME_LIBS):
        return "runtime"
    return "noise"


def kernel_site(kframes: list[str]) -> str:
    if any(IDLE_RE.match(s) for s in kframes):
        return "idle"
    for s in kframes:
        if GENERIC_RE.match(s):
            continue
        if KDRIVER_RE.match(s):
            return "driver-code"
        if NET_RE.match(s):
            return "kernel-net"
        if SCHED_RE.match(s):
            return "kernel-sched"
        if POLL_RE.match(s):
            return "kernel-poll"
        if SYSCALL_RE.match(s):
            return "kernel-syscall"
        return "kernel-other"
    return "kernel-other"


def classify(comm: str, frames: list[tuple[str, str]]) -> tuple[str, str]:
    """(owner, site) for one sample; frames are leaf first."""
    layers = [frame_layer(s, d) for s, d in frames]

    if not frames:
        site = "unresolved"
    else:
        leaf = layers[0]
        if leaf == "kernel":
            site = kernel_site([s for (s, _), l in zip(frames, layers) if l == "kernel"])
        elif leaf == "vdso":
            site = "vdso"
        elif leaf == "runtime":
            site = "libc"
        elif leaf in ("unresolved", "noise"):
            site = "unresolved"
        else:
            site = leaf + "-code"
    if site == "idle":
        return "idle", "idle"

    owner = next((l for l in layers if l in ("app", "transport", "grpc", "envoy")), None)
    if owner is None:
        if comm.startswith("ksoftirqd") or site == "kernel-net":
            owner = "softirq"
        elif any(l == "kernel" for l in layers):
            owner = "kernel-only"
        else:
            owner = "noise"
    return owner, site


# ---------------------------------------------------------------- runs

class Run:
    def __init__(self, path: str):
        self.path = path
        self.meta = {}
        self.container = {}
        for line in open(os.path.join(path, "meta.txt")):
            if "_container=" in line:
                name = line.split("_container=", 1)[1].split()[0]
                self.container[line.rsplit("pid=", 1)[1].strip()] = name
            for tok in line.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    self.meta.setdefault(k, v)
        self.config = self.meta["config"]
        self.body = int(self.meta["body"])
        self.rate = int(float(self.meta["rate"]))
        self.rep = int(self.meta.get("rep", 1))
        self.dpu_cores = float(self.meta.get("dpu_cores") or 0.0)
        self.client_core = self.meta["client_core"]
        self.server_core = self.meta["server_core"]

        self.busy = {}
        with open(os.path.join(path, "core_busy.csv")) as fh:
            for row in csv.DictReader(fh):
                self.busy[row["core"].removeprefix("cpu")] = {
                    k: float(v) for k, v in row.items() if k != "core"
                }
        self.cgroup = {"client": 0.0, "server": 0.0}
        with open(os.path.join(path, "cgroup_cpu.csv")) as fh:
            for row in csv.DictReader(fh):
                self.cgroup[row["role"]] += float(row["cores"])
        self.tid_pid = {}
        for line in open(os.path.join(path, "threads0.csv")):
            f = line.rstrip("\n").split(",")
            if len(f) >= 3:
                self.tid_pid[f[1]] = f[0]
        self.result = {}
        for tok in open(os.path.join(path, "run.txt")).readline().split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                self.result[k] = v
        self.csw = 0.0
        with open(os.path.join(path, "thread_cpu.csv")) as fh:
            for row in csv.DictReader(fh):
                self.csw += float(row["csw_per_s"])

    @property
    def point(self):
        return (self.config, self.body, self.rate)

    def role(self, cpu: str) -> str:
        if cpu == self.client_core:
            return "client"
        if cpu == self.server_core:
            return "server"
        return "other"

    def core_of(self, role: str) -> str:
        return self.client_core if role == "client" else self.server_core


def parse(run: Run):
    """Yield (cpu, comm, tid, frames) per sample, leaf first."""
    comm = cpu = tid = None
    frames: list[tuple[str, str]] = []
    started = False
    with open(os.path.join(run.path, "perf.script"), errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if not line.strip():
                if started:
                    yield cpu, comm, tid, frames
                started, frames = False, []
                continue
            m = SAMPLE_RE.match(line)
            if m and not line[0].isspace():
                comm = m.group("comm").strip()
                tid = m.group("tid") or m.group("pid")
                cpu = str(int(m.group("cpu")))
                frames, started = [], True
                continue
            fm = FRAME_RE.match(line)
            if fm and started and fm.group("addr") != "f" * 16:
                sym = re.sub(r"\+0x[0-9a-f]+$", "", fm.group("sym").strip())
                frames.append((sym, fm.group("dso")))
    if started:
        yield cpu, comm, tid, frames


def fold_name(sym: str, dso: str, layer: str) -> str:
    if layer == "kernel":
        return sym + "_[k]"
    if layer == "unresolved":
        return "[unknown]"
    return sym if sym not in ("", "[unknown]") else f"[{base(dso)}]"


def tally(run: Run, palette: dict[str, str]):
    """Per-role counters for one run; records a colour for every folded frame."""
    owners = defaultdict(Counter)
    sites = defaultdict(Counter)
    cross = defaultdict(Counter)
    syms = defaultdict(Counter)
    folded = defaultdict(Counter)
    total = defaultdict(int)
    for cpu, comm, tid, frames in parse(run):
        role = run.role(cpu)
        if role == "other" or comm in ("perf", "sleep"):
            continue
        owner, site = classify(comm, frames)
        if owner == "idle":
            continue
        owners[role][owner] += 1
        sites[role][site] += 1
        cross[role][(owner, site)] += 1
        total[role] += 1
        if frames:
            sym, dso = frames[0]
            syms[role][(frame_layer(sym, dso), sym, base(dso))] += 1
        names = []
        for sym, dso in reversed(frames):
            layer = frame_layer(sym, dso)
            name = fold_name(sym, dso, layer)
            if names and names[-1] == name:
                continue
            names.append(name)
            palette.setdefault(name, layer if layer in FRAME_COLORS else "noise")
        root = run.container.get(run.tid_pid.get(tid, ""), comm)
        folded[role][";".join([root] + names)] += 1
    return owners, sites, cross, syms, folded, total


# ---------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+", help="run directories from core_profile.sh")
    ap.add_argument("--out", required=True)
    ap.add_argument("--fold", action="store_true", help="also write folded stacks")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    runs = [Run(p) for p in args.runs]
    by_point = defaultdict(list)
    for r in runs:
        by_point[r.point].append(r)
    # Flame graphs are kept for the highest rate of each frame size.
    top_rate = {}
    for config, body, rate in by_point:
        top_rate[(config, body)] = max(rate, top_rate.get((config, body), 0))

    palette: dict[str, str] = {}
    tallies = {r.path: tally(r, palette) for r in runs}
    layer_rows, site_rows, cross_rows, sym_rows, point_rows = [], [], [], [], []

    for point in sorted(by_point):
        reps = sorted(by_point[point], key=lambda r: r.rep)
        config, body, rate = point
        for role in ("client", "server"):
            busy = {r.path: r.cgroup[role] for r in reps}
            chosen = sorted(reps, key=lambda r: busy[r.path])[len(reps) // 2]
            owners, sites, cross, syms, folded, total = tallies[chosen.path]
            n = total[role]
            if n == 0:
                continue
            core = chosen.core_of(role)
            scale = busy[chosen.path] / n
            common = dict(config=config, body=body, rate=rate, role=role, core=core)

            for owner, c in owners[role].most_common():
                layer_rows.append(dict(**common, layer=owner, samples=c,
                                       cores=round(c * scale, 5), share=round(c / n, 5)))
            layer_rows.append(dict(**common, layer="idle", samples=0,
                                   cores=round(max(0.0, 1.0 - busy[chosen.path]), 5),
                                   share=0.0))
            for site, c in sites[role].most_common():
                site_rows.append(dict(**common, site=site, samples=c,
                                      cores=round(c * scale, 5), share=round(c / n, 5)))
            for (owner, site), c in cross[role].most_common():
                cross_rows.append(dict(config=config, body=body, rate=rate, role=role,
                                       layer=owner, site=site, samples=c,
                                       cores=round(c * scale, 5), share=round(c / n, 5)))
            for (layer, sym, dso), c in syms[role].most_common(40):
                sym_rows.append(dict(config=config, body=body, role=role, layer=layer,
                                     dso=dso, symbol=sym, samples=c,
                                     cores=round(c * scale, 5)))
            spread = sorted(busy.values())
            point_rows.append(dict(**common, reps=len(reps),
                                   cores=round(busy[chosen.path], 5),
                                   cores_min=round(spread[0], 5),
                                   cores_max=round(spread[-1], 5),
                                   core_busy=round(chosen.busy[core]["busy"], 5),
                                   softirq=round(chosen.busy[core]["softirq"], 5),
                                   csw_per_s=round(chosen.csw),
                                   dpu_cores=round(statistics.median(
                                       r.dpu_cores for r in reps), 4),
                                   achieved_rps=round(
                                       float(chosen.result.get("mrps", 0)) * 1e6),
                                   p50_us=chosen.result.get("p50", ""),
                                   p99_us=chosen.result.get("p99", ""),
                                   samples=n))

            if args.fold and rate == top_rate[(config, body)]:
                fn = os.path.join(args.out, f"{config}_{body}b_{rate}_{role}.folded")
                with open(fn, "w") as fh:
                    for stack, c in sorted(folded[role].items()):
                        fh.write(f"{stack} {c}\n")

    if args.fold:
        with open(os.path.join(args.out, "palette.map"), "w") as fh:
            for name, layer in sorted(palette.items()):
                r_, g_, b_ = FRAME_COLORS[layer]
                fh.write(f"{name}->rgb({r_},{g_},{b_})\n")

    def write(name, rows, fields):
        with open(os.path.join(args.out, name), "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=fields)
            w.writeheader()
            w.writerows(rows)

    keys = ["config", "body", "rate", "role", "core"]
    write("layers.csv", layer_rows, keys + ["layer", "samples", "cores", "share"])
    write("sites.csv", site_rows, keys + ["site", "samples", "cores", "share"])
    write("cross.csv", cross_rows,
          ["config", "body", "rate", "role", "layer", "site", "samples", "cores", "share"])
    write("symbols.csv", sym_rows,
          ["config", "body", "role", "layer", "dso", "symbol", "samples", "cores"])
    write("points.csv", point_rows, keys + [
        "reps", "cores", "cores_min", "cores_max", "core_busy", "softirq", "csw_per_s",
        "dpu_cores", "achieved_rps", "p50_us", "p99_us", "samples"])

    agg = defaultdict(lambda: defaultdict(float))
    for r in layer_rows:
        if r["layer"] != "idle":
            agg[(r["config"], r["body"], r["rate"])][r["layer"]] += r["cores"]
    print(f"{'config':<24}{'body':>6}{'rate':>9}  "
          + "".join(f"{l:>12}" for l in OWNER_ORDER) + f"{'total':>12}")
    for key in sorted(agg):
        row = agg[key]
        print(f"{key[0]:<24}{key[1]:>6}{key[2]:>9}  "
              + "".join(f"{row.get(l, 0.0):>12.4f}" for l in OWNER_ORDER)
              + f"{sum(row.values()):>12.4f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
