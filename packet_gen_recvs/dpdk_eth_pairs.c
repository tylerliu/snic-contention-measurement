// SPDX-License-Identifier: BSD-3-Clause
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ether.h>

#include "dpdk_common.h"

#define BATCH_SIZE 64
#define DEFAULT_PAYLOAD_SIZE 64
#define MAX_ETH_PAYLOAD 1500
#define STATS_INTERVAL 1

static volatile int global_running = 1;
static int g_show_per_port_stats = 0;

static void signal_handler(int signum) {
    printf("\nReceived signal %d, shutting down...\n", signum);
    global_running = 0;
}

// Global runtime (simple-gen style split workers)
static struct rte_mempool *mbuf_pool = NULL;
static struct rte_ether_addr *src_macs = NULL; // sized to num_ports
static struct rte_ether_addr *tx_dst_macs = NULL; // per port dst mac for TX ports
static uint16_t *tx_port_ids = NULL;
static uint16_t num_tx_ports = 0;
static uint16_t *rx_port_ids = NULL;
static uint16_t num_rx_ports = 0;
static uint16_t payload_size = DEFAULT_PAYLOAD_SIZE;
static uint32_t throttle_pauses = 0;
static uint16_t g_tx_lcores[256], g_tx_lcores_count = 0;
static uint16_t g_rx_lcores[256], g_rx_lcores_count = 0;
static uint16_t g_tx_queues_per_port = 1;
static uint16_t g_rx_queues_per_port = 1;

static struct dpdk_stats g_port_stats_cache[RTE_MAX_ETHPORTS];

static inline int is_tx_port(uint16_t pid) {
    for (uint16_t i = 0; i < num_tx_ports; i++) if (tx_port_ids && tx_port_ids[i] == pid) return 1;
    return 0;
}
static inline int is_rx_port(uint16_t pid) {
    for (uint16_t i = 0; i < num_rx_ports; i++) if (rx_port_ids && rx_port_ids[i] == pid) return 1;
    return 0;
}
static inline uint16_t ceil_div_u16(uint16_t a, uint16_t b) { return (uint16_t)((a + b - 1) / b); }

static inline void fill_eth_frame(struct rte_mbuf *mbuf,
                                  const struct rte_ether_addr *src,
                                  const struct rte_ether_addr *dst)
{
    uint8_t *pkt = rte_pktmbuf_mtod(mbuf, uint8_t *);
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)pkt;
    memset(eth, 0, sizeof(*eth));
    eth->src_addr = *src;
    eth->dst_addr = *dst;
    eth->ether_type = rte_cpu_to_be_16(0x88B5); // experimental ethertype

    uint8_t *payload = pkt + sizeof(struct rte_ether_hdr);
    for (uint16_t i = 0; i < payload_size; i++) payload[i] = (uint8_t)(i & 0xFF);

    uint16_t total_len = (uint16_t)(sizeof(struct rte_ether_hdr) + payload_size);
    mbuf->data_len = total_len;
    mbuf->pkt_len = total_len;
    mbuf->l2_len = sizeof(struct rte_ether_hdr);
    mbuf->ol_flags = 0;
}

static inline int get_tx_worker_index(void) {
    uint16_t lc = rte_lcore_id();
    for (uint16_t i = 0; i < g_tx_lcores_count; i++) if (g_tx_lcores[i] == lc) return (int)i;
    return 0;
}

static inline int get_rx_worker_index(void) {
    uint16_t lc = rte_lcore_id();
    for (uint16_t i = 0; i < g_rx_lcores_count; i++) if (g_rx_lcores[i] == lc) return (int)i;
    return 0;
}

static void reset_port_stats_cache(void) {
    dpdk_stats_cache_reset(g_port_stats_cache, RTE_MAX_ETHPORTS);
}

