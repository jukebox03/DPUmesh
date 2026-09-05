"""Canonical controller-signed v3 WorkloadGrant.

The controller, rather than a node process, owns the Ed25519 private key.  A
node presents kernel-derived Pod/container evidence over mTLS; this module
encodes the latest Kubernetes object snapshot that the controller resolved. The fixed
layout is shared with ``struct dmesh_workload_assert_msg``.
"""

from __future__ import annotations

import re
import secrets
import socket
import struct
import time
from pathlib import Path
from typing import Any

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


ASSERT_VERSION = 3
MSG_WORKLOAD_ASSERT = 13
NONCE_SIZE = 32
MAX_TTL = 300
MAX_CHANNEL_SLOTS = 127
ASSERT = struct.Struct(
    "<BBBBQQ16s32sIQ16s32s64s254s64s64s254s254s254s65s64s16s64s"
)
POD_UID_RE = re.compile(
    r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"
)
SERVICE_NAME_RE = re.compile(r"[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?")
CLUSTER_ID_RE = re.compile(
    r"[a-z0-9](?:[a-z0-9-]*[a-z0-9])?"
    r"(?:\.[a-z0-9](?:[a-z0-9-]*[a-z0-9])?)*"
)

assert ASSERT.size == 1545


class GrantError(RuntimeError):
    pass


def fixed_text(value: str, size: int, field: str, *, allow_empty: bool = False) -> bytes:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as exc:
        raise GrantError(f"{field} is not ASCII") from exc
    if (not encoded and not allow_empty) or len(encoded) >= size or b"\0" in encoded:
        raise GrantError(f"{field} does not fit canonical field size {size}")
    return encoded + bytes(size - len(encoded))


def pod_ipv4(pod: dict[str, Any]) -> str:
    address = str(pod.get("status", {}).get("podIP") or "")
    try:
        packed = socket.inet_pton(socket.AF_INET, address)
    except OSError as exc:
        raise GrantError("Pod has no usable IPv4 address") from exc
    if socket.inet_ntop(socket.AF_INET, packed) != address:
        raise GrantError("Pod has no canonical IPv4 address")
    return address


def resource_target(pod: dict[str, Any], resource_name: str) -> dict[str, Any]:
    def one(value: Any) -> bool:
        return value == 1 or value == "1"

    targets: list[dict[str, Any]] = []
    for container in pod.get("spec", {}).get("containers") or []:
        resources = container.get("resources") or {}
        requests = resources.get("requests") or {}
        limits = resources.get("limits") or {}
        if one(requests.get(resource_name)) and one(limits.get(resource_name)):
            targets.append(container)
        elif resource_name in requests or resource_name in limits:
            raise GrantError(f"{resource_name} request and limit must both equal 1")
    if len(targets) != 1:
        raise GrantError(f"exactly one regular container must request {resource_name}=1")
    return targets[0]


def service_account_token_disabled(pod: dict[str, Any]) -> bool:
    spec = pod.get("spec", {}) or {}
    if spec.get("automountServiceAccountToken") is not False:
        return False
    for volume in spec.get("volumes") or []:
        projected = volume.get("projected") or {}
        for source in projected.get("sources") or []:
            if "serviceAccountToken" in source:
                return False
    return True


def running_container_id(pod: dict[str, Any], container_name: str) -> str:
    matches = []
    for status in pod.get("status", {}).get("containerStatuses") or []:
        if status.get("name") != container_name:
            continue
        value = str(status.get("containerID") or "")
        _runtime, separator, container_id = value.partition("://")
        if separator and container_id:
            matches.append(container_id)
    if len(matches) != 1:
        raise GrantError("target container has no unique running container ID")
    return matches[0]


def authorize_service(service_name: str, pod: dict[str, Any],
                      services: list[dict[str, Any]]) -> None:
    if not service_name:
        return
    if SERVICE_NAME_RE.fullmatch(service_name) is None:
        raise GrantError("requested Service name is malformed")
    metadata = pod.get("metadata", {}) or {}
    namespace = metadata.get("namespace")
    labels = metadata.get("labels") or {}
    candidates = [
        service for service in services
        if service.get("metadata", {}).get("namespace") == namespace
        and service.get("metadata", {}).get("name") == service_name
    ]
    if len(candidates) != 1:
        raise GrantError(f"Service {namespace}/{service_name} is not unique")
    spec = candidates[0].get("spec", {}) or {}
    selector = spec.get("selector") or {}
    if not selector or any(labels.get(key) != value for key, value in selector.items()):
        raise GrantError(f"Pod is not a member of {namespace}/{service_name}")
    try:
        cluster_ip = str(spec.get("clusterIP") or "")
        packed = socket.inet_pton(socket.AF_INET, cluster_ip)
    except OSError as exc:
        raise GrantError(f"Service {namespace}/{service_name} has no usable ClusterIP") from exc
    ports = spec.get("ports") or []
    port = ports[0].get("port") if ports else None
    if (
        socket.inet_ntop(socket.AF_INET, packed) != cluster_ip
        or not isinstance(port, int)
        or not 0 < port < 65536
    ):
        raise GrantError(f"Service {namespace}/{service_name} has no usable address")


