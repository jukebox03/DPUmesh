"""Host runtime and Device Plugin contract tests (no Kubernetes/hardware)."""

from __future__ import annotations

import os
import socket
import sys
import tempfile
import threading
from contextlib import redirect_stderr
from concurrent import futures
from io import StringIO
from pathlib import Path
from unittest import mock

import grpc

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from node import dpumeshd
from node.deviceplugin.v1beta1 import api_pb2, api_pb2_grpc


UID = "c92d7333-854d-47ca-84c6-c6e257f0f9ad"
CONTAINER = "81de570a7a427a5a1db7c73271c431afe38d49fe9f4e52a3585ae2502599cd9d"
CGROUP = (
    "0::/kubepods.slice/kubepods-besteffort.slice/"
    "kubepods-besteffort-podc92d7333_854d_47ca_84c6_c6e257f0f9ad.slice/"
    "cri-containerd-81de570a7a427a5a1db7c73271c431afe38d49fe9f4e52a3585ae2502599cd9d.scope\n"
)


class AbortContext:
    def abort(self, code, detail):
        raise AssertionError((code, detail))


class ActiveContext:
    def __init__(self):
        self.active = True

    def is_active(self):
        result = self.active
        self.active = False
        return result


class Registration(api_pb2_grpc.RegistrationServicer):
    def __init__(self):
        self.request = None
        self.requests = []
        self.called = threading.Event()

    def Register(self, request, context):  # noqa: N802
        del context
        self.request = request
        self.requests.append(request)
        self.called.set()
        return api_pb2.Empty()


def test_identity() -> None:
    pod_uid, container_id = dpumeshd.parse_cgroup_identity(CGROUP)
    assert pod_uid == UID
    assert container_id == CONTAINER
    for invalid in (
        "0::/system.slice/example.service\n",
        f"0::/kubepods.slice/pod{UID}\n",
        f"0::/kubepods.slice/cri-containerd-{CONTAINER}.scope\n",
    ):
        try:
            dpumeshd.parse_cgroup_identity(invalid)
        except dpumeshd.RuntimeError_:
            continue
        raise AssertionError(f"accepted incomplete cgroup identity {invalid!r}")


def test_kernel_evidence(root: Path) -> None:
    pid = os.getpid()
    proc = root / str(pid)
    proc.mkdir(parents=True)
    # process_starttime counts from the field following '(comm)'.
    fields = ["S"] + [str(value) for value in range(4, 23)]
    (proc / "stat").write_text(f"{pid} (a tricky ) name) " + " ".join(fields))
    (proc / "cgroup").write_text(CGROUP)
    assert dpumeshd.process_parent(pid, root) == 4
    left, right = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    try:
        evidence = dpumeshd.kernel_evidence(left, root)
        assert evidence.pod_uid == UID
        assert evidence.container_id == CONTAINER
    finally:
        left.close()
        right.close()


def test_v3_grant_request() -> None:
    class Controller:
        calls = 0

        def grant(self, pod_uid, container_id, service, nonce, **lifecycle):
            self.calls += 1
            assert pod_uid == UID
            assert container_id == CONTAINER
            assert service == "echo-native"
            assert len(nonce) == 32 and any(nonce)
            assert lifecycle == {
                "slot": 1,
                "generation": 7,
                "incarnation": "11" * 16,
            }
            if self.calls < 3:
                raise dpumeshd.RuntimeError_("container status is not published")
            return b"G" * dpumeshd.ASSERT_SIZE

    worker = dpumeshd.Worker(
        slot=1, generation=7, pod_uid=UID, container_id=CONTAINER,
        service="echo-native", pid=123, starttime="1", wrapper_pid=122,
        cgroup=Path("/not-used"), private_root=Path("/not-used-root"),
    )
    supervisor = object.__new__(dpumeshd.BrokerSupervisor)
    supervisor.controller = Controller()
    supervisor.incarnation = "11" * 16
    supervisor.lock = threading.Lock()
    supervisor.workers = {worker.pid: worker}
    supervisor._worker_for_peer = lambda _pid, _uid: worker
    left, right = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    try:
        request = dpumeshd.GRANT_REQUEST.pack(
            b"DMESHGR1", dpumeshd.BROKER_IPC_VERSION,
            b"echo-native\0".ljust(64, b"\0"), bytes(range(1, 33)),
        )
        right.sendall(request)
        with mock.patch.object(dpumeshd.time, "sleep"):
            assert supervisor.grant_for_broker(left) == b"G" * dpumeshd.ASSERT_SIZE
        assert supervisor.controller.calls == 3
        assert worker.registered
    finally:
        left.close()
        right.close()


def test_configuration_validation() -> None:
    required = [
        "--node-name", "rapids4",
        "--cgroup-root", "/sys/fs/cgroup/example",
        "--controller-url", "https://controller.example",
        "--controller-ca", "/keys/ca",
        "--controller-cert", "/keys/cert",
        "--controller-key", "/keys/key",
        "--dpu-feed-host", "192.168.100.2",
        "--node-rdma-addr", "192.168.100.2:47900",
    ]
    args = dpumeshd.parse_args(required + ["--worker-cpu-max", "50000"])
    assert args.worker_cpu_max == "50000 100000"
    for option, value in (
        ("--node-name", "Bad_Node"),
        ("--slots", "128"),
        ("--dpu-feed-port", "0"),
        ("--node-rdma-addr", "192.168.100.02:47900"),
        ("--scope-listen-port", "65536"),
        ("--feed-interval", "0"),
        ("--rings-per-pod", "17"),
        ("--worker-memory-high", "0"),
        ("--worker-pids-max", "0"),
    ):
        candidate = list(required)
        if option in candidate:
            candidate[candidate.index(option) + 1] = value
        else:
            candidate.extend((option, value))
        try:
            with redirect_stderr(StringIO()):
                dpumeshd.parse_args(candidate)
        except SystemExit as exc:
            assert exc.code == 2
        else:
            raise AssertionError(f"accepted invalid {option}={value}")


