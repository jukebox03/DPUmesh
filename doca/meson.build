project('dpumesh_dpu', 'c',
  version : '1.0.0',
  # ARM control-plane binaries use optimized code with debug symbols.
  default_options : ['buildtype=debugoptimized'],
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

srcs = [
    'dpu_main.c',
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
    'workload_grant.c',
    'dpa.c',
    'dpu_proxy.c',
]

# The L7 consumer behind dmesh_l7.h: either the reference consumer in this tree
# or the Rust staticlib carrying linkerd2-proxy. The datapath compiles against
# the contract and does not change between them.
l7_backend = get_option('l7_backend')
l7_link_args = []
if l7_backend == 'linkerd'
  add_project_arguments('-DDMESH_L7_RUNTIME_OWNER=1', language: 'c')
  l7_lib_path = get_option('l7_lib_path')
  if l7_lib_path == ''
    error('l7_backend=linkerd requires -Dl7_lib_path=<libdmesh_l7.a>')
  endif
  # A Rust staticlib brings its own std; these are what it leaves undefined.
  l7_link_args += [l7_lib_path, '-lpthread', '-ldl', '-lm', '-lrt', '-lgcc_s']
else
  srcs += ['../linkerd/shim/l7_null.c']
endif

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
