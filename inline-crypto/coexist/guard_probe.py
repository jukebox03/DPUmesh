#!/usr/bin/env python3
"""Bounded OVS plaintext guard test, with DOCA alive and after SIGKILL.

No QP or IP address is created. Only synthetic IPv4/UDP frames are injected
through the existing SF. Run under an independent systemd lifetime limit.
The expiring guard is for this synthetic test ONLY, never for a live RDMA QP.
"""
import argparse
import json
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import time

import supervisor as base

MACS = ['02:7f:41:72:60:fc', '02:94:0d:03:f5:fd']
IPS = ['10.77.0.1', '10.77.0.2']
PEERS = ['fe80::94:dff:fe03:f5fd', 'fe80::7f:41ff:fe72:60fc']


def frame(side):
    # Deliberately not a valid RoCE BTH: no remote QP is addressed.
    payload = b'\xff\x0f\x00\x00\x00\xff\xff\xff\x00\x00\x00\x00' + b'DPUMESH-COEX-GUARD\0' + b'\0' * 34
    udp = struct.pack('!HHHH', 49191, 4791, 8 + len(payload), 0) + payload
    src, dst = (socket.inet_aton(IPS[i]) for i in [side, 1 - side])
    header = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(udp), 0, 0x4000,
                         64, 17, 0, src, dst)
    total = sum(struct.unpack('!10H', header))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    header = header[:10] + struct.pack('!H', (~total) & 0xffff) + header[12:]
    return (bytes.fromhex(MACS[1 - side].replace(':', ''))
            + bytes.fromhex(MACS[side].replace(':', '')) + b'\x08\x00' + header + udp)


def inject(side):
    packet = frame(side)
    with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0800)) as sock:
        sock.bind(('enp3s0f0s0', 0))
        for _ in range(20):
            if sock.send(packet) != len(packet):
                raise RuntimeError('Short frame write')
            time.sleep(0.1)


def rule(cookie, port, side):
    return (f'cookie={cookie},table=0,priority=42000,hard_timeout=60,in_port={port},udp,'
            f'dl_src={MACS[side]},dl_dst={MACS[1-side]},'
            f'nw_src={IPS[side]},nw_dst={IPS[1-side]},tp_dst=4791,actions=drop')


