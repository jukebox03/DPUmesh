#!/usr/bin/env python3
"""Host-resident DPUmesh runtime and Kubernetes Device Plugin.

There is deliberately one node process.  The kubelet-facing Device Plugin,
allocation socket registry, kernel-evidence verifier, controller mTLS client,
worker cgroup owner, and broker supervisor all share the same slot state.
Neither this process nor a workload receives a Kubernetes bearer token.
"""

from __future__ import annotations

import argparse
import array
import dataclasses
import hashlib
import json
import os
import re
import secrets
import select
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
from concurrent import futures
from pathlib import Path
from typing import Iterator

import grpc

from node.deviceplugin.v1beta1 import api_pb2
from node.deviceplugin.v1beta1 import api_pb2_grpc


RESOURCE_NAME = "dpumesh.io/channel"
PLUGIN_API_VERSION = "v1beta1"
PLUGIN_ENDPOINT = "dpumesh.sock"
CONTAINER_SOCKET = "/run/dpumesh/channel.sock"
BROKER_HELLO = struct.Struct("<8sBB2x64s")
GRANT_REQUEST = struct.Struct("<8sB3x64s32s")
ASSERT_SIZE = 1545
BROKER_IPC_VERSION = 3
MAX_CHANNEL_SLOTS = 127
PEERCRED = struct.Struct("3i")
POD_UID_RE = re.compile(
    r"[0-9a-f]{8}[-_][0-9a-f]{4}[-_][0-9a-f]{4}[-_]"
    r"[0-9a-f]{4}[-_][0-9a-f]{12}", re.IGNORECASE
)
CONTAINER_ID_RE = re.compile(
    r"(?:cri-containerd|crio|docker)-([0-9a-f]{64})(?:\.scope)?(?:/|$)",
    re.IGNORECASE,
)
SERVICE_RE = re.compile(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?")
NODE_RE = re.compile(
    r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
    r"(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)*"
)


class RuntimeError_(RuntimeError):
    """A fail-closed node-runtime error safe to put in local logs."""


def valid_ipv4_endpoint(value: str) -> bool:
    address, separator, port = value.partition(":")
    try:
        packed = socket.inet_pton(socket.AF_INET, address)
    except OSError:
        return False
    return (
        bool(separator) and port.isdigit() and 0 < int(port) <= 65535
        and socket.inet_ntop(socket.AF_INET, packed) == address
    )


@dataclasses.dataclass(frozen=True)
class ProcessEvidence:
    pod_uid: str
    container_id: str


@dataclasses.dataclass
class Worker:
    slot: int
    generation: int
    pod_uid: str
    container_id: str
    service: str
    pid: int
    starttime: str
    wrapper_pid: int
    cgroup: Path
    private_root: Path
    registered: bool = False


def process_starttime(pid: int, proc_root: Path = Path("/proc")) -> str:
    try:
        raw = (proc_root / str(pid) / "stat").read_text(encoding="ascii")
    except (OSError, UnicodeDecodeError) as exc:
        raise RuntimeError_(f"cannot read process {pid} start time") from exc
    fields = raw.rpartition(")")[2].split()
    if len(fields) < 20:
        raise RuntimeError_(f"malformed process {pid} stat")
    return fields[19]


def process_parent(pid: int, proc_root: Path = Path("/proc")) -> int:
    try:
        raw = (proc_root / str(pid) / "stat").read_text(encoding="ascii")
        fields = raw.rpartition(")")[2].split()
        parent = int(fields[1])
    except (OSError, UnicodeDecodeError, ValueError, IndexError) as exc:
        raise RuntimeError_(f"cannot read process {pid} parent") from exc
    if parent <= 1:
        raise RuntimeError_(f"process {pid} has no supervised parent")
    return parent


def parse_cgroup_identity(document: str) -> tuple[str, str]:
    unified = next(
        (line.split(":", 2)[2] for line in document.splitlines()
         if line.startswith("0::")), None
    )
    if unified is None or "kubepods" not in unified:
        raise RuntimeError_("peer is not in the Kubernetes cgroup-v2 tree")
    pod = POD_UID_RE.search(unified)
    container = CONTAINER_ID_RE.search(unified)
    if pod is None or container is None:
        raise RuntimeError_("peer cgroup has no canonical Pod/container identity")
    pod_uid = pod.group(0).replace("_", "-").lower()
    container_id = container.group(1).lower()
    return pod_uid, container_id


def kernel_evidence(connection: socket.socket,
                    proc_root: Path = Path("/proc")) -> ProcessEvidence:
    pid, _uid, _gid = PEERCRED.unpack(
        connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, PEERCRED.size)
    )
    before = process_starttime(pid, proc_root)
    try:
        document = (proc_root / str(pid) / "cgroup").read_text(encoding="ascii")
    except (OSError, UnicodeDecodeError) as exc:
        raise RuntimeError_(f"cannot read process {pid} cgroup") from exc
    after = process_starttime(pid, proc_root)
    if before != after:
        raise RuntimeError_("peer PID was recycled while evidence was read")
    pod_uid, container_id = parse_cgroup_identity(document)
    return ProcessEvidence(pod_uid, container_id)


