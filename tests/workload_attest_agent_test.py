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