def test_private_root_cleanup(root: Path) -> None:
    private_root = root / ".broker-root.123456789abc"
    private_root.mkdir(parents=True)
    dpumeshd.BrokerSupervisor.remove_private_root(private_root)
    assert not private_root.exists()


def test_plugin(root: Path) -> None:
    try:
        dpumeshd.SlotRegistry(128, root / "too-many-slots")
    except dpumeshd.RuntimeError_:
        pass
    else:
        raise AssertionError("slot count exceeded the DPU Pod table")
    registry = dpumeshd.SlotRegistry(2, root / "slots")
    registry.set_ready(True)
    plugin = dpumeshd.DevicePlugin(registry)
    response = plugin.Allocate(api_pb2.AllocateRequest(container_requests=[
        api_pb2.ContainerAllocateRequest(devices_ids=["channel-001"])
    ]), AbortContext())
    mount = response.container_responses[0].mounts[0]
    assert mount.container_path == "/run/dpumesh/channel.sock"
    assert mount.host_path.endswith("channel-001.sock")
    assert mount.read_only
    watched = plugin.ListAndWatch(api_pb2.Empty(), ActiveContext())
    first = next(watched)
    watched.close()
    assert [(device.ID, device.health) for device in first.devices] == [
        ("channel-000", "Healthy"), ("channel-001", "Healthy")
    ]
    registry.slots[0].state = "CLEANUP_WAIT"
    assert registry.devices()[0].health == "Healthy"
    registry.slots[0].state = "FREE_LISTENING"
    try:
        plugin.Allocate(api_pb2.AllocateRequest(container_requests=[
            api_pb2.ContainerAllocateRequest(
                devices_ids=["channel-000", "channel-001"]
            )
        ]), AbortContext())
    except AssertionError as exc:
        assert exc.args[0][0] == grpc.StatusCode.INVALID_ARGUMENT
    else:
        raise AssertionError("multiple channels were accepted for one container")


def test_atomic_socket_replacement(root: Path) -> None:
    root.mkdir()
    path = root / "channel.sock"
    old = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    old.bind(str(path))
    old.listen(1)
    old_inode = path.stat().st_ino
    with mock.patch.object(dpumeshd.os, "chown"):
        new = dpumeshd.Daemon._bind(path, 0o600)
    try:
        assert path.is_socket()
        assert path.stat().st_ino != old_inode
        assert path.stat().st_mode & 0o777 == 0o600
    finally:
        new.close()
        old.close()
    # Recover the exact empty-directory artifact produced by CRI for an
    # absent bind-mount source, but do not broaden cleanup beyond that path.
    path.unlink()
    path.mkdir()
    with mock.patch.object(dpumeshd.os, "chown"):
        recovered = dpumeshd.Daemon._bind(path, 0o666)
    try:
        assert path.is_socket()
    finally:
        recovered.close()


def test_kubelet_registration(root: Path) -> None:
    directory = root / "plugins"
    directory.mkdir()
    registration = Registration()
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=1))
    api_pb2_grpc.add_RegistrationServicer_to_server(registration, server)
    assert server.add_insecure_port(f"unix://{directory / 'kubelet.sock'}") == 1
    server.start()
    registry = dpumeshd.SlotRegistry(1, root / "slots2")
    plugin = dpumeshd.PluginServer(registry, directory)
    try:
        plugin._start_server()
        plugin._register()
        assert registration.request.version == "v1beta1"
        assert registration.request.endpoint == "dpumesh.sock"
        assert registration.request.resource_name == "dpumesh.io/channel"
    finally:
        plugin.close()
        server.stop(0).wait(2.0)


def test_kubelet_reregistration(root: Path) -> None:
    directory = root / "restart-plugins"
    directory.mkdir()

    def kubelet(registration):
        server = grpc.server(futures.ThreadPoolExecutor(max_workers=1))
        api_pb2_grpc.add_RegistrationServicer_to_server(registration, server)
        assert server.add_insecure_port(f"unix://{directory / 'kubelet.sock'}") == 1
        server.start()
        return server

    first = Registration()
    server = kubelet(first)
    registry = dpumeshd.SlotRegistry(1, root / "slots3")
    plugin = dpumeshd.PluginServer(registry, directory)
    thread = threading.Thread(target=plugin.run)
    thread.start()
    try:
        assert first.called.wait(2.0)
        server.stop(0).wait(2.0)
        (directory / "kubelet.sock").unlink(missing_ok=True)
        second = Registration()
        server = kubelet(second)
        assert second.called.wait(2.0)
        assert second.request.resource_name == "dpumesh.io/channel"
    finally:
        plugin.close()
        thread.join(2.0)
        server.stop(0).wait(2.0)


def main() -> None:
    test_identity()
    test_configuration_validation()
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        test_kernel_evidence(root / "proc")
        test_v3_grant_request()
        test_private_root_cleanup(root / "private-root")
        test_plugin(root)
        test_atomic_socket_replacement(root / "atomic")
        test_kubelet_registration(root)
        test_kubelet_reregistration(root)
    print("dpumeshd_test: PASS")


if __name__ == "__main__":
    main()
