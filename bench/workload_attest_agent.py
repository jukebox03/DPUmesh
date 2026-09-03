#!/usr/bin/env python3
"""Root-owned DPUmesh workload attestation agent.

The request deliberately contains only a DPU nonce and the requested Service
name. The agent obtains the caller PID from SO_PEERCRED, resolves its
Kubernetes Pod UID from the host cgroup, reads authoritative Pod/Service
objects, and signs the resulting immutable claims. It does not accept workload
names, labels, namespace, ServiceAccount, or node name from the caller.

It is also the DPU's only control peer. Every authoritative document the DPU
consumes — its membership generation, the cluster topology generation, the L7
Service-target feed and the Linkerd identity bundle — arrives through the
delivery loop here, and the control-plane TCP streams the DPU needs reach the
cluster through the relay here. Nothing else on the host speaks to the DPU.
"""

from __future__ import annotations

import argparse
import array
import concurrent.futures
import hashlib
import hmac
import json
import os
import re
import secrets
import shutil
import signal
import socket
import ssl
import stat
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import feed_delivery                                          # noqa: E402
import linkerd_cp_relay                                       # noqa: E402

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


ASSERT_VERSION = 2
MSG_WORKLOAD_ASSERT = 13
NONCE_SIZE = 32
MAX_TTL = 300
REQUEST = struct.Struct("<8sB3x64s32s")
BROKER_HELLO = struct.Struct("<8sBB2x64s")
BROKER_IPC_VERSION = 3
ASSERT = struct.Struct(
    "<BBBBQQ16s32s32s254s64s64s254s254s64s16s64s"
)
assert REQUEST.size == 108
assert BROKER_HELLO.size == 76
assert ASSERT.size == 1134

SERVICE_NAME_RE = re.compile(r"[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?")
DNS_RE = re.compile(r"[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)*")

POD_UID_RE = re.compile(r"(?:^|[-/_.])pod([0-9a-fA-F][0-9a-fA-F_-]{31,63})(?:[./]|$)")
KEY_ID_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]{0,30}[A-Za-z0-9]")
HEX64_RE = re.compile(r"[0-9a-f]{64}")


class AttestationError(RuntimeError):
    pass


def valid_rdma_address(address: str) -> bool:
    ip, separator, port = address.partition(":")
    try:
        socket.inet_aton(ip)
    except OSError:
        return False
    parts = ip.split(".")
    return (
        bool(separator) and len(parts) == 4
        and all(part.isdigit() and str(int(part)) == part for part in parts)
        and port.isdigit() and 0 < int(port) < 65536
    )


def node_rdma_address(path: Path, node_name: str) -> str:
    """Read this DaemonSet instance's address from the operator node file."""
    try:
        lines = path.read_text(encoding="ascii").splitlines()
    except (OSError, UnicodeDecodeError) as exc:
        raise AttestationError(f"cannot read node configuration {path}") from exc
    records: dict[str, str] = {}
    for line_no, raw in enumerate(lines, 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) != 5:
            raise AttestationError(f"{path}:{line_no}: expected 5 fields")
        name, rdma, key_id, agent_pub, dpu_pub = fields
        if (
            len(name) > 253 or DNS_RE.fullmatch(name) is None
            or not valid_rdma_address(rdma)
            or KEY_ID_RE.fullmatch(key_id) is None
            or HEX64_RE.fullmatch(agent_pub) is None
            or HEX64_RE.fullmatch(dpu_pub) is None
        ):
            raise AttestationError(f"{path}:{line_no}: malformed node record")
        if name in records:
            raise AttestationError(f"{path}:{line_no}: duplicate node {name}")
        records[name] = rdma
    if node_name not in records:
        raise AttestationError(f"node {node_name!r} is absent from {path}")
    return records[node_name]


def fixed_text(value: str, size: int, field: str, allow_empty: bool = False) -> bytes:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as exc:
        raise AttestationError(f"{field} is not ASCII") from exc
    if (not encoded and not allow_empty) or len(encoded) >= size or b"\0" in encoded:
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


def pod_uid_from_cgroup(cgroup: str) -> str:
    match = POD_UID_RE.search(cgroup)
    if match is None:
        raise AttestationError("peer is not in a Kubernetes Pod cgroup")
    return match.group(1).replace("_", "-").lower()


def process_start_time(pid: int) -> str:
    """Boot-relative start time of a process, which a recycled pid never repeats."""
    try:
        status = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
    except (OSError, UnicodeDecodeError) as exc:
        raise AttestationError(f"cannot read peer state for pid {pid}") from exc
    # The comm field can contain spaces and parentheses, so fields are counted
    # from the last ')': state is field 3 and starttime is field 22.
    fields = status.rpartition(")")[2].split()
    if len(fields) < 20:
        raise AttestationError(f"unreadable /proc/{pid}/stat")
    return fields[19]


def pod_uid_for_pid(pid: int) -> str:
    """Resolve the Pod that owns the peer process.

    `SO_PEERCRED` names the pid at connect time, so the start time is checked on
    both sides of the cgroup read: a pid recycled into another Pod between them
    is rejected instead of being attested under the wrong identity.
    """
    started = process_start_time(pid)
    try:
        cgroup = Path(f"/proc/{pid}/cgroup").read_text(encoding="ascii")
    except (OSError, UnicodeDecodeError) as exc:
        raise AttestationError(f"cannot read peer cgroup for pid {pid}") from exc
    if process_start_time(pid) != started:
        raise AttestationError(f"pid {pid} was recycled during attestation")
    try:
        return pod_uid_from_cgroup(cgroup)
    except AttestationError as exc:
        raise AttestationError(f"pid {pid} is not in a Kubernetes Pod cgroup") from exc