def parse_hello(connection: socket.socket) -> str:
    packet = connection.recv(BROKER_HELLO.size + 1, socket.MSG_PEEK)
    if len(packet) != BROKER_HELLO.size:
        raise RuntimeError_("invalid application HELLO length")
    magic, message_type, version, service_field = BROKER_HELLO.unpack(packet)
    if (magic != b"DMESHBR1" or message_type != 1 or
            version != BROKER_IPC_VERSION):
        raise RuntimeError_("invalid application HELLO framing")
    if b"\0" not in service_field:
        raise RuntimeError_("application Service field is not terminated")
    try:
        service = service_field.split(b"\0", 1)[0].decode("ascii")
    except UnicodeDecodeError as exc:
        raise RuntimeError_("application Service is not ASCII") from exc
    if service and SERVICE_RE.fullmatch(service) is None:
        raise RuntimeError_("application Service name is malformed")
    return service


class ControllerClient:
    """The daemon's only authorization client; TLS identity supplies node_name."""

    def __init__(self, base_url: str, ca: Path, certificate: Path, key: Path,
                 timeout: float = 5.0) -> None:
        if not base_url.startswith("https://"):
            raise RuntimeError_("controller URL must use https")
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.context = ssl.create_default_context(cafile=str(ca))
        self.context.minimum_version = ssl.TLSVersion.TLSv1_3
        self.context.load_cert_chain(str(certificate), str(key))

    def grant(self, pod_uid: str, container_id: str, service: str, nonce: bytes,
              *, slot: int, generation: int, incarnation: str) -> bytes:
        if len(nonce) != 32 or not any(nonce):
            raise RuntimeError_("grant nonce must be nonzero and 32 bytes")
        body = json.dumps({
            "pod_uid": pod_uid,
            "container_id": container_id,
            "service": service,
            "nonce": nonce.hex(),
            "slot": slot,
            "generation": generation,
            "daemon_incarnation": incarnation,
        }, separators=(",", ":")).encode("ascii")
        request = urllib.request.Request(
            f"{self.base_url}/workload-grant", data=body,
            headers={"Content-Type": "application/json"}, method="POST",
        )
        try:
            with urllib.request.urlopen(
                request, timeout=self.timeout, context=self.context
            ) as response:
                result = response.read(4097)
        except (OSError, urllib.error.URLError) as exc:
            raise RuntimeError_(f"controller denied/unavailable: {exc}") from exc
        if len(result) != ASSERT_SIZE:
            raise RuntimeError_("controller returned a noncanonical grant")
        return result

    def get(self, path: str, bound: int) -> bytes:
        request = urllib.request.Request(
            f"{self.base_url}{path}", headers={"Accept": "text/plain"}
        )
        try:
            with urllib.request.urlopen(
                request, timeout=self.timeout, context=self.context
            ) as response:
                payload = response.read(bound + 1)
        except (OSError, urllib.error.URLError) as exc:
            raise RuntimeError_(f"controller fetch {path} failed: {exc}") from exc
        if not payload or len(payload) > bound:
            raise RuntimeError_(f"controller response {path} is empty/over bound")
        return payload

    def report_node(self, node_name: str, rdma: str, public_key: str) -> None:
        body = json.dumps({
            "name": node_name, "rdma": rdma, "dpu_public_key": public_key,
        }, separators=(",", ":")).encode("ascii")
        request = urllib.request.Request(
            f"{self.base_url}/node", data=body,
            headers={"Content-Type": "application/json"}, method="POST",
        )
        try:
            with urllib.request.urlopen(
                request, timeout=self.timeout, context=self.context
            ) as response:
                if response.status != 200:
                    raise RuntimeError_("controller refused node report")
        except (OSError, urllib.error.URLError) as exc:
            raise RuntimeError_(f"controller node report failed: {exc}") from exc


def dpu_node_key(address: tuple[str, int], timeout: float) -> str:
    with socket.create_connection(address, timeout=timeout) as connection:
        connection.settimeout(timeout)
        connection.sendall(b"DMESHNODE1\n")
        reply = connection.recv(128).decode("ascii", "replace").strip()
    if not reply.startswith("key ") or re.fullmatch(r"[0-9a-f]{64}", reply[4:]) is None:
        raise RuntimeError_("DPU returned no canonical node public key")
    return reply[4:]


def deliver_feed(address: tuple[str, int], name: str, payload: bytes,
                 timeout: float) -> None:
    digest = hashlib.sha256(payload).hexdigest()
    with socket.create_connection(address, timeout=timeout) as connection:
        connection.settimeout(timeout)
        connection.sendall(
            f"DMESHFEED1 {name} {len(payload)} {digest}\n".encode("ascii")
        )
        decision = connection.recv(128).decode("ascii", "replace").strip()
        if decision == "have":
            return
        if decision != "send":
            raise RuntimeError_(f"DPU refused {name}: {decision or 'no reply'}")
        connection.sendall(payload)
        reply = connection.recv(128).decode("ascii", "replace").strip()
    if not reply.startswith("ok "):
        raise RuntimeError_(f"DPU failed to install {name}: {reply or 'no reply'}")


