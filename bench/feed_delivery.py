#!/usr/bin/env python3
"""The node agent's delivery loop, and the feeds it derives.

The agent is the DPU's only control peer: every authoritative document the DPU
consumes arrives through this loop and through nothing else. The loop owes
three properties and holds no others. It retries with backoff, because a DPU
that is restarting is an ordinary event rather than a failure of the feed. It
resends whatever the far end no longer holds, which is decided by the digest
the receiver answers rather than by a memory of what was sent — so a restart of
either end converges. And it never withdraws: a source that cannot produce a
document this round leaves the one the DPU holds in place, which is the
fail-static contract every consumer already implements.
"""

from __future__ import annotations

import hashlib
import hmac
import socket
import sys
from pathlib import Path
from typing import Callable

MAGIC = b"DMESHFEED1"
NODE_MAGIC = b"DMESHNODE1"
REPLY_MAX = 128

# A feed source returns the document to deliver, or None when it has nothing
# yet. None is not a withdrawal — it leaves the held document alone.
FeedSource = Callable[[], bytes | None]


class DeliveryError(RuntimeError):
    pass


def deliver_once(address: tuple[str, int], name: str, payload: bytes,
                 timeout: float = 30.0) -> bool:
    """One feed, one connection; True when the bytes were transferred.

    The receiver is offered the digest first and asks for the payload only when
    what it holds differs, so an unchanged feed costs a connection and a header
    rather than a 16 MiB generation. It installs by rename, so a failure
    anywhere before its reply leaves the previously held document in place.
    """
    digest = hashlib.sha256(payload).hexdigest()
    with socket.create_connection(address, timeout=timeout) as connection:
        connection.settimeout(timeout)
        connection.sendall(
            MAGIC + f" {name} {len(payload)} {digest}\n".encode("ascii"))
        decision = connection.recv(REPLY_MAX).decode("ascii", "replace").strip()
        if decision == "have":
            return False
        if decision != "send":
            raise DeliveryError(decision or "no reply")
        connection.sendall(payload)
        reply = connection.recv(REPLY_MAX).decode("ascii", "replace").strip()
    if not reply.startswith("ok "):
        raise DeliveryError(reply or "no reply")
    return True


def read_node_key(address: tuple[str, int], timeout: float = 10.0) -> str:
    """Read the DPU's static handshake public key over the same hop.

    Its private half is generated on the DPU and never leaves it; the public
    half has to reach the controller, and this agent is the only peer that can
    carry it. One read, of one file.
    """
    with socket.create_connection(address, timeout=timeout) as connection:
        connection.settimeout(timeout)
        connection.sendall(NODE_MAGIC + b"\n")
        reply = connection.recv(REPLY_MAX).decode("ascii", "replace").strip()
    if not reply.startswith("key "):
        raise DeliveryError(reply or "no reply")
    return reply[len("key "):]


class Delivery:
    """One feed's delivery state.

    There is deliberately no record here of what was delivered. The receiver
    answers with what it holds, so the decision to transfer is taken against
    the far end's actual state — which is the only version of it that survives
    a reboot on either side.
    """

    def __init__(self, name: str, source: FeedSource) -> None:
        self.name = name
        self.source = source
        self.failures = 0

    def step(self, address: tuple[str, int], timeout: float, log) -> bool:
        try:
            payload = self.source()
        except Exception as exc:                      # a source failure is not a withdrawal
            log(f"{self.name}: source unavailable: {exc}")
            return False
        if payload is None:
            return False
        try:
            transferred = deliver_once(address, self.name, payload, timeout)
        except (OSError, DeliveryError) as exc:
            self.failures += 1
            if self.failures == 1 or self.failures % 16 == 0:
                log(f"{self.name}: delivery failed ({self.failures}): {exc}")
            return False
        if self.failures:
            log(f"{self.name}: delivery recovered after {self.failures} failures")
        self.failures = 0
        return transferred


