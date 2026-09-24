# LiteFS/nicfs/meson.build

nicfs_sources = [
  'main.cpp', 
  'stages.cpp'
]

executable('nicfs', nicfs_sources,
  dependencies : [dpdk_dep, common_lib_dep, fs_lib_dep] + doca_dep)