def guard_packets(flows, cookie, source):
    selected = [line for line in flows.splitlines()
                if f'cookie={cookie},' in line and f'nw_src={source},' in line]
    if len(selected) != 1:
        raise RuntimeError('Expected exactly one owned directional guard')
    return int(re.search(r'n_packets=(\d+)', selected[0]).group(1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--side', type=int, choices=[0, 1], required=True)
    parser.add_argument('--cookie', required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    if root.parent != Path('/tmp') or not root.name.startswith('dpumesh-coexist-'):
        parser.error('Unexpected run directory')
    if not re.fullmatch(r'0xd04a[0-9a-f]{12}', args.cookie):
        parser.error('Expected a unique 64-bit test cookie in the d04a namespace')
    receipt = root / 'guard-receipt'
    receipt.mkdir(mode=0o700)
    before = base.snapshot()
    (receipt / 'before.json').write_text(json.dumps(before, indent=2))
    flows = base.command(['ovs-ofctl', 'dump-flows', 'ovsbr1'])
    if args.cookie in flows or any(ip in flows for ip in IPS):
        raise RuntimeError('Existing cookie or test address flow; no changes made')
    # The inspected baseline has only NORMAL. Stop if concurrent rule ownership
    # changes; do not guess about another user's policy priorities.
    if any('priority=0 actions=NORMAL' not in line for line in flows.splitlines() if 'cookie=' in line):
        raise RuntimeError('OVS baseline changed; guard priority needs review')
    for iface in before['interfaces'].values():
        if any(addr[1] in IPS for addr in iface['addresses']):
            raise RuntimeError('Synthetic addresses already assigned')
    pre = subprocess.run(base.ping(PEERS[args.side], 5), capture_output=True, text=True, timeout=10)
    (receipt / 'ping-before.txt').write_text(pre.stdout + pre.stderr)
    if pre.returncode or ' 0% packet loss' not in pre.stdout:
        raise RuntimeError('Baseline connectivity failed')
    ports = [int(base.command(['ovs-vsctl', 'get', 'Interface', name, 'ofport']))
             for name in ['en3f0pf0sf0', 'p0']]
    if min(ports) < 1 or len(set(ports)) != 2:
        raise RuntimeError('Unexpected OpenFlow ports')
    child = health = None
    owned = False
    result = {'stages': {}, 'error': None, 'cleanup_error': None}
    try:
        with (receipt / 'ping-during.txt').open('x') as health_log, (receipt / 'probe.log').open('x') as log:
            health = subprocess.Popen(base.ping(PEERS[args.side], 200), stdout=health_log,
                                      stderr=subprocess.STDOUT, start_new_session=True)
            # Mark ownership before first add so partial installations are removed.
            owned = True
            for port, direction in zip(ports, [args.side, 1 - args.side]):
                base.command(['ovs-ofctl', 'add-flow', 'ovsbr1', rule(args.cookie, port, direction)])
            installed = base.command(['ovs-ofctl', 'dump-flows', 'ovsbr1'])
            for source in IPS:
                guard_packets(installed, args.cookie, source)
            (receipt / 'installed-flows.txt').write_text(installed)
            (receipt / 'resources.json').write_text(json.dumps({
                'cookie': args.cookie, 'bridge': 'ovsbr1', 'ports': ports,
                'source_ips': IPS, 'macs': MACS, 'guard_expires_seconds': 60,
                'qp_created': False, 'address_added': False}, indent=2))
            for stage in ['before-doca', 'during-doca', 'after-crash']:
                if stage == 'during-doca':
                    child = subprocess.Popen([str(root / 'build/coexist-probe'), '--run',
                        '0000:03:00.0', *MACS, '30', '--with-sf0'], stdout=log,
                        stderr=subprocess.STDOUT, start_new_session=True)
                    (receipt / 'process.json').write_text(json.dumps({'pid': child.pid,
                        'start_ticks': Path(f'/proc/{child.pid}/stat').read_text().split()[21]}))
                    deadline = time.monotonic() + 12
                    while 'READY handoff' not in (receipt / 'probe.log').read_text():
                        if child.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError('DOCA failed to become ready')
                        time.sleep(0.1)
                elif stage == 'after-crash':
                    if child.poll() is not None:
                        raise RuntimeError('Probe exited before crash injection')
                    import os
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait(timeout=3)
                    result['crash_rc'] = child.returncode
                start = base.command(['ovs-ofctl', 'dump-flows', 'ovsbr1'])
                old = guard_packets(start, args.cookie, IPS[args.side])
                inject(args.side)
                time.sleep(1.2)  # OVS polls offloaded counters asynchronously.
                current = base.command(['ovs-ofctl', 'dump-flows', 'ovsbr1'])
                (receipt / (stage + '-flows.txt')).write_text(current)
                base.diagnostics(receipt, stage)
                count = guard_packets(current, args.cookie, IPS[args.side]) - old
                result['stages'][stage] = {'injected': 20, 'guard_count_delta': count}
                if count != 20:
                    raise RuntimeError(f'{stage}: guard observed {count} of 20 frames')
                text = (receipt / 'ping-during.txt').read_text()
                if 'Unreachable' in text or health.poll() is not None:
                    raise RuntimeError('Existing connectivity failed')
    except Exception as exc:
        result['error'] = str(exc)
    finally:
        base.stop_group(child)
        if health is not None and health.poll() is None:
            import os
            os.killpg(health.pid, signal.SIGINT)
            health.wait(timeout=3)
        base.stop_group(health)
        if owned:
            try:
                base.command(['ovs-ofctl', 'del-flows', 'ovsbr1', f'cookie={args.cookie}/-1'])
                if args.cookie in base.command(['ovs-ofctl', 'dump-flows', 'ovsbr1']):
                    raise RuntimeError('Owned cookie remains after deletion')
            except Exception as exc:
                result['cleanup_error'] = str(exc)
        after = base.snapshot()
        (receipt / 'after.json').write_text(json.dumps(after, indent=2))
        base.diagnostics(receipt, 'after')
        result['configuration_equal'] = before == after
        result['changed_sections'] = [key for key in before if before[key] != after[key]]
        post = subprocess.run(base.ping(PEERS[args.side], 5), capture_output=True, text=True, timeout=10)
        (receipt / 'ping-after.txt').write_text(post.stdout + post.stderr)
        result['post_health'] = post.returncode == 0 and ' 0% packet loss' in post.stdout
        result['during_zero_loss'] = ' 0% packet loss' in (receipt / 'ping-during.txt').read_text()
        (receipt / 'result.json').write_text(json.dumps(result, indent=2))
        print(json.dumps(result), flush=True)
    raise SystemExit(0 if (not result['error'] and not result['cleanup_error']
        and result['configuration_equal'] and result['post_health'] and result['during_zero_loss']) else 1)


if __name__ == '__main__':
    main()
