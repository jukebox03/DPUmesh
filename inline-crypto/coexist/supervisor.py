#!/usr/bin/env python3
"""One bounded handoff test; observes OVS but never repairs or rewrites it.

Run as a transient systemd service with an independent RuntimeMaxSec and memory
limit. All child processes are bounded; no shell or user-supplied command runs.
"""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

DEVICES = ['p0', 'p1', 'pf0hpf', 'pf1hpf', 'en3f0pf0sf0', 'en3f1pf1sf0',
           'enp3s0f0s0', 'enp3s0f1s0', 'tmfifo_net0', 'oob_net0']

def command(argv):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=8)
    if result.returncode:
        raise RuntimeError(f'{argv[0]} failed: {result.stderr.strip()}')
    return result.stdout.strip()

def normalized_flows(text):
    lines = []
    for line in text.splitlines():
        if 'cookie=' not in line:
            continue
        line = re.sub(r'\b(?:duration|n_packets|n_bytes|idle_age|hard_age)=[^, ]+,?\s*', '', line)
        lines.append(line.strip())
    return sorted(lines)

def ovs_rows(table, columns):
    data = json.loads(command(['ovs-vsctl', '--timeout=5', '--format=json',
                               '--columns=' + columns, 'list', table]))
    return sorted(data['data'], key=lambda row: str(row[0]))

def snapshot():
    links = json.loads(command(['ip', '-j', 'addr']))
    interfaces = {}
    for dev in links:
        if dev['ifname'] not in DEVICES:
            continue
        interfaces[dev['ifname']] = {
            key: dev.get(key) for key in ['address', 'mtu', 'master']}
        interfaces[dev['ifname']]['flags'] = sorted(dev['flags'])
        interfaces[dev['ifname']]['addresses'] = sorted(
            (a['family'], a['local'], a['prefixlen'], a.get('scope'))
            for a in dev['addr_info'])
    flows = {bridge: normalized_flows(command(['ovs-ofctl', 'dump-flows', bridge]))
             for bridge in command(['ovs-vsctl', '--timeout=5', 'list-br']).splitlines()}
    return {
        'boot': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
        'interfaces': interfaces,
        'bridges': ovs_rows('Bridge', 'name,ports,datapath_type,fail_mode,other_config'),
        'ports': ovs_rows('Port', 'name,interfaces,tag,trunks,vlan_mode,other_config'),
        'ovs_interfaces': ovs_rows('Interface', 'name,type,options,ofport,ofport_request,mtu_request'),
        'ovs_config': command(['ovs-vsctl', '--timeout=5', 'get', 'Open_vSwitch', '.', 'other_config']),
        'flows': flows,
        'steering': command(['devlink', 'dev', 'param', 'show', 'pci/0000:03:00.0',
                             'name', 'flow_steering_mode']),
        'eswitch': command(['devlink', 'dev', 'eswitch', 'show', 'pci/0000:03:00.0']),
        'huge2m': Path('/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages').read_text().strip(),
        'services': {unit: command(['systemctl', 'show', unit, '-p', 'MainPID',
                                    '-p', 'ActiveState', '-p', 'ExecMainStartTimestampMonotonic'])
                     for unit in ['ovs-vswitchd', 'ovsdb-server']},
    }

def diagnostics(root, tag):
    commands = {
        'rdma': ['rdma', 'resource', 'show'],
        'tc-p0': ['tc', '-s', 'filter', 'show', 'dev', 'p0', 'ingress'],
        'tc-sf': ['tc', '-s', 'filter', 'show', 'dev', 'en3f0pf0sf0', 'ingress'],
        'p0-counters': ['ethtool', '-S', 'p0'],
    }
    for name, argv in commands.items():
        try:
            (root / f'{tag}-{name}.txt').write_text(command(argv))
        except (RuntimeError, subprocess.TimeoutExpired) as exc:
            (root / f'{tag}-{name}.txt').write_text('ERROR ' + str(exc))

def ping(peer, count):
    return ['ping', '-6', '-n', '-I', 'enp3s0f0s0', '-c', str(count),
            '-i', '0.2', '-W', '1', peer]

