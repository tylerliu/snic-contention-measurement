project('dpdk_mapreduce', 'c', 'cpp',
  version : '0.1',
  default_options : ['warning_level=3', 'c_std=gnu11', 'buildtype=release', 'optimization=3']
)

# Add additional optimization flags for maximum performance
add_project_arguments('-Ofast', '-march=native', '-mtune=native', '-ffast-math', '-funroll-loops', language: ['c', 'cpp'])

dpdk_app_dependencies = [
    dependency('libdpdk'),
    dependency('threads')
]

executable(
    'dpdk_mapreduce',
    ['dpdk_mapreduce.c', 'dpdk_common.c'],
    dependencies: dpdk_app_dependencies,
    install: false
)