class DPUFeed:
    """Fetch controller-signed bytes and deliver them without interpreting them."""

    def __init__(self, *, controller: ControllerClient, registry: "SlotRegistry",
                 address: tuple[str, int], node_name: str, node_rdma: str,
                 interval: float, timeout: float) -> None:
        self.controller = controller
        self.registry = registry
        self.address = address
        self.node_name = node_name
        self.node_rdma = node_rdma
        self.interval = interval
        self.timeout = timeout
        self.stop = threading.Event()

    def once(self) -> None:
        topology = self.controller.get("/topology.v1", 16 * 1024 * 1024)
        membership = self.controller.get("/membership.v1", 256 * 1024)
        service_targets = self.controller.get("/service-targets.v1", 1024 * 1024)
        deliver_feed(self.address, "topology", topology, self.timeout)
        deliver_feed(self.address, "membership", membership, self.timeout)
        deliver_feed(self.address, "service-targets", service_targets, self.timeout)
        key = dpu_node_key(self.address, self.timeout)
        self.controller.report_node(self.node_name, self.node_rdma, key)

    def run(self) -> None:
        delay = self.interval
        while not self.stop.is_set():
            try:
                self.once()
                self.registry.set_ready(True)
                delay = self.interval
            except Exception as exc:
                self.registry.set_ready(False)
                print(f"dpumeshd: DPU/controller readiness failed: {exc}",
                      file=sys.stderr, flush=True)
                delay = min(max(delay * 2.0, 1.0), 30.0)
            self.stop.wait(delay)

    def close(self) -> None:
        self.stop.set()


class NodeMTLSTunnel:
    """Protocol-blind DPU-to-controller tunnel with node mTLS on the far hop."""

    def __init__(self, listen: tuple[str, int], controller: ControllerClient) -> None:
        parsed = urllib.parse.urlparse(controller.base_url)
        if parsed.hostname is None:
            raise RuntimeError_("controller URL has no host")
        self.listen = listen
        self.target = (parsed.hostname, parsed.port or 443)
        self.server_name = parsed.hostname
        self.controller = controller
        self.stop = threading.Event()
        self.listener: socket.socket | None = None
        self.capacity = threading.BoundedSemaphore(16)

    @staticmethod
    def _relay(left: socket.socket, right: socket.socket) -> None:
        peers = {left: right, right: left}
        while peers:
            readable, _, _ = select.select(list(peers), [], [], 5.0)
            if not readable:
                continue
            for source in readable:
                data = source.recv(65536)
                if not data:
                    return
                peers[source].sendall(data)

    def _serve(self, incoming: socket.socket) -> None:
        try:
            raw = socket.create_connection(self.target, timeout=5.0)
            with incoming, raw, self.controller.context.wrap_socket(
                raw, server_hostname=self.server_name
            ) as outgoing:
                incoming.settimeout(None)
                outgoing.settimeout(None)
                self._relay(incoming, outgoing)
        except OSError as exc:
            print(f"dpumeshd: controller tunnel failed: {exc}",
                  file=sys.stderr, flush=True)
        finally:
            self.capacity.release()

    def run(self) -> None:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(self.listen)
        listener.listen(64)
        listener.settimeout(1.0)
        self.listener = listener
        while not self.stop.is_set():
            try:
                incoming, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            if not self.capacity.acquire(blocking=False):
                incoming.close()
                continue
            threading.Thread(target=self._serve, args=(incoming,), daemon=True).start()

    def close(self) -> None:
        self.stop.set()
        if self.listener is not None:
            self.listener.close()


class CgroupManager:
    """Own the systemd-delegated subtree and bounded worker leaves."""

    def __init__(self, root: Path, cpu_max: str, memory_high: int,
                 memory_max: int, pids_max: int) -> None:
        self.root = root
        self.manager = root / "manager"
        self.workers = root / "workers"
        self.cpu_max = cpu_max
        self.memory_high = memory_high
        self.memory_max = memory_max
        self.pids_max = pids_max

    @staticmethod
    def _write(path: Path, value: str) -> None:
        try:
            path.write_text(value, encoding="ascii")
        except OSError as exc:
            raise RuntimeError_(f"cannot write delegated cgroup file {path}") from exc

    def initialize(self) -> None:
        try:
            self.manager.mkdir(exist_ok=True)
            self.workers.mkdir(exist_ok=True)
        except OSError as exc:
            raise RuntimeError_("cannot create delegated cgroup children") from exc
        # A service starts at its root.  Move the daemon into a leaf before
        # enabling domain controllers for sibling worker leaves.
        self._write(self.manager / "cgroup.procs", str(os.getpid()))
        available = set((self.root / "cgroup.controllers").read_text(
            encoding="ascii").split())
        required = {"cpu", "memory", "pids"}
        if not required.issubset(available):
            raise RuntimeError_("systemd did not delegate cpu,memory,pids")
        self._write(self.root / "cgroup.subtree_control", "+cpu +memory +pids")
        self._write(self.workers / "cgroup.subtree_control", "+cpu +memory +pids")

    def create(self, pod_uid: str, generation: int) -> Path:
        if POD_UID_RE.fullmatch(pod_uid) is None or generation < 1:
            raise RuntimeError_("unsafe worker cgroup identity")
        leaf = self.workers / f"pod{pod_uid}.g{generation}"
        try:
            leaf.mkdir()
        except OSError as exc:
            raise RuntimeError_(f"cannot create worker cgroup {leaf.name}") from exc
        try:
            self._write(leaf / "cpu.max", self.cpu_max)
            self._write(leaf / "memory.high", str(self.memory_high))
            self._write(leaf / "memory.max", str(self.memory_max))
            self._write(leaf / "pids.max", str(self.pids_max))
        except Exception:
            try:
                leaf.rmdir()
            except OSError:
                pass
            raise
        return leaf

    @staticmethod
    def remove(leaf: Path) -> None:
        try:
            leaf.rmdir()
        except OSError:
            pass


