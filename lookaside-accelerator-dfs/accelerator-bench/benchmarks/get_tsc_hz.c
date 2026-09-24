#include "driver/benchmark_driver.h"
#include <stdio.h>
#include <rte_cycles.h>

int main(int argc, char **argv) {
    init_dpdk(argc, argv);

    const uint64_t hz = rte_get_timer_hz();
    // Print in both human-friendly and parse-friendly forms
    printf("TSC frequency (rte_get_timer_hz): %lu\n", (unsigned long)hz);
    printf("metadata: {'tsc_hz': %lu}\n", (unsigned long)hz);

    cleanup_dpdk();
    return 0;
}


