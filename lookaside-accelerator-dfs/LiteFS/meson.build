# LiteFS/meson.build

project('LiteFS', ['c', 'cpp'],
  version : '0.1.0',
  default_options : ['c_std=c11', 'cpp_std=c++11', 'warning_level=2'])

add_project_arguments('-faligned-new', language: 'cpp')

# Allow use of experimental DOCA APIs (match working example behavior)
add_project_arguments('-DDOCA_ALLOW_EXPERIMENTAL_API', language : 'c')
add_project_arguments('-DDOCA_ALLOW_EXPERIMENTAL_API', language : 'cpp')

# Find the DPDK dependency (use optional + explicit error to mirror example)
dpdk_dep = dependency('libdpdk', version: '>= 22.11', required : false)
if not dpdk_dep.found()
  error('Please set PKG_CONFIG_PATH to DPDK packages path, for further instructions please refer to the DOCA Reference Applications guide')
endif

# Collect DOCA and related dependencies similar to the working example
doca_dep = [
  dependency('doca-common', required : true),
  dependency('doca-dma', required : true),
  dependency('doca-dpdk-bridge', required : true),
  dependency('threads', required : true),
]

# Add global definitions based on options
if get_option('enable_crypto')
  add_project_arguments('-DENABLE_LITEFS_CRYPTO', language : 'c')
endif

if get_option('enable_compress')
  add_project_arguments('-DENABLE_LITEFS_COMPRESSION', language : 'c')
endif

# Optional AddressSanitizer support
if get_option('enable_asan') and get_option('enable_tsan')
  error('enable_asan and enable_tsan cannot both be enabled')
endif
if get_option('enable_asan')
  add_project_arguments('-fsanitize=address', '-fno-omit-frame-pointer', language : 'c')
  add_project_arguments('-fsanitize=address', '-fno-omit-frame-pointer', language : 'cpp')
  add_project_link_arguments('-fsanitize=address', language : 'c')
  add_project_link_arguments('-fsanitize=address', language : 'cpp')
endif

# Optional ThreadSanitizer support
if get_option('enable_tsan')
  add_project_arguments('-fsanitize=thread', '-fno-omit-frame-pointer', language : 'c')
  add_project_arguments('-fsanitize=thread', '-fno-omit-frame-pointer', language : 'cpp')
  add_project_link_arguments('-fsanitize=thread', language : 'c')
  add_project_link_arguments('-fsanitize=thread', language : 'cpp')
endif



ibverbs_dep = dependency('libibverbs')

# Build the subdirectories
subdir('common')
subdir('file_system')
subdir('hostfs')
subdir('nicfs')
subdir('nic_tester')