def stop_group(proc):
    if proc is None or proc.poll() is not None:
        return
    os.killpg(proc.pid, signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait(timeout=3)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--peer', required=True)
    parser.add_argument('--build-dir', default='build')
    parser.add_argument('--seconds', type=int, default=12, choices=range(1, 31))
    parser.add_argument('--crash-after', type=int, default=0, choices=range(0, 11))
    parser.add_argument('--with-sf0', action='store_true')
    args = parser.parse_args()
    if not ipaddress.IPv6Address(args.peer).is_link_local:
        parser.error('Only an existing IPv6 link-local peer is accepted')
    root = args.root.resolve()
    if Path(args.build_dir).name != args.build_dir or args.build_dir in ('.', '..'):
        parser.error('Build directory must be a direct child of the run directory')
    if root.parent != Path('/tmp') or not root.name.startswith('dpumesh-coexist-'):
        parser.error('Unexpected run directory')
    # Each execution has its own immutable receipt, including failed executions.
    receipt = root / 'receipt'
    receipt.mkdir(mode=0o700)
    before = snapshot()
    (receipt / 'before.json').write_text(json.dumps(before, indent=2))
    diagnostics(receipt, 'before')
    pre = subprocess.run(ping(args.peer, 5), capture_output=True, text=True, timeout=10)
    (receipt / 'ping-before.txt').write_text(pre.stdout + pre.stderr)
    if pre.returncode or ' 0% packet loss' not in pre.stdout:
        raise RuntimeError('Baseline fabric health failed; probe was not started')
    if before['interfaces']['p0']['mtu'] != 1500:
        raise RuntimeError('Probe requires existing MTU 1500; no change attempted')
    child = health = None
    result = {'probe_started': False, 'probe_rc': None, 'deadline_exceeded': False,
              'crash_injected': False}
    try:
        with (receipt / 'ping-during.txt').open('x') as health_log, (receipt / 'probe.log').open('x') as log:
            health = subprocess.Popen(ping(args.peer, 200), stdout=health_log, stderr=subprocess.STDOUT,
                                      start_new_session=True)
            child = subprocess.Popen([str(root / args.build_dir / 'coexist-probe'), '--run', '0000:03:00.0',
                                      '02:7f:41:72:60:fc', '02:94:0d:03:f5:fd', str(args.seconds)]
                                     + (['--with-sf0'] if args.with_sf0 else []),
                                     stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            result['probe_started'] = True
            (receipt / 'process.json').write_text(json.dumps({
                'pid': child.pid, 'start_ticks': Path(f'/proc/{child.pid}/stat').read_text().split()[21]}))
            deadline = time.monotonic() + args.seconds + 15
            health_failure = False
            ready_at = None
            while child.poll() is None and time.monotonic() < deadline:
                probe_text = (receipt / 'probe.log').read_text()
                if ready_at is None and 'READY handoff' in probe_text:
                    ready_at = time.monotonic()
                    diagnostics(receipt, 'during')
                if (args.crash_after and ready_at is not None
                        and time.monotonic() - ready_at >= args.crash_after):
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait(timeout=3)
                    result['crash_injected'] = True
                    break
                # Stop on an explicit ping error or no replies after startup grace.
                text = (receipt / 'ping-during.txt').read_text()
                if 'Unreachable' in text or 'Network is unreachable' in text or health.poll() is not None:
                    health_failure = True
                    break
                replies = re.findall(r'icmp_seq=(\d+).*time=', text)
                elapsed = args.seconds + 15 - (deadline - time.monotonic())
                if elapsed > 3 and (not replies or elapsed - int(replies[-1]) * 0.2 > 2):
                    health_failure = True
                    break
                time.sleep(0.2)
            result['health_failure'] = health_failure
            result['deadline_exceeded'] = child.poll() is None and not health_failure
            stop_group(child)
            result['probe_rc'] = child.returncode
            # SIGINT makes ping write its summary, unlike SIGTERM.
            if health.poll() is None:
                os.killpg(health.pid, signal.SIGINT)
                health.wait(timeout=3)
    finally:
        stop_group(child)
        stop_group(health)
        after = snapshot()
        (receipt / 'after.json').write_text(json.dumps(after, indent=2))
        result['configuration_equal'] = before == after
        result['changed_sections'] = [k for k in before if before[k] != after[k]]
        diagnostics(receipt, 'after')
        post = subprocess.run(ping(args.peer, 5), capture_output=True, text=True, timeout=10)
        (receipt / 'ping-after.txt').write_text(post.stdout + post.stderr)
        result['post_health'] = post.returncode == 0 and ' 0% packet loss' in post.stdout
        log_text = (receipt / 'probe.log').read_text() if (receipt / 'probe.log').exists() else ''
        result['ready'] = 'READY handoff' in log_text
        result['counter_hit'] = any(int(x) > 0 for x in re.findall(r'COUNTER packets=(\d+)', log_text))
        result['cleanup_reported'] = 'cleanup_errors=0' in log_text
        health_text = (receipt / 'ping-during.txt').read_text() if (receipt / 'ping-during.txt').exists() else ''
        result['during_zero_loss'] = ' 0% packet loss' in health_text
        result['direction_packets'] = {
            direction: max([int(x) for x in re.findall('DIRECTION ' + direction + r' packets=(\d+)', log_text)] or [0])
            for direction in ['a_to_b', 'b_to_a']}
        result['bidirectional_handoff'] = all(result['direction_packets'].values())
        (receipt / 'result.json').write_text(json.dumps(result, indent=2))
        print(json.dumps(result), flush=True)
    expected_exit = ((result['probe_rc'] == -signal.SIGKILL and result['crash_injected'])
                     if args.crash_after else (result['probe_rc'] == 0 and result['cleanup_reported']))
    passed = (expected_exit and result['ready'] and result['counter_hit']
              and (not args.with_sf0 or result['bidirectional_handoff'])
              and result['configuration_equal'] and result['post_health']
              and result['during_zero_loss']
              and not result['health_failure'] and not result['deadline_exceeded'])
    raise SystemExit(0 if passed else 1)

if __name__ == '__main__':
    main()
