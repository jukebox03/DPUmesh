#!/usr/bin/env python3
"""Small TCP-only relay for DPU access to Linkerd control-plane services.

The relay deliberately does not terminate TLS.  Connections leave the Host
towards a Kubernetes Service/Pod IP, so the destination Pod's Linkerd inbound
proxy still performs the control-plane mTLS handshake.
"""

import argparse
import asyncio
import json
import ssl
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass


@dataclass(frozen=True)
class Route:
    name: str
    listen_host: str
    listen_port: int
    target_host: str | None
    target_port: int
    target_namespace: str | None = None
    target_service: str | None = None


def endpoint(value: str) -> tuple[str, int]:
    host, separator, raw_port = value.rpartition(":")
    if not separator or not host:
        raise argparse.ArgumentTypeError(f"expected HOST:PORT, got {value!r}")
    try:
        port = int(raw_port)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid port in {value!r}") from error
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError(f"port out of range in {value!r}")
    return host, port


def route(value: str) -> Route:
    name, separator, endpoints = value.partition("=")
    listen, arrow, target = endpoints.partition("->")
    if not separator or not arrow or not name:
        raise argparse.ArgumentTypeError(
            f"expected NAME=LISTEN_HOST:PORT->TARGET_HOST:PORT, got {value!r}"
        )
    listen_host, listen_port = endpoint(listen)
    target_host, target_port = endpoint(target)
    return Route(name, listen_host, listen_port, target_host, target_port)


def kube_route(value: str) -> Route:
    name, separator, endpoints = value.partition("=")
    listen, arrow, target = endpoints.partition("->")
    if not separator or not arrow or not name:
        raise argparse.ArgumentTypeError(
            f"expected NAME=LISTEN_HOST:PORT->NAMESPACE/SERVICE:PORT, got {value!r}"
        )
    listen_host, listen_port = endpoint(listen)
    service, target_port = endpoint(target)
    namespace, slash, service_name = service.partition("/")
    if not slash or not namespace or not service_name:
        raise argparse.ArgumentTypeError(f"invalid Kubernetes service in {value!r}")
    return Route(name, listen_host, listen_port, None, target_port, namespace, service_name)


class KubernetesAPI:
    def __init__(self, server: str, token_file: str, ca_file: str) -> None:
        self.server = server.rstrip("/")
        self.token_file = token_file
        self.context = ssl.create_default_context(cafile=ca_file)

    def get(self, path: str) -> dict:
        with open(self.token_file, encoding="ascii") as token_input:
            token = token_input.read().strip()
        request = urllib.request.Request(
            f"{self.server}{path}",
            headers={"Authorization": f"Bearer {token}", "Accept": "application/json"},
        )
        with urllib.request.urlopen(request, context=self.context, timeout=5) as response:
            return json.load(response)

    def resolve(self, namespace: str, service: str) -> str:
        quoted_ns = urllib.parse.quote(namespace, safe="")
        quoted_service = urllib.parse.quote(service, safe="")
        document = self.get(f"/api/v1/namespaces/{quoted_ns}/services/{quoted_service}")
        cluster_ip = document.get("spec", {}).get("clusterIP")
        if cluster_ip and cluster_ip != "None":
            return cluster_ip
        endpoints = self.get(f"/api/v1/namespaces/{quoted_ns}/endpoints/{quoted_service}")
        addresses = [
            address.get("ip")
            for subset in endpoints.get("subsets", [])
            for address in subset.get("addresses", [])
            if address.get("ip")
        ]
        if not addresses:
            raise RuntimeError(f"{namespace}/{service} has no ready endpoint")
        return sorted(addresses)[0]


async def copy_stream(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    while data := await reader.read(64 * 1024):
        writer.write(data)
        await writer.drain()


async def handle(
    route: Route,
    kube: KubernetesAPI | None,
    reader: asyncio.StreamReader,
    writer: asyncio.StreamWriter,
) -> None:
    peer = writer.get_extra_info("peername")
    try:
        target_host = route.target_host
        if route.target_service is not None:
            if kube is None or route.target_namespace is None:
                raise RuntimeError("Kubernetes resolver is not configured")
            target_host = await asyncio.to_thread(
                kube.resolve, route.target_namespace, route.target_service
            )
        if target_host is None:
            raise RuntimeError("route has no target")
        upstream_reader, upstream_writer = await asyncio.open_connection(
            target_host, route.target_port
        )
    except Exception as error:  # The supervisor log is the diagnostic surface.
        print(f"{route.name}: target connect failed for {peer}: {error}", flush=True)
        writer.close()
        await writer.wait_closed()
        return

    downstream = asyncio.create_task(copy_stream(reader, upstream_writer))
    upstream = asyncio.create_task(copy_stream(upstream_reader, writer))
    done, pending = await asyncio.wait(
        (downstream, upstream), return_when=asyncio.FIRST_COMPLETED
    )
    for task in pending:
        task.cancel()
    await asyncio.gather(*done, *pending, return_exceptions=True)
    upstream_writer.close()
    writer.close()
    await asyncio.gather(
        upstream_writer.wait_closed(), writer.wait_closed(), return_exceptions=True
    )


async def serve_routes(
    routes: list[Route], kube: KubernetesAPI | None, stop: asyncio.Event
) -> None:
    servers: list[asyncio.Server] = []
    for item in routes:
        server = await asyncio.start_server(
            lambda reader, writer, item=item: handle(item, kube, reader, writer),
            item.listen_host,
            item.listen_port,
        )
        servers.append(server)
        print(
            f"{item.name}: {item.listen_host}:{item.listen_port} -> "
            f"{item.target_host or (item.target_namespace + '/' + item.target_service)}:"
            f"{item.target_port}",
            flush=True,
        )

    await stop.wait()
    for server in servers:
        server.close()
    await asyncio.gather(*(server.wait_closed() for server in servers))


def run_relay(routes: list[Route], kube: KubernetesAPI | None) -> None:
    """Run the relay to completion on the calling thread.

    The node agent absorbs the relay, and the process's signals belong to the
    agent, so none are installed here: the relay ends when its thread ends.
    """

    async def forever() -> None:
        await serve_routes(routes, kube, asyncio.Event())

    asyncio.run(forever())
