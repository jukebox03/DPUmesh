#!/usr/bin/env python3
"""Root-owned DPUmesh workload attestation agent.

The request deliberately contains only a DPU nonce and requested compact
Service id. The agent obtains the caller PID from SO_PEERCRED, resolves its
Kubernetes Pod UID from the host cgroup, reads authoritative Pod/Service
objects, and signs the resulting immutable claims. It does not accept workload
names, labels, namespace, ServiceAccount, or node name from the caller.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
import re
import secrets
import signal
import socket
import ssl
import stat
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


GRANT_VERSION = 1
MSG_WORKLOAD_GRANT = 12
NONCE_SIZE = 32
MAX_TTL = 300
REQUEST = struct.Struct("<8sB3xi32s")
GRANT = struct.Struct(
    "<BBBBiqq16s32s64s32s64s64s254s254s254s32s"
)
assert REQUEST.size == 48
assert GRANT.size == 1090

POD_UID_RE = re.compile(r"(?:^|[-/_.])pod([0-9a-fA-F][0-9a-fA-F_-]{31,63})(?:[./]|$)")
KEY_ID_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]{0,30}[A-Za-z0-9]")


class AttestationError(RuntimeError):
    pass


def fixed_text(value: str, size: int, field: str) -> bytes:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as exc:
        raise AttestationError(f"{field} is not ASCII") from exc
    if not encoded or len(encoded) >= size or b"\0" in encoded:
        raise AttestationError(f"{field} does not fit canonical field size {size}")
    return encoded + bytes(size - len(encoded))


def load_key(path: Path) -> bytes:
    st = path.lstat()
    if (
        not stat.S_ISREG(st.st_mode)
        or st.st_uid != os.geteuid()
        or st.st_mode & 0o077
        or not st.st_mode & stat.S_IRUSR
        or st.st_mode & (stat.S_IXUSR | stat.S_ISUID | stat.S_ISGID | stat.S_ISVTX)
    ):
        raise AttestationError(
            f"{path} must be a regular file owned by uid {os.geteuid()} with mode 0600/0400"
        )
    data = path.read_bytes()
    if len(data) == 32:
        key = data
    else:
        try:
            key = bytes.fromhex(data.decode("ascii").strip())
        except (UnicodeDecodeError, ValueError) as exc:
            raise AttestationError(f"{path} is not a raw/hex v1 key") from exc
    if len(key) != 32 or not any(key):
        raise AttestationError(f"{path} must contain a nonzero 32-byte key")
    return key


def load_active_key(directory: Path) -> tuple[str, bytes]:
    """Load the active key on every request so rotation needs no restart."""
    st = directory.stat()
    if not stat.S_ISDIR(st.st_mode) or st.st_uid != os.geteuid() or st.st_mode & 0o077:
        raise AttestationError(
            f"{directory} must be a directory owned by uid {os.geteuid()} with mode 0700"
        )
    active = directory / "active"
    active_st = active.lstat()
    if (
        not stat.S_ISREG(active_st.st_mode)
        or active_st.st_uid != os.geteuid()
        or active_st.st_mode & 0o077
    ):
        raise AttestationError(f"{active} must be a root-owned 0600/0400 regular file")
    try:
        key_id = active.read_text(encoding="ascii").strip()
    except (OSError, UnicodeDecodeError) as exc:
        raise AttestationError(f"cannot read active key id from {active}") from exc
    if KEY_ID_RE.fullmatch(key_id) is None:
        raise AttestationError(f"invalid active key id in {active}")
    return key_id, load_key(directory / f"{key_id}.key")


def read_registry(path: Path) -> dict[int, str]:
    services: dict[int, str] = {}
    for line_no, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) != 3:
            raise AttestationError(f"{path}:{line_no}: expected address name service-id")
        try:
            service_id = int(fields[2], 10)
        except ValueError as exc:
            raise AttestationError(f"{path}:{line_no}: invalid service id") from exc
        if service_id < 0 or service_id >= 128 or service_id in services:
            raise AttestationError(f"{path}:{line_no}: duplicate/out-of-range service id")
        services[service_id] = fields[1]
    return services


def pod_uid_from_cgroup(cgroup: str) -> str:
    match = POD_UID_RE.search(cgroup)
    if match is None:
        raise AttestationError("peer is not in a Kubernetes Pod cgroup")
    return match.group(1).replace("_", "-").lower()


def pod_uid_for_pid(pid: int) -> str:
    try:
        cgroup = Path(f"/proc/{pid}/cgroup").read_text(encoding="ascii")
    except (OSError, UnicodeDecodeError) as exc:
        raise AttestationError(f"cannot read peer cgroup for pid {pid}") from exc
    try:
        return pod_uid_from_cgroup(cgroup)
    except AttestationError as exc:
        raise AttestationError(f"pid {pid} is not in a Kubernetes Pod cgroup") from exc


class KubernetesAPI:
    """Minimal read-only, in-cluster Kubernetes client.

    The projected ServiceAccount token is re-read for every request, which
    preserves kubelet token rotation without restarting the node agent.
    """

    def __init__(
        self, server: str, token_file: Path, ca_file: Path, namespace: str
    ) -> None:
        self.server = server.rstrip("/")
        self.token_file = token_file
        self.namespace = namespace
        self.context = ssl.create_default_context(cafile=str(ca_file))

    def _items(self, resource: str, query: dict[str, str] | None = None) -> list[dict[str, Any]]:
        suffix = urllib.parse.urlencode(query or {})
        url = (
            f"{self.server}/api/v1/namespaces/"
            f"{urllib.parse.quote(self.namespace, safe='')}/{resource}"
        )
        if suffix:
            url += f"?{suffix}"
        try:
            token = self.token_file.read_text(encoding="ascii").strip()
            if not token:
                raise AttestationError("projected ServiceAccount token is empty")
            request = urllib.request.Request(
                url,
                headers={"Authorization": f"Bearer {token}", "Accept": "application/json"},
            )
            with urllib.request.urlopen(request, context=self.context, timeout=5) as response:
                document = json.load(response)
        except (OSError, UnicodeDecodeError, urllib.error.URLError, json.JSONDecodeError) as exc:
            raise AttestationError(f"failed to read Kubernetes {resource}") from exc
        items = document.get("items")
        if not isinstance(items, list):
            raise AttestationError(f"Kubernetes {resource} response has no item list")
        return items

    def pods(self, node_name: str | None) -> list[dict[str, Any]]:
        query = {"fieldSelector": f"spec.nodeName={node_name}"} if node_name else None
        return self._items("pods", query)

    def services(self) -> list[dict[str, Any]]:
        return self._items("services")


def resolve_pod(pod_uid: str, pods: list[dict[str, Any]], node_name: str | None) -> dict[str, Any]:
    matches = [pod for pod in pods if pod.get("metadata", {}).get("uid") == pod_uid]
    if len(matches) != 1:
        raise AttestationError(f"Pod UID {pod_uid} resolved to {len(matches)} objects")
    pod = matches[0]
    metadata = pod.get("metadata", {})
    spec = pod.get("spec", {})
    if metadata.get("deletionTimestamp"):
        raise AttestationError("terminating Pod cannot register")
    if node_name and spec.get("nodeName") != node_name:
        raise AttestationError("Pod is assigned to another node")
    return pod


def authorize_service(
    service_id: int,
    service_names: dict[int, str],
    pod: dict[str, Any],
    services: list[dict[str, Any]],
) -> None:
    if service_id == -1:
        return
    service_name = service_names.get(service_id)
    if service_name is None:
        raise AttestationError(f"service id {service_id} is not in the authoritative registry")
    metadata = pod.get("metadata", {})
    namespace = metadata.get("namespace")
    labels = metadata.get("labels") or {}
    candidates = [
        service
        for service in services
        if service.get("metadata", {}).get("namespace") == namespace
        and service.get("metadata", {}).get("name") == service_name
    ]
    if len(candidates) != 1:
        raise AttestationError(
            f"Service {namespace}/{service_name} resolved to {len(candidates)} objects"
        )
    selector = candidates[0].get("spec", {}).get("selector") or {}
    if not selector or any(labels.get(key) != value for key, value in selector.items()):
        raise AttestationError(
            f"Pod labels do not authorize membership in {namespace}/{service_name}"
        )


def build_grant(
    *,
    key: bytes,
    issuer: str,
    key_id: str,
    service_id: int,
    nonce: bytes,
    pod: dict[str, Any],
    ttl: int,
    now: int | None = None,
) -> bytes:
    metadata = pod.get("metadata", {})
    spec = pod.get("spec", {})
    issued = int(time.time()) - 1 if now is None else now
    expires = issued + ttl
    fields = (
        MSG_WORKLOAD_GRANT,
        GRANT_VERSION,
        0,
        0,
        service_id,
        issued,
        expires,
        secrets.token_bytes(16),
        nonce,
        fixed_text(issuer, 64, "issuer"),
        fixed_text(key_id, 32, "key id"),
        fixed_text(str(metadata.get("uid", "")), 64, "Pod UID"),
        fixed_text(str(metadata.get("namespace", "")), 64, "namespace"),
        fixed_text(str(metadata.get("name", "")), 254, "Pod name"),
        fixed_text(str(spec.get("serviceAccountName") or "default"), 254, "ServiceAccount"),
        fixed_text(str(spec.get("nodeName", "")), 254, "node name"),
        bytes(32),
    )
    unsigned = GRANT.pack(*fields)
    mac = hmac.new(key, unsigned[:-32], hashlib.sha256).digest()
    return unsigned[:-32] + mac


class Agent:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        # Validate the keyring before opening a world-accessible request socket.
        load_active_key(args.key_dir)
        self.service_names = read_registry(args.registry)
        self.kubernetes = KubernetesAPI(
            args.api_server, args.api_token_file, args.api_ca_file, args.namespace
        )
        self.listener: socket.socket | None = None

    def attest(self, connection: socket.socket) -> bytes:
        credentials = connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        pid, _uid, _gid = struct.unpack("3i", credentials)
        request = connection.recv(REQUEST.size + 1)
        if len(request) != REQUEST.size:
            raise AttestationError("invalid request size")
        magic, version, service_id, nonce = REQUEST.unpack(request)
        if magic != b"DMESHAR1" or version != GRANT_VERSION or not any(nonce):
            raise AttestationError("invalid request framing or nonce")
        if service_id < -1 or service_id >= 128:
            raise AttestationError("requested Service id is out of range")

        pod_uid = pod_uid_for_pid(pid)
        pods = self.kubernetes.pods(self.args.node_name)
        pod = resolve_pod(pod_uid, pods, self.args.node_name)
        services = self.kubernetes.services() if service_id != -1 else []
        authorize_service(service_id, self.service_names, pod, services)
        key_id, key = load_active_key(self.args.key_dir)
        return build_grant(
            key=key,
            issuer=self.args.issuer,
            key_id=key_id,
            service_id=service_id,
            nonce=nonce,
            pod=pod,
            ttl=self.args.ttl,
        )

    def run(self) -> None:
        path = self.args.socket
        path.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
        if path.exists() or path.is_socket():
            if not path.is_socket() or path.stat().st_uid != os.geteuid():
                raise AttestationError(f"refusing to replace non-owned socket path {path}")
            path.unlink()
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.listener = listener
        listener.bind(str(path))
        os.chmod(path, self.args.socket_mode)
        listener.listen(128)
        print(f"workload-attest-agent: listening on {path}", flush=True)
        while True:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(2.0)
                try:
                    connection.sendall(self.attest(connection))
                except (AttestationError, OSError) as exc:
                    print(f"workload-attest-agent: reject: {exc}", file=sys.stderr, flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", type=Path, default=Path("/run/dpumesh/attest.sock"))
    parser.add_argument("--key-dir", type=Path, required=True)
    parser.add_argument("--registry", type=Path, default=Path("/etc/dpumesh/registry"))
    parser.add_argument(
        "--api-server", default="https://kubernetes.default.svc"
    )
    parser.add_argument(
        "--api-token-file",
        type=Path,
        default=Path("/var/run/secrets/kubernetes.io/serviceaccount/token"),
    )
    parser.add_argument(
        "--api-ca-file",
        type=Path,
        default=Path("/var/run/secrets/kubernetes.io/serviceaccount/ca.crt"),
    )
    parser.add_argument("--namespace", default=os.getenv("POD_NAMESPACE", "test-bench"))
    parser.add_argument("--node-name", default=os.getenv("NODE_NAME"))
    parser.add_argument("--issuer", default="dpumesh-node-agent")
    parser.add_argument("--ttl", type=int, default=60)
    parser.add_argument("--socket-mode", type=lambda value: int(value, 8), default=0o666)
    args = parser.parse_args()
    if not 1 <= args.ttl <= MAX_TTL:
        parser.error(f"--ttl must be between 1 and {MAX_TTL}")
    if os.geteuid() != 0:
        parser.error("the workload attestation agent must run as root")
    return args


def main() -> int:
    args = parse_args()
    agent = Agent(args)

    def stop(_signum: int, _frame: object) -> None:
        if agent.listener is not None:
            agent.listener.close()
        try:
            args.socket.unlink()
        except FileNotFoundError:
            pass
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        agent.run()
    finally:
        try:
            args.socket.unlink()
        except FileNotFoundError:
            pass
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AttestationError as exc:
        print(f"workload-attest-agent: fatal: {exc}", file=sys.stderr)
        raise SystemExit(1)
