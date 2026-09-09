"""Real TLS handshake/framing, retries and host-side workload eligibility."""
import copy
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
from node.local_registration import LocalControl, REQUEST, RESPONSE, ZERO_IDENTITY, WorkloadVerifier, receive
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
        request = REQUEST.pack(2, 2, session, bytes([7])*32, ZERO_IDENTITY)
        # TCP fragments must assemble into exactly one bounded command.
        client.stream.sendall(request[:23]); client.stream.sendall(request[23:])
        first = receive(client.stream, RESPONSE.size)
        assert RESPONSE.unpack(first)[-1] == 1
        client.stream.sendall(request)
        assert receive(client.stream, RESPONSE.size) == first  # callback not repeated
        client.stream.sendall(REQUEST.pack(3, 2, session, bytes([7])*32, ZERO_IDENTITY))
        assert client.stream.recv(1) == b''  # conflicting request id closes session
        client.close()
        time.sleep(.02)
        client.connect()
        assert client.session != session
        client.stream.sendall(REQUEST.pack(1, 1, session, bytes(32), ZERO_IDENTITY))
        assert client.stream.recv(1) == b''  # old session cannot issue commands
        client.close()
    finally:
        process.terminate(); process.wait(timeout=5)


def test_eligibility():
    uid = '12345678-1234-1234-1234-123456789abc'
    cid = 'a'*64
    pod = {'metadata': {'uid': uid, 'name': 'echo', 'namespace': 'test', 'labels': {'app': 'echo'}},
           'spec': {'nodeName': 'worker-1', 'automountServiceAccountToken': False,
                    'containers': [{'name': 'app', 'resources': {'requests': {'dpumesh.io/channel': 1},
                                                             'limits': {'dpumesh.io/channel': 1}}}]},
           'status': {'podIP': '10.0.0.1', 'conditions': [{'type': 'Ready', 'status': 'False'}],
                      'containerStatuses': [{'name': 'app', 'containerID': 'containerd://'+cid,
                                             'state': {'running': {}}}]}}
    service = {'metadata': {'namespace': 'test', 'name': 'echo'},
               'spec': {'selector': {'app': 'echo'}, 'clusterIP': '10.1.0.1', 'ports': [{'port': 80}]}}
    args = dict(pod_uid=uid, node_name='worker-1', container_id=cid, service_name='echo',
                pods=[pod], services=[service], resource_name='dpumesh.io/channel')
    assert identity.resolve_authorized_pod(**args) is pod  # unready is eligible
    for change in ({'node_name': 'worker-2'}, {'container_id': 'b'*64}, {'service_name': 'other'}):
        try:
            identity.resolve_authorized_pod(**(args | change))
        except identity.GrantError:
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
    except identity.GrantError:
        pass
    else:
        raise AssertionError('accepted deleted Pod')


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as directory:
        test_tls(Path(directory))
    test_eligibility()
    print('local_registration_test: PASS')
