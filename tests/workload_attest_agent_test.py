import hashlib
import hmac
import importlib.util
import os
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "workload_attest_agent", ROOT / "bench" / "workload_attest_agent.py"
)
assert SPEC and SPEC.loader
agent = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(agent)


def pod():
    return {
        "metadata": {
            "uid": "12345678-1234-1234-1234-123456789abc",
            "namespace": "test-bench",
            "name": "echo-dpumesh-abc123",
            "labels": {"app": "echo-dpumesh"},
        },
        "spec": {"serviceAccountName": "default", "nodeName": "worker-1"},
        "status": {"podIP": "10.244.1.17"},
    }


def main():
    assert agent.BROKER_IPC_VERSION == 3
    try:
        agent.pod_uid_from_cgroup("0::/user.slice/user-1000.slice/session-1.scope\n")
    except agent.AttestationError:
        pass
    else:
        raise AssertionError("a non-Pod peer cgroup was accepted")
    assert agent.pod_uid_from_cgroup(
        "0::/kubepods.slice/kubepods-burstable.slice/"
        "kubepods-burstable-pod12345678_1234_1234_1234_123456789abc.slice\n"
    ) == "12345678-1234-1234-1234-123456789abc"

    # Every DaemonSet instance reads its own address from the same operator
    # file as the controller. Ambiguous or incomplete cluster input fails.
    with tempfile.TemporaryDirectory() as temporary:
        nodes = Path(temporary) / "nodes"
        pub = "ab" * 32
        zero = "0" * 64
        nodes.write_text(
            f"jet1 10.77.0.1:47900 node-ed25519-v1 {pub} {zero}\n"
            f"rapids4 10.77.0.2:47900 node-ed25519-v1 {pub} {zero}\n",
            encoding="ascii",
        )
        assert agent.node_rdma_address(nodes, "jet1") == "10.77.0.1:47900"
        assert agent.node_rdma_address(nodes, "rapids4") == "10.77.0.2:47900"
        for bad in (
            f"jet1 no-port node-ed25519-v1 {pub} {zero}\n",
            f"jet1 10.77.0.1:47900 node-ed25519-v1 {pub} {zero}\n"
            f"jet1 10.77.0.2:47900 node-ed25519-v1 {pub} {zero}\n",
        ):
            nodes.write_text(bad, encoding="ascii")
            try:
                agent.node_rdma_address(nodes, "jet1")
            except agent.AttestationError:
                pass
            else:
                raise AssertionError("invalid node configuration was accepted")
        nodes.write_text(
            f"jet1 10.77.0.1:47900 node-ed25519-v1 {pub} {zero}\n",
            encoding="ascii",
        )
        try:
            agent.node_rdma_address(nodes, "absent")
        except agent.AttestationError:
            pass
        else:
            raise AssertionError("an unconfigured node received an address")

    service = {
        "metadata": {"namespace": "test-bench", "name": "echo-dpumesh"},
        "spec": {"selector": {"app": "echo-dpumesh"}},
    }
    agent.authorize_service("echo-dpumesh", pod(), [service])
    try:
        agent.authorize_service("other", pod(), [service])
    except agent.AttestationError:
        pass
    else:
        raise AssertionError("wrong Service membership was accepted")

    key = bytes(range(1, 33))
    nonce = bytes(range(32))
    assertion = agent.build_assert(
        key=key,
        key_id="node-ed25519-v1",
        service_name="echo-dpumesh",
        nonce=nonce,
        pod=pod(),
        ttl=60,
        now=1_800_000_000,
    )
    assert len(assertion) == 1134
    assert assertion[0:2] == bytes([13, 2])
    # The signature verifies with the public half only (raises on failure).
    agent.Ed25519PrivateKey.from_private_bytes(key).public_key().verify(
        assertion[-64:], assertion[:-64]
    )
    unpacked = agent.ASSERT.unpack(assertion)
    assert unpacked[7] == nonce
    assert unpacked[9].rstrip(b"\0") == b"worker-1"
    assert unpacked[11].rstrip(b"\0") == b"test-bench"
    assert unpacked[14].rstrip(b"\0") == b"echo-dpumesh"
    assert unpacked[15].rstrip(b"\0") == b"10.244.1.17"

    # A Pod without a usable IPv4 address cannot be asserted: the address is
    # the authorization input for `networks` clauses.
    no_ip = pod()
    del no_ip["status"]
    try:
        agent.build_assert(
            key=key,
            key_id="node-ed25519-v1",
            service_name="",
            nonce=nonce,
            pod=no_ip,
            ttl=60,
            now=1_800_000_000,
        )
    except agent.AttestationError:
        pass
    else:
        raise AssertionError("a Pod without an IP was asserted")

    # The membership document decides revocation with the same rule that
    # decides a grant, so a Pod appears exactly for the Services it selects.
    other = {
        "metadata": {
            "uid": "abcdef01-2345-6789-abcd-ef0123456789",
            "namespace": "test-bench",
            "name": "bench-dpumesh-xyz",
            "labels": {"app": "bench-dpumesh"},
        },
        "spec": {"serviceAccountName": "default", "nodeName": "worker-1"},
    }
    terminating = {
        "metadata": {
            "uid": "deadbeef-2345-6789-abcd-ef0123456789",
            "namespace": "test-bench",
            "name": "echo-dpumesh-gone",
            "labels": {"app": "echo-dpumesh"},
            "deletionTimestamp": "2026-08-18T00:00:00Z",
        },
        "spec": {"serviceAccountName": "default", "nodeName": "worker-1"},
    }
    document = agent.membership_document(
        7, [pod(), other, terminating], [service]
    )
    lines = document.splitlines()
    assert lines[0] == "version=7"
    assert "member=12345678-1234-1234-1234-123456789abc,-" in lines
    assert "member=12345678-1234-1234-1234-123456789abc,echo-dpumesh" in lines
    # A Pod whose labels do not select the Service holds only its bare entry.
    assert "member=abcdef01-2345-6789-abcd-ef0123456789,-" in lines
    assert "member=abcdef01-2345-6789-abcd-ef0123456789,echo-dpumesh" not in lines
    # A terminating Pod is already withdrawn.
    assert not any("deadbeef" in line for line in lines)

    # The DPU refuses a generation it cannot verify, so the publisher signs it.
    signed = agent.sign_document(document, "feed-key-v1", key)
    assert signed.startswith(document)
    envelope = signed[len(document) :].strip()
    key_id, _, mac = envelope.removeprefix("signature=").partition(",")
    assert key_id == "feed-key-v1"
    assert hmac.compare_digest(
        mac, hmac.new(key, document.encode("ascii"), hashlib.sha256).hexdigest()
    )

    # The ingress guard closes exactly the pairs the mesh serves: an injected
    # Pod's DPUMESH_PORT, and nothing about any other Pod.
    def meshed(uid, ip, port):
        entry = {
            "metadata": {
                "uid": uid,
                "namespace": "test-bench",
                "name": f"pod-{uid[:8]}",
                "labels": {"linkerd.io/control-plane-ns": "linkerd"},
            },
            "spec": {"nodeName": "worker-1", "containers": []},
            "status": {"podIP": ip},
        }
        if port is not None:
            entry["spec"]["containers"] = [
                {"env": [{"name": "DPUMESH_PORT", "value": port}]}
            ]
        return entry

    served = meshed("11111111-aaaa", "10.244.1.20", "9101")
    client = meshed("22222222-bbbb", "10.244.1.21", None)
    badport = meshed("33333333-cccc", "10.244.1.22", "not-a-port")
    leaving = meshed("44444444-dddd", "10.244.1.23", "9101")
    leaving["metadata"]["deletionTimestamp"] = "2026-08-25T00:00:00Z"
    bare = pod()
    bare["spec"]["containers"] = [
        {"env": [{"name": "DPUMESH_PORT", "value": "9102"}]}
    ]
    guard = agent.IngressGuard(enabled=True)
    assert guard.mesh_served([served, client, badport, leaving, bare]) == [
        ("10.244.1.20", 9101)
    ]
    # The rule is emitted in the normal form `iptables -S` prints, which is
    # what lets reconcile compare the observed chain by string equality.
    assert guard.rules([served]) == [
        "-A DPUMESH-PROTECT -d 10.244.1.20/32 -p tcp -m tcp --dport 9101"
        " -j REJECT --reject-with tcp-reset"
    ]
    disabled = agent.IngressGuard(enabled=False)
    disabled.reconcile([served])  # must not touch iptables when disabled

    # The peer is named by pid, so its start time has to be readable and stable.
    assert agent.process_start_time(os.getpid()) == agent.process_start_time(os.getpid())
    try:
        agent.process_start_time(-1)
    except agent.AttestationError:
        pass
    else:
        raise AssertionError("an unreadable peer state was accepted")
    try:
        agent.pod_uid_for_pid(os.getpid())
    except agent.AttestationError:
        pass
    else:
        raise AssertionError("a non-Pod peer process was attested")

    # Per-Pod broker ownership is serialized across simultaneous HELLOs and
    # failed launches use a bounded exponential delay.  Exercise the registry
    # without constructing the Kubernetes/network portions of Agent.
    registry = agent.Agent.__new__(agent.Agent)
    registry.spawned_lock = agent.threading.Lock()
    registry.spawned = {}
    registry.spawning_pods = set()
    registry.broker_retry = {}
    uid = pod()["metadata"]["uid"]
    registry.broker_spawn_claim(uid, now=100.0)
    try:
        registry.broker_spawn_claim(uid, now=100.0)
    except agent.AttestationError:
        pass
    else:
        raise AssertionError("two simultaneous brokers claimed one Pod")
    registry.broker_spawn_release(uid, success=False, now=100.0)
    assert registry.broker_retry[uid] == (105.0, 10.0)
    try:
        registry.broker_spawn_claim(uid, now=104.999)
    except agent.AttestationError:
        pass
    else:
        raise AssertionError("broker restart ignored the quiescence delay")
    registry.broker_spawn_claim(uid, now=105.0)
    registry.broker_spawn_release(uid, success=False, now=105.0)
    assert registry.broker_retry[uid] == (115.0, 20.0)
    registry.broker_spawn_claim(uid, now=115.0)
    registry.broker_spawn_release(uid, success=True, now=115.0)
    assert uid not in registry.broker_retry

    # The real request path waits through the bounded quiescence interval
    # instead of failing the workload's first restart and entering kubelet
    # CrashLoopBackOff.  Supplying `now=` above remains the nonblocking probe.
    registry.broker_retry[uid] = (205.0, 10.0)
    monotonic_values = iter((200.0, 205.0))
    sleeps = []
    original_monotonic = agent.time.monotonic
    original_sleep = agent.time.sleep
    agent.time.monotonic = lambda: next(monotonic_values)
    agent.time.sleep = lambda seconds: sleeps.append(seconds)
    try:
        registry.broker_spawn_claim(uid)
    finally:
        agent.time.monotonic = original_monotonic
        agent.time.sleep = original_sleep
    assert sleeps == [5.0]
    assert uid in registry.spawning_pods
    registry.broker_spawn_release(uid, success=True, now=205.0)

    # A workload process cannot request an assertion directly.  Only a broker
    # that the agent spawned and registered may cross the attestation boundary.
    class DirectRequest:
        def getsockopt(self, _level, _option, _length):
            return agent.struct.pack("3i", os.getpid(), 0, 0)

        def recv(self, _length):
            return agent.REQUEST.pack(
                b"DMESHAR1", agent.ASSERT_VERSION,
                agent.fixed_text("echo-dpumesh", 64, "service"), nonce,
            )

    direct = agent.Agent.__new__(agent.Agent)
    direct.broker_claims = lambda _pid, _uid: None
    try:
        direct.attest(DirectRequest())
    except agent.AttestationError as exc:
        assert "registered broker" in str(exc)
    else:
        raise AssertionError("a workload obtained an assertion without a broker")

    with tempfile.TemporaryDirectory() as temporary:
        key_dir = Path(temporary)
        os.chmod(key_dir, 0o700)
        (key_dir / "old-key-v1.key").write_bytes(key)
        os.chmod(key_dir / "old-key-v1.key", 0o400)
        (key_dir / "new-key-v2.key").write_bytes(bytes(reversed(key)))
        os.chmod(key_dir / "new-key-v2.key", 0o400)
        (key_dir / "active").write_text("old-key-v1\n", encoding="ascii")
        os.chmod(key_dir / "active", 0o400)
        assert agent.load_active_key(key_dir) == ("old-key-v1", key)
        (key_dir / "active").unlink()
        (key_dir / "active").write_text("new-key-v2\n", encoding="ascii")
        os.chmod(key_dir / "active", 0o400)
        assert agent.load_active_key(key_dir) == ("new-key-v2", bytes(reversed(key)))
    print("workload_attest_agent_test: PASS")


if __name__ == "__main__":
    main()
