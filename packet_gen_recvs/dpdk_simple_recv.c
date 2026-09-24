// SPDX-License-Identifier: BSD-3-Clause
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <arpa/inet.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_flow.h>
#include <rte_vxlan.h>

#include "dpdk_common.h"

#define BATCH_SIZE 64
#define STATS_INTERVAL 1
#define PARTIAL_DECRYPTION_TRAFFIC_PORT 3282

static struct rte_mempool *global_mbuf_pool = NULL;
static uint16_t *global_port_ids = NULL;
static uint16_t global_num_ports = 0;
static volatile int global_running = 1;
static uint16_t global_worker_count = 0;

static uint16_t global_filter_port = PARTIAL_DECRYPTION_TRAFFIC_PORT; // 0 = no filtering
static struct rte_flow **global_vxlan_rx_flows = NULL; // per port decap flows

static struct dpdk_stats g_port_stats_cache[RTE_MAX_ETHPORTS];
static uint16_t global_rx_queues_per_port = 0;
static int global_show_per_port_stats = 0;
static struct rte_ether_addr global_src_macs[RTE_MAX_ETHPORTS];
static struct in_addr global_rx_ip_addrs[RTE_MAX_ETHPORTS];
static struct dpdk_shared_rx_worker_ctx global_rx_worker_ctx[RTE_MAX_LCORE];

static void signal_handler(int signum) {
    printf("\nReceived signal %d, shutting down...\n", signum);
    global_running = 0;
}

static void reset_port_stats_cache(void) {
    dpdk_stats_cache_reset(g_port_stats_cache, RTE_MAX_ETHPORTS);
}

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("  -p, --port PORT      Filter UDP destination port (0 = all, default: %d)\n", PARTIAL_DECRYPTION_TRAFFIC_PORT);
    printf("  -a, --device DEVICE  Device to use (can specify multiple times)\n");
    printf("  -l, --lcores LCORES  Logical cores to use (e.g., 0-3, 0,2,4)\n");
    printf("  -f, --file-prefix P  DPDK file-prefix\n");
    printf("  -v, --vxlan IP:PORT  VXLAN decap match outer dst IP:port (repeat)\n");
    printf("  -M, --mbufs N        Number of mbufs in pool (default 8192)\n");
    printf("  -W, --rx-workers N   Total RX workers (default one per port)\n");
    printf("  -S, --per-port-stats Show per-port statistics (default: totals only)\n");
    printf("      --rx-ip IP       Respond to ARP for this IP per port (repeat)\n");
    printf("  -h, --help           Show this help\n");
}

