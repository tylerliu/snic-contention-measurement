project('throughput-experiment', 'c')

dpdk_dep = dependency('libdpdk', required: true)

# Use the local driver (copied from api-perf/driver)
benchmark_driver_src = files('driver/benchmark_driver.c')

# Build benchmarks from local benchmarks/ folder
bench_dir = meson.current_source_dir() / 'benchmarks'

bench_sources = {
  'rte_cryptodev_enqueue_dequeue_burst_decrypt': files('benchmarks/rte_cryptodev_enqueue_dequeue_burst_decrypt.c'),
  'rte_cryptodev_enqueue_dequeue_burst_encrypt': files('benchmarks/rte_cryptodev_enqueue_dequeue_burst_encrypt.c'),
  'rte_cryptodev_enqueue_dequeue_burst_xts_decrypt': files('benchmarks/rte_cryptodev_enqueue_dequeue_burst_xts_decrypt.c'),
  'rte_cryptodev_enqueue_dequeue_burst_xts_encrypt': files('benchmarks/rte_cryptodev_enqueue_dequeue_burst_xts_encrypt.c'),
  'rte_compressdev_enqueue_dequeue_burst_decompress': files('benchmarks/rte_compressdev_enqueue_dequeue_burst_decompress.c'),
  'get_tsc_hz': files('benchmarks/get_tsc_hz.c'),
}

nb_desc_option = get_option('nb_descriptors')

foreach name, src : bench_sources
  if name == 'rte_cryptodev_enqueue_dequeue_burst_decrypt' or name == 'rte_cryptodev_enqueue_dequeue_burst_encrypt' or name == 'rte_cryptodev_enqueue_dequeue_burst_xts_decrypt' or name == 'rte_cryptodev_enqueue_dequeue_burst_xts_encrypt' or name == 'rte_compressdev_enqueue_dequeue_burst_decompress'
    executable(name,
      sources: [benchmark_driver_src, src],
      dependencies: [dpdk_dep],
      include_directories: include_directories('.'),
      c_args: ['-DNB_DESCRIPTORS=' + nb_desc_option.to_string()]
    )
  else
    executable(name,
      sources: [benchmark_driver_src, src],
      dependencies: [dpdk_dep],
      include_directories: include_directories('.')
    )
  endif
endforeach