def pod_cgroup_for_pid(pid: int) -> str:
    """Return the cgroup-v2 path through the Pod slice, excluding its
    container scope. A broker placed here is charged to the Pod while remaining
    outside every workload container cgroup."""
    try:
        cgroup = Path(f"/proc/{pid}/cgroup").read_text(encoding="ascii")
    except (OSError, UnicodeDecodeError) as exc:
        raise AttestationError(f"cannot read peer cgroup for pid {pid}") from exc
    unified = next((line.split(":", 2)[2] for line in cgroup.splitlines()
                    if line.startswith("0::")), None)
    if unified is None:
        raise AttestationError("peer has no cgroup-v2 path")
    # A cgroup namespace renders processes outside its root with leading
    # `../` components. /host-cgroup is a bind of the host root, so rebuild
    # from the first canonical kubepods component and never carry traversal
    # components into the writable mount.
    parts = Path(unified).parts
    kube_index = next((index for index, part in enumerate(parts)
                       if part == "kubepods.slice" or
                       part.startswith("kubepods-")), None)
    if kube_index is None:
        raise AttestationError("peer is not below the Kubernetes cgroup root")
    pod_index = next((index for index, part in enumerate(parts)
                      if index >= kube_index and POD_UID_RE.search(part)), None)
    if pod_index is None:
        raise AttestationError("peer is not in a Kubernetes Pod cgroup")
    selected = list(parts[kube_index:pod_index + 1])
    pod_slice = selected[-1]
    # With a private cgroup namespace the kernel may expose an external Pod as
    # just ``../../<pod-slice>/<container-scope>``.  Reconstruct the systemd
    # QoS parents that exist below the separately mounted host cgroup root.
    # Do this from fixed prefixes only; no peer-controlled path component is
    # ever used as traversal.
    if len(selected) == 1 and pod_slice.startswith("kubepods-"):
        if pod_slice.startswith("kubepods-besteffort-pod"):
            selected = ["kubepods.slice", "kubepods-besteffort.slice", pod_slice]
        elif pod_slice.startswith("kubepods-burstable-pod"):
            selected = ["kubepods.slice", "kubepods-burstable.slice", pod_slice]
        elif pod_slice.startswith("kubepods-pod"):
            selected = ["kubepods.slice", pod_slice]
        else:
            raise AttestationError("unrecognized Kubernetes Pod slice")
    return "/" + str(Path(*selected))


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

    def _call(self, path: str, payload: bytes | None = None) -> dict[str, Any]:
        try:
            token = self.token_file.read_text(encoding="ascii").strip()
            if not token:
                raise AttestationError("projected ServiceAccount token is empty")
            headers = {"Authorization": f"Bearer {token}", "Accept": "application/json"}
            if payload is not None:
                headers["Content-Type"] = "application/json"
            request = urllib.request.Request(
                f"{self.server}{path}", data=payload, headers=headers
            )
            with urllib.request.urlopen(request, context=self.context, timeout=5) as response:
                return json.load(response)
        except (OSError, UnicodeDecodeError, urllib.error.URLError, json.JSONDecodeError) as exc:
            raise AttestationError(f"failed to reach Kubernetes {path}") from exc

    def request_token(self, service_account: str, audience: str, seconds: int) -> str:
        """A bound ServiceAccount token for the identity the DPU presents to
        the Linkerd control plane, minted on the cadence the token's lifetime
        asks for."""
        body = json.dumps({
            "apiVersion": "authentication.k8s.io/v1",
            "kind": "TokenRequest",
            "spec": {"audiences": [audience], "expirationSeconds": seconds},
        }).encode("ascii")
        namespace = urllib.parse.quote(self.namespace, safe="")
        account = urllib.parse.quote(service_account, safe="")
        document = self._call(
            f"/api/v1/namespaces/{namespace}/serviceaccounts/{account}/token", body
        )
        token = str(document.get("status", {}).get("token") or "")
        if not token:
            raise AttestationError("TokenRequest returned no token")
        return token

    def trust_anchors(self, namespace: str, name: str) -> str:
        document = self._call(
            f"/api/v1/namespaces/{urllib.parse.quote(namespace, safe='')}"
            f"/configmaps/{urllib.parse.quote(name, safe='')}"
        )
        anchors = str((document.get("data") or {}).get("ca-bundle.crt") or "")
        if not anchors:
            raise AttestationError(f"{namespace}/{name} carries no ca-bundle.crt")
        return anchors


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
    service_name: str,
    pod: dict[str, Any],
    services: list[dict[str, Any]],
) -> None:
    if not service_name:
        return
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


def pod_ipv4(pod: dict[str, Any]) -> str:
    """The Pod's cluster IP, an authoritative input to `networks` authorization."""
    address = str(pod.get("status", {}).get("podIP") or "")
    try:
        socket.inet_aton(address)
        octets = address.split(".")
        if len(octets) != 4 or any(not part.isdigit() for part in octets):
            raise OSError
    except OSError as exc:
        raise AttestationError(f"Pod has no usable IPv4 address: {address!r}") from exc
    return address


def build_assert(
    *,
    key: bytes,
    key_id: str,
    service_name: str,
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
        MSG_WORKLOAD_ASSERT,
        ASSERT_VERSION,
        0,
        0,
        issued,
        expires,
        secrets.token_bytes(16),
        nonce,
        fixed_text(key_id, 32, "key id"),
        fixed_text(str(spec.get("nodeName", "")), 254, "node name"),
        fixed_text(str(metadata.get("uid", "")), 64, "Pod UID"),
        fixed_text(str(metadata.get("namespace", "")), 64, "namespace"),
        fixed_text(str(metadata.get("name", "")), 254, "Pod name"),
        fixed_text(str(spec.get("serviceAccountName") or "default"), 254, "ServiceAccount"),
        fixed_text(service_name, 64, "Service name", allow_empty=True),
        fixed_text(pod_ipv4(pod), 16, "Pod IP"),
        bytes(64),
    )
    unsigned = ASSERT.pack(*fields)
    signature = Ed25519PrivateKey.from_private_bytes(key).sign(unsigned[:-64])
    return unsigned[:-64] + signature


def membership_document(
    version: int,
    pods: list[dict[str, Any]],
    services: list[dict[str, Any]],
) -> str:
    """The (Pod UID, Service name) pairs this node is authorized to hold.

    Every live Pod contributes a bare `-` pair, which is what a Pod registering
    without Service membership holds, plus one pair per namespace Service its
    labels select. A registration whose pair leaves the document has lost
    membership, so the same rule decides an assertion and a revocation.
    """
    lines = [f"version={version}"]
    for pod in pods:
        metadata = pod.get("metadata", {})
        uid = str(metadata.get("uid", ""))
        if not uid or metadata.get("deletionTimestamp"):
            continue
        lines.append(f"member={uid},-")
        for service in services:
            name = str(service.get("metadata", {}).get("name") or "")
            if not name or SERVICE_NAME_RE.fullmatch(name) is None:
                continue
            try:
                authorize_service(name, pod, services)
            except AttestationError:
                continue
            lines.append(f"member={uid},{name}")
    return "\n".join(lines) + "\n"


def sign_document(body: str, key_id: str, key: bytes) -> str:
    """Append the feed envelope the DPU verifies before adopting a generation."""
    mac = hmac.new(key, body.encode("ascii"), hashlib.sha256).hexdigest()
    return f"{body}signature={key_id},{mac}\n"


def published_version(path: Path) -> int:
    """The generation already installed at `path`, or zero."""
    try:
        for line in path.read_text(encoding="ascii").splitlines():
            if line.startswith("version="):
                return int(line[len("version=") :])
    except (OSError, UnicodeDecodeError, ValueError):
        return 0
    return 0


