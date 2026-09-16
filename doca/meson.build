project('dpumesh_dpu', 'c',
  version : '1.0.0',
  # ARM control-plane binaries use optimized code with debug symbols.
  default_options : ['buildtype=debugoptimized', 'warning_level=2'],
  meson_version : '>=0.61.2'
)

program_name = 'dpa_kernel'

add_project_arguments('-DDOCA_ALLOW_EXPERIMENTAL_API', language: 'c')

inc_dirs = []
inc_dirs += include_directories('/opt/mellanox/doca/include/')
inc_dirs += include_directories('../include')          # dmesh_common.h (shared host/DPU constants)
inc_dirs += include_directories('../linkerd/include')  # dmesh_l7.h (adapter contract)

deps = []
deps += dependency('doca-common')
deps += dependency('doca-comch')
deps += dependency('doca-argp')
deps += dependency('doca-dpa')
deps += dependency('doca-dma')   # ARM generic SG-DMA engine for the L7-proxy egress (dpu_proxy.c)
deps += dependency('libflexio')
deps += dependency('libcrypto')  # HMAC/RNG for trusted workload registration
deps += dependency('libssl')     # mutually authenticated sessions between nodes (peer_tls.c)
# The inter-node carrier: connection setup on librdmacm, data path on verbs.
deps += dependency('librdmacm')
deps += dependency('libibverbs')

srcs = [
    'dpu_main.c',
    'local_control.c',
    'dpu_worker.c',
    'config.c',
    'object.c',
    'common.c',
    'buffer.c',
    'ring.c',
    'comch_common.c',
    'comch_server.c',
    'comch_client.c',
    'comch_msgq.c',
    'workload_identity.c',
    'topology.c',
    'peer_channel.c',
    'peer_pair_wire.c',
    'peer_tls.c',
    'peer_security_wire.c',
    'peer_control.c',
    'peer_association.c',
    'peer_manager.c',
    'peer_crypto_ipc.c',
    'peer_pair_transport.c',
    'peer_pair_verbs.c',
    'peer_transport.c',
    'peer_wire_tcp.c',
    'peer_wire_rdma.c',
    'control_scope.c',
    'dpa.c',
    'dpu_proxy.c',
]

# Linkerd is the mesh implementation, not a selectable benchmark backend.
add_project_arguments('-DDMESH_L7_RUNTIME_OWNER=1', language: 'c')
l7_lib_path = get_option('l7_lib_path')
if l7_lib_path == ''
  error('the DPU build requires -Dl7_lib_path=<libdmesh_l7.a>')
endif
# A Rust staticlib brings its own std; these are what it leaves undefined.
l7_link_args = [l7_lib_path, '-lpthread', '-ldl', '-lm', '-lrt', '-lgcc_s']

# Build DPA kernel via DPACC
dpacc_build_script = files('build_dpacc.sh')
device_src = files('device/dpa_kernel.c')
device_hdr = files('dpa_common.h')
dpacc_mcpu_flag = 'nv-dpa-bf3'
doca_dep = dependency('doca-dpa')
doca_lib_dir = doca_dep.get_variable(pkgconfig : 'libdir')

dpacc_lib = custom_target('dpacc_dpa_kernel',
  input : [device_src],
  output : program_name + '.a',
  command : [
    dpacc_build_script,
    meson.current_build_dir(),
    meson.current_source_dir(),
    '@INPUT@',
    program_name,
    dpacc_mcpu_flag,
    doca_lib_dir,
    '@OUTPUT@',
  ],
  depend_files : [device_hdr],
  build_by_default : true,
)

dpa_program_lib_path = meson.current_build_dir() / (program_name + '.a')

executable('dpumesh_dpu', srcs,
  dependencies : deps,
  link_args : [dpa_program_lib_path] + l7_link_args,
  include_directories : inc_dirs,
  link_depends : [dpacc_lib],
  install : false,
)

# The crypto owner: the separate process that holds the NIC's DOCA Flow IPsec
# state for Pod pairs and answers the runtime over a UNIX socket. Built only
# where DOCA Flow and DPDK are installed; the runtime itself never links them.
flow_dep = dependency('doca-flow', required : false)
dpdk_dep = dependency('libdpdk', required : false)
bridge_dep = dependency('doca-dpdk-bridge', required : false)
if flow_dep.found() and dpdk_dep.found() and bridge_dep.found()
  owner_args = ['-Wno-deprecated-declarations']
  if flow_dep.version().version_compare('>=3.5')
    owner_args += '-DDMESH_DOCA35'
  endif
  executable('dpumesh_crypto_owner', ['peer_crypto_owner.c', 'peer_crypto_ipc.c'],
    dependencies : [dependency('doca-common'), flow_dep, bridge_dep, dpdk_dep, dependency('libcrypto')],
    c_args : owner_args,
    include_directories : inc_dirs,
    install : false,
  )
else
  message('dpumesh_crypto_owner skipped: doca-flow, doca-dpdk-bridge or libdpdk not found')
endif
