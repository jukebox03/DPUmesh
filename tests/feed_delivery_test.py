"""The node agent's delivery hop: what it installs, and what it refuses.

The hop carries no authority — every feed it moves is signed and the DPU
refuses what it cannot verify — so what is worth testing is the least
privilege it does hold: four feeds and no others, a bound at the door, an
install that is atomic, and a loop that resends what a lost connection may
have taken with it.
"""

import hashlib
import hmac
import importlib.util
import socket
import sys
import tempfile
import threading
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


receiver = load("dpumesh_feed_receiver", "bench/dpumesh_feed_receiver.py")
delivery = load("feed_delivery", "bench/feed_delivery.py")

BUNDLE = {
    "key.p8": b"\x01\x02",
    "csr.der": b"\x03",
    "token.txt": b"token",
    "trust-anchors.pem": b"-----BEGIN CERTIFICATE-----\n",
}


def bundle_payload(members=BUNDLE):
    parts = []
    for name, content in members.items():
        parts.append(f"member {name} {len(content)}\n".encode("ascii"))
        parts.append(content)
    return b"".join(parts)


DIGEST = "ab" * 32


def test_header_whitelist():
    assert receiver.parse_header(f"DMESHFEED1 topology 10 {DIGEST}") == ("topology", 10, DIGEST)
    for line, why in (
        (f"DMESHFEED0 topology 10 {DIGEST}", "unknown framing"),
        (f"DMESHFEED1 ../../etc/shadow 10 {DIGEST}", "a path is not a feed name"),
        (f"DMESHFEED1 admission 10 {DIGEST}", "a feed outside the four"),
        (f"DMESHFEED1 topology x {DIGEST}", "a non-numeric length"),
        (f"DMESHFEED1 membership {256 * 1024 + 1} {DIGEST}", "over the feed's bound"),
        ("DMESHFEED1 topology 10", "a truncated header"),
        ("DMESHFEED1 topology 10 nothex", "a malformed digest"),
    ):
        try:
            receiver.parse_header(line)
        except receiver.FeedError:
            continue
        raise AssertionError(f"accepted {why}: {line!r}")


def test_bundle_framing():
    members = receiver.parse_bundle(bundle_payload())
    assert dict(members) == BUNDLE
    for payload, why in (
        (bundle_payload({k: v for k, v in list(BUNDLE.items())[:2]}), "an incomplete bundle"),
        (bundle_payload() + b"member key.p8 1\n!", "a duplicate member"),
        (b"member evil.sh 1\n!", "a member outside the four"),
        (b"member key.p8 99\n!", "a truncated member"),
        (b"garbage", "an unframed payload"),
    ):
        try:
            receiver.parse_bundle(payload)
        except receiver.FeedError:
            continue
        raise AssertionError(f"accepted {why}")


def test_install_is_atomic_and_bounded():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        receiver.install_feed(root, "topology", b"version=1\n")
        assert (root / "topology.v1").read_bytes() == b"version=1\n"
        # A second install replaces by rename, so no partial document is ever
        # observable at the target the consumer polls.
        receiver.install_feed(root, "topology", b"version=2\n")
        assert (root / "topology.v1").read_bytes() == b"version=2\n"
        assert not list(root.glob(".*.new"))

        receiver.install_feed(root, "identity-bundle", bundle_payload())
        for name, content in BUNDLE.items():
            assert (root / "linkerd-identity" / name).read_bytes() == content
        assert (root / "linkerd-identity" / "key.p8").stat().st_mode & 0o077 == 0


def serve_one(root, results):
    server = receiver.Receiver(("127.0.0.1", 0), root, 5.0)
    results.append(server.server_address)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def test_round_trip_and_resend():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        addresses = []
        server = serve_one(root, addresses)
        address = addresses[0]

        published = {"payload": b"version=1\n"}
        item = delivery.Delivery("membership", lambda: published["payload"])
        log = []
        assert item.step(address, 5.0, log.append) is True
        assert (root / "membership.v1").read_bytes() == b"version=1\n"

        # An unchanged document costs a header, not a transfer: the receiver
        # answers `have` from the digest of what it holds.
        assert item.step(address, 5.0, log.append) is False
        assert item.failures == 0

        # An unreachable receiver is a failure and withdraws nothing.
        server.shutdown()
        server.server_close()
        assert item.step(("127.0.0.1", address[1]), 0.5, log.append) is False
        assert item.failures == 1

        # What decides a resend is the far end's state, not this end's memory,
        # so a receiver that lost the document gets it again.
        addresses = []
        server = serve_one(root, addresses)
        (root / "membership.v1").unlink()
        assert item.step(addresses[0], 5.0, log.append) is True
        assert (root / "membership.v1").read_bytes() == b"version=1\n"
        assert item.failures == 0
        server.shutdown()
        server.server_close()


