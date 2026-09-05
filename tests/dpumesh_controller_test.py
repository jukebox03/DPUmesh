import argparse
import hashlib
import hmac
import importlib.util
import json
import sys
import tempfile
import threading
import urllib.error
import urllib.request
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "dpumesh_controller", ROOT / "controller" / "dpumesh_controller.py"
)
assert SPEC and SPEC.loader
controller = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(controller)

UID_A = "12345678-1234-1234-1234-123456789abc"
UID_B = "abcdef01-2345-6789-abcd-ef0123456789"
PUB_HEX = "62b8205a7e8ee63039adca07a1e9aa6d069605ce54648314ee77ca74b457b5bd"


def pods():
    return [
        {
            "metadata": {
                "uid": UID_A,
                "namespace": "test-bench",
                "labels": {"app": "echo-dpumesh"},
            },
            "spec": {
                "nodeName": "rapids4",
                "serviceAccountName": "default",
                "automountServiceAccountToken": False,
                "containers": [{
                    "name": "app",
                    "resources": {
                        "requests": {"dpumesh.io/channel": 1},
                        "limits": {"dpumesh.io/channel": 1},
                    },
                }],
            },
            "status": {
                "podIP": "10.244.0.5",
                "containerStatuses": [{
                    "name": "app",
                    "containerID": f"containerd://{'a' * 64}",
                }],
            },
        },
        {   # no IP yet: not placeable, silently held for the next generation
            "metadata": {"uid": UID_B, "namespace": "test-bench"},
            "spec": {"nodeName": "rapids4"},
            "status": {},
        },
    ]


def services():
    return [
        {
            "metadata": {"namespace": "test-bench", "name": "echo-dpumesh"},
            "spec": {
                "clusterIP": "10.96.0.11",
                "ports": [{"port": 9091}],
                "selector": {"app": "echo-dpumesh"},
            },
        },
        {   # headless: no dialable ClusterIP, so no service= record
            "metadata": {"namespace": "test-bench", "name": "headless"},
            "spec": {"clusterIP": "None", "ports": [{"port": 1}]},
        },
    ]


def slices():
    return [
        {
            "metadata": {
                "namespace": "test-bench",
                "labels": {"kubernetes.io/service-name": "echo-dpumesh"},
            },
            "endpoints": [
                {"conditions": {"ready": True}, "targetRef": {"kind": "Pod", "uid": UID_A}},
                # not placed (no IP): dropped so the consumer never refuses
                {"conditions": {"ready": True}, "targetRef": {"kind": "Pod", "uid": UID_B}},
                {"conditions": {"ready": False}, "targetRef": {"kind": "Pod", "uid": UID_A}},
            ],
        },
    ]


class FakeKube:
    def __init__(self):
        self.pod_list = pods()

    def pods(self):
        return self.pod_list

    def services(self):
        return services()

    def endpoint_slices(self):
        return slices()

def http_status(url, data=None):
    request = urllib.request.Request(url, data=data)
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()


