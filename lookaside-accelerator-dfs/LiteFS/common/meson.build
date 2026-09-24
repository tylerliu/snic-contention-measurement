# LiteFS/common/meson.build

# This file will define a library for code shared between components.

subdir('arg_parser')
meson.override_dependency('litefs_arg_parser', arg_parser_dep)

shared_sources = [
  'dpdk_init.cpp',
  'doca_dma.cpp',
  'dpdk_offloads.cpp',
  'verbs_helper.cpp',
  'tcp_helper.cpp',
  'doorbell_channel.cpp',
  'local_region.cpp',
  'pause_tracker.cpp'
]

# This library will be linked against by the other components
common_deps = [dpdk_dep, ibverbs_dep, arg_parser_dep] + doca_dep
common_lib = static_library('litefs_common', shared_sources,
  dependencies : common_deps,
  include_directories : include_directories('..'))

# Make the library discoverable by other subdirectories
# Export project root so includes like "common/xyz.h" resolve everywhere
common_lib_dep = declare_dependency(
  link_with : common_lib,
  include_directories : include_directories('..')
)