def require_ready_endpoint(service_name: str, pod: dict[str, Any],
                           endpoint_slices: list[dict[str, Any]]) -> None:
    if not service_name:
        return
    metadata = pod.get("metadata", {}) or {}
    namespace = metadata.get("namespace")
    pod_uid = metadata.get("uid")
    for endpoint_slice in endpoint_slices:
        slice_metadata = endpoint_slice.get("metadata", {}) or {}
        if (slice_metadata.get("namespace") != namespace or
                (slice_metadata.get("labels") or {}).get(
                    "kubernetes.io/service-name"
                ) != service_name):
            continue
        for endpoint in endpoint_slice.get("endpoints") or []:
            if (endpoint.get("conditions") or {}).get("ready") is not True:
                continue
            target = endpoint.get("targetRef") or {}
            if target.get("kind") == "Pod" and target.get("uid") == pod_uid:
                return
    raise GrantError(f"Pod is not a ready endpoint of {namespace}/{service_name}")


def resolve_authorized_pod(
    *, pod_uid: str, node_name: str, container_id: str,
    service_name: str, pods: list[dict[str, Any]], services: list[dict[str, Any]],
    endpoint_slices: list[dict[str, Any]], resource_name: str,
) -> dict[str, Any]:
    matches = [pod for pod in pods if pod.get("metadata", {}).get("uid") == pod_uid]
    if len(matches) != 1:
        raise GrantError(f"Pod UID resolved to {len(matches)} snapshot objects")
    pod = matches[0]
    metadata = pod.get("metadata", {}) or {}
    spec = pod.get("spec", {}) or {}
    if metadata.get("deletionTimestamp"):
        raise GrantError("terminating Pod cannot receive a grant")
    if spec.get("nodeName") != node_name:
        raise GrantError("Pod is assigned to another node")
    if not service_account_token_disabled(pod):
        raise GrantError("workload ServiceAccount token must not be mounted")
    target = resource_target(pod, resource_name)
    expected = running_container_id(pod, str(target.get("name") or ""))
    if expected != container_id:
        raise GrantError("kernel container ID does not match the resource target")
    authorize_service(service_name, pod, services)
    require_ready_endpoint(service_name, pod, endpoint_slices)
    pod_ipv4(pod)
    return pod


def load_private_seed(key_dir: Path, node_name: str, key_loader) -> tuple[str, bytes]:
    directory = key_dir / node_name
    if not directory.is_dir():
        raise GrantError(f"no registration key directory for node {node_name}")
    return key_loader(directory)


def build_grant(*, key: bytes, key_id: str, cluster_id: str,
                service_name: str, nonce: bytes, pod: dict[str, Any],
                container: dict[str, Any], container_id: str,
                channel_slot: int, channel_generation: int,
                daemon_incarnation: bytes, ttl: int,
                now: int | None = None) -> bytes:
    if len(nonce) != NONCE_SIZE or not any(nonce):
        raise GrantError("DPU nonce must be nonzero and 32 bytes")
    if not 1 <= ttl <= MAX_TTL:
        raise GrantError("grant lifetime is out of range")
    if not 0 <= channel_slot < MAX_CHANNEL_SLOTS:
        raise GrantError("channel slot is out of range")
    if not 1 <= channel_generation < 2**64:
        raise GrantError("channel generation is out of range")
    if len(daemon_incarnation) != 16 or not any(daemon_incarnation):
        raise GrantError("daemon incarnation must be nonzero and 16 bytes")
    if len(cluster_id) > 63 or CLUSTER_ID_RE.fullmatch(cluster_id) is None:
        raise GrantError("cluster id must be a DNS subdomain of at most 63 bytes")
    if re.fullmatch(r"[0-9a-f]{64}", container_id) is None:
        raise GrantError("container ID is not canonical")
    metadata = pod.get("metadata", {}) or {}
    spec = pod.get("spec", {}) or {}
    issued = int(time.time()) - 1 if now is None else now
    fields = (
        MSG_WORKLOAD_ASSERT,
        ASSERT_VERSION,
        0,
        0,
        issued,
        issued + ttl,
        secrets.token_bytes(16),
        nonce,
        channel_slot,
        channel_generation,
        daemon_incarnation,
        fixed_text(key_id, 32, "key id"),
        fixed_text(cluster_id, 64, "cluster id"),
        fixed_text(str(spec.get("nodeName") or ""), 254, "node name"),
        fixed_text(str(metadata.get("uid") or ""), 64, "Pod UID"),
        fixed_text(str(metadata.get("namespace") or ""), 64, "namespace"),
        fixed_text(str(metadata.get("name") or ""), 254, "Pod name"),
        fixed_text(str(spec.get("serviceAccountName") or "default"), 254,
                   "ServiceAccount"),
        fixed_text(str(container.get("name") or ""), 254, "container name"),
        fixed_text(container_id, 65, "container ID"),
        fixed_text(service_name, 64, "Service name", allow_empty=True),
        fixed_text(pod_ipv4(pod), 16, "Pod IP"),
        bytes(64),
    )
    unsigned = ASSERT.pack(*fields)
    signature = Ed25519PrivateKey.from_private_bytes(key).sign(unsigned[:-64])
    return unsigned[:-64] + signature