def routes_and_publishing(temporary):
    key_dir = Path(temporary) / "keys"
    key_dir.mkdir()
    (key_dir / "active").write_text("controller-v1", encoding="ascii")
    key_file = key_dir / "controller-v1.key"
    key_file.write_bytes(bytes(range(1, 33)))
    key_file.chmod(0o600)
    feed_key_dir = Path(temporary) / "feed-keys"
    feed_key_dir.mkdir()
    (feed_key_dir / "active").write_text("feed-v1", encoding="ascii")
    feed_key = bytes(range(33, 65))
    (feed_key_dir / "feed-v1.key").write_bytes(feed_key)
    (feed_key_dir / "feed-v1.key").chmod(0o600)
    nodes_file = Path(temporary) / "nodes"
    nodes_file.write_text(
        f"rapids4 192.168.100.2:4791 node-ed25519-v1 {PUB_HEX} {'0' * 64}\n",
        encoding="ascii",
    )
    args = argparse.Namespace(
        key_dir=key_dir, feed_key_dir=feed_key_dir, nodes_file=nodes_file,
        output=Path(temporary) / "topology.v1", protected=[],
        resource_name="dpumesh.io/channel", cluster_id="test-cluster",
        api_server="", api_token_file=Path("/nonexistent"), api_ca_file=Path("/nonexistent"),
    )
    real_kube, controller.KubernetesAPI = controller.KubernetesAPI, lambda *_args: FakeKube()
    try:
        ctl = controller.Controller(args)
    finally:
        controller.KubernetesAPI = real_kube

    version = ctl.publish()
    assert version is not None
    published = args.output.read_text(encoding="ascii")

    # An unchanged cluster retains the same signed document and version.
    assert ctl.publish() is None
    assert args.output.read_text(encoding="ascii") == published

    # A restart serves the installed generation before any publication.
    controller.KubernetesAPI = lambda *_args: FakeKube()
    try:
        again = controller.Controller(args)
    finally:
        controller.KubernetesAPI = real_kube
    assert again.held()[0] == published

    reporter = controller.ControllerHandler.reporter
    controller.ControllerHandler.reporter = lambda _self: "rapids4"
    server = controller.ControllerServer(("127.0.0.1", 0), ctl)
    assert controller.CONTROLLER_REQUEST_TIMEOUT == 10.0
    for _index in range(controller.CONTROLLER_REQUEST_MAX):
        assert server.request_slots.acquire(blocking=False)
    assert not server.request_slots.acquire(blocking=False)
    for _index in range(controller.CONTROLLER_REQUEST_MAX):
        server.request_slots.release()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"
    try:
        status, membership = http_status(f"{base}/membership.v1")
        assert status == 200
        prefix, envelope = membership.rsplit(b"signature=", 1)
        key_id, mac = envelope.strip().split(b",", 1)
        assert key_id == b"feed-v1"
        assert hmac.new(feed_key, prefix, hashlib.sha256).hexdigest().encode() == mac
        assert f"member={UID_A},-\n".encode() in membership
        assert f"member={UID_A},echo-dpumesh\n".encode() in membership

        assert http_status(f"{base}/healthz")[0] == 200

        status, targets = http_status(f"{base}/service-targets.v1")
        assert status == 200
        prefix, envelope = targets.rsplit(b"signature=", 1)
        key_id, mac = envelope.strip().split(b",", 1)
        assert key_id == b"feed-v1"
        assert hmac.new(feed_key, prefix, hashlib.sha256).hexdigest().encode() == mac
        assert b"test-bench/echo-dpumesh=10.96.0.11:9091\n" in targets
        assert f"endpoint=test-bench/echo-dpumesh,10.244.0.5:9091,{UID_A}\n".encode() in targets

        status, body = http_status(f"{base}/topology.v1")
        assert status == 200 and body.decode("ascii") == published

        # The mTLS caller can query only workloads placed on its node.
        status, body = http_status(f"{base}/workload-scope?pod_uid={UID_A}")
        assert status == 200 and json.loads(body) == {"pod_uid": UID_A, "node": "rapids4"}
        elsewhere = dict(ctl.kubernetes.pod_list[0], metadata={"uid": UID_B, "namespace": "x"})
        elsewhere["spec"] = {"nodeName": "rapids5", "serviceAccountName": "default"}
        ctl.kubernetes.pod_list.append(elsewhere)
        assert ctl.publish() is not None
        assert http_status(f"{base}/workload-scope?pod_uid={UID_B}")[0] == 403
        missing = "9999f0f0-0000-4000-8000-000000000000"
        assert http_status(f"{base}/workload-scope?pod_uid={missing}")[0] == 404
        assert http_status(f"{base}/workload-scope?pod_uid=not-a-uid")[0] == 400

        # A node report may fill the DPU key but may not rewrite the address
        # the operator assigned to that node.
        report = json.dumps({"name": "rapids4", "rdma": "192.168.100.2:4791",
                             "dpu_public_key": "ab" * 32}).encode("ascii")
        assert http_status(f"{base}/node", data=report)[0] == 200
        wrong_address = report.replace(b"192.168.100.2", b"192.168.100.9")
        assert http_status(f"{base}/node", data=wrong_address)[0] == 400
        someone_else = report.replace(b"rapids4", b"rapids5")
        assert http_status(f"{base}/node", data=someone_else)[0] == 403
        assert http_status(f"{base}/node", data=b"x" * 5000)[0] == 413
        assert http_status(f"{base}/node", data=b"{not json")[0] == 400
        assert ctl.publish() is not None
        assert f"{'ab' * 32}" in args.output.read_text(encoding="ascii")

        # A rotated signing key republishes even an unchanged cluster: the
        # held document must never outlive the key that signed it.
        rotated = key_dir / "controller-v2.key"
        rotated.write_bytes(bytes(range(2, 34)))
        rotated.chmod(0o600)
        (key_dir / "active").write_text("controller-v2", encoding="ascii")
        assert ctl.publish() is not None
        assert controller.signature_key_id(ctl.held()[0]) == "controller-v2"
        assert ctl.publish() is None
    finally:
        server.shutdown()
        server.server_close()
        controller.ControllerHandler.reporter = reporter


