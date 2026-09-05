#!/usr/bin/env python3
"""DPUmesh cluster controller.

Publishes one signed, versioned topology generation carrying every
cluster-wide fact a DPU needs: node identities and keys, Pod placements,
Services with their ClusterIPs, ready endpoints, and the protected-Service
set. It does not infer host-local evidence; `dpumeshd` supplies the kernel
binding used for a WorkloadGrant. The generation is Ed25519-signed; DPUs hold
public keys only.

The document grammar is design/CONTROL.md's, one record per line:

    version=<u64, strictly increasing>
    node=<name>,<rdma-ip>:<port>,<grant-key-id>,<grant-pub-hex64>,<dpu-pub-hex64>
    pod=<pod-uid>,<node>,<namespace>,<service-account>,<pod-ipv4>
    service=<namespace>/<name>,<cluster-ipv4>:<port>
    endpoint=<namespace>/<name>,<pod-uid>
    protected=<namespace>/<name>
    signature=<key-id>,<hex128>

A record the consumer would refuse is dropped here with a log line rather
than poisoning the whole generation. The generation bounds are enforced here
too, refused whole rather than truncated — a truncated topology would be a
lie every DPU adopts, while a refused publication leaves the last good
generation standing, which is the fail-static rule. A cluster whose facts
have not changed republishes nothing, so consumers re-adopt only when there
is something to adopt.

The controller is reached only by each host's ``dpumeshd`` over node mTLS.
The runtime relays signed feeds to its DPU and forwards the DPU's scoped
control-plane queries without interpreting them.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import http.server
import json
import os
import re
import socket
import socketserver
import ssl
import stat
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

sys.path.insert(0, str(Path(__file__).resolve().parent))
import workload_grant                                      # noqa: E402

GENERATION_INTERVAL = 5.0
# The consumer's generation bounds (doca/topology.h), enforced at the
# publisher too so an over-bound cluster fails loudly here instead of
# publishing a document every DPU refuses.
GEN_NODE_MAX = 1024
GEN_POD_MAX = 65536
GEN_SERVICE_MAX = 4096
GEN_ENDPOINT_MAX = 65536
TOPOLOGY_MAX_BYTES = 16 * 1024 * 1024
# A node report is three short JSON fields.
NODE_REPORT_MAX = 4096
WORKLOAD_GRANT_REQUEST_MAX = 8192
MEMBERSHIP_MAX_BYTES = 256 * 1024
SERVICE_TARGETS_MAX_BYTES = 1024 * 1024
CONTROLLER_REQUEST_MAX = 32
CONTROLLER_REQUEST_TIMEOUT = 10.0
ZERO_KEY = "0" * 64
POD_UID_RE = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
KEY_ID_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]{0,30}[A-Za-z0-9]")
DNS_RE = re.compile(r"[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)*")
HEX64_RE = re.compile(r"[0-9a-f]{64}")


class ControllerError(RuntimeError):
    pass


def load_key(path: Path) -> bytes:
    try:
        base = path.parent.resolve(strict=True)
        resolved = path.resolve(strict=True)
        st = resolved.stat()
    except OSError as exc:
        raise ControllerError(f"cannot resolve private key {path}") from exc
    # Kubernetes projected Secrets use an atomic symlink into a timestamped
    # directory below the mount. Follow that one bounded indirection, but never
    # accept a target that escaped the mounted key directory.
    if resolved != base / path.name and base not in resolved.parents:
        raise ControllerError(f"private key {path} escaped its key directory")
    mode = stat.S_IMODE(st.st_mode)
    owner_readable = st.st_uid == os.geteuid() and bool(mode & stat.S_IRUSR)
    group_readable = (
        st.st_uid == 0 and st.st_gid in {os.getegid(), *os.getgroups()}
        and bool(mode & stat.S_IRGRP) and not mode & (stat.S_IWGRP | stat.S_IXGRP)
    )
    if (
        not stat.S_ISREG(st.st_mode)
        or st.st_uid not in (0, os.geteuid())
        or mode & 0o007
        or not (owner_readable or group_readable)
        or mode & (stat.S_IXUSR | stat.S_IWGRP | stat.S_IXGRP)
    ):
        raise ControllerError(
            f"{path} must be a private regular file readable by uid/gid {os.geteuid()}:{os.getegid()}"
        )
    data = path.read_bytes()
    if len(data) == 32:
        key = data
    else:
        try:
            key = bytes.fromhex(data.decode("ascii").strip())
        except (UnicodeDecodeError, ValueError) as exc:
            raise ControllerError(f"{path} is not a raw/hex key") from exc
    if len(key) != 32 or not any(key):
        raise ControllerError(f"{path} must contain a nonzero 32-byte key")
    return key


def load_active_key(directory: Path) -> tuple[str, bytes]:
    """Load the active signing key on every publication so rotation needs no restart."""
    active = directory / "active"
    key_id = active.read_text(encoding="ascii").strip()
    if KEY_ID_RE.fullmatch(key_id) is None:
        raise ControllerError(f"invalid active key id in {active}")
    return key_id, load_key(directory / f"{key_id}.key")


def valid_ipv4(address: str) -> bool:
    try:
        socket.inet_aton(address)
    except OSError:
        return False
    parts = address.split(".")
    return len(parts) == 4 and all(p.isdigit() and str(int(p)) == p for p in parts)


def valid_rdma(address: str) -> bool:
    ip, separator, port = address.partition(":")
    return bool(separator) and valid_ipv4(ip) and port.isdigit() and 0 < int(port) < 65536


def valid_node_record(name: str, rdma: str, key_id: str, grant_pub: str, dpu_pub: str) -> bool:
    return (
        DNS_RE.fullmatch(name) is not None and len(name) <= 253
        and all(len(label) <= 63 for label in name.split("."))
        and valid_rdma(rdma)
        and KEY_ID_RE.fullmatch(key_id) is not None
        and HEX64_RE.fullmatch(grant_pub) is not None
        and HEX64_RE.fullmatch(dpu_pub) is not None
    )


def read_nodes_file(path: Path) -> dict[str, tuple[str, str, str, str]]:
    """The operator's per-node input, keyed by node name:
    `<node-name> <rdma-ip:port> <grant-key-id> <grant-pub-hex64> <dpu-pub-hex64>`.

    The grant key is operator material that binds grants to this node. The host
    runtime may report only the public half of the static key its DPU generated.
    """
    records: dict[str, tuple[str, str, str, str]] = {}
    for line_no, raw in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) != 5:
            raise ControllerError(f"{path}:{line_no}: expected 5 fields")
        name, rdma, key_id, grant_pub, dpu_pub = fields
        if not valid_node_record(name, rdma, key_id, grant_pub, dpu_pub):
            raise ControllerError(f"{path}:{line_no}: malformed node record")
        if name in records:
            raise ControllerError(f"{path}:{line_no}: duplicate node {name}")
        records[name] = (rdma, key_id, grant_pub, dpu_pub)
    return records


class NodeRegistry:
    """The operator-owned node set and the DPU keys host runtimes report.

    The file is the anchor: a node the operator did not configure is not
    published, so a report can add nothing. Addresses and grant keys are
    operator facts and cannot be changed by a node. A report supplies only
    the DPU static handshake key generated at first boot.
    """

    def __init__(self, path: Path) -> None:
        self.path = path
        self.reports: dict[str, str] = {}
        self.lock = threading.Lock()

    def report(self, name: str, rdma: str, dpu_public_key: str) -> None:
        if not valid_rdma(rdma) or HEX64_RE.fullmatch(dpu_public_key) is None:
            raise ControllerError("malformed node report")
        if dpu_public_key == ZERO_KEY:
            raise ControllerError("a node may not report an all-zero static key")
        configured = read_nodes_file(self.path)
        if name not in configured:
            raise ControllerError("node is not configured")
        if rdma != configured[name][0]:
            raise ControllerError("reported RDMA address differs from operator config")
        with self.lock:
            self.reports[name] = dpu_public_key

    def names(self) -> set[str]:
        return set(read_nodes_file(self.path))

    def lines(self) -> list[str]:
        """`node=` records for the generation. A configured node whose DPU has
        not reported yet is published with its placeholder key: it can be named
        by `pod=` lines and refused as a peer, which is the honest state."""
        configured = read_nodes_file(self.path)
        with self.lock:
            reports = dict(self.reports)
        lines: list[str] = []
        for name in sorted(configured):
            rdma, key_id, grant_pub, dpu_pub = configured[name]
            reported = reports.get(name)
            if reported is not None:
                dpu_pub = reported
            lines.append(f"node={name},{rdma},{key_id},{grant_pub},{dpu_pub}")
        return lines


class KubernetesAPI:
    """Minimal read-only, in-cluster client with cluster-wide list scope."""

    def __init__(self, server: str, token_file: Path, ca_file: Path) -> None:
        self.server = server.rstrip("/")
        self.token_file = token_file
        self.context = ssl.create_default_context(cafile=str(ca_file))

    def items(self, path: str) -> list[dict[str, Any]]:
        url = f"{self.server}{path}"
        try:
            token = self.token_file.read_text(encoding="ascii").strip()
            request = urllib.request.Request(
                url,
                headers={"Authorization": f"Bearer {token}", "Accept": "application/json"},
            )
            with urllib.request.urlopen(request, context=self.context, timeout=10) as response:
                document = json.load(response)
        except (OSError, UnicodeDecodeError, urllib.error.URLError, json.JSONDecodeError) as exc:
            raise ControllerError(f"failed to read Kubernetes {path}") from exc
        items = document.get("items")
        if not isinstance(items, list):
            raise ControllerError(f"Kubernetes {path} response has no item list")
        return items

    def pods(self) -> list[dict[str, Any]]:
        return self.items("/api/v1/pods")

    def services(self) -> list[dict[str, Any]]:
        return self.items("/api/v1/services")

    def endpoint_slices(self) -> list[dict[str, Any]]:
        return self.items("/apis/discovery.k8s.io/v1/endpointslices")

def build_body(
    version: int,
    node_lines: list[str],
    pods: list[dict[str, Any]],
    services: list[dict[str, Any]],
    slices: list[dict[str, Any]],
    protected: list[str],
    log=lambda message: None,
) -> str:
    lines = [f"version={version}"]
    if len(node_lines) > GEN_NODE_MAX:
        raise ControllerError(
            f"{len(node_lines)} nodes exceed GEN_NODE_MAX={GEN_NODE_MAX}; refused, not truncated"
        )
    lines.extend(node_lines)

    pod_uids: set[str] = set()
    for pod in pods:
        metadata = pod.get("metadata", {})
        spec = pod.get("spec", {})
        status = pod.get("status", {})
        uid = str(metadata.get("uid", ""))
        node = str(spec.get("nodeName") or "")
        namespace = str(metadata.get("namespace", ""))
        account = str(spec.get("serviceAccountName") or "default")
        ip = str(status.get("podIP") or "")
        if POD_UID_RE.fullmatch(uid) is None or not node or not valid_ipv4(ip):
            continue    # placements need a node and an IP; both arrive shortly
        if uid in pod_uids:
            continue
        pod_uids.add(uid)
        lines.append(f"pod={uid},{node},{namespace},{account},{ip}")
    if len(pod_uids) > GEN_POD_MAX:
        raise ControllerError(
            f"{len(pod_uids)} pods exceed GEN_POD_MAX={GEN_POD_MAX}; refused, not truncated"
        )

    service_keys: set[str] = set()
    for service in services:
        metadata = service.get("metadata", {})
        spec = service.get("spec", {})
        key = f'{metadata.get("namespace", "")}/{metadata.get("name", "")}'
        cluster_ip = str(spec.get("clusterIP") or "")
        ports = spec.get("ports") or []
        if not valid_ipv4(cluster_ip) or not ports:
            continue    # headless / ExternalName Services have no dialable IP
        port = ports[0].get("port")
        if not isinstance(port, int) or not 0 < port < 65536:
            continue
        service_keys.add(key)
        lines.append(f"service={key},{cluster_ip}:{port}")
    if len(service_keys) > GEN_SERVICE_MAX:
        raise ControllerError(
            f"{len(service_keys)} services exceed GEN_SERVICE_MAX={GEN_SERVICE_MAX};"
            " refused, not truncated"
        )

    seen_endpoints: set[tuple[str, str]] = set()
    for endpoint_slice in slices:
        metadata = endpoint_slice.get("metadata", {})
        namespace = metadata.get("namespace", "")
        service_name = (metadata.get("labels") or {}).get("kubernetes.io/service-name")
        key = f"{namespace}/{service_name}"
        if not service_name or key not in service_keys:
            continue
        for endpoint in endpoint_slice.get("endpoints") or []:
            if (endpoint.get("conditions") or {}).get("ready") is not True:
                continue
            target = endpoint.get("targetRef") or {}
            uid = str(target.get("uid", ""))
            if target.get("kind") != "Pod" or uid not in pod_uids:
                if uid:
                    log(f"skipping endpoint {key} -> {uid}: pod not placed yet")
                continue
            if (key, uid) in seen_endpoints:
                continue
            seen_endpoints.add((key, uid))
            lines.append(f"endpoint={key},{uid}")
    if len(seen_endpoints) > GEN_ENDPOINT_MAX:
        raise ControllerError(
            f"{len(seen_endpoints)} endpoints exceed GEN_ENDPOINT_MAX={GEN_ENDPOINT_MAX};"
            " refused, not truncated"
        )

    for key in protected:
        if key not in service_keys:
            log(f"protected Service {key} is not in this generation; dropped")
            continue
        lines.append(f"protected={key}")

    return "".join(line + "\n" for line in lines)


def sign_document(body: str, key_id: str, key: bytes) -> str:
    signature = Ed25519PrivateKey.from_private_bytes(key).sign(body.encode("ascii"))
    return f"{body}signature={key_id},{signature.hex()}\n"


def sign_feed(body: str, key_id: str, key: bytes) -> str:
    signature = hmac.new(key, body.encode("ascii"), hashlib.sha256).hexdigest()
    return f"{body}signature={key_id},{signature}\n"


def membership_body(version: int, node_name: str,
                    pods: list[dict[str, Any]],
                    services: list[dict[str, Any]],
                    endpoint_slices: list[dict[str, Any]],
                    resource_name: str) -> str:
    """Controller-authorized (Pod UID, Service) pairs for exactly one node."""
    lines = [f"version={version}"]
    for pod in sorted(pods, key=lambda item: str(item.get("metadata", {}).get("uid") or "")):
        metadata = pod.get("metadata", {}) or {}
        if (metadata.get("deletionTimestamp") or
                pod.get("spec", {}).get("nodeName") != node_name or
                workload_grant.POD_UID_RE.fullmatch(str(metadata.get("uid") or "")) is None or
                not workload_grant.service_account_token_disabled(pod)):
            continue
        try:
            target = workload_grant.resource_target(pod, resource_name)
            workload_grant.running_container_id(pod, str(target.get("name") or ""))
            workload_grant.pod_ipv4(pod)
        except workload_grant.GrantError:
            continue
        uid = str(metadata["uid"])
        lines.append(f"member={uid},-")
        names = {
            str(service.get("metadata", {}).get("name") or "")
            for service in services
            if service.get("metadata", {}).get("namespace") == metadata.get("namespace")
        }
        for name in sorted(names):
            if workload_grant.SERVICE_NAME_RE.fullmatch(name) is None:
                continue
            try:
                workload_grant.authorize_service(name, pod, services)
                workload_grant.require_ready_endpoint(name, pod, endpoint_slices)
            except workload_grant.GrantError:
                continue
            lines.append(f"member={uid},{name}")
    return "".join(line + "\n" for line in lines)


def service_targets_body(version: int, topology: str) -> str:
    """Build the Linkerd adapter's Service map from one signed topology."""
    services: dict[str, str] = {}
    pod_ips: dict[str, str] = {}
    endpoints: list[tuple[str, str]] = []
    for line in topology.splitlines():
        if line.startswith("service="):
            key, separator, address = line[len("service="):].partition(",")
            if separator:
                services[key] = address
        elif line.startswith("pod="):
            fields = line[len("pod="):].split(",")
            if len(fields) == 5:
                pod_ips[fields[0]] = fields[4]
        elif line.startswith("endpoint="):
            key, separator, uid = line[len("endpoint="):].partition(",")
            if separator:
                endpoints.append((key, uid))
        elif line.startswith("signature="):
            break
    lines = [f"version={version}"]
    for key in sorted(services):
        lines.append(f"{key}={services[key]}")
    seen: set[tuple[str, str]] = set()
    for key, uid in sorted(endpoints):
        address = services.get(key)
        ip = pod_ips.get(uid)
        if address is None or ip is None:
            continue
        port = address.rpartition(":")[2]
        endpoint = (key, ip)
        if endpoint in seen:
            continue
        seen.add(endpoint)
        lines.append(f"endpoint={key},{ip}:{port},{uid}")
    return "".join(line + "\n" for line in lines)