class Slot:
    def __init__(self, number: int, path: Path) -> None:
        self.number = number
        self.device_id = f"channel-{number:03d}"
        self.path = path
        self.generation = 0
        self.state = "UNHEALTHY"
        self.worker: Worker | None = None
        self.lock = threading.Lock()
        self.listener: socket.socket | None = None

    def health(self, runtime_ready: bool) -> str:
        with self.lock:
            # CLEANUP_WAIT is a short software reuse barrier, not a failed
            # device. Kubelet already owns allocated device IDs; advertising
            # the slot Unhealthy here causes admission-failure Pod churn while
            # a deleted workload's DPU mapping drains.
            healthy = runtime_ready and self.state != "UNHEALTHY"
        return "Healthy" if healthy else "Unhealthy"


class SlotRegistry:
    def __init__(self, count: int, directory: Path) -> None:
        if not 1 <= count <= MAX_CHANNEL_SLOTS:
            raise RuntimeError_(f"slot count must be between 1 and {MAX_CHANNEL_SLOTS}")
        self.slots = [Slot(index, directory / f"channel-{index:03d}.sock")
                      for index in range(count)]
        self.runtime_ready = False
        self.changed = threading.Condition()

    def set_ready(self, ready: bool) -> None:
        with self.changed:
            self.runtime_ready = ready
            for slot in self.slots:
                with slot.lock:
                    if slot.state == "UNHEALTHY" and ready:
                        slot.state = "FREE_LISTENING"
                    elif not ready and slot.worker is None:
                        slot.state = "UNHEALTHY"
            self.changed.notify_all()

    def devices(self) -> list[api_pb2.Device]:
        return [api_pb2.Device(ID=slot.device_id,
                               health=slot.health(self.runtime_ready))
                for slot in self.slots]

    def by_id(self, device_id: str) -> Slot:
        matches = [slot for slot in self.slots if slot.device_id == device_id]
        if len(matches) != 1:
            raise RuntimeError_(f"unknown channel device {device_id!r}")
        return matches[0]

    def notify(self) -> None:
        with self.changed:
            self.changed.notify_all()


class DevicePlugin(api_pb2_grpc.DevicePluginServicer):
    def __init__(self, registry: SlotRegistry) -> None:
        self.registry = registry

    def GetDevicePluginOptions(self, request, context):  # noqa: N802
        del request, context
        return api_pb2.DevicePluginOptions()

    def ListAndWatch(self, request, context) -> Iterator[api_pb2.ListAndWatchResponse]:  # noqa: N802,E501
        del request
        while context.is_active():
            yield api_pb2.ListAndWatchResponse(devices=self.registry.devices())
            with self.registry.changed:
                self.registry.changed.wait(timeout=10.0)

    def Allocate(self, request, context):  # noqa: N802
        responses = []
        try:
            for allocation in request.container_requests:
                ids = list(allocation.devices_ids)
                if len(ids) != 1:
                    raise RuntimeError_("each target container must receive one channel")
                slot = self.registry.by_id(ids[0])
                if slot.health(self.registry.runtime_ready) != "Healthy":
                    raise RuntimeError_(f"channel {ids[0]} is unhealthy")
                responses.append(api_pb2.ContainerAllocateResponse(mounts=[
                    api_pb2.Mount(container_path=CONTAINER_SOCKET,
                                  host_path=str(slot.path), read_only=True)
                ]))
        except RuntimeError_ as exc:
            context.abort(grpc.StatusCode.INVALID_ARGUMENT, str(exc))
        return api_pb2.AllocateResponse(container_responses=responses)

    def GetPreferredAllocation(self, request, context):  # noqa: N802
        del request
        context.abort(grpc.StatusCode.UNIMPLEMENTED,
                      "all DPUmesh channels are equivalent")

    def PreStartContainer(self, request, context):  # noqa: N802
        del request, context
        return api_pb2.PreStartContainerResponse()