def feed_delivery_bound(name: str) -> int:
    """The byte bound the DPU end of the hop enforces for one feed. Reading it
    from the receiver's own table keeps one definition of each bound."""
    from dpumesh_feed_receiver import FEEDS

    return FEEDS[name][1]


def dpumesh_feed_receiver_members() -> list[str]:
    from dpumesh_feed_receiver import BUNDLE_MEMBERS

    return list(BUNDLE_MEMBERS)


def frame_bundle(members: list[tuple[str, bytes]]) -> bytes:
    from dpumesh_feed_receiver import frame_bundle as frame

    return frame(members)


class GenerationCache:
    """The topology generation this node holds.

    The controller signs it and the DPU verifies it, so the copy held here
    carries no authority — it is what the delivery loop hands to the DPU and
    what the Service-target feed is derived from. A fetch that fails leaves the
    held generation alone: the controller being unavailable withdraws nothing.
    """

    def __init__(self, url: str | None, bound: int, timeout: float = 5.0) -> None:
        self.url = url
        self.bound = bound
        self.timeout = timeout
        self.lock = threading.Lock()
        self.document: str | None = None
        self.version = 0

    def fetch(self) -> bool:
        if self.url is None:
            return False
        request = urllib.request.Request(self.url, headers={"Accept": "text/plain"})
        with urllib.request.urlopen(request, timeout=self.timeout) as response:
            payload = response.read(self.bound + 1)
        if len(payload) > self.bound:
            raise AttestationError(f"topology generation is over the {self.bound}-byte bound")
        document = payload.decode("ascii")
        version = 0
        for line in document.splitlines():
            if line.startswith("version="):
                try:
                    version = int(line[len("version="):])
                except ValueError:
                    raise AttestationError("topology generation has no usable version") from None
                break
        if version == 0:
            raise AttestationError("topology generation has no version line")
        with self.lock:
            # A rollback is refused here as well as at the DPU, so a stale
            # controller replica cannot walk this node backwards.
            if version <= self.version:
                return False
            self.document = document
            self.version = version
        return True

    def held(self) -> tuple[str | None, int]:
        with self.lock:
            return self.document, self.version


class IngressGuard:
    """Closes the node's kernel road to what the mesh serves.

    A meshed Pod's `DPUMESH_PORT` is served over DMA, so a kernel-TCP SYN to
    it is by definition traffic around the mesh. The guard rejects exactly
    those (address, port) pairs in a chain of its own on the FORWARD hook —
    the only hook Pod-to-Pod traffic traverses. Host-sourced traffic (kubelet
    probes, the harness control plane) travels OUTPUT and is untouched, and
    replies to connections a meshed Pod opened itself arrive on ephemeral
    ports, so neither needs an exemption rule.

    The chain is replaced atomically — `iptables-restore --noflush` flushes
    exactly the chains its payload declares — and only when the observed
    chain differs from the desired one. The rules survive the agent:
    enforcement outliving its manager is the direction fail-closed asks for,
    and a deleted Pod's address stays closed for at most one publication
    interval.
    """

    CHAIN = "DPUMESH-PROTECT"

    def __init__(self, enabled: bool) -> None:
        self.enabled = enabled
        self.held: list[str] | None = None

    @staticmethod
    def mesh_served(pods: list[dict[str, Any]]) -> list[tuple[str, int]]:
        """The (Pod address, port) pairs the mesh serves on this node.

        The injection label marks a Pod the webhook meshed, and its
        `DPUMESH_PORT` names the one port its shim converted to a DMA
        listener. A Pod without both keeps whatever kernel listeners it has.
        """
        pairs: set[tuple[str, int]] = set()
        for pod in pods:
            metadata = pod.get("metadata") or {}
            labels = metadata.get("labels") or {}
            if metadata.get("deletionTimestamp") is not None:
                continue
            if labels.get("linkerd.io/control-plane-ns") != "linkerd":
                continue
            try:
                address = pod_ipv4(pod)
            except AttestationError:
                continue
            for container in (pod.get("spec") or {}).get("containers") or []:
                for entry in container.get("env") or []:
                    if entry.get("name") != "DPUMESH_PORT":
                        continue
                    value = str(entry.get("value") or "")
                    if value.isdigit() and 0 < int(value) < 65536:
                        pairs.add((address, int(value)))
        return sorted(pairs)

    def rules(self, pods: list[dict[str, Any]]) -> list[str]:
        # Written in the normal form `iptables -S` prints, so the observed
        # chain and the desired one compare as equal strings.
        return [
            f"-A {self.CHAIN} -d {address}/32 -p tcp -m tcp --dport {port}"
            f" -j REJECT --reject-with tcp-reset"
            for address, port in self.mesh_served(pods)
        ]

    @staticmethod
    def _iptables(
        argv: list[str], payload: str | None = None
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            argv, input=payload, text=True, capture_output=True, timeout=10
        )

    def reconcile(self, pods: list[dict[str, Any]]) -> None:
        """Never raises: a guard failure is loud, and the next pass retries."""
        if not self.enabled:
            return
        wanted = self.rules(pods)
        try:
            observed = self._iptables(["iptables", "-S", self.CHAIN])
            if (observed.returncode != 0
                    or observed.stdout.splitlines()[1:] != wanted):
                payload = "\n".join(
                    ["*filter", f":{self.CHAIN} - [0:0]", *wanted, "COMMIT", ""]
                )
                replaced = self._iptables(["iptables-restore", "--noflush"],
                                          payload)
                if replaced.returncode != 0:
                    raise OSError(replaced.stderr.strip())
            if self._iptables(
                    ["iptables", "-C", "FORWARD", "-j", self.CHAIN]
            ).returncode != 0:
                inserted = self._iptables(
                    ["iptables", "-I", "FORWARD", "1", "-j", self.CHAIN])
                if inserted.returncode != 0:
                    raise OSError(inserted.stderr.strip())
        except (OSError, subprocess.SubprocessError) as exc:
            print(
                f"workload-attest-agent: ingress guard failed; the kernel road"
                f" to {len(wanted)} mesh-served ports may be open: {exc}",
                file=sys.stderr,
                flush=True,
            )
            self.held = None
            return
        if wanted != self.held:
            print(
                f"workload-attest-agent: ingress guard holds "
                f"{len(wanted)} mesh-served ports closed to kernel TCP",
                flush=True,
            )
            self.held = wanted


