#!/usr/bin/env python3
"""Render the single host/DPU pair's environment as Helm values (no secrets)."""
import json
import os

env = os.environ
address = env['DMESH_DEPLOY_ADDRESS']
values = dict(
    image=env['DMESH_DEPLOY_IMAGE'], hostNode=env['DMESH_DEPLOY_HOST'],
    dpuNode=env['DMESH_DEPLOY_DPU'], clusterID=env.get('DPUMESH_CLUSTER_ID', 'dpumesh-test'),
    pci=env['DMESH_DEPLOY_PCI'], representor=env['DMESH_DEPLOY_REPRESENTOR'],
    localBind=address, linkerd={'enabled': True},
    env={
        'DPUMESH_CONTROLLER_KEY_DIR': '/etc/dpumesh/controller.pub.keys',
        'DPUMESH_FEED_KEY_DIR': '/etc/dpumesh/feed.keys',
        'DPUMESH_TOPOLOGY_FILE': '/etc/dpumesh/feeds/topology.v1',
        'DPUMESH_L7_SERVICE_TARGETS_FILE': '/etc/dpumesh/feeds/service-targets.v1',
        'DPUMESH_NODE_KEY_FILE': '/etc/dpumesh/node-static.key',
        'DPUMESH_NODE_KEY_PUBLIC_FILE': '/etc/dpumesh/node-static.pub',
        'DPUMESH_LOCAL_CA': '/etc/dpumesh/local-tls/ca.crt',
        'DPUMESH_LOCAL_CERT': '/etc/dpumesh/local-tls/dpu-server.crt',
        'DPUMESH_LOCAL_KEY': '/etc/dpumesh/local-tls/dpu-server.key',
        'DPUMESH_IDENTITY_TRUST_DOMAIN': 'linkerd.cluster.local',
        'DPUMESH_CONTROLLER_SCOPE_URL': env.get('DPUMESH_CONTROLLER_SCOPE_URL', 'http://192.168.100.1:28089'),
        'DPUMESH_L7_LINKERD_WORKER': env.get('DPUMESH_L7_LINKERD_WORKER', 'all'),
        'DPUMESH_L7_OPAQUE_SVC': env.get('DPUMESH_L7_OPAQUE_SVC', env.get('NS', 'test-bench') + '/echo-dpumesh-native'),
        'DPUMESH_DPA_THREADS': env.get('DPUMESH_DPA_THREADS', '32'),
        'DPUMESH_ARM_WORKERS': env.get('DPUMESH_ARM_WORKERS', '8'),
        'DPUMESH_RINGS_PER_POD': env.get('DPUMESH_RINGS_PER_POD', '8'),
    })
for key in ('DPUMESH_ARM_CPU_LIST', 'DPUMESH_L7_SVC', 'DPUMESH_PEER_TRANSPORT',
            'DPUMESH_PEER_BIND', 'DPUMESH_PEER_PORT', 'DPUMESH_ADMISSION_FILE'):
    if env.get(key):
        values['env'][key] = env[key]
print(json.dumps(values, indent=2))