static int tx_worker(__rte_unused void *arg) {
    uint16_t w = (uint16_t)get_tx_worker_index();
    uint16_t tx_pid = tx_port_ids[w % (num_tx_ports ? num_tx_ports : 1)];
    uint16_t qid = (num_tx_ports ? (w / num_tx_ports) : 0);
    printf("TX worker core %u on port %u queue %u\n", rte_lcore_id(), tx_pid, qid);

    struct rte_mbuf *pending[BATCH_SIZE];
    uint16_t pending_count = 0;
    while (global_running) {
        // Refill pending up to BATCH_SIZE
        while (pending_count < BATCH_SIZE) {
            struct rte_mbuf *m = rte_pktmbuf_alloc(mbuf_pool);
            if (!m) break;
            fill_eth_frame(m, &src_macs[tx_pid], &tx_dst_macs[tx_pid]);
            pending[BATCH_SIZE - pending_count - 1] = m;
            pending_count++;
        }

        uint16_t sent = 0;
        if (pending_count > 0) {
            sent = rte_eth_tx_burst(tx_pid, qid, pending, pending_count);
            pending_count -= sent;
            if (sent == 0) rte_pause();
        }

        for (uint32_t i = 0; i < throttle_pauses; i++) rte_pause();
    }
    return 0;
}

static int rx_worker(__rte_unused void *arg) {
    uint16_t w = (uint16_t)get_rx_worker_index();
    uint16_t rx_pid = rx_port_ids[w % (num_rx_ports ? num_rx_ports : 1)];
    uint16_t qid = (num_rx_ports ? (w / num_rx_ports) : 0);
    printf("RX worker core %u on port %u queue %u\n", rte_lcore_id(), rx_pid, qid);
    while (global_running) {
        struct rte_mbuf *rx_bufs[BATCH_SIZE];
        const uint16_t n = rte_eth_rx_burst(rx_pid, qid, rx_bufs, BATCH_SIZE);
        if (n > 0) {
            for (uint16_t i = 0; i < n; i++) { rte_pktmbuf_free(rx_bufs[i]); }
        }
    }
    return 0;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("  -z, --size N         Payload size (1-%d, default %d)\n", MAX_ETH_PAYLOAD, DEFAULT_PAYLOAD_SIZE);
    printf("  -t, --throttle N     rte_pause() calls per loop (default 0)\n");
	printf("  -a, --device DEV     Sender device BDF (repeat to add multiple)\n");
	printf("  -R, --rx-device DEV  Receiver device BDF (repeat to add multiple)\n");
	printf("  -l, --lcores SET     Sender lcores (e.g., 1-3,5)\n");
	printf("  -L, --rx-lcores SET  Receiver lcores (e.g., 6-7)\n");
    printf("  -f, --file-prefix P  DPDK file-prefix\n");
    printf("  -M, --mbufs N        Number of mbufs in pool (default 65536)\n");
    printf("  -S, --per-port-stats Show per-port statistics (default: totals only)\n");
    printf("  -h, --help           Show this help\n");
	printf("Pairs are formed by BDF order: ith sender -> ith receiver.\n");
}

// Match BDF helper moved to dpdk_common

