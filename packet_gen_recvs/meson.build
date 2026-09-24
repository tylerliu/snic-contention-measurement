

project('testing_tools', 'c', 'cpp',
  version : '0.1',
  default_options : ['warning_level=3', 'c_std=gnu11', 'buildtype=release', 'optimization=3']
)

# Add additional optimization flags for maximum performance
add_project_arguments('-Ofast', '-march=native', '-mtune=native', '-ffast-math', '-funroll-loops', language: ['c', 'cpp'])

deps = [
  dependency('openssl'),
  dependency('threads'),  # For pthread
]

executable('pkt_gen', 'pkt_gen.c')
executable('pkt_recv', 'pkt_recv.c')
executable('aes_gcm_pkt_gen', 'aes_gcm_pkt_gen.c', dependencies: deps)

dpdk_app_dependencies = [
    dependency('libdpdk'),
    dependency('openssl'),
    dependency('threads')
]

dpdk_aes_gcm_gen = executable(
    'dpdk_aes_gcm_gen',
    'dpdk_aes_gcm_gen.c',
    dependencies: dpdk_app_dependencies,
    install: false
)

# Simple generator with OpenSSL dependency (for ESP)
dpdk_esp_gen = executable(
    'dpdk_esp_gen',
    ['dpdk_esp_gen.c', 'dpdk_common.c'],
    dependencies: dpdk_app_dependencies,
    install: false
)

dpdk_esp_exclusive_gen = executable(
    'dpdk_esp_exclusive_gen',
    ['dpdk_esp_exclusive_gen.c', 'dpdk_common.c'],
    dependencies: dpdk_app_dependencies,
    install: false
)

# Simple generator without OpenSSL dependency
dpdk_simple_gen_dependencies = [
    dependency('libdpdk'),
    dependency('threads')
]

dpdk_simple_gen = executable(
    'dpdk_simple_gen',
    ['dpdk_simple_gen.c', 'dpdk_common.c'],
    dependencies: dpdk_simple_gen_dependencies,
    install: false
)

dpdk_simple_recv = executable(
    'dpdk_simple_recv',
    ['dpdk_simple_recv.c', 'dpdk_common.c'],
    dependencies: dpdk_simple_gen_dependencies,
    install: false
)

# Simplified Ethernet frame pair service
dpdk_eth_pairs = executable(
    'dpdk_eth_pairs',
    ['dpdk_eth_pairs.c', 'dpdk_common.c'],
    dependencies: dpdk_simple_gen_dependencies,
    install: false
)

dpdk_kv_gen = executable(
    'dpdk_kv_gen',
    ['dpdk_kv_gen.c', 'dpdk_common.c'],
    dependencies: dpdk_simple_gen_dependencies,
    install: false
)

dpdk_kv_latency = executable(
    'dpdk_kv_latency',
    ['dpdk_kv_latency.c', 'dpdk_common.c'],
    dependencies: dpdk_simple_gen_dependencies,
    install: false
)

dpdk_single_core_multi_queue = executable(
    'dpdk_single_core_multi_queue',
    ['dpdk_single_core_multi_queue.c', 'dpdk_common.c'],
    dependencies: dpdk_simple_gen_dependencies,
    install: false
)
