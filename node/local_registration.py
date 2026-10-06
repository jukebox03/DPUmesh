"""Paired-DPU control client, sealed registration assertions, and Kubernetes
workload verification from a watched view."""
from __future__ import annotations

import hashlib
import hmac
import json
import socket
import ssl
import struct
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

import grpc
from node import workload_identity as identity
from node.podresources import api_pb2, api_pb2_grpc

MAGIC = b'DMESHLC2'          # mirrors DMESH_LOCAL_MAGIC
PROTOCOL_VERSION = 6         # mirrors DMESH_LOCAL_VERSION
REG_ASSERTION = 16           # mirrors DMESH_MSG_REG_ASSERTION
IDENTITY_TTL_SECONDS = 30
REQUEST = struct.Struct('<B7xQ16s32s')
RESPONSE = struct.Struct('<8s16sQI')
ASSERTION_HEADER = struct.Struct('<BB2x')
SEAL_SIZE = 32
ASSERTION_SIZE = ASSERTION_HEADER.size + identity.IDENTITY.size + SEAL_SIZE
SEAL_LABEL = b'dpumesh-registration'
WATCH_SECONDS = 240          # server-side lifetime of one watch stream


def seal(session, record):
    """The REG_ASSERTION a broker forwards for one connection: header, canonical
    record, and HMAC-SHA256 over both under a key derived from the control
    session id. Mirrors dmesh_registration_seal."""
    if len(session) != 16 or len(record) != identity.IDENTITY.size:
        raise ValueError('a seal needs a 16-byte session id and a canonical record')
    body = ASSERTION_HEADER.pack(REG_ASSERTION, PROTOCOL_VERSION) + record
    key = hmac.new(session, SEAL_LABEL, hashlib.sha256).digest()
    return body + hmac.new(key, body, hashlib.sha256).digest()


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

    def request(self, operation, connection_id=bytes(32), expected_session=None):
        with self.lock:
            if self.stream is None or (expected_session is not None and expected_session != self.session):
                raise ConnectionError('DPU control session is unavailable or changed')
            self.sequence += 1
            try:
                self.stream.sendall(REQUEST.pack(operation, self.sequence, self.session,
                                                connection_id))
                magic, session, seq, status = RESPONSE.unpack(receive(self.stream, RESPONSE.size))
                if magic != MAGIC or session != self.session or seq != self.sequence:
                    raise ConnectionError('DPU control response mismatch')
                return status
            except Exception:
                self.stream.close()
                self.stream = None
                raise

    def seal(self, record):
        """Seal `record` under the live session; returns (assertion, session).
        Without a session nothing is sealed, so registration fails closed."""
        with self.lock:
            if self.stream is None:
                raise ConnectionError('DPU control session is unavailable')
            return seal(self.session, record), self.session

    def close(self):
        with self.lock:
            if self.stream:
                self.stream.close()
            self.stream = None


class _Expired(Exception):
    """The API server no longer holds the history a watch asked for."""


class WorkloadVerifier:
    def __init__(self, server, ca, cert, key, node, cluster, podresources):
        if not server.startswith('https://'):
            raise ValueError('Kubernetes API requires HTTPS')
        self.server, self.node, self.cluster = server.rstrip('/'), node, cluster
        self.context = ssl.create_default_context(cafile=str(ca))
        self.context.load_cert_chain(str(cert), str(key))
        self.podresources = podresources
        self._watched()

    def _watched(self):
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._objects = {'pods': {}, 'services': {}}
        self._synced = {'pods': False, 'services': False}

    def _paths(self):
        selector = urllib.parse.urlencode({'fieldSelector': 'spec.nodeName=' + self.node})
        return {'pods': '/api/v1/pods?' + selector, 'services': '/api/v1/services'}

    def open(self, path, timeout):
        request = urllib.request.Request(self.server + path,
                                         headers={'Accept': 'application/json'})
        return urllib.request.urlopen(request, context=self.context, timeout=timeout)

    def listing(self, path):
        """Every item under `path` and the resourceVersion the list was taken at."""
        result, continuation = [], ''
        while True:
            suffix = ('&' if '?' in path else '?') + urllib.parse.urlencode(
                {'limit': '500', **({'continue': continuation} if continuation else {})})
            with self.open(path + suffix, 5) as response:
                data = json.load(response)
            if not isinstance(data.get('items'), list):
                raise ValueError('Kubernetes response has no item list')
            result.extend(data['items'])
            metadata = data.get('metadata', {}) or {}
            continuation = metadata.get('continue', '')
            if not continuation:
                return result, str(metadata.get('resourceVersion', ''))

    def items(self, path):
        return self.listing(path)[0]

    def snapshot(self):
        """A fresh answer from the API server, bypassing the watched view."""
        paths = self._paths()
        return self.items(paths['pods']), self.items(paths['services'])

    def start(self):
        for kind, path in self._paths().items():
            threading.Thread(target=self._follow, args=(kind, path), daemon=True).start()

    def close(self):
        self._stop.set()

    def observed(self):
        """The watched view as (pods, services), or None until both are synced."""
        with self._lock:
            if not all(self._synced.values()):
                return None
            return (list(self._objects['pods'].values()),
                    list(self._objects['services'].values()))

    @staticmethod
    def _key(obj):
        metadata = obj.get('metadata', {}) or {}
        return metadata.get('uid') or (metadata.get('namespace'), metadata.get('name'))

    def _follow(self, kind, path):
        backoff = 0.5
        while not self._stop.is_set():
            try:
                items, version = self.listing(path)
                with self._lock:
                    self._objects[kind] = {self._key(item): item for item in items}
                    self._synced[kind] = True
                backoff = 0.5
                while not self._stop.is_set():
                    version = self._watch(kind, path, version)
            except _Expired:
                continue
            except Exception as exc:
                # An interrupted view may already be stale by an unknown amount,
                # so it stops answering until a new list replaces it.
                with self._lock:
                    self._synced[kind] = False
                print(f'dpumeshd: Kubernetes {kind} watch interrupted: {exc}',
                      file=sys.stderr, flush=True)
                self._stop.wait(backoff)
                backoff = min(backoff * 2, 10.0)

    def _watch(self, kind, path, version):
        """Apply one watch stream from `version`; return the version it reached."""
        query = urllib.parse.urlencode({
            'watch': '1', 'resourceVersion': version, 'allowWatchBookmarks': 'true',
            'timeoutSeconds': str(WATCH_SECONDS)})
        try:
            stream = self.open(path + ('&' if '?' in path else '?') + query,
                               WATCH_SECONDS + 30)
        except urllib.error.HTTPError as exc:
            if exc.code == 410:
                raise _Expired() from exc
            raise
        with stream:
            for line in stream:
                if self._stop.is_set():
                    break
                if not line.strip():
                    continue
                event = json.loads(line)
                change, obj = event.get('type'), event.get('object') or {}
                if change == 'ERROR':
                    if obj.get('code') == 410:
                        raise _Expired()
                    raise ValueError(f"watch error: {obj.get('message', obj)}")
                version = str((obj.get('metadata') or {}).get('resourceVersion', version))
                if change not in ('ADDED', 'MODIFIED', 'DELETED'):
                    continue
                with self._lock:
                    if change == 'DELETED':
                        self._objects[kind].pop(self._key(obj), None)
                    else:
                        self._objects[kind][self._key(obj)] = obj
        return version

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
        observed = self.observed()
        try:
            if observed is None:
                raise identity.IdentityError('watched view not synced')
            pod, target = self.resolve(worker, observed)
        except identity.IdentityError:
            # The watch may not yet carry a container that just started; the
            # API server's own answer decides.
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
