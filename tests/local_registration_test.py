"""Real TLS handshake/framing, registration seals, the watched Kubernetes view,
and host-side workload eligibility."""
import copy
import io
import json
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from node.local_registration import (ASSERTION_SIZE, LocalControl, REQUEST, RESPONSE,
                                     WorkloadVerifier, receive, seal)
from node import workload_identity as identity


def openssl(*args):
    subprocess.run(['openssl', *map(str, args)], check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)


def test_tls(root):
    openssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
            '-subj', '/CN=test-ca', '-keyout', root/'ca.key', '-out', root/'ca.crt')
    for name, san, usage in [('server', 'DNS:localhost', 'serverAuth'),
                             ('client', 'URI:spiffe://dpumesh.io/node/worker-1', 'clientAuth'),
                             ('wrong', 'URI:spiffe://dpumesh.io/node/worker-2', 'clientAuth')]:
        openssl('req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN='+name,
                '-keyout', root/(name+'.key'), '-out', root/(name+'.csr'))
        (root/(name+'.ext')).write_text(f'subjectAltName={san}\nextendedKeyUsage={usage}\n')
        openssl('x509', '-req', '-in', root/(name+'.csr'), '-CA', root/'ca.crt',
                '-CAkey', root/'ca.key', '-CAcreateserial', '-days', '1',
                '-extfile', root/(name+'.ext'), '-out', root/(name+'.crt'))
    with socket.socket() as reserved:
        reserved.bind(('127.0.0.1', 0)); port = reserved.getsockname()[1]
    process = subprocess.Popen([str(ROOT/'build/test/local_control_server'), str(port),
                                str(root/'ca.crt'), str(root/'server.crt'), str(root/'server.key')],
                               stdout=subprocess.PIPE, text=True)
    try:
        assert process.stdout.readline().strip() == 'ready'
        wrong = LocalControl(('127.0.0.1', port), 'localhost', root/'ca.crt', root/'wrong.crt', root/'wrong.key')
        try:
            wrong.connect()
        except (OSError, ValueError):
            pass
        else:
            raise AssertionError('accepted another authenticated host')
        wrong.close()
        client = LocalControl(('127.0.0.1', port), 'localhost', root/'ca.crt', root/'client.crt', root/'client.key')
        client.connect()
        assert client.request(1) == 0
        session = client.session
        request = REQUEST.pack(4, 2, session, bytes([7])*32)
        # TCP fragments must assemble into exactly one bounded command.
        client.stream.sendall(request[:23]); client.stream.sendall(request[23:])
        first = receive(client.stream, RESPONSE.size)
        assert RESPONSE.unpack(first)[-1] == 1
        client.stream.sendall(request)
        assert receive(client.stream, RESPONSE.size) == first  # callback not repeated
        client.stream.sendall(REQUEST.pack(3, 2, session, bytes([7])*32))
        assert client.stream.recv(1) == b''  # conflicting request id closes session
        client.close()
        time.sleep(.02)
        client.connect()
        assert client.session != session
        assertion, sealed_under = client.seal(bytes(identity.IDENTITY.size))
        assert sealed_under == client.session and len(assertion) == ASSERTION_SIZE
        client.stream.sendall(REQUEST.pack(1, 1, session, bytes(32)))
        assert client.stream.recv(1) == b''  # old session cannot issue commands
        client.close()
    finally:
        process.terminate(); process.wait(timeout=5)


def test_seal():
    # The same vector tests/workload_identity_test.c checks on the DPU side.
    session = bytes(range(16))
    record = bytearray(identity.IDENTITY.size)
    record[0] = record[1] = 1
    record[24:56] = bytes(range(0x40, 0x60))
    assertion = seal(session, bytes(record))
    assert len(assertion) == ASSERTION_SIZE == 1469
    assert assertion[:4] == bytes([16, 6, 0, 0]) and assertion[4:-32] == record
    assert assertion[-32:].hex() == (
        'ba7da03c8e26adb8f0d7a7bc8cb49efe06923557ae74e98c306cc4d1ff05eda3')
    assert seal(bytes(16), bytes(record))[-32:] != assertion[-32:]
    unsealed = LocalControl.__new__(LocalControl)
    unsealed.lock, unsealed.stream = __import__('threading').Lock(), None
    try:
        unsealed.seal(bytes(record))
    except ConnectionError:
        pass
    else:
        raise AssertionError('sealed without a control session')