class PluginServer:
    def __init__(self, registry: SlotRegistry, plugin_dir: Path) -> None:
        self.registry = registry
        self.plugin_dir = plugin_dir
        self.endpoint = plugin_dir / PLUGIN_ENDPOINT
        self.kubelet = plugin_dir / "kubelet.sock"
        self.server: grpc.Server | None = None
        self.stop = threading.Event()

    def _kubelet_identity(self) -> tuple[int, int, int]:
        try:
            item = self.kubelet.stat()
        except FileNotFoundError as exc:
            raise RuntimeError_("kubelet Device Plugin socket is absent") from exc
        if not stat.S_ISSOCK(item.st_mode):
            raise RuntimeError_("kubelet Device Plugin endpoint is not a socket")
        return item.st_dev, item.st_ino, item.st_ctime_ns

    def _start_server(self) -> None:
        self.endpoint.unlink(missing_ok=True)
        server = grpc.server(futures.ThreadPoolExecutor(max_workers=4))
        api_pb2_grpc.add_DevicePluginServicer_to_server(
            DevicePlugin(self.registry), server
        )
        if server.add_insecure_port(f"unix://{self.endpoint}") != 1:
            raise RuntimeError_("cannot bind Device Plugin endpoint")
        server.start()
        os.chmod(self.endpoint, 0o600)
        self.server = server

    def _register(self) -> tuple[int, int, int]:
        identity = self._kubelet_identity()
        channel = grpc.insecure_channel(f"unix://{self.kubelet}")
        try:
            grpc.channel_ready_future(channel).result(timeout=5.0)
            api_pb2_grpc.RegistrationStub(channel).Register(
                api_pb2.RegisterRequest(
                    version=PLUGIN_API_VERSION, endpoint=PLUGIN_ENDPOINT,
                    resource_name=RESOURCE_NAME,
                    options=api_pb2.DevicePluginOptions(),
                ), timeout=5.0,
            )
        finally:
            channel.close()
        if self._kubelet_identity() != identity:
            raise RuntimeError_("kubelet restarted during Device Plugin registration")
        return identity

    def run(self) -> None:
        self.plugin_dir.mkdir(parents=True, exist_ok=True)
        retry = 0.02
        registered_kubelet: tuple[int, int, int] | None = None
        while not self.stop.is_set():
            try:
                if self.server is None or not self.endpoint.exists():
                    if self.server is not None:
                        self.server.stop(0).wait(2.0)
                    self._start_server()
                    registered_kubelet = None
                current_kubelet = self._kubelet_identity()
                if registered_kubelet != current_kubelet:
                    registered_kubelet = self._register()
                    print("dpumeshd: registered dpumesh.io/channel with kubelet",
                          flush=True)
                    retry = 0.02
                # Kubelet removes every Device Plugin socket on restart. Poll
                # fast enough to re-register before its first Pod admission
                # reconciliation; this thread is otherwise idle.
                self.stop.wait(0.02)
            except Exception as exc:
                registered_kubelet = None
                print(f"dpumeshd: Device Plugin registration failed: {exc}",
                      file=sys.stderr, flush=True)
                self.stop.wait(retry)
                retry = min(retry * 2.0, 0.5)

    def close(self) -> None:
        self.stop.set()
        if self.server is not None:
            self.server.stop(0).wait(2.0)
        self.endpoint.unlink(missing_ok=True)


