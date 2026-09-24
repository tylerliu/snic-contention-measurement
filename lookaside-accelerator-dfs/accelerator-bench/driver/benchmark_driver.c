#include "benchmark_driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <rte_pause.h>
#include <rte_cycles.h>

#define MAX_PARAMS 16

static struct benchmark_param {
    char key[32];
    char value[128];
} g_params[MAX_PARAMS];
static int g_param_count = 0;

const char *get_benchmark_param(const char *key) {
    for (int i = 0; i < g_param_count; i++) {
        if (strcmp(g_params[i].key, key) == 0) {
            return g_params[i].value;
        }
    }
    return NULL;
}

// Defaults, override via command line args
unsigned int g_duration = 5; // default 5 seconds

static void parse_command_line_args(int argc, char **argv) {
    int separator_index = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            separator_index = i;
            break;
        }
    }

    // Default: no benchmark args
    int bench_start = (separator_index >= 0) ? (separator_index + 1) : argc;
    for (int i = bench_start; i < argc; i++) {
        if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--duration") == 0) && i + 1 < argc) {
            g_duration = (unsigned int)strtoul(argv[i + 1], NULL, 10);
            if (g_duration == 0) {
                g_duration = 5; // fallback to default
            }
            i++; // skip the value
        } else if (strncmp(argv[i], "--", 2) == 0 && i + 1 < argc) {
            const char *key = argv[i] + 2;
            const char *value = argv[i + 1];
            if (g_param_count < MAX_PARAMS) {
                strncpy(g_params[g_param_count].key, key, sizeof(g_params[g_param_count].key) - 1);
                strncpy(g_params[g_param_count].value, value, sizeof(g_params[g_param_count].value) - 1);
                g_param_count++;
            }
            i++; // Skip value
        }
    }
    
    // EAL args: everything before '--' (or all args if no '--')
    int dpdk_argc = (separator_index >= 0) ? separator_index : argc;

    int ret = rte_eal_init(dpdk_argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
    }
}

void init_dpdk(int argc, char **argv) {
    parse_command_line_args(argc, argv);
}

void cleanup_dpdk(void) {
    rte_eal_cleanup();
}

static int g_pause_loops = -1;
static uint64_t g_total_wait_cycles = 0;

void execute_pause_loops(unsigned int batch_size) {
    if (g_pause_loops == -1) {
        const char *pause_str = get_benchmark_param("pause_loops");
        g_pause_loops = pause_str ? atoi(pause_str) : 0;
    }
    if (g_pause_loops > 0) {
        uint64_t wait_start = rte_rdtsc();
        unsigned int total_pauses = g_pause_loops * batch_size;
        for (unsigned int i = 0; i < total_pauses; i++) {
            rte_pause();
        }
        g_total_wait_cycles += (rte_rdtsc() - wait_start);
    }
}

uint64_t get_wait_cycles(void) {
    return g_total_wait_cycles;
}