int main(int argc, char **argv) {
    int opt;
    const char *file_prefix = NULL;
	const char *tx_lcores_str = NULL;
	const char *rx_lcores_str = NULL;
	const char *tx_devices[64];
	uint16_t tx_device_count = 0;
	const char *rx_devices[64];
	uint16_t rx_device_count = 0;

	static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"size", required_argument, 0, 'z'},
        {"throttle", required_argument, 0, 't'},
		{"device", required_argument, 0, 'a'},
		{"rx-device", required_argument, 0, 'R'},
		{"lcores", required_argument, 0, 'l'},
		{"rx-lcores", required_argument, 0, 'L'},
        {"file-prefix", required_argument, 0, 'f'},
        {"mbufs", required_argument, 0, 'M'},
        {"per-port-stats", no_argument, 0, 'S'},
        {0, 0, 0, 0}
    };

	while ((opt = getopt_long(argc, argv, "hz:t:a:R:l:L:f:M:S", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h': print_usage(argv[0]); return 0;
            case 'z': {
                int sz = atoi(optarg);
                if (sz < 1 || sz > MAX_ETH_PAYLOAD) { fprintf(stderr, "Invalid size\n"); return 1; }
                payload_size = (uint16_t)sz;
                break;
            }
            case 't': {
                int pauses = atoi(optarg);
                if (pauses < 0) { fprintf(stderr, "Invalid throttle value\n"); return 1; }
                throttle_pauses = (uint32_t)pauses;
                break;
            }
			case 'a': if (tx_device_count < 64) tx_devices[tx_device_count++] = optarg; break;
			case 'R': if (rx_device_count < 64) rx_devices[rx_device_count++] = optarg; break;
			case 'l': tx_lcores_str = optarg; break;
			case 'L': rx_lcores_str = optarg; break;
            case 'f': file_prefix = optarg; break;
            case 'M': /* parsed later */ break;
            case 'S': g_show_per_port_stats = 1; break;
            default: print_usage(argv[0]); return 1;
        }
    }

    // Build EAL argv using common function
	char *eal_argv[argc + 128];
    int eal_argc = dpdk_build_eal_argv(argv[0], tx_devices, tx_device_count,
		rx_devices, rx_device_count, tx_lcores_str, rx_lcores_str,
		file_prefix, eal_argv, argc + 128);
    if (eal_argc < 0) {
        fprintf(stderr, "Failed to build EAL arguments\n");
        return 1;
	}

    printf("EAL arguments (%d): ", eal_argc);
    for (int i = 0; i < eal_argc; i++) printf("%s ", eal_argv[i]);
    printf("\n");
    fflush(stdout);

    if (rte_eal_init(eal_argc, eal_argv) < 0) { fprintf(stderr, "Failed to init DPDK EAL\n"); return 1; }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    uint16_t num_ports = rte_eth_dev_count_avail();
    if (num_ports < 2) { fprintf(stderr, "Need at least 2 ports for pairing (have %u)\n", num_ports); return 1; }

    uint32_t mbuf_pool_size = 65536;
    // Re-parse argv for -M/--mbufs (simple, safe second pass)
    optind = 1; // reset for a quick scan
    while ((opt = getopt_long(argc, argv, "hz:t:a:R:l:L:f:M:S", long_options, NULL)) != -1) {
        if (opt == 'M') {
            long v = strtol(optarg, NULL, 10);
            if (v > 0) mbuf_pool_size = (uint32_t)v;
        }
    }
    mbuf_pool = dpdk_create_mbuf_pool("ETH_PAIRS_MBUF", mbuf_pool_size, 512);
    if (!mbuf_pool) {
        return 1;
    }

	// Match TX and RX port ids from BDFs
	if (tx_device_count > 0) {
		tx_port_ids = calloc(num_ports, sizeof(uint16_t));
		if (dpdk_match_ports_by_bdf(tx_devices, tx_device_count, tx_port_ids, &num_tx_ports) != 0 || num_tx_ports == 0) {
			fprintf(stderr, "No TX ports matched -a list\n"); return 1;
		}
	}
	if (rx_device_count > 0) {
		rx_port_ids = calloc(num_ports, sizeof(uint16_t));
		if (dpdk_match_ports_by_bdf(rx_devices, rx_device_count, rx_port_ids, &num_rx_ports) != 0 || num_rx_ports == 0) {
			fprintf(stderr, "No RX ports matched -R list\n"); return 1;
		}
	}
	uint16_t num_pairs = (num_tx_ports && num_rx_ports) ? (num_tx_ports < num_rx_ports ? num_tx_ports : num_rx_ports) : (num_ports / 2);
	if (num_tx_ports && num_rx_ports && (num_tx_ports != num_rx_ports)) {
		printf("Warning: TX ports (%u) != RX ports (%u). Using %u pairs.\n", num_tx_ports, num_rx_ports, num_pairs);
	}

    // Build lcore lists BEFORE computing queues per port
    if (tx_lcores_str) { if (dpdk_parse_lcore_set(tx_lcores_str, g_tx_lcores, &g_tx_lcores_count) != 0) { fprintf(stderr, "Invalid -l set\n"); return 1; } }
    if (rx_lcores_str) { if (dpdk_parse_lcore_set(rx_lcores_str, g_rx_lcores, &g_rx_lcores_count) != 0) { fprintf(stderr, "Invalid -L set\n"); return 1; } }

	// Filter lcores: exclude main lcore and ensure RX/TX sets are disjoint
	if (dpdk_filter_lcores(g_tx_lcores, &g_tx_lcores_count, g_rx_lcores, &g_rx_lcores_count) != 0) {
		fprintf(stderr, "Failed to filter lcores\n");
		return 1;
	}

	if (g_tx_lcores_count == 0 && g_rx_lcores_count == 0) {
		fprintf(stderr, "No worker lcores available (main lcore excluded).\n");
		return 1;
	}

    // Configure each involved port with RX/TX queues based on worker counts (simple-gen style)
    src_macs = malloc(num_ports * sizeof(struct rte_ether_addr));
    if (!src_macs) { 
        fprintf(stderr, "Failed to allocate MAC array\n"); 
        return 1;
    }

    g_tx_queues_per_port = (g_tx_lcores_count == 0 || num_tx_ports == 0) ? 1 : ceil_div_u16(g_tx_lcores_count, num_tx_ports);
    g_rx_queues_per_port = (g_rx_lcores_count == 0 || num_rx_ports == 0) ? 1 : ceil_div_u16(g_rx_lcores_count, num_rx_ports);

    printf("RX queues per port: %u, TX queues per port: %u\n", g_rx_queues_per_port, g_tx_queues_per_port);
    struct rte_eth_conf port_conf;
    memset(&port_conf, 0, sizeof(port_conf));
    for (uint16_t pid = 0; pid < num_ports; pid++) {
        uint16_t rxq = is_rx_port(pid) ? g_rx_queues_per_port : 0;
        uint16_t txq = is_tx_port(pid) ? g_tx_queues_per_port : 0;
        printf("Configuring port %u with RX %u queues, TX %u queues\n", pid, rxq, txq);
        int ret = rte_eth_dev_configure(pid, rxq, txq, &port_conf);
        if (ret < 0) { 
            fprintf(stderr, "Configure failed on port %u\n", pid); 
            return 1; 
        }
        struct rte_eth_rxconf rxq_conf = {0};
        for (uint16_t q = 0; q < rxq; q++) {
            if ((ret = rte_eth_rx_queue_setup(pid, q, 1024, rte_eth_dev_socket_id(pid), &rxq_conf, mbuf_pool)) < 0) {
                fprintf(stderr, "RX queue setup failed on port %u q%u\n", pid, q); return 1; }
        }
        struct rte_eth_txconf txq_conf = {0};
        for (uint16_t q = 0; q < txq; q++) {
            if ((ret = rte_eth_tx_queue_setup(pid, q, 1024, rte_eth_dev_socket_id(pid), &txq_conf)) < 0) {
                fprintf(stderr, "TX queue setup failed on port %u q%u\n", pid, q); return 1; }
        }
        if ((ret = rte_eth_dev_start(pid)) < 0) { 
            fprintf(stderr, "Start failed on port %u (ret=%d)\n", pid, ret); 
            return 1; 
        }
        rte_eth_promiscuous_enable(pid);
        if ((ret = rte_eth_macaddr_get(pid, &src_macs[pid])) < 0) { 
            fprintf(stderr, "MAC get failed on %u\n", pid); 
            return 1; 
        }
        // Determine role and peer (if any)
        int tx_idx = -1, rx_idx = -1;
        for (uint16_t i = 0; i < num_tx_ports; i++) if (tx_port_ids && tx_port_ids[i] == pid) { tx_idx = (int)i; break; }
        for (uint16_t i = 0; i < num_rx_ports; i++) if (rx_port_ids && rx_port_ids[i] == pid) { rx_idx = (int)i; break; }
        const char *role = "IDLE";
        int peer = -1;
        if (tx_idx >= 0 && tx_idx < num_rx_ports) { role = "TX"; peer = rx_port_ids[tx_idx]; }
        else if (rx_idx >= 0 && rx_idx < num_tx_ports) { role = "RX"; peer = tx_port_ids[rx_idx]; }
        if (peer >= 0) {
            printf("Port %u MAC %02x:%02x:%02x:%02x:%02x:%02x  [%s -> Port %d]\n",
                   pid,
                   src_macs[pid].addr_bytes[0], src_macs[pid].addr_bytes[1], src_macs[pid].addr_bytes[2],
                   src_macs[pid].addr_bytes[3], src_macs[pid].addr_bytes[4], src_macs[pid].addr_bytes[5],
                   role, peer);
        } else {
            printf("Port %u MAC %02x:%02x:%02x:%02x:%02x:%02x  [%s]\n",
                   pid,
                   src_macs[pid].addr_bytes[0], src_macs[pid].addr_bytes[1], src_macs[pid].addr_bytes[2],
                   src_macs[pid].addr_bytes[3], src_macs[pid].addr_bytes[4], src_macs[pid].addr_bytes[5],
                   role);
        }
    }

    reset_port_stats_cache();

    // Build pairs and destination MAC map for TX ports

    // Assign to available lcores; if none, run first worker on main.
    // Expose globals
    tx_dst_macs = calloc(num_ports, sizeof(struct rte_ether_addr));
    for (uint16_t p = 0; p < num_pairs; p++) {
        uint16_t a = tx_port_ids ? tx_port_ids[p] : (uint16_t)(2*p);
        uint16_t b = rx_port_ids ? rx_port_ids[p] : (uint16_t)(2*p + 1);
        tx_dst_macs[a] = src_macs[b];
    }

    // Launch workers
    for (uint16_t i = 0; i < g_rx_lcores_count; i++) {
        rte_eal_remote_launch(rx_worker, NULL, g_rx_lcores[i]);
    }
    for (uint16_t i = 0; i < g_tx_lcores_count; i++) {
        rte_eal_remote_launch(tx_worker, NULL, g_tx_lcores[i]);
    }

    // Main-thread aggregation loop
    uint64_t last_tsc = rte_get_timer_cycles();
    const uint64_t stats_hz = rte_get_timer_hz();
    struct dpdk_stats port_deltas[RTE_MAX_ETHPORTS];

    while (global_running) {
        uint64_t now = rte_get_timer_cycles();
        uint64_t elapsed = now - last_tsc;
        if (elapsed >= STATS_INTERVAL * stats_hz) {
            double secs = (double)elapsed / (double)stats_hz;
            
            for (uint16_t pid = 0; pid < num_ports; pid++) {
                 if (dpdk_stats_get_delta(pid, &g_port_stats_cache[pid], &port_deltas[pid]) < 0) {
                     memset(&port_deltas[pid], 0, sizeof(port_deltas[pid]));
                 }
            }

            if (num_tx_ports > 0) {
                dpdk_print_port_stats("TX", tx_port_ids, num_tx_ports, secs, 1, g_show_per_port_stats, port_deltas);
            }
            if (num_rx_ports > 0) {
                dpdk_print_port_stats("RX", rx_port_ids, num_rx_ports, secs, 0, g_show_per_port_stats, port_deltas);
            }
            last_tsc = now;
        }
        rte_pause();
    }

    rte_eal_mp_wait_lcore();

    // Cleanup
    for (uint16_t pid = 0; pid < num_ports; pid++) {
        rte_eth_dev_stop(pid);
        rte_eth_dev_close(pid);
    }
    free(src_macs);
    free(tx_port_ids);
    free(rx_port_ids);
    free(tx_dst_macs);
    rte_eal_cleanup();
    return 0;
}