def test_over_bound_is_refused_at_the_door():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        addresses = []
        server = serve_one(root, addresses)
        oversized = b"x" * (256 * 1024 + 1)
        try:
            delivery.deliver_once(addresses[0], "membership", oversized, 5.0)
        except delivery.DeliveryError as exc:
            assert "bound" in str(exc)
        else:
            raise AssertionError("an over-bound payload was installed")
        assert not (root / "membership.v1").exists()
        server.shutdown()
        server.server_close()


def test_one_failing_publisher_does_not_stop_the_others():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        addresses = []
        server = serve_one(root, addresses)

        def broken():
            raise RuntimeError("publisher is down")

        loop = delivery.DeliveryLoop(
            addresses[0],
            [delivery.Delivery("service-targets", broken),
             delivery.Delivery("membership", lambda: b"version=3\n")],
            interval=0.01,
        )
        assert loop.round() == 1
        assert (root / "membership.v1").read_bytes() == b"version=3\n"
        # A source that raises is not a delivery failure: it withdraws nothing
        # and does not back the loop off.
        assert loop.backoff == loop.interval
        server.shutdown()
        server.server_close()


GENERATION = """version=42
node=rapids4,192.168.100.2:4791,node-ed25519-v1,{a},{b}
pod=12345678-1234-1234-1234-123456789abc,rapids4,test-bench,default,10.244.0.5
pod=abcdef01-2345-6789-abcd-ef0123456789,rapids5,test-bench,default,10.244.1.7
service=test-bench/echo-dpumesh,10.96.0.11:9091
service=test-bench/other,10.96.0.12:80
endpoint=test-bench/echo-dpumesh,12345678-1234-1234-1234-123456789abc
endpoint=test-bench/echo-dpumesh,abcdef01-2345-6789-abcd-ef0123456789
endpoint=test-bench/other,12345678-1234-1234-1234-123456789abc
protected=test-bench/echo-dpumesh
signature=controller-v1,{sig}
""".format(a="aa" * 32, b="bb" * 32, sig="cc" * 64)


def test_the_hop_serves_exactly_one_read():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        addresses = []
        server = serve_one(root, addresses)
        address = addresses[0]

        # A DPU that has not generated a credential has none to report.
        try:
            delivery.read_node_key(address, 5.0)
        except delivery.DeliveryError:
            pass
        else:
            raise AssertionError("a missing node credential was reported as one")

        key = "ab" * 32
        (root / "node-static.pub").write_text(key + "\n", encoding="ascii")
        assert delivery.read_node_key(address, 5.0) == key

        # Only a well-formed key is served: anything else is not a credential
        # and reporting it would publish a binding nothing can authenticate.
        (root / "node-static.pub").write_text("not-a-key\n", encoding="ascii")
        try:
            delivery.read_node_key(address, 5.0)
        except delivery.DeliveryError:
            pass
        else:
            raise AssertionError("a malformed node credential was served")

        server.shutdown()
        server.server_close()


def test_service_targets_derive_from_the_generation():
    key = b"\x11" * 32
    feed = delivery.service_targets_document(
        GENERATION, ["test-bench/echo-dpumesh"], 99, "feed-hmac-v1", key
    )
    lines = feed.splitlines()
    assert lines[0] == "version=99"
    assert "test-bench/echo-dpumesh=10.96.0.11:9091" in lines
    # Endpoints are the generation's, resolved through its signed Pod IPs, and
    # a Service the caller did not name contributes nothing.
    assert ("endpoint=test-bench/echo-dpumesh,10.244.0.5:9091,"
            "12345678-1234-1234-1234-123456789abc") in lines
    assert ("endpoint=test-bench/echo-dpumesh,10.244.1.7:9091,"
            "abcdef01-2345-6789-abcd-ef0123456789") in lines
    assert not any("other" in line for line in lines)

    body = "".join(line + "\n" for line in lines[:-1])
    key_id, _, mac = lines[-1].removeprefix("signature=").partition(",")
    assert key_id == "feed-hmac-v1"
    assert hmac.compare_digest(
        mac, hmac.new(key, body.encode("ascii"), hashlib.sha256).hexdigest()
    )

    # A Service no generation names yet produces nothing rather than a
    # withdrawal of the targets the DPU already holds.
    assert delivery.service_targets_document(
        GENERATION, ["test-bench/absent"], 99, "feed-hmac-v1", key
    ) is None


def main():
    test_header_whitelist()
    test_bundle_framing()
    test_install_is_atomic_and_bounded()
    test_round_trip_and_resend()
    test_over_bound_is_refused_at_the_door()
    test_one_failing_publisher_does_not_stop_the_others()
    test_the_hop_serves_exactly_one_read()
    test_service_targets_derive_from_the_generation()
    print("feed_delivery_test: PASS")


if __name__ == "__main__":
    main()