def main():
    class Peer:
        def __init__(self, certificate):
            self.certificate = certificate

        def getpeercert(self):
            return self.certificate

    assert controller.node_from_peer_certificate(Peer(None)) is None
    assert controller.node_from_peer_certificate(Peer({})) is None
    assert controller.node_from_peer_certificate(Peer({
        "subjectAltName": (("URI", "spiffe://dpumesh.io/node/rapids4"),)
    })) == "rapids4"
    assert controller.node_from_peer_certificate(Peer({
        "subjectAltName": (
            ("URI", "spiffe://dpumesh.io/node/rapids4"),
            ("URI", "spiffe://example.test/extra"),
        )
    })) is None

    node_line = f"node=rapids4,192.168.100.2:4791,node-ed25519-v1,{PUB_HEX},{'0' * 64}"
    body = controller.build_body(
        7,
        [node_line],
        pods(),
        services(),
        slices(),
        ["test-bench/echo-dpumesh", "test-bench/absent"],
    )
    lines = body.splitlines()
    assert lines[0] == "version=7"
    assert node_line in lines
    assert f"pod={UID_A},rapids4,test-bench,default,10.244.0.5" in lines
    assert not any(UID_B in line for line in lines)
    assert "service=test-bench/echo-dpumesh,10.96.0.11:9091" in lines
    assert not any("headless" in line for line in lines)
    assert lines.count(f"endpoint=test-bench/echo-dpumesh,{UID_A}") == 1
    assert "protected=test-bench/echo-dpumesh" in lines
    # A protected Service the generation does not define is dropped, never
    # emitted dangling: the consumer would refuse the whole document.
    assert not any("absent" in line for line in lines)

    # The generation bounds refuse the whole publication, never truncate.
    for over, name in (
        (lambda: controller.build_body(1, [node_line] * (controller.GEN_NODE_MAX + 1),
                                       [], [], [], []), "nodes"),
        (lambda: controller.build_body(
            1, [],
            [],
            [{"metadata": {"namespace": "ns", "name": f"s{i}"},
              "spec": {"clusterIP": "10.96.0.1", "ports": [{"port": 1}]}}
             for i in range(controller.GEN_SERVICE_MAX + 1)],
            [], []), "services"),
    ):
        try:
            over()
        except controller.ControllerError:
            pass
        else:
            raise AssertionError(f"an over-bound {name} generation was published")

    # The envelope verifies with the public half only, over the exact body.
    key = bytes(range(1, 33))
    document = controller.sign_document(body, "controller-v1", key)
    assert document.startswith(body)
    envelope = document[len(body):].strip()
    key_id, _, signature_hex = envelope.removeprefix("signature=").partition(",")
    assert key_id == "controller-v1" and len(signature_hex) == 128
    public = controller.Ed25519PrivateKey.from_private_bytes(key).public_key()
    public.verify(bytes.fromhex(signature_hex), body.encode("ascii"))

    # Publication continues from the installed generation across restarts.
    with tempfile.TemporaryDirectory() as temporary:
        out = Path(temporary) / "topology.v1"
        out.write_text(document, encoding="ascii")
        assert controller.published_version(out) == 7

        nodes = Path(temporary) / "nodes"
        nodes.write_text(
            f"# comment\nrapids4 192.168.100.2:4791 node-ed25519-v1 {PUB_HEX} {'0' * 64}\n",
            encoding="ascii",
        )
        registry = controller.NodeRegistry(nodes)
        assert registry.lines() == [node_line]
        assert registry.names() == {"rapids4"}

        # A report supplies the DPU static key; operator configuration owns the
        # address and host-runtime identity.
        registry.report("rapids4", "192.168.100.2:4791", "ab" * 32)
        assert registry.lines() == [
            f"node=rapids4,192.168.100.2:4791,node-ed25519-v1,{PUB_HEX},{'ab' * 32}"
        ]
        for rdma, key in (("no-port", "ab" * 32),
                          ("192.168.100.9:4791", "ab" * 32),
                          ("192.168.100.2:4791", "0" * 64),
                          ("192.168.100.2:4791", "zz" * 32)):
            try:
                registry.report("rapids4", rdma, key)
            except controller.ControllerError:
                pass
            else:
                raise AssertionError(f"a malformed report was accepted: {rdma} {key}")

        nodes.write_text("rapids4 bad-addr k deadbeef 00\n", encoding="ascii")
        try:
            controller.read_nodes_file(nodes)
        except controller.ControllerError:
            pass
        else:
            raise AssertionError("a malformed node record was accepted")

        nodes.write_text(
            f"rapids4 192.168.100.2:4791 node-ed25519-v1 {PUB_HEX} {'0' * 64}\n"
            f"rapids4 192.168.100.3:4791 node-ed25519-v1 {PUB_HEX} {'0' * 64}\n",
            encoding="ascii",
        )
        try:
            controller.read_nodes_file(nodes)
        except controller.ControllerError:
            pass
        else:
            raise AssertionError("a duplicate node record was accepted")

    # The mediated lookup answers from the same document every DPU holds.
    placements = controller.pod_placements(document)
    assert placements == {UID_A: "rapids4"}

    with tempfile.TemporaryDirectory() as temporary:
        routes_and_publishing(temporary)

    print("dpumesh_controller_test: PASS")


if __name__ == "__main__":
    main()
