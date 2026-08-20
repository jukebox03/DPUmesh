#!/usr/bin/env python3
"""DPUmesh cluster controller.

Publishes one signed, versioned topology generation carrying every
cluster-wide fact a DPU needs: node identities and keys, Pod placements,
Services with their ClusterIPs, ready endpoints, and the protected-Service
set. It performs no attestation — it has no host-local evidence and never
asks for it. The generation is Ed25519-signed; DPUs hold public keys only.

The document grammar is design/CLUSTER.md's, one record per line:

    version=<u64, strictly increasing>
    node=<name>,<rdma-ip>:<port>,<agent-key-id>,<agent-pub-hex64>,<dpu-pub-hex64>
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

The controller never speaks to a DPU: a DPU has no route into the cluster
CIDRs, so its node agent relays. The listener is therefore addressed to
agents, and serves three things — the current generation, the node
registration each agent reports for its own node, and the mediated workload
lookup that keeps a DPU's questions inside the node the generation places it
on.
"""

from __future__ import annotations

import argparse
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

GENERATION_INTERVAL = 5.0
# The consumer's generation bounds (doca/topology.h), enforced at the
# publisher too so an over-bound cluster fails loudly here instead of
# publishing a document every DPU refuses.
GEN_NODE_MAX = 1024
GEN_POD_MAX = 65536
GEN_SERVICE_MAX = 4096
GEN_ENDPOINT_MAX = 65536
TOPOLOGY_MAX_BYTES = 16 * 1024 * 1024
# A node registration is five short fields; nothing an agent reports is large.
NODE_REPORT_MAX = 4096
ZERO_KEY = "0" * 64
POD_UID_RE = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
KEY_ID_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,30}")
DNS_RE = re.compile(r"[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)*")
HEX64_RE = re.compile(r"[0-9a-f]{64}")


class ControllerError(RuntimeError):
    pass


def load_key(path: Path) -> bytes:
    st = path.lstat()
    if (
        not stat.S_ISREG(st.st_mode)
        or st.st_uid != os.geteuid()
        or st.st_mode & 0o077
        or not st.st_mode & stat.S_IRUSR
    ):
        raise ControllerError(
            f"{path} must be a regular file owned by uid {os.geteuid()} with mode 0600/0400"
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


def valid_node_record(name: str, rdma: str, key_id: str, agent_pub: str, dpu_pub: str) -> bool:
    return (
        DNS_RE.fullmatch(name) is not None and len(name) <= 253
        and valid_rdma(rdma)
        and KEY_ID_RE.fullmatch(key_id) is not None
        and HEX64_RE.fullmatch(agent_pub) is not None
        and HEX64_RE.fullmatch(dpu_pub) is not None
    )


def read_nodes_file(path: Path) -> dict[str, tuple[str, str, str, str]]:
    """The operator's per-node input, keyed by node name:
    `<node-name> <rdma-ip:port> <agent-key-id> <agent-pub-hex64> <dpu-pub-hex64>`.

    The agent key is deployment-time material and stays that way — it is the
    key that binds Pods to this node, and a node reporting its own would be
    reporting its own identity. What an agent may report is the half its DPU
    generates at first boot, which is the `dpu-pub` placeholder here.
    """
    records: dict[str, tuple[str, str, str, str]] = {}
    for line_no, raw in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) != 5:
            raise ControllerError(f"{path}:{line_no}: expected 5 fields")
        name, rdma, key_id, agent_pub, dpu_pub = fields
        if not valid_node_record(name, rdma, key_id, agent_pub, dpu_pub):
            raise ControllerError(f"{path}:{line_no}: malformed node record")
        records[name] = (rdma, key_id, agent_pub, dpu_pub)
    return records


class NodeRegistry:
    """The node set, and the half of it the agents report.

    The file is the anchor: a node the operator did not configure is not
    published, so a report can add nothing. What a report supplies is the DPU
    static handshake key its node generated at first boot and the transport
    address it listens on — the two facts no operator can know in advance and
    the peer-channel handshake refuses a channel without.
    """

    def __init__(self, path: Path) -> None:
        self.path = path
        self.reports: dict[str, tuple[str, str]] = {}
        self.lock = threading.Lock()

    def report(self, name: str, rdma: str, dpu_public_key: str) -> None:
        if not valid_rdma(rdma) or HEX64_RE.fullmatch(dpu_public_key) is None:
            raise ControllerError("malformed node report")
        if dpu_public_key == ZERO_KEY:
            raise ControllerError("a node may not report an all-zero static key")
        with self.lock:
            self.reports[name] = (rdma, dpu_public_key)

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
            rdma, key_id, agent_pub, dpu_pub = configured[name]
            reported = reports.get(name)
            if reported is not None:
                rdma, dpu_pub = reported
            lines.append(f"node={name},{rdma},{key_id},{agent_pub},{dpu_pub}")
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

    def nodes(self) -> list[dict[str, Any]]:
        return self.items("/api/v1/nodes")


def ipv4_int(address: str) -> int | None:
    if not valid_ipv4(address):
        return None
    a, b, c, d = (int(part) for part in address.split("."))
    return (a << 24) | (b << 16) | (c << 8) | d


def parse_cidr(cidr: str) -> tuple[int, int] | None:
    prefix, separator, bits = cidr.partition("/")
    base = ipv4_int(prefix)
    if not separator or base is None or not bits.isdigit():
        return None
    width = int(bits)
    if not 0 <= width <= 32:
        return None
    mask = 0 if width == 0 else (0xFFFFFFFF << (32 - width)) & 0xFFFFFFFF
    return base & mask, mask


