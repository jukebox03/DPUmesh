#!/usr/bin/env python3
"""Root-owned DPUmesh workload attestation agent.

The request deliberately contains only a DPU nonce and requested compact
Service id. The agent obtains the caller PID from SO_PEERCRED, resolves its
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
import concurrent.futures
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
ASSERT = struct.Struct(
    "<BBBBQQ16s32s32s254s64s64s254s254s64s16s64s"
)
assert REQUEST.size == 108
assert ASSERT.size == 1134

SERVICE_NAME_RE = re.compile(r"[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?")

POD_UID_RE = re.compile(r"(?:^|[-/_.])pod([0-9a-fA-F][0-9a-fA-F_-]{31,63})(?:[./]|$)")
KEY_ID_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]{0,30}[A-Za-z0-9]")
HEX64_RE = re.compile(r"[0-9a-f]{64}")


class AttestationError(RuntimeError):
    pass


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
        self.service_targets_version = 0
        self.service_targets_feed: str | None = None
        self.reported: tuple[str, str] | None = None
        self.identity_material: tuple[str, str] | None = None
        self.identity_deadline = 0.0

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

    def publish_membership(self) -> int:
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
        return version

    def publish_membership_forever(self) -> None:
        while True:
            try:
                version = self.publish_membership()
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
            time.sleep(self.args.membership_interval)

    def attest(self, connection: socket.socket) -> bytes:
        credentials = connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        pid, _uid, _gid = struct.unpack("3i", credentials)
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
    parser.add_argument("--node-rdma-addr", default="192.168.100.2:4791",
                        help="the peer-channel address this node publishes")
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
    # The absorbed control-plane relay: one Pod, two listeners.
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
