"""Controller-side workload authorization and grant contract."""

import copy
import importlib.util
import tempfile
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "controller_workload_grant", ROOT / "controller" / "workload_grant.py"
)
assert SPEC and SPEC.loader
grant = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(grant)

UID = "12345678-1234-1234-1234-123456789abc"
CONTAINER_ID = "a" * 64
RESOURCE = "dpumesh.io/channel"


def pod():
    return {
        "metadata": {
            "uid": UID,
            "name": "echo-a",
            "namespace": "test-bench",
            "labels": {"app": "echo"},
        },
        "spec": {
            "nodeName": "rapids4",
            "serviceAccountName": "default",
            "automountServiceAccountToken": False,
            "containers": [{
                "name": "app",
                "resources": {
                    "requests": {RESOURCE: "1"},
                    "limits": {RESOURCE: "1"},
                },
            }],
        },
        "status": {
            "podIP": "10.244.0.9",
            "containerStatuses": [{
                "name": "app",
                "containerID": f"containerd://{CONTAINER_ID}",
            }],
        },
    }


def services():
    return [{
        "metadata": {"namespace": "test-bench", "name": "echo"},
        "spec": {
            "selector": {"app": "echo"},
            "clusterIP": "10.96.0.8",
            "ports": [{"port": 9091}],
        },
    }]


def slices(ready=True):
    return [{
        "metadata": {
            "namespace": "test-bench",
            "labels": {"kubernetes.io/service-name": "echo"},
        },
        "endpoints": [{
            "conditions": {"ready": ready},
            "targetRef": {"kind": "Pod", "uid": UID},
        }],
    }]


def authorize(candidate, endpoint_slices=None):
    return grant.resolve_authorized_pod(
        pod_uid=UID,
        node_name="rapids4",
        container_id=CONTAINER_ID,
        service_name="echo",
        pods=[candidate],
        services=services(),
        endpoint_slices=slices() if endpoint_slices is None else endpoint_slices,
        resource_name=RESOURCE,
    )


def refused(candidate):
    try:
        authorize(candidate)
    except grant.GrantError:
        return
    raise AssertionError("invalid workload received a grant")


def main():
    accepted = authorize(pod())
    seed = bytes(range(1, 33))
    nonce = bytes(range(1, 33))
    message = grant.build_grant(
        key=seed,
        key_id="node-ed25519-v1",
        cluster_id="test-cluster",
        service_name="echo",
        nonce=nonce,
        pod=accepted,
        container=accepted["spec"]["containers"][0],
        container_id=CONTAINER_ID,
        channel_slot=3,
        channel_generation=9,
        daemon_incarnation=bytes(range(16)),
        ttl=60,
        now=1_800_000_000,
    )
    assert len(message) == grant.ASSERT.size
    unpacked = grant.ASSERT.unpack(message)
    assert unpacked[0:2] == (grant.MSG_WORKLOAD_ASSERT, grant.ASSERT_VERSION)
    assert unpacked[7] == nonce
    assert unpacked[8:10] == (3, 9)
    assert unpacked[12].rstrip(b"\0") == b"test-cluster"
    assert unpacked[14].rstrip(b"\0") == UID.encode()
    assert unpacked[18].rstrip(b"\0") == b"app"
    assert unpacked[19].rstrip(b"\0") == CONTAINER_ID.encode()
    Ed25519PrivateKey.from_private_bytes(seed).public_key().verify(
        message[-64:], message[:-64]
    )
    try:
        grant.build_grant(
            key=seed,
            key_id="node-ed25519-v1",
            cluster_id="test-cluster",
            service_name="echo",
            nonce=nonce,
            pod=accepted,
            container=accepted["spec"]["containers"][0],
            container_id=CONTAINER_ID,
            channel_slot=grant.MAX_CHANNEL_SLOTS,
            channel_generation=9,
            daemon_incarnation=bytes(range(16)),
            ttl=60,
        )
    except grant.GrantError:
        pass
    else:
        raise AssertionError("grant exceeded the DPU channel-slot table")

    bad = copy.deepcopy(pod())
    bad["spec"]["automountServiceAccountToken"] = True
    refused(bad)
    bad = copy.deepcopy(pod())
    bad["spec"]["volumes"] = [{
        "name": "token",
        "projected": {"sources": [{"serviceAccountToken": {"path": "token"}}]},
    }]
    refused(bad)
    bad = copy.deepcopy(pod())
    bad["spec"]["containers"].append(copy.deepcopy(bad["spec"]["containers"][0]))
    bad["spec"]["containers"][1]["name"] = "second"
    refused(bad)
    bad = copy.deepcopy(pod())
    bad["spec"]["containers"][0]["resources"]["limits"][RESOURCE] = "2"
    refused(bad)
    bad = copy.deepcopy(pod())
    bad["status"]["containerStatuses"][0]["containerID"] = "containerd://" + "b" * 64
    refused(bad)
    bad = copy.deepcopy(pod())
    bad["metadata"]["deletionTimestamp"] = "2026-09-05T00:00:00Z"
    refused(bad)
    for endpoint_slices in (slices(False), slices(None), []):
        try:
            authorize(pod(), endpoint_slices)
        except grant.GrantError:
            pass
        else:
            raise AssertionError("Pod without a ready Service endpoint received a grant")
    headless = services()
    headless[0]["spec"]["clusterIP"] = "None"
    try:
        grant.resolve_authorized_pod(
            pod_uid=UID,
            node_name="rapids4",
            container_id=CONTAINER_ID,
            service_name="echo",
            pods=[pod()],
            services=headless,
            endpoint_slices=slices(),
            resource_name=RESOURCE,
        )
    except grant.GrantError:
        pass
    else:
        raise AssertionError("Pod for a headless Service received a grant")

    with tempfile.TemporaryDirectory() as temporary:
        node_dir = Path(temporary) / "rapids4"
        node_dir.mkdir()
        assert grant.load_private_seed(Path(temporary), "rapids4", lambda path: (
            "node-ed25519-v1", seed if path == node_dir else b""
        )) == ("node-ed25519-v1", seed)

    print("workload_grant_controller_test: PASS")


if __name__ == "__main__":
    main()
