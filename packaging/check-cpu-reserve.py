#!/usr/bin/env python3
"""Check that dpumeshd's AllowedCPUs lie inside kubelet's reservedSystemCPUs.

dpumeshd and the brokers it starts run outside Pods, on the unit's AllowedCPUs.
kubelet keeps Pods off exactly its reservedSystemCPUs, which
configure-kubelet-reserve.sh sets, so the two must agree.

    check-cpu-reserve.py DPUMESHD_UNIT KUBELET_CONFIG
"""
import re
import sys


def cpu_set(text):
    cpus = set()
    for part in text.strip().strip('"').split(','):
        part = part.strip()
        if not part:
            continue
        low, _, high = part.partition('-')
        cpus.update(range(int(low), int(high or low) + 1))
    return cpus


def main(argv):
    if len(argv) != 3:
        sys.exit('usage: check-cpu-reserve.py DPUMESHD_UNIT KUBELET_CONFIG')
    with open(argv[1]) as unit:
        allowed = re.search(r'^AllowedCPUs=(.+)$', unit.read(), re.M)
    if allowed is None:
        return 0
    try:
        with open(argv[2]) as config:
            reserved = re.search(r'^reservedSystemCPUs:\s*(.+)$', config.read(), re.M)
    except FileNotFoundError:
        sys.exit(f'{argv[2]} not found: is this a kubelet node?')
    want = allowed.group(1).strip()
    have = reserved.group(1).strip().strip('"') if reserved else '(unset)'
    if reserved is None or not cpu_set(want) <= cpu_set(have):
        sys.exit(f'dpumeshd AllowedCPUs={want} is not inside kubelet reservedSystemCPUs={have}; '
                 f'run packaging/configure-kubelet-reserve.sh {argv[2]} {want} <memory> first')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