def published_version(path: Path) -> int:
    try:
        for line in path.read_text(encoding="ascii").splitlines():
            if line.startswith("version="):
                return int(line[len("version="):])
    except (OSError, UnicodeDecodeError, ValueError):
        return 0
    return 0


def signature_key_id(document: str) -> str:
    for line in document.splitlines():
        if line.startswith("signature="):
            return line[len("signature="):].partition(",")[0]
    return ""


def generation_records(document: str) -> str:
    """The record lines of a body or signed document — version, signature and
    comments stripped. Two generations that agree here say the same thing, so
    publishing the newer one would only make every DPU re-adopt what it holds."""
    return "\n".join(
        line for line in document.splitlines()
        if not line.startswith(("version=", "signature=", "#"))
    )


def pod_placements(document: str) -> dict[str, str]:
    """Pod UID -> node name, read back from a published generation. The
    mediated lookup answers from the same document every DPU holds, so the
    controller and the DPU cannot disagree about where a Pod is."""
    placements: dict[str, str] = {}
    for line in document.splitlines():
        if line.startswith("pod="):
            fields = line[len("pod="):].split(",")
            if len(fields) == 5:
                placements[fields[0]] = fields[1]
        elif line.startswith("signature="):
            break
    return placements


class Controller:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        load_active_key(args.key_dir)   # refuse to start without a signing key
        self.kubernetes = KubernetesAPI(args.api_server, args.api_token_file, args.api_ca_file)
        self.nodes = NodeRegistry(args.nodes_file)
        # The consumer refuses a generation that is not newer than the one it
        # holds, so publication continues from whatever is already installed.
        self.version = published_version(args.output)
        # What the listener serves. Held whole rather than re-read, so a
        # publication that fails leaves the last good generation being served.
        # A restart re-seeds it from the installed file: an unchanged cluster
        # publishes nothing, and the listener must not answer 503 meanwhile.
        self.state_lock = threading.Lock()
        self.document = ""
        self.placements: dict[str, str] = {}
        self.pod_snapshot: list[dict[str, Any]] = []
        self.service_snapshot: list[dict[str, Any]] = []
        self.slice_snapshot: list[dict[str, Any]] = []
        self.membership_cache: dict[str, tuple[str, str, str, int]] = {}
        self.service_targets_cache: tuple[str, str, str, int] | None = None
        try:
            installed = args.output.read_text(encoding="ascii")
        except (OSError, UnicodeDecodeError):
            installed = ""
        if installed:
            self.document = installed
            self.placements = pod_placements(installed)

    def publish(self) -> int | None:
        """Publish a new generation, or return None when facts are unchanged."""
        version = max(time.time_ns(), self.version + 1)
        node_lines = self.nodes.lines()
        pods = self.kubernetes.pods()
        services = self.kubernetes.services()
        slices = self.kubernetes.endpoint_slices()
        body = build_body(
            version,
            node_lines,
            pods,
            services,
            slices,
            self.args.protected,
            log=lambda message: print(f"dpumesh-controller: {message}", file=sys.stderr, flush=True),
        )
        key_id, key = load_active_key(self.args.key_dir)
        with self.state_lock:
            held = self.document
        # A rotated signing key republishes even an unchanged cluster: the
        # held document must never outlive the key that signed it.
        if held and generation_records(body) == generation_records(held) \
                and signature_key_id(held) == key_id:
            with self.state_lock:
                self.pod_snapshot = pods
                self.service_snapshot = services
                self.slice_snapshot = slices
            return None
        document = sign_document(body, key_id, key)
        if len(document) > TOPOLOGY_MAX_BYTES:
            raise ControllerError(
                f"generation is {len(document)} bytes, over TOPOLOGY_MAX_BYTES="
                f"{TOPOLOGY_MAX_BYTES}; refused, not truncated"
            )
        path = self.args.output
        path.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
        temporary = path.with_name(f".{path.name}.new")
        temporary.write_text(document, encoding="ascii")
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
        self.version = version
        with self.state_lock:
            self.document = document
            self.placements = pod_placements(document)
            self.pod_snapshot = pods
            self.service_snapshot = services
            self.slice_snapshot = slices
        return version

    def held(self) -> tuple[str, dict[str, str]]:
        with self.state_lock:
            return self.document, dict(self.placements)

    def objects(self) -> tuple[
        list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]]
    ]:
        with self.state_lock:
            return (
                list(self.pod_snapshot),
                list(self.service_snapshot),
                list(self.slice_snapshot),
            )

    def issue_workload_grant(self, node_name: str, request: dict[str, Any]) -> bytes:
        try:
            pod_uid = str(request["pod_uid"])
            container_id = str(request["container_id"])
            service_name = str(request.get("service") or "")
            nonce = bytes.fromhex(str(request["nonce"]))
            channel_slot = int(request["slot"])
            channel_generation = int(request["generation"])
            daemon_incarnation = bytes.fromhex(str(request["daemon_incarnation"]))
        except (KeyError, TypeError, ValueError) as exc:
            raise workload_grant.GrantError("malformed workload grant request") from exc
        if workload_grant.POD_UID_RE.fullmatch(pod_uid) is None:
            raise workload_grant.GrantError("malformed Pod UID")
        pods, services, endpoint_slices = self.objects()
        pod = workload_grant.resolve_authorized_pod(
            pod_uid=pod_uid,
            node_name=node_name,
            container_id=container_id,
            service_name=service_name,
            pods=pods,
            services=services,
            endpoint_slices=endpoint_slices,
            resource_name=self.args.resource_name,
        )
        container = workload_grant.resource_target(pod, self.args.resource_name)
        key_id, key = workload_grant.load_private_seed(
            self.args.registration_key_dir, node_name, load_active_key
        )
        configured = read_nodes_file(self.args.nodes_file).get(node_name)
        if configured is None or configured[1] != key_id:
            raise workload_grant.GrantError(
                "registration signing key does not match the configured node key id"
            )
        return workload_grant.build_grant(
            key=key,
            key_id=key_id,
            cluster_id=self.args.cluster_id,
            service_name=service_name,
            nonce=nonce,
            pod=pod,
            container=container,
            container_id=container_id,
            channel_slot=channel_slot,
            channel_generation=channel_generation,
            daemon_incarnation=daemon_incarnation,
            ttl=self.args.grant_ttl,
        )

    def membership(self, node_name: str) -> str:
        # Compare only semantic records. A fetch does not advance a generation
        # unless membership changed or the signing key rotated.
        key_id, key = load_active_key(self.args.feed_key_dir)
        with self.state_lock:
            records = membership_body(
                1, node_name, self.pod_snapshot, self.service_snapshot,
                self.slice_snapshot, self.args.resource_name,
            ).partition("\n")[2]
            held = self.membership_cache.get(node_name)
            if held is not None and held[0] == records and held[1] == key_id:
                return held[2]
            previous = 0 if held is None else held[3]
            version = max(time.time_ns(), previous + 1)
            document = sign_feed(f"version={version}\n{records}", key_id, key)
            if len(document) > MEMBERSHIP_MAX_BYTES:
                raise ControllerError("node membership exceeds its protocol bound")
            self.membership_cache[node_name] = (records, key_id, document, version)
            return document

    def service_targets(self) -> str:
        key_id, key = load_active_key(self.args.feed_key_dir)
        with self.state_lock:
            if not self.document:
                raise ControllerError("no topology generation is available")
            records = service_targets_body(1, self.document).partition("\n")[2]
            held = self.service_targets_cache
            if held is not None and held[0] == records and held[1] == key_id:
                return held[2]
            previous = 0 if held is None else held[3]
            version = max(time.time_ns(), previous + 1)
            document = sign_feed(f"version={version}\n{records}", key_id, key)
            if len(document) > SERVICE_TARGETS_MAX_BYTES:
                raise ControllerError("Service target feed exceeds its protocol bound")
            self.service_targets_cache = (records, key_id, document, version)
            return document

    def run(self) -> None:
        while True:
            try:
                version = self.publish()
                if version is not None:
                    print(f"dpumesh-controller: generation {version}", flush=True)
            except (ControllerError, OSError) as exc:
                print(f"dpumesh-controller: publish failed: {exc}", file=sys.stderr, flush=True)
            time.sleep(self.args.interval)