def workload():
    uid = '12345678-1234-1234-1234-123456789abc'
    cid = 'a'*64
    pod = {'metadata': {'uid': uid, 'name': 'echo', 'namespace': 'test', 'labels': {'app': 'echo'}},
           'spec': {'nodeName': 'worker-1', 'automountServiceAccountToken': False,
                    'containers': [{'name': 'app', 'resources': {'requests': {'dpumesh.io/channel': 1},
                                                             'limits': {'dpumesh.io/channel': 1}}}]},
           'status': {'podIP': '10.0.0.1', 'conditions': [{'type': 'Ready', 'status': 'False'}],
                      'containerStatuses': [{'name': 'app', 'containerID': 'containerd://'+cid,
                                             'state': {'running': {}}}]}}
    service = {'metadata': {'namespace': 'test', 'name': 'echo', 'uid': 'svc-echo'},
               'spec': {'selector': {'app': 'echo'}, 'clusterIP': '10.1.0.1', 'ports': [{'port': 80}]}}
    return uid, cid, pod, service


def test_eligibility():
    uid, cid, pod, service = workload()
    args = dict(pod_uid=uid, node_name='worker-1', container_id=cid, service_name='echo',
                pods=[pod], services=[service], resource_name='dpumesh.io/channel')
    assert identity.resolve_authorized_pod(**args) is pod  # unready is eligible
    for change in ({'node_name': 'worker-2'}, {'container_id': 'b'*64}, {'service_name': 'other'}):
        try:
            identity.resolve_authorized_pod(**(args | change))
        except identity.IdentityError:
            pass
        else:
            raise AssertionError('accepted wrong workload identity')
    verifier = WorkloadVerifier.__new__(WorkloadVerifier)
    verifier.node = 'worker-1'
    verifier.podresources = '/unused'
    worker = SimpleNamespace(pod_uid=uid, container_id=cid, service='echo', slot=7)
    from node.podresources import api_pb2
    listing = api_pb2.ListPodResourcesResponse()
    entry = listing.pod_resources.add(name='echo', namespace='test')
    container = entry.containers.add(name='app')
    device = container.devices.add(resource_name='dpumesh.io/channel', device_ids=['channel-007'])
    with patch('node.local_registration.grpc.insecure_channel'), patch(
            'node.local_registration.api_pb2_grpc.PodResourcesListerStub') as stub:
        stub.return_value.List.return_value = listing
        assert verifier.resolve(worker, ([pod], [service]))[0] is pod
        for ids in (['channel-008'], [], ['channel-007', 'channel-008']):
            del device.device_ids[:]
            device.device_ids.extend(ids)
            try:
                verifier.resolve(worker, ([pod], [service]))
            except ValueError:
                pass
            else:
                raise AssertionError('accepted mismatched kubelet allocation')
    invalid = copy.deepcopy(pod); invalid['metadata']['deletionTimestamp'] = 'now'
    try:
        identity.resolve_authorized_pod(**(args | {'pods': [invalid]}))
    except identity.IdentityError:
        pass
    else:
        raise AssertionError('accepted deleted Pod')


class FakeAPI:
    """Scripted list and watch answers in the shape the API server streams."""

    def __init__(self, verifier, lists, watches):
        self.verifier, self.lists, self.watches, self.calls = verifier, lists, watches, []

    def open(self, path, timeout):
        del timeout
        kind = 'pods' if path.startswith('/api/v1/pods') else 'services'
        self.calls.append(path)
        if 'watch=1' in path:
            if self.watches[kind]:
                script = self.watches[kind].pop(0)
                if isinstance(script, Exception):
                    raise script
                return io.BytesIO(b''.join(json.dumps(e).encode() + b'\n' for e in script))
            self.verifier._stop.wait(5)  # an idle stream until the follower stops
            return io.BytesIO(b'')
        answer = self.lists[kind].pop(0) if len(self.lists[kind]) > 1 else self.lists[kind][0]
        if isinstance(answer, Exception):
            raise answer
        items, version = answer
        return io.BytesIO(json.dumps(
            {'items': items, 'metadata': {'resourceVersion': version}}).encode())


