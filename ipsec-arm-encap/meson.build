project('monitoring_app', ['c', 'cpp'],
  version: '0.1.0',
  default_options: [
    'warning_level=2',
    'c_std=gnu11',
    'cpp_std=c++11',
    'buildtype=release'
  ]
)

if host_machine.system() != 'linux'
  error('monitoring_app supports Linux only')
endif

add_project_arguments('-DDOCA_ALLOW_EXPERIMENTAL_API', language: 'c')
add_project_arguments('-DDOCA_ALLOW_EXPERIMENTAL_API', language: 'cpp')

dpdk_dep = dependency('libdpdk', required: true)
doca_common_dep = dependency('doca-common', required: true)
doca_flow_dep = dependency('doca-flow', required: true)
doca_dpdk_bridge_dep = dependency('doca-dpdk-bridge', required: true)
threads_dep = dependency('threads')
yaml_dep = dependency('yaml-0.1', required: true)

subdir('arg_parser')

app_dependencies = [
  dpdk_dep,
  doca_common_dep,
  doca_flow_dep,
  doca_dpdk_bridge_dep,
  threads_dep,
  yaml_dep,
  arg_parser_dep,
]

monitoring_sources = files(
  'monitoring_config.cpp',
  'device_manager.cpp',
  'monitoring_doca.cpp',
  'monitoring_dpdk.cpp',
  'monitoring_main.cpp',
  'utils.c',
  'dpdk_utils.c'
)

include_dirs = [
  include_directories('.'),
]

cpp_args = ['-std=c++11']

executable('monitoring_app',
  monitoring_sources,
  cpp_args: cpp_args,
  dependencies: app_dependencies,
  include_directories: include_dirs,
  install: true
)



layout_test = executable('esp_layout_test', 'esp_layout_test.cpp')
test('esp-layout', layout_test)
replay_test = executable('esp_replay_test', 'esp_replay_test.cpp')
test('esp-replay', replay_test)