class ControllerHandler(http.server.BaseHTTPRequestHandler):
    """The mTLS API exposed to configured node runtimes.

    `GET /topology.v1` is the generation each runtime delivers to its DPU.
    `POST /node` reports the DPU key for the runtime's node.
    `GET /workload-scope` is the mediated lookup of *Scope of the control-plane
    credential*: it answers only for Pods the generation places on the asking
    node, so a DPU asking about a Pod somewhere else is refused by the
    component that already binds Pods to nodes.
    """

    protocol_version = "HTTP/1.1"
    server_version = "dpumesh-controller"

    def setup(self) -> None:
        self.request.settimeout(CONTROLLER_REQUEST_TIMEOUT)
        super().setup()

    def log_message(self, _format: str, *_args: Any) -> None:
        return                                  # one line per request is noise at the delivery cadence

    def reply(self, status: int, body: bytes, content_type: str = "text/plain") -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def reporter(self) -> str | None:
        """The node the caller may speak for, or None."""
        node = node_from_peer_certificate(self.connection)
        return node if node in self.server.controller.nodes.names() else None

    def do_GET(self) -> None:                   # noqa: N802 - BaseHTTPRequestHandler
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/healthz":
            document, _placements = self.server.controller.held()
            self.reply(200 if document else 503, b"ok\n" if document else b"not ready\n")
            return
        if parsed.path == "/topology.v1":
            if self.reporter() is None:
                self.reply(403, b"a node client certificate is required\n")
                return
            document, _placements = self.server.controller.held()
            if not document:
                self.reply(503, b"no generation published yet\n")
                return
            self.reply(200, document.encode("ascii"))
            return
        if parsed.path == "/membership.v1":
            node = self.reporter()
            if node is None:
                self.reply(403, b"a configured node client certificate is required\n")
                return
            try:
                document = self.server.controller.membership(node)
            except (ControllerError, OSError) as exc:
                self.reply(503, f"{exc}\n".encode("ascii", "replace"))
                return
            self.reply(200, document.encode("ascii"))
            return
        if parsed.path == "/service-targets.v1":
            if self.reporter() is None:
                self.reply(403, b"a configured node client certificate is required\n")
                return
            try:
                document = self.server.controller.service_targets()
            except (ControllerError, OSError) as exc:
                self.reply(503, f"{exc}\n".encode("ascii", "replace"))
                return
            self.reply(200, document.encode("ascii"))
            return
        if parsed.path == "/workload-scope":
            self.workload_scope(urllib.parse.parse_qs(parsed.query))
            return
        self.reply(404, b"no such route\n")

    def workload_scope(self, query: dict[str, list[str]]) -> None:
        node = self.reporter()
        if node is None:
            self.reply(403, b"caller is not a known node\n")
            return
        uids = query.get("pod_uid") or []
        if len(uids) != 1 or POD_UID_RE.fullmatch(uids[0]) is None:
            self.reply(400, b"one well-formed pod_uid is required\n")
            return
        _document, placements = self.server.controller.held()
        placed = placements.get(uids[0])
        if placed is None:
            self.reply(404, b"no generation places that Pod\n")
            return
        if placed != node:
            self.reply(403, b"the generation places that Pod on another node\n")
            return
        self.reply(200, json.dumps({"pod_uid": uids[0], "node": node}).encode("ascii"),
                   "application/json")

    def do_POST(self) -> None:                  # noqa: N802 - BaseHTTPRequestHandler
        path = urllib.parse.urlparse(self.path).path
        if path == "/workload-grant":
            self.workload_grant()
            return
        if path != "/node":
            self.reply(404, b"no such route\n")
            return
        node = self.reporter()
        if node is None:
            self.reply(403, b"caller is not a known node\n")
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.reply(400, b"malformed Content-Length\n")
            return
        if not 0 < length <= NODE_REPORT_MAX:
            self.reply(413, b"report over bound\n")
            return
        try:
            report = json.loads(self.rfile.read(length))
            name = str(report["name"])
            rdma = str(report["rdma"])
            dpu_public_key = str(report["dpu_public_key"])
        except (json.JSONDecodeError, KeyError, TypeError, UnicodeDecodeError):
            self.reply(400, b"malformed report\n")
            return
        if name != node:
            self.reply(403, b"a node may report only for itself\n")
            return
        if name not in self.server.controller.nodes.names():
            self.reply(403, b"node is not configured\n")
            return
        try:
            self.server.controller.nodes.report(name, rdma, dpu_public_key)
        except ControllerError as exc:
            self.reply(400, f"{exc}\n".encode("ascii", "replace"))
            return
        print(f"dpumesh-controller: node {name} reported {rdma} {dpu_public_key[:16]}...",
              flush=True)
        self.reply(200, b"ok\n")

    def workload_grant(self) -> None:
        node = self.reporter()
        if node is None:
            self.reply(403, b"a node client certificate is required\n")
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.reply(400, b"malformed Content-Length\n")
            return
        if not 0 < length <= WORKLOAD_GRANT_REQUEST_MAX:
            self.reply(413, b"grant request over bound\n")
            return
        try:
            request = json.loads(self.rfile.read(length))
            if not isinstance(request, dict):
                raise TypeError
            grant = self.server.controller.issue_workload_grant(node, request)
        except (json.JSONDecodeError, UnicodeDecodeError, TypeError) as exc:
            self.reply(400, f"malformed grant request: {exc}\n".encode("ascii", "replace"))
            return
        except workload_grant.GrantError as exc:
            self.reply(403, f"{exc}\n".encode("ascii", "replace"))
            return
        except (ControllerError, OSError) as exc:
            self.reply(503, f"{exc}\n".encode("ascii", "replace"))
            return
        self.reply(200, grant, "application/octet-stream")