def watched(lists, watches):
    verifier = WorkloadVerifier.__new__(WorkloadVerifier)
    verifier._watched()
    verifier.node, verifier.cluster, verifier.podresources = 'worker-1', 'test-cluster', '/unused'
    api = FakeAPI(verifier, lists, watches)
    verifier.open = api.open
    return verifier, api


def eventually(predicate):
    deadline = time.monotonic() + 5
    while not predicate():
        if time.monotonic() > deadline:
            raise AssertionError('watched view never converged')
        time.sleep(.01)


def test_watched_view():
    _uid, _cid, pod, service = workload()
    starting = copy.deepcopy(pod); starting['status']['containerStatuses'][0]['state'] = {'waiting': {}}
    other = copy.deepcopy(pod); other['metadata']['uid'] = 'other'
    replaced = copy.deepcopy(pod); replaced['metadata']['uid'] = 'replaced'
    event = lambda kind, obj: {'type': kind, 'object': obj}
    bookmark = {'metadata': {'resourceVersion': '14'}}

    # Changes apply in order; a bookmark moves the next watch forward.
    verifier, api = watched(
        {'pods': [([starting], '10')], 'services': [([service], '5')]},
        {'pods': [[event('MODIFIED', pod), event('ADDED', other), event('DELETED', other),
                   event('BOOKMARK', bookmark)]], 'services': []})
    assert verifier.observed() is None
    verifier.start()
    eventually(lambda: verifier.observed() is not None and
               verifier.observed()[0] == [pod] and verifier.observed()[1] == [service])
    eventually(lambda: any('resourceVersion=14' in call for call in api.calls))
    verifier.close()

    # Expired history is listed again rather than guessed at.
    verifier, api = watched(
        {'pods': [([pod], '10'), ([replaced], '20')], 'services': [([service], '5')]},
        {'pods': [[event('ERROR', {'code': 410})]], 'services': []})
    verifier.start()
    eventually(lambda: verifier.observed() is not None and verifier.observed()[0] == [replaced])
    verifier.close()

    # An interrupted watch stops answering until a new list succeeds.
    verifier, api = watched(
        {'pods': [([pod], '10'), OSError('api down')], 'services': [([service], '5')]},
        {'pods': [OSError('stream reset')], 'services': []})
    from contextlib import redirect_stderr
    with redirect_stderr(io.StringIO()):
        verifier.start()
        eventually(lambda: any('watch=1' in call for call in api.calls if 'pods' in call))
        eventually(lambda: verifier.observed() is None)
        verifier.close()


def test_metadata_prefers_watched_view():
    uid, cid, pod, service = workload()
    verifier, _api = watched({'pods': [([], '1')], 'services': [([], '1')]},
                             {'pods': [], 'services': []})
    fresh = []
    verifier.snapshot = lambda: fresh.append(1) or ([pod], [service])
    worker = SimpleNamespace(pod_uid=uid, container_id=cid, service='echo', slot=7, generation=3)
    from node.podresources import api_pb2
    listing = api_pb2.ListPodResourcesResponse()
    entry = listing.pod_resources.add(name='echo', namespace='test')
    entry.containers.add(name='app').devices.add(
        resource_name='dpumesh.io/channel', device_ids=['channel-007'])
    with patch('node.local_registration.grpc.insecure_channel'), patch(
            'node.local_registration.api_pb2_grpc.PodResourcesListerStub') as stub:
        stub.return_value.List.return_value = listing
        with verifier._lock:
            verifier._objects = {'pods': {uid: pod}, 'services': {'svc-echo': service}}
            verifier._synced = {'pods': True, 'services': True}
        record = verifier.metadata(worker, bytes([5])*32, '00'*16)
        assert len(record) == identity.IDENTITY.size and not fresh
        # A container the watch has not delivered yet is looked up directly.
        with verifier._lock:
            verifier._objects['pods'] = {}
        assert len(verifier.metadata(worker, bytes([5])*32, '00'*16)) == len(record)
        assert fresh == [1]
        # Before the view is synced every lookup is direct.
        with verifier._lock:
            verifier._synced['pods'] = False
        verifier.metadata(worker, bytes([5])*32, '00'*16)
        assert fresh == [1, 1]


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as directory:
        test_tls(Path(directory))
    test_seal()
    test_eligibility()
    test_watched_view()
    test_metadata_prefers_watched_view()
    print('local_registration_test: PASS')