class BrokerSupervisor:
    def __init__(self, *, registry: SlotRegistry, cgroups: CgroupManager,
                 controller: ControllerClient, broker: Path, manager_socket: Path,
                 runtime_dir: Path, incarnation: str, launch_timeout: float,
                 pci_addr: str, rings_per_pod: int) -> None:
        self.registry = registry
        self.cgroups = cgroups
        self.controller = controller
        self.broker = broker
        self.manager_socket = manager_socket
        self.runtime_dir = runtime_dir
        self.incarnation = incarnation
        self.launch_timeout = launch_timeout
        self.pci_addr = pci_addr
        self.rings_per_pod = rings_per_pod
        self.workers: dict[int, Worker] = {}
        self.lock = threading.Lock()

    @staticmethod
    def remove_private_root(path: Path) -> None:
        try:
            path.rmdir()
        except FileNotFoundError:
            pass
        except OSError as exc:
            print(f"dpumeshd: private root cleanup failed for {path}: {exc}",
                  file=sys.stderr, flush=True)

    def _worker_for_peer(self, pid: int, uid: int) -> Worker:
        if uid not in (0, 65532):
            raise RuntimeError_("grant requester has an unexpected uid")
        with self.lock:
            worker = self.workers.get(pid)
        if worker is None or process_starttime(pid) != worker.starttime:
            raise RuntimeError_("grant requester is not a live owned broker")
        return worker

    def grant_for_broker(self, connection: socket.socket) -> bytes:
        pid, uid, _gid = PEERCRED.unpack(connection.getsockopt(
            socket.SOL_SOCKET, socket.SO_PEERCRED, PEERCRED.size
        ))
        packet = connection.recv(GRANT_REQUEST.size + 1)
        if len(packet) != GRANT_REQUEST.size:
            raise RuntimeError_("invalid broker grant request length")
        magic, version, service_field, nonce = GRANT_REQUEST.unpack(packet)
        if (magic != b"DMESHGR1" or version != BROKER_IPC_VERSION or
                not any(nonce)):
            raise RuntimeError_("invalid broker grant request")
        if b"\0" not in service_field:
            raise RuntimeError_("unterminated broker Service field")
        try:
            service = service_field.split(b"\0", 1)[0].decode("ascii")
        except UnicodeDecodeError as exc:
            raise RuntimeError_("broker Service is not ASCII") from exc
        worker = self._worker_for_peer(pid, uid)
        if service != worker.service:
            raise RuntimeError_("broker requested a Service outside its authorization")
        # containerStatuses.containerID can appear shortly after the process
        # enters cgroupfs. Retry the same nonce across two controller snapshots;
        # the DPU accepts only the one signed, connection-bound grant returned.
        for attempt in range(15):
            try:
                result = self.controller.grant(
                    worker.pod_uid, worker.container_id, service, nonce,
                    slot=worker.slot, generation=worker.generation,
                    incarnation=self.incarnation,
                )
                break
            except RuntimeError_:
                if attempt == 14:
                    raise
                time.sleep(0.5)
        with self.lock:
            if self.workers.get(worker.pid) is not worker:
                raise RuntimeError_("broker exited before its grant was delivered")
            worker.registered = True
        return result

    def _watch(self, worker: Worker) -> None:
        status: int | None = None
        try:
            _pid, status = os.waitpid(worker.wrapper_pid, 0)
        except ChildProcessError:
            pass
        with self.lock:
            self.workers.pop(worker.pid, None)
            registered = worker.registered
        self.remove_private_root(worker.private_root)
        slot = self.registry.slots[worker.slot]
        with slot.lock:
            if slot.worker is worker:
                slot.worker = None
                slot.state = "CLEANUP_WAIT" if registered else "FREE_LISTENING"
        # DPU disconnect handling is bounded; delay reuse until the previous
        # Comch mapping has drained.
        if registered:
            time.sleep(5.0)
            with slot.lock:
                if slot.worker is None and slot.state == "CLEANUP_WAIT":
                    slot.state = "FREE_LISTENING"
        self.cgroups.remove(worker.cgroup)
        self.registry.notify()
        if status is None:
            result = "unknown"
        elif os.WIFEXITED(status):
            result = f"exit={os.WEXITSTATUS(status)}"
        elif os.WIFSIGNALED(status):
            result = f"signal={os.WTERMSIG(status)}"
        else:
            result = f"wait-status={status}"
        print(f"dpumeshd: broker exited pid={worker.pid} {result} "
              f"pod={worker.pod_uid}", flush=True)

    def launch(self, slot: Slot, connection: socket.socket,
               evidence: ProcessEvidence, service: str, generation: int) -> Worker:
        leaf = self.cgroups.create(evidence.pod_uid, generation)
        suffix = secrets.token_hex(6)
        launch_path = self.runtime_dir / f"launch.{slot.number}.{suffix}.sock"
        private_root = self.runtime_dir / f".broker-root.{suffix}"
        launch: socket.socket | None = None
        wrapper_pid = -1
        final_pid = -1
        try:
            private_root.mkdir(mode=0o700)
            launch = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            launch.settimeout(self.launch_timeout)
            launch.bind(str(launch_path))
            os.chmod(launch_path, 0o600)
            launch.listen(1)
            environment = os.environ.copy()
            library_dir = self.broker.parent.parent / "lib"
            environment["LD_LIBRARY_PATH"] = ":".join([
                str(library_dir), "/opt/mellanox/doca/lib/x86_64-linux-gnu",
                "/opt/mellanox/flexio/lib",
            ])
            environment["DPUMESH_PCI_ADDR"] = self.pci_addr
            environment["DPUMESH_RINGS_PER_POD"] = str(self.rings_per_pod)
            process = subprocess.Popen([
                str(self.broker), "--expected-parent", str(os.getpid()),
                "--launch-sock", str(launch_path),
                "--manager-sock", str(self.manager_socket),
                "--private-root", str(private_root),
            ], env=environment, stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL, stderr=None, close_fds=True)
            wrapper_pid = process.pid
            peer, _ = launch.accept()
            with peer:
                final_pid, peer_uid, _peer_gid = PEERCRED.unpack(peer.getsockopt(
                    socket.SOL_SOCKET, socket.SO_PEERCRED, PEERCRED.size
                ))
                presented = peer.recv(2)
                if (peer_uid != 0 or presented != b"B" or
                        process_parent(final_pid) != wrapper_pid):
                    raise RuntimeError_("broker bootstrap parent/credential mismatch")
                cgroup_fd = os.open(leaf, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    rights = array.array("i", [connection.fileno(), cgroup_fd])
                    sent = peer.sendmsg([b"F"], [(
                        socket.SOL_SOCKET, socket.SCM_RIGHTS, rights.tobytes()
                    )])
                    if sent != 1 or peer.recv(1) != b"M":
                        raise RuntimeError_("broker did not enter its worker cgroup")
                finally:
                    os.close(cgroup_fd)
                started = process_starttime(final_pid)
                current = Path(f"/proc/{final_pid}/cgroup").read_text(encoding="ascii")
                if f"/{leaf.name}" not in current:
                    raise RuntimeError_("broker cgroup migration could not be verified")
                worker = Worker(
                    slot.number, generation, evidence.pod_uid,
                    evidence.container_id, service, final_pid, started,
                    wrapper_pid, leaf, private_root,
                )
                with self.lock:
                    if final_pid in self.workers:
                        raise RuntimeError_("broker PID collision")
                    self.workers[final_pid] = worker
                with slot.lock:
                    slot.worker = worker
                    slot.state = "REGISTERING"
                try:
                    peer.sendall(b"G")
                except OSError:
                    with self.lock:
                        self.workers.pop(final_pid, None)
                    raise
            threading.Thread(target=self._watch, args=(worker,), daemon=True).start()
            print(f"dpumeshd: broker pid={final_pid} slot={slot.device_id} "
                  f"generation={generation} pod={evidence.pod_uid}", flush=True)
            return worker
        except Exception:
            with self.lock:
                if final_pid > 1:
                    self.workers.pop(final_pid, None)
            for pid in (final_pid, wrapper_pid):
                if pid > 1:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
            if wrapper_pid > 1:
                try:
                    os.waitpid(wrapper_pid, 0)
                except ChildProcessError:
                    pass
            self.cgroups.remove(leaf)
            self.remove_private_root(private_root)
            raise
        finally:
            if launch is not None:
                launch.close()
            launch_path.unlink(missing_ok=True)

    def terminate_all(self) -> None:
        with self.lock:
            workers = list(self.workers.values())
        for worker in workers:
            try:
                os.kill(worker.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            with self.lock:
                if not self.workers:
                    break
            time.sleep(0.05)
        with self.lock:
            workers = list(self.workers.values())
        for worker in workers:
            try:
                os.kill(worker.wrapper_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


class Daemon:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.stop = threading.Event()
        self.incarnation = secrets.token_hex(16)
        args.runtime_dir.mkdir(mode=0o755, parents=True, exist_ok=True)
        for private_root in args.runtime_dir.glob(".broker-root.*"):
            if private_root.is_dir() and not private_root.is_symlink():
                BrokerSupervisor.remove_private_root(private_root)
        args.slot_dir.mkdir(mode=0o755, parents=True, exist_ok=True)
        os.chmod(args.slot_dir, 0o755)
        self.registry = SlotRegistry(args.slots, args.slot_dir)
        self.cgroups = CgroupManager(
            args.cgroup_root, args.worker_cpu_max, args.worker_memory_high,
            args.worker_memory_max, args.worker_pids_max,
        )
        self.controller = ControllerClient(
            args.controller_url, args.controller_ca,
            args.controller_cert, args.controller_key, args.controller_timeout,
        )
        self.supervisor = BrokerSupervisor(
            registry=self.registry, cgroups=self.cgroups,
            controller=self.controller, broker=args.broker_bin,
            manager_socket=args.manager_socket, runtime_dir=args.runtime_dir,
            incarnation=self.incarnation, launch_timeout=args.launch_timeout,
            pci_addr=args.pci_addr, rings_per_pod=args.rings_per_pod,
        )
        self.plugin = PluginServer(self.registry, args.device_plugin_dir)
        self.feed = DPUFeed(
            controller=self.controller, registry=self.registry,
            address=(args.dpu_feed_host, args.dpu_feed_port),
            node_name=args.node_name, node_rdma=args.node_rdma_addr,
            interval=args.feed_interval, timeout=args.feed_timeout,
        )
        self.scope_tunnel = NodeMTLSTunnel(
            (args.scope_listen_address, args.scope_listen_port), self.controller
        ) if args.scope_listen_port else None
        self.manager_listener: socket.socket | None = None

    @staticmethod
    def _bind(path: Path, mode: int) -> socket.socket:
        temporary = path.with_name(
            f".{path.name}.{os.getpid()}.{secrets.token_hex(4)}"
        )
        temporary.unlink(missing_ok=True)
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        try:
            listener.bind(str(temporary))
            os.chown(temporary, 0, 0)
            os.chmod(temporary, mode)
            listener.listen(128)
            # A stopped daemon leaves the socket inode in place so CRI
            # can never auto-create a directory at this file mount source.
            # Replace it only after the new listener is ready.
            try:
                existing = path.lstat()
            except FileNotFoundError:
                existing = None
            if existing is not None and stat.S_ISDIR(existing.st_mode):
                # Recover only an empty directory created when CRI observed an
                # absent file mount source. Never remove a non-empty path.
                path.rmdir()
            elif existing is not None and not stat.S_ISSOCK(existing.st_mode):
                raise RuntimeError_(f"refusing to replace non-socket path {path}")
            os.replace(temporary, path)
            listener.settimeout(1.0)
            return listener
        except Exception:
            listener.close()
            temporary.unlink(missing_ok=True)
            raise

    def _serve_manager(self) -> None:
        assert self.manager_listener is not None
        while not self.stop.is_set():
            try:
                connection, _ = self.manager_listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                with connection:
                    connection.settimeout(self.args.request_timeout)
                    connection.sendall(self.supervisor.grant_for_broker(connection))
            except Exception as exc:
                print(f"dpumeshd: broker grant failed: {exc}",
                      file=sys.stderr, flush=True)

    def _serve_slot(self, slot: Slot) -> None:
        assert slot.listener is not None
        while not self.stop.is_set():
            try:
                connection, _ = slot.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            connection.settimeout(self.args.request_timeout)
            with slot.lock:
                if slot.state != "FREE_LISTENING" or slot.worker is not None:
                    connection.close()
                    continue
                slot.state = "AUTHORIZING"
                slot.generation += 1
                generation = slot.generation
            success = False
            try:
                service = parse_hello(connection)
                evidence = kernel_evidence(connection)
                with slot.lock:
                    slot.state = "STARTING"
                self.supervisor.launch(
                    slot, connection, evidence, service, generation
                )
                success = True
            except Exception as exc:
                print(f"dpumeshd: denied {slot.device_id}: {exc}",
                      file=sys.stderr, flush=True)
            finally:
                connection.close()
                if not success:
                    with slot.lock:
                        slot.worker = None
                        slot.state = "FREE_LISTENING" if self.registry.runtime_ready \
                            else "UNHEALTHY"
                    self.registry.notify()

    def start(self) -> None:
        if os.geteuid() != 0:
            raise RuntimeError_("dpumeshd must start as root")
        broker_stat = self.args.broker_bin.stat()
        if (not stat.S_ISREG(broker_stat.st_mode) or broker_stat.st_uid != 0 or
                broker_stat.st_mode & 0o022 or not broker_stat.st_mode & 0o111):
            raise RuntimeError_("broker binary must be immutable to non-root users")
        self.cgroups.initialize()
        self.manager_listener = self._bind(self.args.manager_socket, 0o600)
        threading.Thread(target=self._serve_manager, daemon=True).start()
        for slot in self.registry.slots:
            slot.listener = self._bind(slot.path, 0o666)
            threading.Thread(target=self._serve_slot, args=(slot,), daemon=True).start()
        threading.Thread(target=self.plugin.run, daemon=True).start()
        threading.Thread(target=self.feed.run, daemon=True).start()
        if self.scope_tunnel is not None:
            threading.Thread(target=self.scope_tunnel.run, daemon=True).start()
        print(f"dpumeshd: local runtime started incarnation={self.incarnation} "
              f"slots={len(self.registry.slots)}", flush=True)

    def close(self) -> None:
        self.registry.set_ready(False)
        self.stop.set()
        self.feed.close()
        if self.scope_tunnel is not None:
            self.scope_tunnel.close()
        self.plugin.close()
        self.supervisor.terminate_all()
        for slot in self.registry.slots:
            if slot.listener is not None:
                slot.listener.close()
        if self.manager_listener is not None:
            self.manager_listener.close()

    def run(self) -> None:
        self.start()
        while not self.stop.wait(1.0):
            pass


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node-name", required=True)
    parser.add_argument("--slots", type=int, default=8)
    parser.add_argument("--runtime-dir", type=Path, default=Path("/run/dpumesh"))
    parser.add_argument("--slot-dir", type=Path,
                        default=Path("/run/dpumesh/slots"))
    parser.add_argument("--manager-socket", type=Path,
                        default=Path("/run/dpumesh/manager.sock"))
    parser.add_argument("--device-plugin-dir", type=Path,
                        default=Path("/var/lib/kubelet/device-plugins"))
    parser.add_argument("--cgroup-root", type=Path, required=True)
    parser.add_argument("--controller-url", required=True)
    parser.add_argument("--controller-ca", type=Path, required=True)
    parser.add_argument("--controller-cert", type=Path, required=True)
    parser.add_argument("--controller-key", type=Path, required=True)
    parser.add_argument("--controller-timeout", type=float, default=5.0)
    parser.add_argument("--broker-bin", type=Path,
                        default=Path("/opt/dpumesh/bin/dmesh_broker"))
    parser.add_argument("--dpu-feed-host", required=True)
    parser.add_argument("--dpu-feed-port", type=int, default=4788)
    parser.add_argument("--node-rdma-addr", required=True)
    parser.add_argument("--feed-interval", type=float, default=2.0)
    parser.add_argument("--feed-timeout", type=float, default=10.0)
    parser.add_argument("--scope-listen-address", default="192.168.100.1")
    parser.add_argument("--scope-listen-port", type=int, default=28089)
    parser.add_argument("--pci-addr", default="")
    parser.add_argument("--rings-per-pod", type=int, default=8)
    parser.add_argument("--launch-timeout", type=float, default=10.0)
    parser.add_argument("--request-timeout", type=float, default=10.0)
    parser.add_argument("--worker-cpu-max", default="100000 100000")
    parser.add_argument("--worker-memory-high", type=int, default=768 * 1024 * 1024)
    parser.add_argument("--worker-memory-max", type=int, default=1024 * 1024 * 1024)
    parser.add_argument("--worker-pids-max", type=int, default=64)
    args = parser.parse_args(argv)
    if NODE_RE.fullmatch(args.node_name) is None or len(args.node_name) > 253:
        parser.error("--node-name must be a DNS subdomain of at most 253 bytes")
    if not 1 <= args.slots <= MAX_CHANNEL_SLOTS:
        parser.error(f"--slots must be between 1 and {MAX_CHANNEL_SLOTS}")
    if not 1 <= args.dpu_feed_port <= 65535:
        parser.error("--dpu-feed-port out of range")
    if not valid_ipv4_endpoint(args.node_rdma_addr):
        parser.error("--node-rdma-addr must be canonical IPv4:PORT")
    if not 0 <= args.scope_listen_port <= 65535:
        parser.error("--scope-listen-port out of range")
    if any(value <= 0 for value in (
            args.controller_timeout, args.feed_interval, args.feed_timeout,
            args.launch_timeout, args.request_timeout)):
        parser.error("timeouts and --feed-interval must be positive")
    if args.rings_per_pod < 1 or args.rings_per_pod > 16:
        parser.error("--rings-per-pod must be between 1 and 16")
    if args.worker_cpu_max.isdigit():
        args.worker_cpu_max = f"{args.worker_cpu_max} 100000"
    elif re.fullmatch(r"(?:max|[1-9][0-9]*) [1-9][0-9]*",
                      args.worker_cpu_max) is None:
        parser.error("--worker-cpu-max must be QUOTA or 'QUOTA PERIOD'")
    if args.worker_memory_high <= 0 or args.worker_memory_max <= 0:
        parser.error("worker memory limits must be positive")
    if args.worker_memory_high > args.worker_memory_max:
        parser.error("worker memory.high exceeds memory.max")
    if args.worker_pids_max < 1:
        parser.error("--worker-pids-max must be positive")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    daemon: Daemon | None = None
    try:
        daemon = Daemon(args)

        def stop(_signum, _frame) -> None:
            assert daemon is not None
            daemon.stop.set()

        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        daemon.run()
    except (RuntimeError_, OSError) as exc:
        print(f"dpumeshd: fatal: {exc}", file=sys.stderr)
        return 1
    finally:
        if daemon is not None:
            daemon.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