def node_from_peer_certificate(connection: Any) -> str | None:
    """Return the node named by ``spiffe://dpumesh.io/node/<name>``."""
    try:
        certificate = connection.getpeercert()
    except (AttributeError, ValueError, ssl.SSLError):
        return None
    # With CERT_OPTIONAL, a probe/client that supplies no certificate returns
    # None here.  Treat that as an unauthenticated caller; never let it escape
    # as a handler exception and an ambiguous EOF.
    if not isinstance(certificate, dict):
        return None
    prefix = "spiffe://dpumesh.io/node/"
    uris = [
        value
        for kind, value in certificate.get("subjectAltName", ())
        if kind == "URI"
    ]
    if len(uris) != 1 or not uris[0].startswith(prefix):
        return None
    name = uris[0][len(prefix):]
    return name if DNS_RE.fullmatch(name) is not None else None


class ControllerServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address: tuple[str, int], controller: Controller) -> None:
        self.controller = controller
        self.request_slots = threading.BoundedSemaphore(CONTROLLER_REQUEST_MAX)
        super().__init__(address, ControllerHandler)

    def process_request(self, request: socket.socket,
                        client_address: tuple[str, int]) -> None:
        if not self.request_slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.request_slots.release()
            raise

    def process_request_thread(self, request: socket.socket,
                               client_address: tuple[str, int]) -> None:
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.request_slots.release()


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--key-dir", type=Path, required=True)
    parser.add_argument("--registration-key-dir", type=Path, required=True,
                        help="per-node Ed25519 grant key directories")
    parser.add_argument("--feed-key-dir", type=Path, required=True,
                        help="controller-only HMAC keys for node-scoped feeds")
    parser.add_argument("--nodes-file", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("/run/dpumesh/topology.v1"))
    parser.add_argument("--interval", type=float, default=GENERATION_INTERVAL)
    parser.add_argument(
        "--protected", action="append", default=[],
        metavar="NAMESPACE/NAME",
        help="Service carrying the strict interaction rules (repeatable)",
    )
    parser.add_argument("--listen", default="0.0.0.0",
                        help="address node runtimes reach over mTLS")
    parser.add_argument("--listen-port", type=int, default=8080)
    parser.add_argument("--tls-cert", type=Path, required=True)
    parser.add_argument("--tls-key", type=Path, required=True)
    parser.add_argument("--client-ca", type=Path, required=True)
    parser.add_argument("--resource-name", default="dpumesh.io/channel")
    parser.add_argument("--cluster-id", required=True)
    parser.add_argument("--grant-ttl", type=int, default=60)
    parser.add_argument("--api-server", default="https://kubernetes.default.svc")
    parser.add_argument(
        "--api-token-file", type=Path,
        default=Path("/var/run/secrets/kubernetes.io/serviceaccount/token"),
    )
    parser.add_argument(
        "--api-ca-file", type=Path,
        default=Path("/var/run/secrets/kubernetes.io/serviceaccount/ca.crt"),
    )
    args = parser.parse_args(argv)
    if not 1 <= args.interval <= 300:
        parser.error("--interval must be between 1 and 300 seconds")
    if not 1 <= args.listen_port <= 65535:
        parser.error("--listen-port out of range")
    if not 1 <= args.grant_ttl <= workload_grant.MAX_TTL:
        parser.error(f"--grant-ttl must be between 1 and {workload_grant.MAX_TTL}")
    if (len(args.cluster_id) > 63 or
            workload_grant.CLUSTER_ID_RE.fullmatch(args.cluster_id) is None):
        parser.error("--cluster-id must be a DNS subdomain of at most 63 bytes")
    for key in args.protected:
        fields = key.split("/")
        if (len(fields) != 2 or any(
                workload_grant.SERVICE_NAME_RE.fullmatch(field) is None
                for field in fields)):
            parser.error(f"--protected takes namespace/name, got {key!r}")
    return args


def main() -> int:
    args = parse_args()
    controller = Controller(args)
    # Publish once before the listener opens, so the first fetch is
    # answered with a generation rather than with the fail-static 503.
    try:
        controller.publish()
    except (ControllerError, OSError) as exc:
        print(f"dpumesh-controller: first publish failed: {exc}", file=sys.stderr, flush=True)
    server = ControllerServer((args.listen, args.listen_port), controller)
    context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    context.load_cert_chain(str(args.tls_cert), str(args.tls_key))
    context.load_verify_locations(cafile=str(args.client_ca))
    # Health probes need no certificate; every data-bearing route checks the
    # URI SAN after the TLS handshake.
    context.verify_mode = ssl.CERT_OPTIONAL
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    server.socket = context.wrap_socket(
        server.socket, server_side=True, do_handshake_on_connect=False,
    )
    threading.Thread(target=server.serve_forever, name="listener", daemon=True).start()
    print(f"dpumesh-controller: serving node mTLS on {args.listen}:{args.listen_port}",
          flush=True)
    controller.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