class NodeBinding:
    """Which node a request came from.

    This is what binds a report to its reporter, and a node agent may speak
    only for the node it runs on. Two facts decide it, and both are the
    cluster's rather than the caller's: the addresses Kubernetes records for a
    node, and the Pod CIDR it allocates to that node. The second is needed
    because a host-network agent reaching a ClusterIP is source-translated to
    its node's CNI address, which is inside that node's Pod CIDR and inside no
    other's.
    """

    def __init__(self, nodes: list[dict[str, Any]]) -> None:
        self.exact: dict[str, str] = {}
        self.ranges: list[tuple[int, int, str]] = []
        for node in nodes:
            name = str(node.get("metadata", {}).get("name") or "")
            if not name:
                continue
            for address in node.get("status", {}).get("addresses") or []:
                value = str(address.get("address") or "")
                if valid_ipv4(value):
                    self.exact[value] = name
            spec = node.get("spec", {}) or {}
            for cidr in [spec.get("podCIDR")] + list(spec.get("podCIDRs") or []):
                parsed = parse_cidr(str(cidr or ""))
                if parsed is not None:
                    self.ranges.append((parsed[0], parsed[1], name))

    def of(self, address: str) -> str | None:
        name = self.exact.get(address)
        if name is not None:
            return name
        value = ipv4_int(address)
        if value is None:
            return None
        for base, mask, node in self.ranges:
            if value & mask == base:
                return node
        return None


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
            if (endpoint.get("conditions") or {}).get("ready") is False:
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
        self.binding = NodeBinding([])
        try:
            installed = args.output.read_text(encoding="ascii")
        except (OSError, UnicodeDecodeError):
            installed = ""
        if installed:
            self.document = installed
            self.placements = pod_placements(installed)

    def publish(self) -> int | None:
        """Publish a new generation, or return None when the cluster's facts
        are unchanged — the node binding still refreshes, because node
        addresses are not generation content."""
        version = max(time.time_ns(), self.version + 1)
        node_lines = self.nodes.lines()
        body = build_body(
            version,
            node_lines,
            self.kubernetes.pods(),
            self.kubernetes.services(),
            self.kubernetes.endpoint_slices(),
            self.args.protected,
            log=lambda message: print(f"dpumesh-controller: {message}", file=sys.stderr, flush=True),
        )
        binding = NodeBinding(self.kubernetes.nodes())
        key_id, key = load_active_key(self.args.key_dir)
        with self.state_lock:
            held = self.document
        # A rotated signing key republishes even an unchanged cluster: the
        # held document must never outlive the key that signed it.
        if held and generation_records(body) == generation_records(held) \
                and signature_key_id(held) == key_id:
            with self.state_lock:
                self.binding = binding
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
            self.binding = binding
        return version

    def held(self) -> tuple[str, dict[str, str], NodeBinding]:
        with self.state_lock:
            return self.document, self.placements, self.binding

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
    """The face the node agents see. Three routes and no more.

    `GET /topology.v1` is the generation every agent delivers to its DPU.
    `POST /node` is the report an agent makes for its own node.
    `GET /workload-scope` is the mediated lookup of *Scope of the control-plane
    credential*: it answers only for Pods the generation places on the asking
    node, so a DPU asking about a Pod somewhere else is refused by the
    component that already binds Pods to nodes.
    """

    protocol_version = "HTTP/1.1"
    server_version = "dpumesh-controller"

    def log_message(self, fmt: str, *args: Any) -> None:
        pass                                    # one line per request is noise at 5 s

    def reply(self, status: int, body: bytes, content_type: str = "text/plain") -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def reporter(self) -> str | None:
        """The node the caller may speak for, or None."""
        _document, _placements, binding = self.server.controller.held()
        return binding.of(self.client_address[0])

    def do_GET(self) -> None:                   # noqa: N802 - BaseHTTPRequestHandler
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/healthz":
            self.reply(200, b"ok\n")
            return
        if parsed.path == "/topology.v1":
            document, _placements, _binding = self.server.controller.held()
            if not document:
                self.reply(503, b"no generation published yet\n")
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
        _document, placements, _binding = self.server.controller.held()
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
        if urllib.parse.urlparse(self.path).path != "/node":
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


class ControllerServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address: tuple[str, int], controller: Controller) -> None:
        self.controller = controller
        super().__init__(address, ControllerHandler)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--key-dir", type=Path, required=True)
    parser.add_argument("--nodes-file", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("/run/dpumesh/topology.v1"))
    parser.add_argument("--interval", type=float, default=GENERATION_INTERVAL)
    parser.add_argument(
        "--protected", action="append", default=[],
        metavar="NAMESPACE/NAME",
        help="Service carrying the strict interaction rules (repeatable)",
    )
    parser.add_argument("--listen", default="0.0.0.0",
                        help="address the node agents reach the controller on")
    parser.add_argument("--listen-port", type=int, default=8080)
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
    for key in args.protected:
        if len(key.split("/")) != 2:
            parser.error(f"--protected takes namespace/name, got {key!r}")
    return args


def main() -> int:
    args = parse_args()
    controller = Controller(args)
    # Publish once before the listener opens, so an agent's first fetch is
    # answered with a generation rather than with the fail-static 503.
    try:
        controller.publish()
    except (ControllerError, OSError) as exc:
        print(f"dpumesh-controller: first publish failed: {exc}", file=sys.stderr, flush=True)
    server = ControllerServer((args.listen, args.listen_port), controller)
    threading.Thread(target=server.serve_forever, name="listener", daemon=True).start()
    print(f"dpumesh-controller: serving agents on {args.listen}:{args.listen_port}", flush=True)
    controller.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
