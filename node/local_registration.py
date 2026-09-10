"""Paired-DPU TLS control client and direct Kubernetes workload verification."""
from __future__ import annotations

import json
import socket
import ssl
import struct
import threading
import time
import urllib.parse
import urllib.request

import grpc
from node import workload_identity as identity
from node.podresources import api_pb2, api_pb2_grpc

MAGIC = b'DMESHLC1'          # mirrors DMESH_LOCAL_MAGIC
IDENTITY_TTL_SECONDS = 30
REQUEST = struct.Struct('<B7xQ16s32s1433s')
RESPONSE = struct.Struct('<8s16sQI')
ZERO_IDENTITY = bytes(1433)


def receive(stream, count):
    chunks = bytearray()
    while len(chunks) < count:
        chunk = stream.recv(count - len(chunks))
        if not chunk:
            raise ConnectionError('DPU control connection closed')
        chunks.extend(chunk)
    return bytes(chunks)


class LocalControl:
    def __init__(self, address, server_name, ca, cert, key):
        self.address, self.server_name = address, server_name
        self.context = ssl.create_default_context(cafile=str(ca))
        self.context.minimum_version = ssl.TLSVersion.TLSv1_3
        self.context.load_cert_chain(str(cert), str(key))
        self.lock = threading.Lock()
        self.stream = None
        self.session = None
        self.sequence = 0

    def connect(self):
        with self.lock:
            if self.stream is not None:
                return
            raw = socket.create_connection(self.address, timeout=5)
            try:
                stream = self.context.wrap_socket(raw, server_hostname=self.server_name)
                magic, session, seq, status = RESPONSE.unpack(receive(stream, RESPONSE.size))
                if magic != MAGIC or seq or status or not any(session):
                    raise ConnectionError('invalid DPU control greeting')
            except Exception:
                raw.close()
                if 'stream' in locals():
                    stream.close()
                raise
            self.stream, self.session, self.sequence = stream, session, 0

    def request(self, operation, connection_id=bytes(32), metadata=ZERO_IDENTITY,
                expected_session=None):
        with self.lock:
            if self.stream is None or (expected_session is not None and expected_session != self.session):
                raise ConnectionError('DPU control session is unavailable or changed')
            self.sequence += 1
            try:
                self.stream.sendall(REQUEST.pack(operation, self.sequence, self.session,
                                                connection_id, metadata))
                magic, session, seq, status = RESPONSE.unpack(receive(self.stream, RESPONSE.size))
                if magic != MAGIC or session != self.session or seq != self.sequence:
                    raise ConnectionError('DPU control response mismatch')
                return status
            except Exception:
                self.stream.close()
                self.stream = None
                raise

    def close(self):
        with self.lock:
            if self.stream:
                self.stream.close()
            self.stream = None


class WorkloadVerifier:
    def __init__(self, server, ca, cert, key, node, cluster, podresources):
        if not server.startswith('https://'):
            raise ValueError('Kubernetes API requires HTTPS')
        self.server, self.node, self.cluster = server.rstrip('/'), node, cluster
        self.context = ssl.create_default_context(cafile=str(ca))
        self.context.load_cert_chain(str(cert), str(key))
        self.podresources = podresources

    def items(self, path):
        result, continuation = [], ''
        while True:
            suffix = ('&' if '?' in path else '?') + urllib.parse.urlencode(
                {'limit': '500', **({'continue': continuation} if continuation else {})})
            headers = {"Accept": "application/json"}
            request = urllib.request.Request(self.server + path + suffix, headers=headers)
            with urllib.request.urlopen(request,
                                        context=self.context, timeout=5) as response:
                data = json.load(response)
            if not isinstance(data.get('items'), list):
                raise ValueError('Kubernetes response has no item list')
            result.extend(data['items'])
            continuation = data.get('metadata', {}).get('continue', '')
            if not continuation:
                return result

    def snapshot(self):
        selector = urllib.parse.urlencode({'fieldSelector': 'spec.nodeName=' + self.node})
        return self.items('/api/v1/pods?' + selector), self.items('/api/v1/services')

    def resolve(self, worker, snapshot=None, check_allocation=True):
        pods, services = snapshot if snapshot is not None else self.snapshot()
        pod = identity.resolve_authorized_pod(
            pod_uid=worker.pod_uid, node_name=self.node, container_id=worker.container_id,
            service_name=worker.service, pods=pods, services=services,
            resource_name='dpumesh.io/channel')
        target = identity.resource_target(pod, 'dpumesh.io/channel')
        if check_allocation:
            with grpc.insecure_channel('unix://' + self.podresources) as channel:
                listing = api_pb2_grpc.PodResourcesListerStub(channel).List(
                    api_pb2.ListPodResourcesRequest(), timeout=3)
            matches = [d for p in listing.pod_resources
                       if p.name == pod['metadata']['name'] and p.namespace == pod['metadata']['namespace']
                       for c in p.containers if c.name == target['name']
                       for d in c.devices if d.resource_name == 'dpumesh.io/channel']
            expected = f'channel-{worker.slot:03d}'
            if len(matches) != 1 or list(matches[0].device_ids) != [expected]:
                raise ValueError('workload does not own this kubelet channel allocation')
        return pod, target

    def metadata(self, worker, connection_id, incarnation):
        pod, target = self.resolve(worker)
        text = identity.fixed_text
        now = int(time.time())
        return identity.IDENTITY.pack(
            identity.IDENTITY_TYPE, identity.IDENTITY_VERSION, 0, 0,
            now, now + IDENTITY_TTL_SECONDS, connection_id,
            worker.slot, worker.generation, bytes.fromhex(incarnation),
            text(self.cluster, 64, 'cluster'),
            text(self.node, 254, 'node'), text(worker.pod_uid, 64, 'Pod UID'),
            text(pod['metadata']['namespace'], 64, 'namespace'),
            text(pod['metadata']['name'], 254, 'Pod name'),
            text(pod['spec'].get('serviceAccountName', 'default'), 254, 'ServiceAccount'),
            text(target['name'], 254, 'container name'),
            text(worker.container_id, 65, 'container ID'),
            text(worker.service, 64, 'Service', allow_empty=True),
            text(identity.pod_ipv4(pod), 16, 'Pod IP'))
