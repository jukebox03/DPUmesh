#!/usr/bin/env python3
"""Automatic Pod-pair inline E2E on one DPU: fabric address, crypto owner, and
the manager-driven e2e probe. Two nodes run this concurrently; node A (side 0)
opens the stream. Reuses the coexistence fixture's address/neighbor discipline;
changes only test-owned addresses/neighbors and removes them at the end."""
import argparse
import ipaddress
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import supervisor as base
import guard_probe as guard

SF = 'enp3s0f0s0'
RDMA_DEV = 'mlx5_2'
CTRL_PORT = 47950


def gid_index(side):
    expected = '0000:0000:0000:0000:0000:ffff:0a4d:000' + str(side + 1)
    gids = Path(f'/sys/class/infiniband/{RDMA_DEV}/ports/1/gids')
    idx = [p.name for p in gids.iterdir() if p.read_text().strip() == expected
           and (gids.parent / 'gid_attrs/types' / p.name).read_text().strip() == 'RoCE v2']
    if len(idx) != 1:
        raise RuntimeError(f'expected one RoCEv2 GID for {guard.IPS[side]}, found {idx}')
    return idx[0]


def own_address(side):
    addrs = json.loads(base.command(['ip', '-j', 'addr', 'show', 'dev', SF]))[0].get('addr_info', [])
    if any(a['local'] == guard.IPS[side] for a in addrs):
        raise RuntimeError('fixture address already present')
    if json.loads(base.command(['ip', '-j', 'neigh', 'show', 'to', guard.IPS[1 - side], 'dev', SF])):
        raise RuntimeError('fixture neighbor already present')
    dad = subprocess.run(['arping', '-D', '-I', SF, '-c', '3', '-w', '4', guard.IPS[side]],
                         capture_output=True, text=True)
    if dad.returncode == 0:
        pass  # no reply: address is free
    base.command(['ip', 'addr', 'add', guard.IPS[side] + '/30', 'dev', SF, 'label', SF + ':dm'])
    base.command(['ip', 'neigh', 'replace', guard.IPS[1 - side], 'lladdr', guard.MACS[1 - side],
                  'nud', 'permanent', 'dev', SF])


