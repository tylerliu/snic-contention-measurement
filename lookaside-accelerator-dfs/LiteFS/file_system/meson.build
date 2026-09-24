# LiteFS/file_system/meson.build

fs_lib_sources = [
  'formatter.cpp',
  'operator.cpp',
  'log_gen.cpp',
  'log_processor.cpp',
  'xts_log_processor.cpp',
  'gcm_log_processor.cpp',
]

fs_lib_deps = [dpdk_dep] + doca_dep + [common_lib_dep]
fs_lib = static_library('fs_lib', fs_lib_sources,
  dependencies : fs_lib_deps)

fs_lib_dep = declare_dependency(link_with : fs_lib,
  include_directories: include_directories('..'))