int main(int argc, char **argv) {
    int ret, opt;
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"device", required_argument, 0, 'a'},
        {"lcores", required_argument, 0, 'l'},
        {"vxlan", required_argument, 0, 'v'},
        {"file-prefix", required_argument, 0, 'f'},
        {"mbufs", required_argument, 0, 'M'},
        {"rx-workers", required_argument, 0, 'W'},
        {"per-port-stats", no_argument, 0, 'S'},
        {"rx-ip", required_argument, 0, 1},
        {0, 0, 0, 0}
    };

    // Collect devices and lcores for EAL argument building
    const char *devices[64];
    uint16_t device_count = 0;
    const char *lcores_str = NULL;
    const char *file_prefix = NULL;
    const char *rx_ip_strs[64];
    uint16_t rx_ip_count = 0;
    uint16_t requested_rx_workers = 0;

    const char *vxlan_specs[64]; uint16_t vxlan_spec_count = 0;
    while ((opt = getopt_long(argc, argv, "hp:a:l:v:f:M:W:S", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'p': {
                int port = atoi(optarg);
                if (port < 0 || port > 65535) {
                    fprintf(stderr, "Invalid port. Must be 0-65535 (0 = no filter)\n");
                    return 1;
                }
                global_filter_port = (uint16_t)port;
                break;
            }
            case 'a':
                if (device_count < 64) devices[device_count++] = optarg;
                break;
            case 'l':
                lcores_str = optarg;
                break;
            case 'v':
                if (vxlan_spec_count < 64) vxlan_specs[vxlan_spec_count++] = optarg;
                break;
            case 'f':
                file_prefix = optarg;
                break;
            case 'M':
                /* parsed later */
                break;
            case 'W': {
                char *end = NULL;
                long count = strtol(optarg, &end, 10);
                if (end == optarg || *end != '\0' || count < 1 || count >= RTE_MAX_LCORE) {
                    fprintf(stderr, "Invalid --rx-workers value: %s\n", optarg);
                    return 1;
                }
                requested_rx_workers = (uint16_t)count;
                break;
            }
            case 'S':
                global_show_per_port_stats = 1;
                break;
            case 1:
                if (rx_ip_count < 64) rx_ip_strs[rx_ip_count++] = optarg;
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // Build EAL arguments using common function
    // Since simple_recv doesn't distinguish TX/RX, pass all devices as TX devices
    char *eal_argv[argc + 128];
    int eal_argc = dpdk_build_eal_argv(argv[0], devices, device_count,
		NULL, 0, lcores_str, NULL, file_prefix, eal_argv, argc + 128);
    if (eal_argc < 0) {
        fprintf(stderr, "Failed to build EAL arguments\n");
        return 1;
    }

    // Print EAL arguments for debugging
    printf("EAL arguments (%d): ", eal_argc);
    for (int i = 0; i < eal_argc; i++) {
        printf("%s ", eal_argv[i]);
    }
    printf("\n");
    fflush(stdout);

    ret = rte_eal_init(eal_argc, eal_argv);
    if (ret < 0) {
        fprintf(stderr, "Failed to init DPDK EAL\n");
        return 1;
    }

    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    uint16_t num_ports = rte_eth_dev_count_avail();
    printf("Available DPDK ports: %d\n", num_ports);
    if (num_ports == 0) {
        fprintf(stderr, "No DPDK ports available. Check device binding.\n");
        return 1;
    }

    // Use all available ports
    uint16_t num_ports_to_use = num_ports;
    printf("Using %d ports (0-%d)\n", num_ports_to_use, num_ports_to_use - 1);

    uint16_t total_lcores = rte_lcore_count();
    uint16_t main_lcore = rte_get_main_lcore();
    uint16_t available_workers = total_lcores > 0 ? (total_lcores - 1) : 0;

    printf("Total lcores: %d, Main lcore: %d, Worker lcores available: %d\n",
           total_lcores, main_lcore, available_workers);

    global_worker_count = requested_rx_workers ? requested_rx_workers : num_ports_to_use;
    if (global_worker_count < num_ports_to_use ||
        global_worker_count % num_ports_to_use != 0) {
        fprintf(stderr, "RX workers must be a positive multiple of the %u ports\n", num_ports_to_use);
        return 1;
    }
    if (available_workers < global_worker_count) {
        fprintf(stderr, "Need %u worker lcores; provide main plus workers via -l.\n", global_worker_count);
        return 1;
    }

    uint16_t queues_per_port = global_worker_count / num_ports_to_use;
    global_rx_queues_per_port = queues_per_port;
    printf("Setting up %u RX queue(s) per port (%u total)\n",
           queues_per_port, queues_per_port * num_ports_to_use);

    // Create mbuf pool
    uint32_t mbuf_pool_size = 8192;
    // Re-parse for -M/--mbufs
    optind = 1;
    while ((opt = getopt_long(argc, argv, "hp:a:l:v:f:M:W:S", long_options, NULL)) != -1) {
        if (opt == 'M') {
            long v = strtol(optarg, NULL, 10);
            if (v > 0) mbuf_pool_size = (uint32_t)v;
        }
    }
    struct rte_mempool *mbuf_pool = dpdk_create_mbuf_pool("MBUF_POOL_RX", mbuf_pool_size, 256);
    if (!mbuf_pool) {
        fprintf(stderr, "Failed to create mbuf pool\n");
        return 1;
    }

    // Configure and start all ports for RX
    struct rte_eth_conf port_conf;
    memset(&port_conf, 0, sizeof(port_conf));
    if (queues_per_port > 1) {
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
        port_conf.rx_adv_conf.rss_conf.rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP;
    }

    global_port_ids = malloc(num_ports_to_use * sizeof(uint16_t));
    if (!global_port_ids) {
        fprintf(stderr, "Failed to allocate port id array\n");
        return 1;
    }

    for (uint16_t port_id = 0; port_id < num_ports_to_use; port_id++) {
        if (!rte_eth_dev_is_valid_port(port_id)) {
            fprintf(stderr, "Port %d is not valid\n", port_id);
            free(global_port_ids);
            return 1;
        }

        struct rte_eth_dev_info dev_info;
        ret = rte_eth_dev_info_get(port_id, &dev_info);
        if (ret < 0) {
            fprintf(stderr, "Failed to query port %d capabilities\n", port_id);
            free(global_port_ids);
            return 1;
        }
        if (queues_per_port > dev_info.max_rx_queues ||
            queues_per_port > dev_info.max_tx_queues) {
            fprintf(stderr, "Port %d supports only %u RX and %u TX queues\n",
                    port_id, dev_info.max_rx_queues, dev_info.max_tx_queues);
            free(global_port_ids);
            return 1;
        }
        if (queues_per_port > 1)
            port_conf.rx_adv_conf.rss_conf.rss_hf =
                (RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP) & dev_info.flow_type_rss_offloads;
        ret = rte_eth_dev_configure(port_id, queues_per_port, queues_per_port, &port_conf);
        if (ret < 0) {
            fprintf(stderr, "Failed to configure port %d with %d RX queues\n", port_id, queues_per_port);
            free(global_port_ids);
            return 1;
        }

        struct rte_eth_rxconf rxq_conf;
        memset(&rxq_conf, 0, sizeof(rxq_conf));
        for (uint16_t q = 0; q < queues_per_port; q++) {
            ret = rte_eth_rx_queue_setup(port_id, q, 1024, rte_eth_dev_socket_id(port_id), &rxq_conf, mbuf_pool);
            if (ret < 0) {
                fprintf(stderr, "Failed to setup RX queue %d on port %d\n", q, port_id);
                free(global_port_ids);
                return 1;
            }
        }

        struct rte_eth_txconf txq_conf;
        memset(&txq_conf, 0, sizeof(txq_conf));
        for (uint16_t q = 0; q < queues_per_port; q++) {
            ret = rte_eth_tx_queue_setup(port_id, q, 1024,
                                         rte_eth_dev_socket_id(port_id), &txq_conf);
            if (ret < 0) {
                fprintf(stderr, "Failed to setup TX queue %u on port %d\n", q, port_id);
                free(global_port_ids);
                return 1;
            }
        }

        ret = rte_eth_dev_start(port_id);
        if (ret < 0) {
            fprintf(stderr, "Failed to start port %d (ret=%d). This may be due to:\n", port_id, ret);
            fprintf(stderr, "  - Device not properly bound to DPDK\n");
            fprintf(stderr, "  - Insufficient permissions\n");
            free(global_port_ids);
            return 1;
        }

        rte_eth_promiscuous_enable(port_id);
        global_port_ids[port_id] = port_id;
        rte_eth_macaddr_get(port_id, &global_src_macs[port_id]);
    }

    memset(global_rx_ip_addrs, 0, sizeof(global_rx_ip_addrs));
    if (rx_ip_count > 0) {
        if (rx_ip_count != num_ports_to_use) {
            fprintf(stderr, "Number of --rx-ip values (%u) must match number of RX ports (%u)\n", rx_ip_count, num_ports_to_use);
            return 1;
        }
        for (uint16_t i = 0; i < num_ports_to_use; i++) {
            struct in_addr ip = {0};
            if (!inet_aton(rx_ip_strs[i], &ip)) {
                fprintf(stderr, "Invalid --rx-ip value '%s'\n", rx_ip_strs[i]);
                return 1;
            }
            global_rx_ip_addrs[global_port_ids[i]] = ip;
        }
    }

    reset_port_stats_cache();

    // Install VXLAN decap flows if requested (map by RX port order)
    if (vxlan_spec_count > 0) {
        global_vxlan_rx_flows = calloc(num_ports_to_use ? num_ports_to_use : 1, sizeof(struct rte_flow *));
        for (uint16_t i = 0; i < vxlan_spec_count && i < num_ports_to_use; i++) {
            struct in_addr ip = {0}; uint16_t prt = 0; char err[128];
            if (dpdk_parse_ip_port(vxlan_specs[i], &ip, &prt) != 0) {
                fprintf(stderr, "Invalid -v value '%s' (expected IP:PORT)\n", vxlan_specs[i]);
                continue;
            }
            struct rte_flow *flow = NULL;
            if (dpdk_install_vxlan_decap_flow(global_port_ids[i], ip, prt, &flow, err, sizeof(err)) != 0) {
                fprintf(stderr, "Failed to install VXLAN decap on RX port %u: %s\n", global_port_ids[i], err);
            } else {
                global_vxlan_rx_flows[global_port_ids[i]] = flow;
                printf("Installed VXLAN decap on RX port %u for %s:%u\n", global_port_ids[i], inet_ntoa(ip), prt);
            }
        }
    }

    global_mbuf_pool = mbuf_pool;
    global_num_ports = num_ports_to_use;

    printf("DPDK Simple Packet Receiver\n");
    printf("  Ports: %d\n", global_num_ports);
    printf("  Filter: %s\n", global_filter_port == 0 ? "All UDP" : "UDP dst port only");
    if (global_filter_port != 0) printf("  UDP dst port: %u\n", global_filter_port);
    printf("  Batch size: %d\n", BATCH_SIZE);

    unsigned int lc = rte_get_next_lcore(-1, 1, 0);
    uint16_t worker_idx = 0;
    while (lc < RTE_MAX_LCORE && worker_idx < global_worker_count) {
        uint16_t port_id = global_port_ids[worker_idx % global_num_ports];
        uint16_t queue_id = worker_idx / global_num_ports;
        struct dpdk_shared_rx_worker_ctx *ctx = &global_rx_worker_ctx[worker_idx];
        memset(ctx, 0, sizeof(*ctx));
        ctx->running = &global_running;
        ctx->port_id = port_id;
        ctx->rx_queue_id = queue_id;
        ctx->tx_queue_id = queue_id;
        ctx->filter_udp_port = global_filter_port;
        ctx->warn_on_mismatch = 1;
        ctx->port_macs = global_src_macs;
        ctx->rx_ip_addrs = global_rx_ip_addrs;
        ret = rte_eal_remote_launch(dpdk_shared_rx_worker, ctx, lc);
    if (ret < 0) {
            fprintf(stderr, "Failed to launch RX worker on lcore %u\n", lc);
            return 1;
        }
        worker_idx++;
        lc = rte_get_next_lcore(lc, 1, 0);
    }
    if (worker_idx < global_worker_count) {
        fprintf(stderr, "Insufficient enabled lcores for %u RX workers\n", global_worker_count);
        return 1;
    }

    // Main-thread aggregation loop: compute deltas and print totals + per-thread averages
    uint64_t last_tsc = rte_get_timer_cycles();
    const uint64_t stats_hz = rte_get_timer_hz();
    struct dpdk_stats port_deltas[RTE_MAX_ETHPORTS];

    while (global_running) {
        uint64_t now = rte_get_timer_cycles();
        uint64_t elapsed = now - last_tsc;
        if (elapsed >= STATS_INTERVAL * stats_hz) {
            double secs = (double)elapsed / (double)stats_hz;
            
            for (uint16_t pid = 0; pid < global_num_ports; pid++) {
                 if (dpdk_stats_get_delta(pid, &g_port_stats_cache[pid], &port_deltas[pid]) < 0) {
                     memset(&port_deltas[pid], 0, sizeof(port_deltas[pid]));
                 }
            }

            dpdk_print_port_stats("RX", global_port_ids, global_num_ports, secs, 0, global_show_per_port_stats, port_deltas);
            last_tsc = now;
        }
        rte_pause();
    }

    rte_eal_mp_wait_lcore();

    printf("Stopping all ports...\n");
    for (uint16_t i = 0; i < global_num_ports; i++) {
        if (global_vxlan_rx_flows && global_vxlan_rx_flows[global_port_ids[i]]) {
            struct rte_flow_error e = {0};
            rte_flow_destroy(global_port_ids[i], global_vxlan_rx_flows[global_port_ids[i]], &e);
            global_vxlan_rx_flows[global_port_ids[i]] = NULL;
        }
        printf("Stopping port %d...\n", global_port_ids[i]);
        rte_eth_dev_stop(global_port_ids[i]);
        rte_eth_dev_close(global_port_ids[i]);
    }

    rte_eal_cleanup();
    free(global_port_ids);
    if (global_vxlan_rx_flows) free(global_vxlan_rx_flows);
    return 0;
}