def release_address(side):
    try:
        base.command(['ip', 'neigh', 'del', guard.IPS[1 - side], 'dev', SF])
    except Exception:
        pass
    try:
        base.command(['ip', 'addr', 'del', guard.IPS[side] + '/30', 'dev', SF])
    except Exception:
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--side', type=int, required=True, choices=[0, 1])
    ap.add_argument('--peer', required=True, help='peer IPv6 link-local for the health ping')
    ap.add_argument('--rounds', type=int, default=8)
    ap.add_argument('--msg-bytes', type=int, default=0, help='fixed size -> latency+throughput; 0 -> functional')
    ap.add_argument('--plaintext', action='store_true', help='control: stub adapter, no owner, no encryption')
    ap.add_argument('--seconds', type=int, default=120)
    args = ap.parse_args()
    if not ipaddress.IPv6Address(args.peer).is_link_local:
        ap.error('peer must be link-local')
    root = args.root.resolve()
    if root.parent != Path('/tmp') or not root.name.startswith('dpumesh-coexist-'):
        ap.error('unexpected run directory')
    receipt = root / 'e2e'
    receipt.mkdir(mode=0o700)
    sock = root / 'crypto.sock'
    result = {'side': args.side, 'owner_ready': False, 'e2e_pass': False, 'e2e_done': False,
              'lane_ready': False, 'health_failure': False}
    before = base.snapshot()
    (receipt / 'before.json').write_text(json.dumps(before, indent=2))
    pre = subprocess.run(base.ping(args.peer, 4), capture_output=True, text=True, timeout=15)
    (receipt / 'ping-before.txt').write_text(pre.stdout + pre.stderr)
    if pre.returncode or ' 0% packet loss' not in pre.stdout:
        raise RuntimeError('baseline ping failed; nothing started')
    owner = probe = health = None
    address_up = False
    try:
        own_address(args.side)
        address_up = True
        gidx = gid_index(args.side)
        with (receipt / 'owner.log').open('x') as olog, (receipt / 'probe.log').open('x') as plog, \
             (receipt / 'ping-during.txt').open('x') as hlog:
            health = subprocess.Popen(base.ping(args.peer, args.seconds * 5), stdout=hlog,
                                      stderr=subprocess.STDOUT, start_new_session=True)
            if not args.plaintext:
                owner = subprocess.Popen(['taskset', '-c', '14', str(root / 'doca/build/dpumesh_crypto_owner'),
                                          '--socket', str(sock), '--pci', '0000:03:00.0',
                                          '--sf-iface', 'en3f0pf0sf0', '--sa-limit', '64'],
                                         stdout=olog, stderr=subprocess.STDOUT, start_new_session=True)
                deadline = time.monotonic() + 30
                while owner.poll() is None and time.monotonic() < deadline:
                    if 'ready:' in (receipt / 'owner.log').read_text(errors='replace'):
                        result['owner_ready'] = True
                        break
                    time.sleep(0.3)
                if not result['owner_ready']:
                    raise RuntimeError('owner not ready')
            else:
                result['owner_ready'] = None
            env = dict(os.environ)
            if args.plaintext:
                env['E2E_PLAINTEXT'] = '1'
            probe = subprocess.Popen(['taskset', '-c', '15', str(root / 'peer_e2e_probe'), str(args.side),
                                      RDMA_DEV, gidx, guard.IPS[args.side], guard.IPS[1 - args.side],
                                      str(CTRL_PORT), str(sock), str(args.rounds)]
                                     + ([str(args.msg_bytes)] if args.msg_bytes else []),
                                     stdout=plog, stderr=subprocess.STDOUT, start_new_session=True, env=env)
            deadline = time.monotonic() + args.seconds
            while probe.poll() is None and time.monotonic() < deadline:
                text = (receipt / 'probe.log').read_text(errors='replace')
                result['lane_ready'] |= 'LANE_READY' in text
                result['e2e_pass'] |= 'E2E_PASS' in text
                if 'E2E_DONE' in text:
                    break
                if 'Unreachable' in (receipt / 'ping-during.txt').read_text():
                    result['health_failure'] = True
                    break
                time.sleep(0.3)
            result['probe_rc'] = probe.poll()
            text = (receipt / 'probe.log').read_text(errors='replace')
            result['lane_ready'] |= 'LANE_READY' in text
            result['e2e_pass'] |= 'E2E_PASS' in text
            result['e2e_done'] |= 'E2E_DONE' in text
            import re as _re
            for line in text.splitlines():
                if line.startswith('E2E_LAT') or line.startswith('E2E_BW'):
                    result.setdefault('perf', []).append(line.strip())
            result['plaintext'] = args.plaintext
    finally:
        for p in (probe, owner, health):
            if p and p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGINT if p is health else signal.SIGTERM)
                    p.wait(timeout=15)
                except Exception:
                    try:
                        os.killpg(p.pid, signal.SIGKILL)
                    except Exception:
                        pass
        if address_up:
            release_address(args.side)
        after = base.snapshot()
        (receipt / 'after.json').write_text(json.dumps(after, indent=2))
        result['configuration_equal'] = before == after
        result['changed_sections'] = [k for k in before if before[k] != after[k]]
        post = subprocess.run(base.ping(args.peer, 4), capture_output=True, text=True, timeout=15)
        (receipt / 'ping-after.txt').write_text(post.stdout + post.stderr)
        result['post_health'] = post.returncode == 0 and ' 0% packet loss' in post.stdout
        (receipt / 'result.json').write_text(json.dumps(result, indent=2))
        print(json.dumps(result), flush=True)
    ok = result['e2e_done'] and result['configuration_equal'] and result['post_health'] and not result['health_failure']
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