class Agent:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        # Validate both keyrings before opening a world-accessible request
        # socket: grants sign with the registration key, the membership feed
        # signs with the separate feed key.
        load_active_key(args.key_dir)
        load_active_key(args.feed_key_dir)
        self.kubernetes = KubernetesAPI(
            args.api_server, args.api_token_file, args.api_ca_file, args.namespace
        )
        # Every Pod on the node shares one socket, so a caller that stalls must
        # not hold the only server. Admission is bounded rather than queued.
        self.slots = threading.BoundedSemaphore(args.max_concurrency)
        self.listener: socket.socket | None = None
        # The consumer refuses a generation that is not newer than the one it
        # holds, so publication continues from whatever is already installed.
        self.membership_version = published_version(args.membership_file)
        self.generation = GenerationCache(
            args.controller_url and f"{args.controller_url.rstrip('/')}/topology.v1",
            feed_delivery_bound("topology"),
        )
        self.ingress = IngressGuard(args.protect_ingress)
        self.service_targets_version = 0
        self.service_targets_feed: str | None = None
        self.reported: tuple[str, str] | None = None
        self.identity_material: tuple[str, str] | None = None
        self.identity_deadline = 0.0
        self.broker_bin: Path | None = getattr(args, "broker_bin", None)
        self.broker_lib: Path = getattr(
            args, "broker_lib", Path("/usr/local/lib/libdpumesh.so.5")
        )
        self.broker_runtime_dir: Path = getattr(
            args, "broker_runtime_dir", Path("/var/lib/dpumesh/broker-runtime")
        )
        self.broker_runtime_bin: Path | None = None
        self.broker_runtime_lib: Path | None = None
        self.spawned_lock = threading.Lock()
        # final broker pid -> (None, starttime, Pod, Service). The broker is a
        # systemd unit, never this process's child, so no Popen is ever held.
        self.spawned: dict[int, tuple[subprocess.Popen[bytes] | None, str,
                                      dict[str, Any], str]] = {}
        # Pod UID is the singleton key.  A second workload thread can reach the
        # node socket while the first host-supervised broker is still starting,
        # and a crashing workload can reconnect before DPU teardown has
        # quiesced.  Serialize both cases and back failed launches off instead
        # of creating two Comch owners for one Pod identity.
        self.spawning_pods: set[str] = set()
        # pod UID -> (not-before monotonic time, next delay seconds)
        self.broker_retry: dict[str, tuple[float, float]] = {}
        self.broker_state_dir: Path = getattr(
            args, "broker_state_dir", Path("/run/dpumesh/brokers")
        )
        self.broker_state_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(self.broker_state_dir, 0o700)
        # A broker's tmpfs mount exists only in its private namespace. The
        # underlying hostPath directory is empty here and can be unlinked even
        # while that private mount remains the broker's root.
        for root in self.broker_state_dir.parent.glob(".broker-root.*"):
            try:
                root.rmdir()
            except OSError:
                pass
        if self.broker_bin is not None:
            self.install_broker_runtime()

    def install_broker_runtime(self) -> None:
        """Publish a content-addressed broker outside the agent rootfs.

        A child orphaned by a container is adopted by that container's shim,
        so double-fork alone cannot survive a DaemonSet rollout.  The host
        service manager starts the steady process instead.  Keeping the
        executable and project DSO on the hostPath means neither its parent nor
        its executable mappings retain the old agent container.
        """
        assert self.broker_bin is not None
        try:
            broker_bytes = self.broker_bin.read_bytes()
            library_bytes = self.broker_lib.read_bytes()
        except OSError as exc:
            raise AttestationError(f"cannot stage broker runtime: {exc}") from exc
        digest = hashlib.sha256(broker_bytes + library_bytes).hexdigest()[:24]
        runtime = self.broker_runtime_dir / digest
        runtime.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(runtime, 0o700)

        def publish(source: Path, target: Path) -> None:
            try:
                if target.read_bytes() == source.read_bytes():
                    os.chmod(target, 0o555)
                    return
            except FileNotFoundError:
                pass
            temporary = runtime / f".{target.name}.{os.getpid()}.{secrets.token_hex(4)}"
            try:
                shutil.copyfile(source, temporary)
                os.chmod(temporary, 0o555)
                os.replace(temporary, target)
            finally:
                temporary.unlink(missing_ok=True)

        publish(self.broker_bin, runtime / "dmesh_broker")
        publish(self.broker_lib, runtime / "libdpumesh.so.5")
        self.broker_runtime_bin = runtime / "dmesh_broker"
        self.broker_runtime_lib = runtime / "libdpumesh.so.5"

    # ---- the DPU's only control peer -------------------------------------

    def report_node(self) -> None:
        """Report this node's DPU static handshake key to the controller.

        The key is generated on the DPU at first boot and its private half
        never leaves it; the controller publishes the public half in this
        node's `node=` line, which is what a peer checks a handshake against.
        """
        if self.args.controller_url is None:
            return
        # The key lives on the DPU, so it is read back over the same hop the
        # feeds travel — the agent is the only peer that can carry it, and a
        # DPU that has not generated one yet simply has none to report.
        key = feed_delivery.read_node_key(
            (self.args.dpu_feed_host, self.args.dpu_feed_port))
        if HEX64_RE.fullmatch(key) is None:
            raise AttestationError("the DPU static public key is not 64 hex characters")
        report = (self.args.node_rdma_addr, key)
        if report == self.reported:
            return
        body = json.dumps({
            "name": self.args.node_name,
            "rdma": self.args.node_rdma_addr,
            "dpu_public_key": key,
        }).encode("ascii")
        request = urllib.request.Request(
            f"{self.args.controller_url.rstrip('/')}/node",
            data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=5):
            pass
        self.reported = report
        print(f"workload-attest-agent: reported node key {key[:16]}... to the controller",
              flush=True)

    def topology_source(self) -> bytes | None:
        """Fetch, then deliver. A failed fetch is not a withdrawal — the
        previously fetched generation keeps being delivered."""
        try:
            self.generation.fetch()
        except (AttestationError, OSError, UnicodeDecodeError, urllib.error.URLError) as exc:
            print(f"workload-attest-agent: topology fetch failed: {exc}",
                  file=sys.stderr, flush=True)
        document, _version = self.generation.held()
        return None if document is None else document.encode("ascii")

    def service_targets_source(self) -> bytes | None:
        """Derive the L7 adapter's Service-target feed from the held
        generation. The generation already names each Service's ClusterIP and
        its ready endpoints, so this publisher reads no Kubernetes object of
        its own — one source of truth for both consumers."""
        if not self.args.l7_services:
            return None
        document, version = self.generation.held()
        if document is None:
            return None
        # An unchanged generation derives unchanged bytes (the signature is
        # deterministic), so the hop answers "have" and the adapter re-adopts
        # nothing. The version advances only when the derivation changes: with
        # the generation when it moved, past the held version when the service
        # list changed under an unmoved one.
        version = max(version, self.service_targets_version)
        key_id, key = load_active_key(self.args.feed_key_dir)
        feed = feed_delivery.service_targets_document(
            document, self.args.l7_services, version, key_id, key
        )
        if feed is None:
            return None
        if feed == self.service_targets_feed:
            return feed.encode("ascii")
        if version == self.service_targets_version and self.service_targets_feed is not None:
            version += 1
            feed = feed_delivery.service_targets_document(
                document, self.args.l7_services, version, key_id, key
            )
            if feed is None:
                return None
        self.service_targets_version = version
        self.service_targets_feed = feed
        return feed.encode("ascii")

    def identity_source(self) -> bytes | None:
        """The Linkerd identity bundle, framed for the hop.

        The key and its certificate request are provisioned once and read from
        the staging directory; the ServiceAccount token and the trust anchors
        are re-requested here on the cadence the token's lifetime asks for.
        Framing them together is what makes the four move as one: a proxy
        holding a key from one issuance and a token from another authenticates
        as neither.
        """
        if self.args.identity_dir is None:
            return None
        now = time.monotonic()
        if now >= self.identity_deadline:
            token = self.kubernetes.request_token(
                self.args.identity_service_account,
                self.args.identity_audience,
                int(self.args.identity_duration),
            )
            anchors = self.kubernetes.trust_anchors(
                self.args.identity_trust_namespace, self.args.identity_trust_configmap
            )
            self.identity_material = (token, anchors)
            # Refresh well inside the token's lifetime: a bundle that expires
            # between two deliveries is a bundle the proxy cannot use.
            self.identity_deadline = now + self.args.identity_duration / 2.0
        if self.identity_material is None:
            return None
        token, anchors = self.identity_material
        members = []
        for member in ("key.p8", "csr.der"):
            try:
                members.append((member, (self.args.identity_dir / member).read_bytes()))
            except OSError:
                return None                     # not provisioned yet
        members.append(("token.txt", token.encode("ascii")))
        members.append(("trust-anchors.pem", anchors.encode("ascii")))
        order = dpumesh_feed_receiver_members()
        members.sort(key=lambda item: order.index(item[0]))
        return frame_bundle(members)

    def deliveries(self) -> list[feed_delivery.Delivery]:
        """The four feeds, and nothing else, is what this hop can install."""
        bound = feed_delivery_bound
        items = [
            feed_delivery.Delivery(
                "membership",
                feed_delivery.file_source(self.args.membership_file, bound("membership")),
            ),
            feed_delivery.Delivery("topology", self.topology_source),
            feed_delivery.Delivery("service-targets", self.service_targets_source),
        ]
        if self.args.identity_dir is not None:
            items.append(feed_delivery.Delivery("identity-bundle", self.identity_source))
        return items

    def deliver_forever(self) -> None:
        loop = feed_delivery.DeliveryLoop(
            (self.args.dpu_feed_host, self.args.dpu_feed_port),
            self.deliveries(),
            interval=self.args.delivery_interval,
        )
        while True:
            try:
                self.report_node()
            except Exception as exc:            # a failed report withdraws nothing
                print(f"workload-attest-agent: node report failed: {exc}",
                      file=sys.stderr, flush=True)
            loop.round()
            time.sleep(loop.backoff)

    def relay_forever(self) -> None:
        kube = linkerd_cp_relay.KubernetesAPI(
            str(self.args.api_server), str(self.args.api_token_file), str(self.args.api_ca_file)
        )
        linkerd_cp_relay.run_relay(self.args.relay_routes, kube)

    def publish_membership(self) -> tuple[int, list[dict[str, Any]]]:
        """Install one membership generation for this node."""
        # The generation is stamped before the reads, so it names the world at
        # snapshot time rather than at publication time.
        version = max(time.time_ns(), self.membership_version + 1)
        pods = self.kubernetes.pods(self.args.node_name)
        services = self.kubernetes.services()
        body = membership_document(version, pods, services)
        key_id, key = load_active_key(self.args.feed_key_dir)
        document = sign_document(body, key_id, key)

        path = self.args.membership_file
        path.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
        temporary = path.with_name(f".{path.name}.new")
        temporary.write_text(document, encoding="ascii")
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
        self.membership_version = version
        return version, pods

    def publish_membership_forever(self) -> None:
        while True:
            pods: list[dict[str, Any]] | None = None
            try:
                version, pods = self.publish_membership()
                print(
                    f"workload-attest-agent: membership generation {version}",
                    flush=True,
                )
            except (AttestationError, OSError) as exc:
                print(
                    f"workload-attest-agent: membership publish failed: {exc}",
                    file=sys.stderr,
                    flush=True,
                )
            # The same listing that grants and revokes membership closes the
            # kernel road around what it granted.
            if pods is not None:
                self.ingress.reconcile(pods)
            time.sleep(self.args.membership_interval)

    def authorized_pod(self, pid: int, service_name: str) -> dict[str, Any]:
        pod_uid = pod_uid_for_pid(pid)
        pods = self.kubernetes.pods(self.args.node_name)
        pod = resolve_pod(pod_uid, pods, self.args.node_name)
        # A Pod registers the moment its process starts, which can precede the
        # apiserver observing status.podIP. The IP is a signed claim, so wait
        # briefly for the authoritative object instead of refusing the race.
        for _ in range(5):
            if pod.get("status", {}).get("podIP"):
                break
            time.sleep(0.3)
            pods = self.kubernetes.pods(self.args.node_name)
            pod = resolve_pod(pod_uid, pods, self.args.node_name)
        services = self.kubernetes.services() if service_name else []
        authorize_service(service_name, pod, services)
        return pod

    def broker_spawn_claim(self, pod_uid: str, now: float | None = None) -> None:
        if now is None:
            now = time.monotonic()
        with self.spawned_lock:
            if pod_uid in self.spawning_pods:
                raise AttestationError(
                    f"broker launch is already in progress for Pod {pod_uid}"
                )
            for _pid, (_process, _started, pod, _service) in self.spawned.items():
                if str(pod["metadata"]["uid"]) == pod_uid:
                    raise AttestationError(
                        f"a live broker already owns Pod {pod_uid}"
                    )
            not_before, _delay = self.broker_retry.get(pod_uid, (0.0, 5.0))
            if now < not_before:
                raise AttestationError(
                    f"broker restart for Pod {pod_uid} is backed off for "
                    f"{not_before - now:.2f}s"
                )
            self.spawning_pods.add(pod_uid)

    def broker_spawn_release(self, pod_uid: str, success: bool,
                             now: float | None = None) -> None:
        if now is None:
            now = time.monotonic()
        with self.spawned_lock:
            self.spawning_pods.discard(pod_uid)
            if success:
                # A completed launch starts a fresh lifecycle.  If that broker
                # later exits, sweep_brokers applies the initial grace period.
                self.broker_retry.pop(pod_uid, None)
                return
            _not_before, delay = self.broker_retry.get(pod_uid, (0.0, 5.0))
            delay = max(5.0, min(delay, 30.0))
            self.broker_retry[pod_uid] = (now + delay, min(delay * 2.0, 30.0))

    def sweep_brokers(self) -> None:
        stale: list[tuple[int, str]] = []
        now = time.monotonic()
        with self.spawned_lock:
            for pid, (process, started, pod, _service) in self.spawned.items():
                try:
                    alive = ((process is None or process.poll() is None) and
                             process_start_time(pid) == started)
                except AttestationError:
                    alive = False
                if not alive:
                    stale.append((pid, str(pod["metadata"]["uid"])))
            for pid, pod_uid in stale:
                self.spawned.pop(pid, None)
                _not_before, delay = self.broker_retry.get(pod_uid, (0.0, 5.0))
                delay = max(5.0, min(delay, 30.0))
                self.broker_retry[pod_uid] = (
                    now + delay, min(delay * 2.0, 30.0)
                )

        for pid, _pod_uid in stale:
            for record in self.broker_state_dir.glob("*.state"):
                try:
                    document = json.loads(record.read_text(encoding="utf-8"))
                    if int(document.get("pid", -1)) == pid:
                        record.unlink(missing_ok=True)
                except (OSError, ValueError, json.JSONDecodeError):
                    continue

    def write_broker_state(self, pid: int, started: str, pod: dict[str, Any],
                           service_name: str, target_cgroup: str) -> None:
        pod_uid = str(pod["metadata"]["uid"])
        document = {
            "pid": pid,
            "starttime": started,
            "pod_uid": pod_uid,
            "service": service_name,
            "cgroup": target_cgroup,
        }
        temporary = self.broker_state_dir / f".{pod_uid}.{pid}.tmp"
        target = self.broker_state_dir / f"{pod_uid}.state"
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            os.write(fd, (json.dumps(document, sort_keys=True) + "\n").encode("utf-8"))
            os.fsync(fd)
        finally:
            os.close(fd)
        os.replace(temporary, target)

    def re_adopt_brokers(self) -> None:
        if self.broker_bin is None:
            return
        adopted = 0
        for record in self.broker_state_dir.glob("*.state"):
            try:
                if record.is_symlink() or stat.S_IMODE(record.stat().st_mode) & 0o077:
                    raise AttestationError("broker state is not root-private")
                document = json.loads(record.read_text(encoding="utf-8"))
                pid = int(document["pid"])
                started = str(document["starttime"])
                pod_uid = str(document["pod_uid"])
                service_name = str(document.get("service") or "")
                if process_start_time(pid) != started:
                    raise AttestationError("broker PID/starttime changed")
                executable = os.readlink(f"/proc/{pid}/exe")
                if Path(executable).name != self.broker_bin.name:
                    raise AttestationError("state PID is not dmesh_broker")
                if pod_uid_for_pid(pid) != pod_uid:
                    raise AttestationError("broker state/cgroup Pod UID mismatch")
                pods = self.kubernetes.pods(self.args.node_name)
                pod = resolve_pod(pod_uid, pods, self.args.node_name)
                authorize_service(
                    service_name, pod,
                    self.kubernetes.services() if service_name else [],
                )
                with self.spawned_lock:
                    self.spawned[pid] = (None, started, pod, service_name)
                adopted += 1
                print(
                    f"workload-attest-agent: re-adopted broker pid={pid} "
                    f"pod={pod_uid} service={service_name or '-'}",
                    flush=True,
                )
            except (AttestationError, KeyError, OSError, ValueError,
                    json.JSONDecodeError) as exc:
                print(
                    f"workload-attest-agent: discarded broker state "
                    f"{record.name}: {exc}", file=sys.stderr, flush=True,
                )
                record.unlink(missing_ok=True)
        if adopted:
            print(f"workload-attest-agent: re-adopted {adopted} broker(s)", flush=True)

    def broker_claims(self, pid: int, uid: int) -> tuple[dict[str, Any], str] | None:
        if uid not in (0, 65532):
            return None
        self.sweep_brokers()
        with self.spawned_lock:
            entry = self.spawned.get(pid)
        if entry is None:
            return None
        process, started, pod, service_name = entry
        if ((process is not None and process.poll() is not None) or
                process_start_time(pid) != started):
            with self.spawned_lock:
                self.spawned.pop(pid, None)
            raise AttestationError("stale broker registry entry")
        if pod_uid_for_pid(pid) != pod["metadata"]["uid"]:
            raise AttestationError("broker registry and cgroup Pod UID disagree")
        return pod, service_name

    def spawn_broker(self, connection: socket.socket) -> None:
        if self.broker_bin is None or self.broker_runtime_bin is None:
            raise AttestationError("broker mode is disabled")
        credentials = connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        pid, _uid, _gid = struct.unpack("3i", credentials)
        packet = connection.recv(BROKER_HELLO.size + 1, socket.MSG_PEEK)
        if len(packet) != BROKER_HELLO.size:
            raise AttestationError("invalid broker HELLO size")
        magic, message_type, version, service_field = BROKER_HELLO.unpack(packet)
        if (magic != b"DMESHBR1" or message_type != 1 or
                version != BROKER_IPC_VERSION):
            raise AttestationError("invalid broker HELLO framing")
        try:
            service_name = service_field.split(b"\0", 1)[0].decode("ascii")
        except UnicodeDecodeError as exc:
            raise AttestationError("broker Service name is not ASCII") from exc
        if service_name and SERVICE_NAME_RE.fullmatch(service_name) is None:
            raise AttestationError(f"invalid requested Service name {service_name!r}")
        pod = self.authorized_pod(pid, service_name)
        target_cgroup = pod_cgroup_for_pid(pid)
        pod_uid = str(pod["metadata"]["uid"])
        self.sweep_brokers()
        self.broker_spawn_claim(pod_uid)
        spawn_succeeded = False

        try:
            cgroup_root = self.args.host_cgroup_root.resolve()
            target = (cgroup_root / target_cgroup.lstrip("/")).resolve()
            if target == cgroup_root or cgroup_root not in target.parents:
                raise AttestationError("resolved broker cgroup escaped host root")
            if not target.is_dir():
                raise AttestationError(
                    f"Pod cgroup disappeared before broker start: {target_cgroup}"
                )
            # A cgroup with enabled domain controllers cannot hold processes while
            # it also has container children.  The dedicated child remains inside
            # recursive Pod accounting but outside all workload containers.
            broker_cgroup = target / "dpumesh-broker"
            broker_cgroup.mkdir(exist_ok=True)
            cgroup_dir_fd = os.open(broker_cgroup, os.O_RDONLY | os.O_DIRECTORY)
            token = secrets.token_hex(32)
            launch_path = self.broker_state_dir.parent / f"launch.{token[:16]}.sock"
            launch = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            launch.settimeout(5.0)
            launch.bind(str(launch_path))
            os.chmod(launch_path, 0o600)
            launch.listen(1)
            unit = f"dpumesh-broker-{pod_uid.replace('-', '')[:12]}-{token[:8]}"
            final_pid: int | None = None
            try:
                library_path = (
                    f"{self.broker_runtime_bin.parent}:"
                    "/opt/mellanox/doca/lib/x86_64-linux-gnu:"
                    "/opt/mellanox/flexio/lib"
                )
                environment = [
                    f"LD_LIBRARY_PATH={library_path}",
                    f"DPUMESH_PCI_ADDR={os.getenv('DPUMESH_PCI_ADDR', '')}",
                    f"DPUMESH_RINGS_PER_POD={os.getenv('DPUMESH_RINGS_PER_POD', '8')}",
                ]
                command = [
                    "/usr/bin/systemd-run", "--quiet", "--collect",
                    f"--unit={unit}", "--service-type=exec",
                    "--property=Restart=no",
                    "--property=KillMode=control-group",
                    "--property=TimeoutStopSec=5s", "--property=UMask=0077",
                    # Never inherit systemd-run's short-lived capture pipe.
                    # DOCA emits diagnostics after launch; writing to the
                    # closed pipe would otherwise deliver SIGPIPE and kill an
                    # otherwise healthy long-lived broker.
                    "--property=StandardOutput=journal",
                    "--property=StandardError=journal",
                    "--property=LimitMEMLOCK=infinity",
                    "--property=CapabilityBoundingSet=CAP_SYS_ADMIN CAP_IPC_LOCK CAP_SETUID CAP_SETGID CAP_SETPCAP CAP_SYS_RESOURCE CAP_DAC_OVERRIDE CAP_KILL",
                ]
                command.extend(f"--setenv={entry}" for entry in environment)
                command.extend([
                    "/usr/bin/unshare", "--pid", "--fork", "--kill-child=SIGTERM",
                    "--mount-proc", str(self.broker_runtime_bin),
                    "--launch-sock", str(launch_path),
                    "--launch-token", token, "--agent-sock", str(self.args.socket),
                ])
                started_unit = subprocess.run(
                    command, check=False, stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE, text=True, timeout=5.0,
                )
                if started_unit.returncode != 0:
                    detail = started_unit.stderr.strip() or "unknown systemd error"
                    raise AttestationError(
                        f"host supervisor rejected broker: {detail}"
                    )
                peer, _ = launch.accept()
                try:
                    credentials = peer.getsockopt(
                        socket.SOL_SOCKET, socket.SO_PEERCRED, 12
                    )
                    final_pid, peer_uid, _peer_gid = struct.unpack("3i", credentials)
                    presented = peer.recv(65)
                    if peer_uid != 0 or presented != token.encode("ascii"):
                        raise AttestationError(
                            "broker launch credential/token mismatch"
                        )
                    rights = array.array("i", [connection.fileno(), cgroup_dir_fd])
                    peer.sendmsg(
                        [b"F"],
                        [(socket.SOL_SOCKET, socket.SCM_RIGHTS, rights.tobytes())],
                    )
                    if peer.recv(1) != b"M":
                        raise AttestationError(
                            "broker did not confirm cgroup migration"
                        )
                    if final_pid <= 1:
                        raise AttestationError(
                            "broker reported an invalid final PID"
                        )
                    started = process_start_time(final_pid)
                    if pod_uid_for_pid(final_pid) != pod_uid:
                        raise AttestationError(
                            "final broker cgroup does not match Pod"
                        )
                    with self.spawned_lock:
                        self.spawned[final_pid] = (
                            None, started, pod, service_name
                        )
                    self.write_broker_state(
                        final_pid, started, pod, service_name, target_cgroup
                    )
                    try:
                        peer.sendall(b"G")
                    except OSError as exc:
                        with self.spawned_lock:
                            self.spawned.pop(final_pid, None)
                        raise AttestationError(
                            "final broker exited before release"
                        ) from exc
                finally:
                    peer.close()
                print(
                    f"workload-attest-agent: spawned broker pid={final_pid} "
                    f"pod={pod_uid} service={service_name or '-'} unit={unit}",
                    flush=True,
                )
                spawn_succeeded = True
            except Exception:
                if final_pid is not None:
                    try:
                        os.kill(final_pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                subprocess.run(
                    ["/usr/bin/systemctl", "stop", unit], check=False,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                )
                raise
            finally:
                launch.close()
                launch_path.unlink(missing_ok=True)
                os.close(cgroup_dir_fd)
        finally:
            self.broker_spawn_release(pod_uid, spawn_succeeded)

    def attest(self, connection: socket.socket) -> bytes:
        credentials = connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        pid, uid, _gid = struct.unpack("3i", credentials)
        request = connection.recv(REQUEST.size + 1)
        if len(request) != REQUEST.size:
            raise AttestationError("invalid request size")
        magic, version, service_field, nonce = REQUEST.unpack(request)
        if magic != b"DMESHAR1" or version != ASSERT_VERSION or not any(nonce):
            raise AttestationError("invalid request framing or nonce")
        try:
            service_name = service_field.split(b"\0", 1)[0].decode("ascii")
        except UnicodeDecodeError as exc:
            raise AttestationError("requested Service name is not ASCII") from exc
        if service_name and SERVICE_NAME_RE.fullmatch(service_name) is None:
            raise AttestationError(f"invalid requested Service name {service_name!r}")

        broker = self.broker_claims(pid, uid)
        if broker is not None:
            pod, registered_service = broker
            if service_name != registered_service:
                raise AttestationError("broker requested a Service outside its registry claim")
        else:
            pod = self.authorized_pod(pid, service_name)
        key_id, key = load_active_key(self.args.key_dir)
        return build_assert(
            key=key,
            key_id=key_id,
            service_name=service_name,
            nonce=nonce,
            pod=pod,
            ttl=self.args.ttl,
        )

    def serve(self, connection: socket.socket) -> None:
        try:
            with connection:
                connection.settimeout(self.args.request_timeout)
                try:
                    peek = connection.recv(BROKER_HELLO.size + 1, socket.MSG_PEEK)
                    if (len(peek) == BROKER_HELLO.size and
                            peek.startswith(b"DMESHBR1")):
                        self.spawn_broker(connection)
                    else:
                        connection.sendall(self.attest(connection))
                except (AttestationError, OSError) as exc:
                    print(f"workload-attest-agent: reject: {exc}", file=sys.stderr, flush=True)
        finally:
            self.slots.release()

    def run(self) -> None:
        path = self.args.socket
        path.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
        if path.exists():
            if not path.is_socket() or path.stat().st_uid != os.geteuid():
                raise AttestationError(f"refusing to replace non-owned socket path {path}")
            path.unlink()
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.listener = listener
        listener.bind(str(path))
        os.chmod(path, self.args.socket_mode)
        listener.listen(128)
        # Rebuild supervision before the accept loop can sign for a broker.
        # Existing pod↔broker sockets do not depend on this listener and remain
        # live across the DaemonSet rollout.
        self.re_adopt_brokers()
        print(
            f"workload-attest-agent: listening on {path} "
            f"(concurrency={self.args.max_concurrency})",
            flush=True,
        )
        threading.Thread(
            target=self.publish_membership_forever,
            name="membership",
            daemon=True,
        ).start()
        threading.Thread(target=self.deliver_forever, name="delivery", daemon=True).start()
        if self.args.relay_routes:
            threading.Thread(target=self.relay_forever, name="relay", daemon=True).start()
        with concurrent.futures.ThreadPoolExecutor(
            max_workers=self.args.max_concurrency,
            thread_name_prefix="attest",
        ) as pool:
            while True:
                connection, _ = listener.accept()
                if not self.slots.acquire(blocking=False):
                    print(
                        "workload-attest-agent: reject: concurrency limit reached",
                        file=sys.stderr,
                        flush=True,
                    )
                    connection.close()
                    continue
                pool.submit(self.serve, connection)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", type=Path, default=Path("/run/dpumesh/attest.sock"))
    parser.add_argument("--key-dir", type=Path, required=True)
    parser.add_argument("--feed-key-dir", type=Path, required=True)
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
    parser.add_argument("--ttl", type=int, default=60)
    parser.add_argument("--socket-mode", type=lambda value: int(value, 8), default=0o666)
    parser.add_argument("--max-concurrency", type=int, default=8)
    parser.add_argument("--request-timeout", type=float, default=2.0)
    parser.add_argument("--broker-bin", type=Path, default=None,
                        help="per-Pod broker executable; unset keeps legacy mode")
    parser.add_argument("--broker-lib", type=Path,
                        default=Path("/usr/local/lib/libdpumesh.so.5"),
                        help="project DSO staged with the host-supervised broker")
    parser.add_argument("--broker-runtime-dir", type=Path,
                        default=Path("/var/lib/dpumesh/broker-runtime"),
                        help="host exec-enabled content-addressed broker runtime")
    parser.add_argument("--host-cgroup-root", type=Path,
                        default=Path("/host-cgroup"),
                        help="writable host cgroup-v2 mount used for broker accounting")
    parser.add_argument("--broker-state-dir", type=Path,
                        default=Path("/run/dpumesh/brokers"),
                        help="root-private supervision records for agent re-adoption")
    parser.add_argument(
        "--membership-file", type=Path, default=Path("/run/dpumesh/membership.v1")
    )
    parser.add_argument("--membership-interval", type=float, default=10.0)
    # The delivery hop: where this node's DPU receives its feeds.
    parser.add_argument("--dpu-feed-host", default="192.168.100.2")
    parser.add_argument("--dpu-feed-port", type=int, default=4788)
    parser.add_argument("--delivery-interval", type=float, default=2.0)
    parser.add_argument("--controller-url", default=None,
                        help="where the cluster controller serves the generation")
    parser.add_argument("--nodes-file", type=Path, default=None,
                        help="operator node records shared with the controller")
    parser.add_argument("--node-rdma-addr", default=None,
                        help="single-node peer address (use --nodes-file in a cluster)")
    parser.add_argument("--identity-dir", type=Path, default=None,
                        help="where the Linkerd key and certificate request are staged")
    parser.add_argument("--identity-service-account", default="dpumesh-dpu")
    parser.add_argument("--identity-audience", default="identity.l5d.io")
    parser.add_argument("--identity-duration", type=float, default=3600.0)
    parser.add_argument("--identity-trust-namespace", default="linkerd")
    parser.add_argument("--identity-trust-configmap",
                        default="linkerd-identity-trust-roots")
    parser.add_argument("--l7-service", action="append", default=[], dest="l7_services",
                        metavar="NAMESPACE/NAME",
                        help="Service the derived L7 target feed names (repeatable)")
    parser.add_argument("--protect-ingress", action="store_true",
                        help="reject kernel-TCP ingress to mesh-served Pod "
                             "ports (FORWARD chain; needs CAP_NET_ADMIN)")
    # The control-plane relay: one listener per route, beside the attest socket.
    parser.add_argument("--route", action="append", type=linkerd_cp_relay.route,
                        default=[], dest="routes")
    parser.add_argument("--kube-route", action="append", type=linkerd_cp_relay.kube_route,
                        default=[], dest="kube_routes")
    args = parser.parse_args()
    args.relay_routes = args.routes + args.kube_routes
    if not 1 <= args.ttl <= MAX_TTL:
        parser.error(f"--ttl must be between 1 and {MAX_TTL}")
    if not 1 <= args.max_concurrency <= 256:
        parser.error("--max-concurrency must be between 1 and 256")
    if not 0 < args.request_timeout <= 30:
        parser.error("--request-timeout must be between 0 and 30 seconds")
    if not 1 <= args.membership_interval <= 300:
        parser.error("--membership-interval must be between 1 and 300 seconds")
    if not 1 <= args.dpu_feed_port <= 65535:
        parser.error("--dpu-feed-port out of range")
    if not 0.5 <= args.delivery_interval <= 300:
        parser.error("--delivery-interval must be between 0.5 and 300 seconds")
    if args.nodes_file is not None:
        if args.node_rdma_addr is not None:
            parser.error("--nodes-file and --node-rdma-addr are mutually exclusive")
        if not args.node_name:
            parser.error("--nodes-file requires --node-name")
        try:
            args.node_rdma_addr = node_rdma_address(args.nodes_file, args.node_name)
        except AttestationError as exc:
            parser.error(str(exc))
    elif args.node_rdma_addr is None:
        args.node_rdma_addr = "192.168.100.2:47900"
    if not valid_rdma_address(args.node_rdma_addr):
        parser.error("--node-rdma-addr must be canonical IPv4:port")
    for key in args.l7_services:
        if len(key.split("/")) != 2:
            parser.error(f"--l7-service takes namespace/name, got {key!r}")
    if not 600 <= args.identity_duration <= 86400:
        parser.error("--identity-duration must be between 600 and 86400 seconds")
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
