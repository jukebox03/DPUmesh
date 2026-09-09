"""Host-side workload eligibility. Readiness governs routing, not eligibility."""
from __future__ import annotations

import re
import socket
import struct
from typing import Any

ASSERT_VERSION = 3
MSG_WORKLOAD_ASSERT = 13
ASSERT = struct.Struct(
    "<BBBBQQ16s32sIQ16s32s64s254s64s64s254s254s254s65s64s16s64s"
)
SERVICE_NAME_RE = re.compile(r"[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?")

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
        if "running" not in (status.get("state") or {}):
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


def resolve_authorized_pod(
    *, pod_uid: str, node_name: str, container_id: str,
    service_name: str, pods: list[dict[str, Any]], services: list[dict[str, Any]],
    resource_name: str,
) -> dict[str, Any]:
    matches = [pod for pod in pods if pod.get("metadata", {}).get("uid") == pod_uid]
    if len(matches) != 1:
        raise GrantError(f"Pod UID resolved to {len(matches)} snapshot objects")
    pod = matches[0]
    metadata = pod.get("metadata", {}) or {}
    spec = pod.get("spec", {}) or {}
    if metadata.get("deletionTimestamp"):
        raise GrantError("terminating Pod cannot be registered")
    if spec.get("nodeName") != node_name:
        raise GrantError("Pod is assigned to another node")
    if not service_account_token_disabled(pod):
        raise GrantError("workload ServiceAccount token must not be mounted")
    target = resource_target(pod, resource_name)
    expected = running_container_id(pod, str(target.get("name") or ""))
    if expected != container_id:
        raise GrantError("kernel container ID does not match the resource target")
    authorize_service(service_name, pod, services)
    pod_ipv4(pod)
    return pod

