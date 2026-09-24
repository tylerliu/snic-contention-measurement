#ifndef BENCHMARK_DRIVER_H
#define BENCHMARK_DRIVER_H

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>

// Runtime-configurable parameters with sensible defaults
extern unsigned int g_duration; // duration in seconds for the benchmark loop

// Generic parameter retrieval
const char *get_benchmark_param(const char *key);

void init_dpdk(int argc, char **argv);
void cleanup_dpdk(void);

void execute_pause_loops(unsigned int batch_size);
uint64_t get_wait_cycles(void);

#endif // BENCHMARK_DRIVER_H
