#!/usr/bin/env python3
r"""The DPU end of the node agent's delivery hop.

The node agent is the DPU's only control peer, and this is the door it puts
feeds through. The daemon holds no authentication authority of its own: every
feed it installs is signed and every consumer refuses what it cannot verify.
What it does hold is the least privilege that installing them needs — it can
write the four feed files below and nothing else, it runs as an unprivileged
account under a system unit (so a node reboot restores it with no operator
session), it bounds each payload at the door, and it installs by rename, which
is the contract every consumer already assumes.

Wire framing, one feed per connection:

    > DMESHFEED1 <feed-name> <length> <sha256-hex>\n
    < send | have | refused <reason>
    > <length bytes>                        (only after `send`)
    < ok <feed> <length> | refused <reason>

and one read, of one thing:

    > DMESHNODE1\n
    < key <hex64> | refused <reason>

The node credential is generated on the DPU and its private half never leaves;
the public half has to reach the controller, and the agent is the only peer
that can carry it. Serving exactly that one file, and nothing else, is what
keeps the hop's privilege at what carrying it needs.

The digest in the header is what makes resend-on-reconnect cheap and correct.
The receiver answers `have` only when the document it currently holds hashes to
the same value, so a feed is re-transferred exactly when this end lost it —
after a reboot, an interrupted install, or a receiver that never got it — and
never merely because a connection dropped. The sender therefore needs no memory
of what it delivered, which is what lets the loop recover from its own restarts
as well as from the DPU's.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import socket
import socketserver
import sys
from pathlib import Path

MAGIC = "DMESHFEED1"
NODE_MAGIC = "DMESHNODE1"
HEADER_MAX = 160
NAME_RE = re.compile(r"[a-z][a-z0-9-]{0,31}")
MEMBER_RE = re.compile(r"[a-z0-9][a-z0-9._-]{0,31}")
HEX64_RE = re.compile(r"[0-9a-f]{64}")

# Feed name -> (relative target, byte bound, mode). The bounds are the
# consumers' own: membership is one node's 256 KiB document, the topology
# generation carries the whole cluster, and the Service-target feed is a
# per-Service list. A payload over the bound is refused at the door rather than
# written and rejected later.
FEEDS: dict[str, tuple[str, int, int]] = {
    "membership": ("membership.v1", 256 * 1024, 0o644),
    "topology": ("topology.v1", 16 * 1024 * 1024, 0o644),
    "service-targets": ("service-targets.v1", 1024 * 1024, 0o644),
    "identity-bundle": ("linkerd-identity", 256 * 1024, 0o700),
}

# Members of the identity bundle, in the order they are framed, with the mode
# each is installed under. A private key and a service-account token are
# readable only by the account that runs the proxy; the certificate request and
# the trust anchors are public.
BUNDLE_MEMBERS: dict[str, int] = {
    "key.p8": 0o600,
    "csr.der": 0o644,
    "token.txt": 0o600,
    "trust-anchors.pem": 0o644,
}


class FeedError(RuntimeError):
    pass


def read_exactly(connection: socket.socket, count: int) -> bytes:
    chunks: list[bytes] = []
    remaining = count
    while remaining:
        chunk = connection.recv(min(remaining, 1 << 16))
        if not chunk:
            raise FeedError("payload truncated")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_node_key(path: Path) -> str:
    """The DPU's static handshake public key, as 64 hex characters."""
    try:
        key = path.read_text(encoding="ascii").strip()
    except OSError as exc:
        raise FeedError("no node credential") from exc
    if HEX64_RE.fullmatch(key) is None:
        raise FeedError("malformed node credential")
    return key


def parse_header(line: str) -> tuple[str, int, str]:
    try:
        magic, name, length, digest = line.split(" ")
    except ValueError as exc:
        raise FeedError("malformed header") from exc
    if magic != MAGIC:
        raise FeedError("unknown framing")
    if NAME_RE.fullmatch(name) is None or name not in FEEDS:
        raise FeedError("unknown feed")
    if not length.isdigit():
        raise FeedError("malformed length")
    size = int(length)
    if size > FEEDS[name][1]:
        raise FeedError("over bound")
    if HEX64_RE.fullmatch(digest) is None:
        raise FeedError("malformed digest")
    return name, size, digest


def read_header(connection: socket.socket) -> tuple[str, int, str]:
    """One bounded line. A header that does not terminate inside HEADER_MAX
    bytes is refused without allocating anything the sender chose the size of."""
    line = bytearray()
    while len(line) < HEADER_MAX:
        byte = connection.recv(1)
        if not byte:
            raise FeedError("header truncated")
        if byte == b"\n":
            break
        line += byte
    else:
        raise FeedError("header over bound")
    try:
        text = line.decode("ascii")
    except UnicodeDecodeError as exc:
        raise FeedError("malformed header") from exc
    if text == NODE_MAGIC:
        return (NODE_MAGIC, 0, "")
    return parse_header(text)


def install_file(target: Path, payload: bytes, mode: int) -> None:
    """`.<target>.new` then rename: a consumer polling the target never
    observes a partial document, which is the contract the membership and
    topology consumers are both written against."""
    target.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
    temporary = target.with_name(f".{target.name}.new")
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
    try:
        os.write(descriptor, payload)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    os.chmod(temporary, mode)
    os.replace(temporary, target)