class DeliveryLoop:
    """Every feed on one cadence, with per-round backoff.

    Losing one publisher stops its own updates and nothing else: a source that
    raises is skipped for the round while the other feeds keep delivering.
    """

    def __init__(self, address: tuple[str, int], deliveries: list[Delivery],
                 interval: float = 2.0, backoff_max: float = 30.0,
                 timeout: float = 30.0, log=None) -> None:
        self.address = address
        self.deliveries = deliveries
        self.interval = interval
        self.backoff_max = backoff_max
        self.timeout = timeout
        self.log = log or (lambda message: print(
            f"workload-attest-agent: {message}", file=sys.stderr, flush=True))
        self.backoff = interval

    def round(self) -> int:
        installed = 0
        for delivery in self.deliveries:
            if delivery.step(self.address, self.timeout, self.log):
                installed += 1
        # Back off only while every feed is failing; one healthy feed keeps the
        # loop at its cadence so a fresh membership generation is not delayed
        # by an unrelated publisher's outage.
        if any(delivery.failures for delivery in self.deliveries):
            self.backoff = min(self.backoff * 2.0, self.backoff_max)
        else:
            self.backoff = self.interval
        return installed


def file_source(path: Path, bound: int) -> FeedSource:
    """A feed another component publishes to a local path. A missing file is
    'nothing yet'; an oversized one is refused here rather than at the door."""

    def source() -> bytes | None:
        try:
            payload = path.read_bytes()
        except FileNotFoundError:
            return None
        if not payload:
            return None
        if len(payload) > bound:
            raise DeliveryError(f"{path} is {len(payload)} bytes, over the {bound}-byte bound")
        return payload

    return source


def parse_generation(document: str) -> dict[str, list[str]]:
    """The signed prefix of a topology generation, by line kind. The signature
    is the controller's and this is not its verifier — the DPU verifies what it
    adopts. Reading it here only decides what to derive from it."""
    records: dict[str, list[str]] = {}
    for line in document.splitlines():
        if line.startswith("#") or not line:
            continue
        kind, separator, value = line.partition("=")
        if not separator:
            continue
        records.setdefault(kind, []).append(value)
        if kind == "signature":
            break
    return records


def service_targets_document(generation: str, services: list[str], version: int,
                             key_id: str, key: bytes) -> str | None:
    """Derive the L7 adapter's Service-target feed from the held generation.

    One source of truth: the generation already names each Service's ClusterIP
    and its ready endpoints, so the publisher reads no Kubernetes object of its
    own. Each endpoint carries both halves the adapter needs: the address the
    Linkerd balancer selects, resolved from the generation's signed Pod IP, and
    the Pod UID that address resolves back to. A recreated Pod carries a new
    UID, so a mapping cannot be inherited.
    """
    records = parse_generation(generation)
    wanted = set(services)
    ports: dict[str, str] = {}
    lines: list[str] = [f"version={version}"]
    for record in records.get("service", []):
        key_field, separator, address = record.partition(",")
        host, colon, port = address.rpartition(":")
        if not separator or not colon or key_field not in wanted:
            continue
        ports[key_field] = port
        lines.append(f"{key_field}={host}:{port}")
    if len(lines) == 1:
        return None                     # no named Service is in this generation yet
    pod_ips: dict[str, str] = {}
    for record in records.get("pod", []):
        fields = record.split(",")
        if len(fields) == 5:
            pod_ips[fields[0]] = fields[4]
    seen: set[tuple[str, str]] = set()
    for record in records.get("endpoint", []):
        key_field, separator, uid = record.partition(",")
        if not separator or key_field not in ports:
            continue
        ip = pod_ips.get(uid)
        if ip is None or (key_field, ip) in seen:
            continue
        seen.add((key_field, ip))
        lines.append(f"endpoint={key_field},{ip}:{ports[key_field]},{uid}")
    body = "".join(line + "\n" for line in lines)
    mac = hmac.new(key, body.encode("ascii"), hashlib.sha256).hexdigest()
    return f"{body}signature={key_id},{mac}\n"
