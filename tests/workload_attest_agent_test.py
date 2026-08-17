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
    }


def main():
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

    service = {
        "metadata": {"namespace": "test-bench", "name": "echo-dpumesh"},
        "spec": {"selector": {"app": "echo-dpumesh"}},
    }
    agent.authorize_service(11, {11: "echo-dpumesh"}, pod(), [service])
    try:
        agent.authorize_service(12, {12: "other"}, pod(), [service])
    except agent.AttestationError:
        pass
    else:
        raise AssertionError("wrong Service membership was accepted")

    key = bytes(range(1, 33))
    nonce = bytes(range(32))
    grant = agent.build_grant(
        key=key,
        issuer="dpumesh-node-agent",
        key_id="node-hmac-v1",
        service_id=11,
        nonce=nonce,
        pod=pod(),
        ttl=60,
        now=1_800_000_000,
    )
    assert len(grant) == 1090
    assert grant[0:2] == bytes([12, 1])
    assert hmac.compare_digest(
        grant[-32:], hmac.new(key, grant[:-32], hashlib.sha256).digest()
    )
    unpacked = agent.GRANT.unpack(grant)
    assert unpacked[4] == 11
    assert unpacked[8] == nonce
    assert unpacked[12].rstrip(b"\0") == b"test-bench"

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