def frame_bundle(members: list[tuple[str, bytes]]) -> bytes:
    parts: list[bytes] = []
    for name, content in members:
        parts.append(f"member {name} {len(content)}\n".encode("ascii"))
        parts.append(content)
    return b"".join(parts)


def parse_bundle(payload: bytes) -> list[tuple[str, bytes]]:
    """`member <name> <length>\\n<bytes>`, repeated. The identity material is
    four files that have to move together — a proxy holding a key from one
    issuance and a token from another authenticates as neither."""
    members: list[tuple[str, bytes]] = []
    seen: set[str] = set()
    offset = 0
    while offset < len(payload):
        end = payload.find(b"\n", offset, offset + HEADER_MAX)
        if end < 0:
            raise FeedError("malformed bundle member header")
        try:
            keyword, name, length = payload[offset:end].decode("ascii").split(" ")
        except (UnicodeDecodeError, ValueError) as exc:
            raise FeedError("malformed bundle member header") from exc
        if keyword != "member" or MEMBER_RE.fullmatch(name) is None:
            raise FeedError("malformed bundle member header")
        if name not in BUNDLE_MEMBERS:
            raise FeedError(f"unknown bundle member {name}")
        if name in seen:
            raise FeedError(f"duplicate bundle member {name}")
        if not length.isdigit():
            raise FeedError("malformed bundle member length")
        size = int(length)
        start = end + 1
        if start + size > len(payload):
            raise FeedError("bundle member truncated")
        seen.add(name)
        members.append((name, payload[start : start + size]))
        offset = start + size
    if seen != set(BUNDLE_MEMBERS):
        raise FeedError("incomplete identity bundle")
    return members


def held_digest(root: Path, name: str) -> str | None:
    """What this end currently holds for one feed, or None when it holds
    nothing. Read back from the installed file rather than remembered, so a
    receiver that restarts still recognizes the document it already has."""
    relative, bound, _mode = FEEDS[name]
    target = root / relative
    try:
        if name != "identity-bundle":
            payload = target.read_bytes()
        else:
            payload = frame_bundle(
                [(member, (target / member).read_bytes()) for member in BUNDLE_MEMBERS]
            )
    except OSError:
        return None
    if len(payload) > bound:
        return None
    return hashlib.sha256(payload).hexdigest()


def install_feed(root: Path, name: str, payload: bytes) -> None:
    relative, _bound, mode = FEEDS[name]
    target = root / relative
    if name != "identity-bundle":
        install_file(target, payload, mode)
        return
    members = parse_bundle(payload)
    target.mkdir(mode=mode, parents=True, exist_ok=True)
    os.chmod(target, mode)
    for member, content in members:
        install_file(target / member, content, BUNDLE_MEMBERS[member])


class Handler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        self.request.settimeout(self.server.feed_timeout)
        try:
            name, size, digest = read_header(self.request)
            if name == NODE_MAGIC:
                key = read_node_key(self.server.node_key_path)
                self.request.sendall(f"key {key}\n".encode("ascii"))
                return
            if held_digest(self.server.feed_root, name) == digest:
                self.request.sendall(b"have\n")
                return
            self.request.sendall(b"send\n")
            payload = read_exactly(self.request, size)
            if hashlib.sha256(payload).hexdigest() != digest:
                raise FeedError("payload does not match its digest")
            install_feed(self.server.feed_root, name, payload)
        except (FeedError, OSError, ValueError) as exc:
            reason = str(exc).replace("\n", " ")[:64] or "refused"
            print(f"dpumesh-feed-receiver: refused: {reason}", file=sys.stderr, flush=True)
            try:
                self.request.sendall(f"refused {reason}\n".encode("ascii", "replace"))
            except OSError:
                pass
            return
        print(f"dpumesh-feed-receiver: installed {name} ({size} bytes)", flush=True)
        try:
            self.request.sendall(f"ok {name} {size}\n".encode("ascii"))
        except OSError:
            pass


class Receiver(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address: tuple[str, int], root: Path, timeout: float,
                 node_key_path: Path | None = None) -> None:
        self.feed_root = root
        self.feed_timeout = timeout
        self.node_key_path = node_key_path or (root / "node-static.pub")
        super().__init__(address, Handler)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    # The management interface only. The daemon is reachable from this node's
    # own agent and from nothing on the data-plane fabric.
    parser.add_argument("--bind", default="192.168.100.2")
    parser.add_argument("--port", type=int, default=4788)
    parser.add_argument("--root", type=Path, default=Path("/etc/dpumesh"))
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--node-public-key", type=Path, default=None,
                        help="the DPU static handshake public key this hop serves")
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535:
        parser.error("--port out of range")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    args.root.mkdir(mode=0o755, parents=True, exist_ok=True)
    with Receiver((args.bind, args.port), args.root, args.timeout,
                  args.node_public_key) as receiver:
        print(
            f"dpumesh-feed-receiver: listening on {args.bind}:{args.port} "
            f"root={args.root} feeds={','.join(sorted(FEEDS))}",
            flush=True,
        )
        receiver.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
