"""DPU feed receiver and host delivery contract tests."""

from __future__ import annotations

import hashlib
import importlib.util
import socket
import sys
import tempfile
import threading
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from node import dpumeshd

SPEC = importlib.util.spec_from_file_location(
    "dpu_feed_receiver", ROOT / "dpu" / "feed_receiver.py"
)
assert SPEC and SPEC.loader
receiver = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(receiver)


def main() -> None:
    for malformed in (
        "BAD topology 1 " + "0" * 64,
        "DMESHFEED1 unknown 1 " + "0" * 64,
        "DMESHFEED1 topology x " + "0" * 64,
        "DMESHFEED1 topology 0 " + hashlib.sha256(b"").hexdigest(),
        "DMESHFEED1 topology 1 bad",
    ):
        try:
            receiver.parse_header(malformed)
        except receiver.FeedError:
            pass
        else:
            raise AssertionError(f"accepted malformed header {malformed!r}")

    over = receiver.FEEDS["membership"][1] + 1
    try:
        receiver.parse_header(f"DMESHFEED1 membership {over} {'0' * 64}")
    except receiver.FeedError:
        pass
    else:
        raise AssertionError("accepted an oversized feed")

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        node_key = "ab" * 32
        (root / "node-static.pub").write_text(node_key, encoding="ascii")
        server = receiver.Receiver(("127.0.0.1", 0), root, 2.0)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        address = server.server_address
        try:
            payload = b"version=1\nsignature=test,valid\n"
            dpumeshd.deliver_feed(address, "topology", payload, 2.0)
            target = root / "topology.v1"
            assert target.read_bytes() == payload
            assert target.stat().st_mode & 0o777 == 0o644
            assert receiver.held_digest(root, "topology") == hashlib.sha256(payload).hexdigest()
            dpumeshd.deliver_feed(address, "topology", payload, 2.0)
            assert dpumeshd.dpu_node_key(address, 2.0) == node_key

            with socket.create_connection(address, timeout=2.0) as connection:
                connection.sendall(f"DMESHFEED1 unknown 1 {'0' * 64}\n".encode())
                assert connection.recv(128).startswith(b"refused ")
        finally:
            server.shutdown()
            server.server_close()
            thread.join(2.0)

    print("dpu_feed_test: PASS")


if __name__ == "__main__":
    main()
