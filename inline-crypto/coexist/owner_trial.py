#!/usr/bin/env python3
"""Bounded standalone trial of dpumesh_crypto_owner on one DPU.

Starts the owner on the existing PF and SF representor, waits for its graph,
drives it with the fixture probe (SA and rule install, rekey, block, remove;
no QP, no traffic), stops it, and compares the OVS/interface configuration
and the existing IPv6 fabric ping before and after. Run as root under the
coexistence receipts layout. Nothing here changes OVS, addresses or drivers.
"""
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
import supervisor as base  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--peer', required=True, help='existing IPv6 link-local of the other DPU SF')
    ap.add_argument('--pci', default='0000:03:00.0')
    ap.add_argument('--sf-iface', default='en3f0pf0sf0')
    ap.add_argument('--local-ip', default='10.77.0.1')
    ap.add_argument('--peer-ip', default='10.77.0.2')
    ap.add_argument('--endpoint', default='0', choices=['0', '1'])
    ap.add_argument('--seconds', type=int, default=90, choices=range(20, 301))
    ap.add_argument('--cpu', default='15')
    ap.add_argument('--tag', default='owner-trial', help='receipt directory name, fresh per run')
    ap.add_argument('--step-delay-ms', default='0')
    ap.add_argument('--plans', default='r1,t1,r2,t2,o1,b,a', help='semicolon-separated probe step plans')
    ap.add_argument('--stop-grace', type=int, default=30, help='seconds allowed for a clean stop before SIGKILL')
    ap.add_argument('--random-spi', action='store_true', help='derive unique SPIs per run from the association bytes')
    args = ap.parse_args()
    if not ipaddress.IPv6Address(args.peer).is_link_local:
        ap.error('peer must be an IPv6 link-local address')
    root = args.root.resolve()
    if root.parent != Path('/tmp') or not root.name.startswith('dpumesh-coexist-'):
        ap.error('unexpected run directory')
    receipt = root / args.tag
    receipt.mkdir(mode=0o700)
    owner_bin = root / 'doca/build/dpumesh_crypto_owner'
    probe_bin = root / 'owner_probe'
    sock = root / 'crypto.sock'
    before = base.snapshot()
    (receipt / 'before.json').write_text(json.dumps(before, indent=2))
    base.diagnostics(receipt, 'before')
    pre = subprocess.run(base.ping(args.peer, 5), capture_output=True, text=True, timeout=15)
    (receipt / 'ping-before.txt').write_text(pre.stdout + pre.stderr)
    if pre.returncode or ' 0% packet loss' not in pre.stdout:
        raise RuntimeError('baseline fabric ping failed; owner not started')
    result = {'owner_started': False, 'ready': False, 'probe_rc': None, 'owner_rc': None,
              'health_failure': False, 'killed': False}
    owner = health = None
    try:
        with (receipt / 'owner.log').open('x') as log, (receipt / 'ping-during.txt').open('x') as hlog:
            health = subprocess.Popen(base.ping(args.peer, 400), stdout=hlog, stderr=subprocess.STDOUT,
                                      start_new_session=True)
            env = dict(os.environ)
            if args.random_spi:
                os.environ['PROBE_RANDOM_SPI'] = '1'
            owner = subprocess.Popen(['taskset', '-c', args.cpu, str(owner_bin), '--socket', str(sock),
                                      '--pci', args.pci, '--sf-iface', args.sf_iface, '--sa-limit', '64'],
                                     stdout=log, stderr=subprocess.STDOUT, start_new_session=True, env=env)
            result['owner_started'] = True
            (receipt / 'process.json').write_text(json.dumps({'pid': owner.pid}))
            deadline = time.monotonic() + args.seconds
            while owner.poll() is None and time.monotonic() < deadline:
                text = (receipt / 'owner.log').read_text(errors='replace')
                if 'ready:' in text:
                    result['ready'] = True
                    break
                time.sleep(0.2)
            if result['ready']:
                base.diagnostics(receipt, 'during')
                result['probe_rc'] = 0
                result['probe_steps'] = []
                for n, plan in enumerate(args.plans.split(';')):
                    probe = subprocess.run([str(probe_bin), str(sock), args.local_ip, args.peer_ip, args.endpoint,
                                            args.step_delay_ms, plan],
                                           capture_output=True, text=True, timeout=120)
                    with (receipt / 'probe.log').open('a') as f:
                        f.write(f'== plan {plan}\n' + probe.stdout + probe.stderr)
                    result['probe_rc'] |= probe.returncode
                    result['probe_steps'] += [f'[{plan}] ' + line for line in probe.stdout.splitlines()
                                              if line.startswith('STEP')]
                base.diagnostics(receipt, 'after-probe')
            htext = (receipt / 'ping-during.txt').read_text()
            result['health_failure'] = 'Unreachable' in htext or 'Network is unreachable' in htext
            if owner.poll() is None:
                os.killpg(owner.pid, signal.SIGTERM)
                try:
                    owner.wait(timeout=args.stop_grace)
                except subprocess.TimeoutExpired:
                    os.killpg(owner.pid, signal.SIGKILL)
                    owner.wait(timeout=5)
                    result['killed'] = True
            result['owner_rc'] = owner.returncode
            if health.poll() is None:
                os.killpg(health.pid, signal.SIGINT)
                health.wait(timeout=5)
    finally:
        base.stop_group(owner)
        base.stop_group(health)
        after = base.snapshot()
        (receipt / 'after.json').write_text(json.dumps(after, indent=2))
        result['configuration_equal'] = before == after
        result['changed_sections'] = [k for k in before if before[k] != after[k]]
        base.diagnostics(receipt, 'after')
        post = subprocess.run(base.ping(args.peer, 5), capture_output=True, text=True, timeout=15)
        (receipt / 'ping-after.txt').write_text(post.stdout + post.stderr)
        result['post_health'] = post.returncode == 0 and ' 0% packet loss' in post.stdout
        htext = (receipt / 'ping-during.txt').read_text() if (receipt / 'ping-during.txt').exists() else ''
        result['during_zero_loss'] = ' 0% packet loss' in htext
        text = (receipt / 'owner.log').read_text(errors='replace') if (receipt / 'owner.log').exists() else ''
        result['shutdown_logged'] = 'shutting down' in text
        result['socket_removed'] = not sock.exists()
        result['passed'] = bool(result['ready'] and result['probe_rc'] == 0 and result['owner_rc'] == 0
                                and not result['killed'] and result['configuration_equal']
                                and result['post_health'] and result['during_zero_loss']
                                and result['socket_removed'])
        (receipt / 'result.json').write_text(json.dumps(result, indent=2))
        print(json.dumps(result), flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
