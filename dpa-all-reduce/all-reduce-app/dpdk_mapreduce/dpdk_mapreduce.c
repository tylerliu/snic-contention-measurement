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
#include <rte_malloc.h>

#include "dpdk_common.h"
#include <rte_jhash.h>

typedef uint64_t flexio_uintptr_t;
#include "../flexio_packet_processor_com.h"

static int dpdk_mapreduce_worker(void *arg);
static void dpdk_mapreduce_main_worker(void *arg);

#define BATCH_SIZE 64
#define STATS_INTERVAL 1
#define PARTIAL_DECRYPTION_TRAFFIC_PORT 3282

static struct rte_mempool *global_mbuf_pool = NULL;
static uint16_t *global_port_ids = NULL;
static uint16_t global_num_ports = 0;
static volatile int global_running = 1;
static uint16_t global_worker_count = 0;

static uint16_t global_filter_port = PARTIAL_DECRYPTION_TRAFFIC_PORT; // 0 = no filtering

static struct rte_eth_stats g_port_stats_cache[RTE_MAX_ETHPORTS];
static uint16_t global_rx_queues_per_port = 0;
static int global_show_per_port_stats = 0;
static struct rte_ether_addr global_src_macs[RTE_MAX_ETHPORTS];
static struct in_addr global_rx_ip_addrs[RTE_MAX_ETHPORTS];
static struct dpdk_shared_rx_worker_ctx global_rx_worker_ctx[RTE_MAX_LCORE];

static uint32_t global_limit = 10;
static uint32_t global_sport_shift = 0;

#define AGGREGATION_TABLE_SIZE (1 << 16)
#define AGGREGATION_TABLE_MASK (AGGREGATION_TABLE_SIZE - 1)

// Per-thread aggregation context
struct thread_aggregation_ctx {
    struct aggregation_entry *buffer;
    uint32_t buffer_size_entries;
};
static struct thread_aggregation_ctx global_agg_ctx[RTE_MAX_LCORE];

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
    printf("  -M, --mbufs N        Number of mbufs in pool (default 8192 * queues)\n");
    printf("  -S, --per-port-stats Show per-port statistics (default: totals only)\n");
    printf("      --rx-ip IP       Respond to ARP for this IP per port (repeat)\n");
    printf("      --limit N        Aggregation count limit (default %u)\n", global_limit);
    printf("  -h, --help           Show this help\n");
}

int main(int argc, char **argv) {
    int ret, opt;
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"device", required_argument, 0, 'a'},
        {"lcores", required_argument, 0, 'l'},
        {"vxlan", required_argument, 0, 'v'}, // Keeping arg parsing to avoid errors if passed, but ignored
        {"file-prefix", required_argument, 0, 'f'},
        {"mbufs", required_argument, 0, 'M'},
        {"per-port-stats", no_argument, 0, 'S'},
        {"rx-ip", required_argument, 0, 1},
        {"limit", required_argument, 0, 2},
        {0, 0, 0, 0}
    };

    // Collect devices and lcores for EAL argument building
    const char *devices[64];
    uint16_t device_count = 0;
    const char *lcores_str = NULL;
    const char *file_prefix = NULL;
    const char *rx_ip_strs[64];
    uint16_t rx_ip_count = 0;

    // We largely ignore VXLAN args now but keep parsing loop compatible
    while ((opt = getopt_long(argc, argv, "hp:a:l:v:f:M:S", long_options, NULL)) != -1) {
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
                // Ignored
                break;
            case 'f':
                file_prefix = optarg;
                break;
            case 'M':
                /* parsed later */
                break;
            case 'S':
                global_show_per_port_stats = 1;
                break;
            case 1:
                if (rx_ip_count < 64) rx_ip_strs[rx_ip_count++] = optarg;
                break;
            case 2:
                global_limit = (uint32_t)atoi(optarg);
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // Build EAL arguments using common function
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
    uint16_t available_workers = total_lcores; // use main thread too

    printf("Total lcores: %d, Main lcore: %d, Worker lcores available: %d\n",
           total_lcores, main_lcore, available_workers);

    // MapReduce Steering Logic
    if (available_workers == 0) {
        fprintf(stderr, "Need at least 1 worker lcore.\n");
        return 1;
    }

    // Calculate queues per port (power of 2)
    uint16_t queues_per_port = available_workers / num_ports_to_use;
    if (queues_per_port == 0) queues_per_port = 1;
    
    // Ensure queues_per_port is power of 2 for easy masking
    uint16_t p2 = 1;
    while (p2 * 2 <= queues_per_port) p2 *= 2;
    queues_per_port = p2;

    global_rx_queues_per_port = queues_per_port;
    global_worker_count = queues_per_port * num_ports_to_use;

    // Automatically calculate shift to avoid holes in the table
    // shift = log2(queues_per_port)
    global_sport_shift = 0;
    while ((1U << global_sport_shift) < queues_per_port) {
        global_sport_shift++;
    }

    printf("Setting up %u RX queue(s) per port (Total workers: %u, Mask: 0x%x)\n",
           queues_per_port, global_worker_count, queues_per_port - 1);
    printf("Aggregation: Limit=%u, Shift=%u (Auto-calculated)\n", global_limit, global_sport_shift);

    // Create mbuf pool
    uint32_t mbuf_pool_size = 8192 * queues_per_port; 
    // Re-parse for -M/--mbufs
    optind = 1;
    while ((opt = getopt_long(argc, argv, "hp:a:l:v:f:M:S", long_options, NULL)) != -1) {
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
    global_mbuf_pool = mbuf_pool;

    // Configure and start all ports for RX
    struct rte_eth_conf port_conf;
    memset(&port_conf, 0, sizeof(port_conf));
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    port_conf.rx_adv_conf.rss_conf.rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP;
    port_conf.txmode.offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM;

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

        ret = rte_eth_dev_configure(port_id, queues_per_port, queues_per_port, &port_conf);
        if (ret < 0) {
            fprintf(stderr, "Failed to configure port %d with %d RX queues and %d TX queues\n", port_id, queues_per_port, queues_per_port);
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
        // Setup same number of TX queues as RX queues
        for (uint16_t q = 0; q < queues_per_port; q++) {
            ret = rte_eth_tx_queue_setup(port_id, q, 1024, rte_eth_dev_socket_id(port_id), &txq_conf);
            if (ret < 0) {
                fprintf(stderr, "Failed to setup TX queue %d on port %d\n", q, port_id);
                free(global_port_ids);
                return 1;
            }
        }

        ret = rte_eth_dev_start(port_id);
        if (ret < 0) {
            fprintf(stderr, "Failed to start port %d\n", port_id);
            free(global_port_ids);
            return 1;
        }

        rte_eth_promiscuous_enable(port_id);
        global_port_ids[port_id] = port_id;
        rte_eth_macaddr_get(port_id, &global_src_macs[port_id]);

        // === Steering Logic ===
        // Create flows to steer packets based on UDP source port bits to queues
        printf("Creating steering flows for port %d...\n", port_id);
        uint16_t mask = queues_per_port - 1;
        
        for (uint16_t q = 0; q < queues_per_port; q++) {
            struct rte_flow_attr attr;
            memset(&attr, 0, sizeof(attr));
            attr.ingress = 1;

            struct rte_flow_item pattern[4];
            struct rte_flow_action actions[2];

            // Item 0: Ethernet (Any)
            memset(&pattern[0], 0, sizeof(pattern[0]));
            pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;

            // Item 1: IPv4 (Any)
            memset(&pattern[1], 0, sizeof(pattern[1]));
            pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;

            // Item 2: UDP with Source Port Mask
            struct rte_flow_item_udp udp_spec;
            struct rte_flow_item_udp udp_mask;
            memset(&udp_spec, 0, sizeof(udp_spec));
            memset(&udp_mask, 0, sizeof(udp_mask));
            
            memset(&pattern[2], 0, sizeof(pattern[2]));
            pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
            pattern[2].spec = &udp_spec; 
            pattern[2].mask = &udp_mask; 

            // Steering Rule: Match strict local bits of UDP Source Port
            udp_spec.hdr.src_port = rte_cpu_to_be_16(q);
            udp_mask.hdr.src_port = rte_cpu_to_be_16(mask);
            
            if (global_filter_port != 0) {
                 udp_spec.hdr.dst_port = rte_cpu_to_be_16(global_filter_port);
                 udp_mask.hdr.dst_port = 0xFFFF;
            }

            // Item 3: End
            memset(&pattern[3], 0, sizeof(pattern[3]));
            pattern[3].type = RTE_FLOW_ITEM_TYPE_END;

            // Action 0: Queue
            struct rte_flow_action_queue queue_action;
            queue_action.index = q;
            memset(&actions[0], 0, sizeof(actions[0]));
            actions[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
            actions[0].conf = &queue_action;

            // Action 1: End
            memset(&actions[1], 0, sizeof(actions[1]));
            actions[1].type = RTE_FLOW_ACTION_TYPE_END;

            struct rte_flow_error error;
            struct rte_flow *flow = rte_flow_create(port_id, &attr, pattern, actions, &error);
            if (!flow) {
                 fprintf(stderr, "Failed to create flow for queue %d: %s\n", q, error.message);
            } else if (global_show_per_port_stats) {
                 printf("  Queue %d: Flow created\n", q);
            }
        }
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
    global_num_ports = num_ports_to_use;
    
    printf("DPDK MapReduce Packet Processor\n");
    printf("  Ports: %d\n", global_num_ports);
    printf("Aggregation: Limit=%u, Shift=%u\n", global_limit, global_sport_shift);

    unsigned int lc = rte_get_next_lcore(-1, 0, 0);
    uint16_t total_workers_scheduled = 0;
    struct dpdk_shared_rx_worker_ctx *main_ctx = NULL;
    
    // Launch workers: 1 per queue per port
    for (uint16_t p = 0; p < num_ports_to_use; p++) {
        for (uint16_t q = 0; q < queues_per_port; q++) {
             struct dpdk_shared_rx_worker_ctx *ctx = &global_rx_worker_ctx[total_workers_scheduled];
             memset(ctx, 0, sizeof(*ctx));
             ctx->running = &global_running;
             ctx->port_id = global_port_ids[p];
             ctx->rx_queue_id = q; 
             ctx->tx_queue_id = q; // Map RX queue to TX queue (one-to-one per worker)
             ctx->filter_udp_port = global_filter_port;
             ctx->port_macs = global_src_macs;
             ctx->rx_ip_addrs = global_rx_ip_addrs;
             
             // Alloc Aggregation Buffer (Reduced size)
             // Note: We need to alloc for main lcore too, based on its ID.
             // But we need to find which lc is assigned to this work.
             
             if (lc >= RTE_MAX_LCORE) {
                  fprintf(stderr, "Not enough lcores! (lc=%u)\n", lc);
                  break;
             }

             struct thread_aggregation_ctx *agg_ctx = &global_agg_ctx[lc];
             agg_ctx->buffer_size_entries = AGGREGATION_TABLE_SIZE;
             // Ensure at least min size
             if (agg_ctx->buffer_size_entries < 1) agg_ctx->buffer_size_entries = 1;

             size_t alloc_size = agg_ctx->buffer_size_entries * sizeof(struct aggregation_entry);
             void *ptr = NULL;
             int ret = posix_memalign(&ptr, 4096, alloc_size);
             if (ret == 0 && ptr != NULL) {
                 memset(ptr, 0, alloc_size);
                 agg_ctx->buffer = ptr;
             } else {
                 agg_ctx->buffer = NULL;
             }
             
             if (!agg_ctx->buffer) {
                 fprintf(stderr, "Failed to allocate aggregation buffer for lcore %u\n", lc);
                 return 1;
             }
             
             if (lc == main_lcore) {
                 // Don't remote launch, save context for local execution
                 main_ctx = ctx;
                 printf("Lcore %u (Main) assigned to Port %u Queue %u\n", lc, ctx->port_id, ctx->rx_queue_id);
             } else {
                 ret = rte_eal_remote_launch(dpdk_mapreduce_worker, ctx, lc);
                 if (ret < 0) {
                     fprintf(stderr, "Failed to launch worker on lcore %u\n", lc);
                     return 1;
                 }
             }
             total_workers_scheduled++;
             lc = rte_get_next_lcore(lc, 0, 0);
        }
    }
    
    if (main_ctx) {
        dpdk_mapreduce_main_worker(main_ctx);
    } else {
        // Fallback if main thread somehow wasn't assigned work (e.g. fewer queues than cores)
        // Just run the stats loop
        printf("Main thread not assigned packet processing work. Running stats only.\n");
        // Main-thread stats loop (Original Logic)
        uint64_t last_tsc = rte_get_timer_cycles();
        const uint64_t stats_hz = rte_get_timer_hz();
        struct rte_eth_stats port_deltas[RTE_MAX_ETHPORTS];

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

                dpdk_print_port_stats("MapReduce", global_port_ids, global_num_ports, secs, 0, global_show_per_port_stats, port_deltas);
                last_tsc = now;
            }
            rte_pause();
        }
    }

    rte_eal_mp_wait_lcore();

    printf("Stopping all ports...\n");
    for (uint16_t i = 0; i < global_num_ports; i++) {
        rte_eth_dev_stop(global_port_ids[i]);
        rte_eth_dev_close(global_port_ids[i]);
    }

    rte_eal_cleanup();
    free(global_port_ids);
    return 0;
}

static inline void process_mapreduce_batch(struct dpdk_shared_rx_worker_ctx *ctx, struct thread_aggregation_ctx *agg_ctx) {
    struct rte_mbuf *mbufs[BATCH_SIZE];
    uint16_t received = rte_eth_rx_burst(ctx->port_id, ctx->rx_queue_id, mbufs, BATCH_SIZE);
    
    if (received == 0) return;
    
    uint32_t headers_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr);
    
    for (uint16_t i = 0; i < received; i++) {
        struct rte_mbuf *mbuf = mbufs[i];
        
        // ARP Check
        if (dpdk_handle_arp_request(ctx->port_id, mbuf, ctx->port_macs, ctx->rx_ip_addrs, ctx->tx_queue_id)) {
            continue;
        }
        
        // Validation: Ensure valid length and contiguous data (simple check)
        if (mbuf->data_len < headers_len + AGGREGATION_PAD_BYTES + (AGGREGATION_NUM_INTS * 8)) {
            rte_pktmbuf_free(mbuf);
            continue;
        }
        
        struct rte_ether_hdr *eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
            rte_pktmbuf_free(mbuf);
            continue;
        }
        
        struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
        struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((char*)ip + sizeof(struct rte_ipv4_hdr));
        
        // Filter by Destination Port
        if (ctx->filter_udp_port != 0 && rte_be_to_cpu_16(udp->dst_port) != ctx->filter_udp_port) {
            rte_pktmbuf_free(mbuf);
            continue;
        }

        uint16_t sport = rte_be_to_cpu_16(udp->src_port);
        
        // Aggregation: Hash using sport >> shift and padding bytes
        // Pointer to padding (after UDP header)
        const void *pad_ptr = (const void *)((const char *)udp + sizeof(struct rte_udp_hdr));

        // Optimize: Pack data into 2 words (8 bytes) and use inline jhash_2words
        // Key: 6 bytes padding + sport (2 bytes effective) = 8 bytes
        uint32_t word1;
        uint16_t pad16;

        // Load first 4 bytes of padding
        memcpy(&word1, pad_ptr, 4);

        // Load last 2 bytes of padding
        memcpy(&pad16, (const char *)pad_ptr + 4, 2);

        // Pack Word 2: Padding[4-5] (low 16) + Sport (high 16)
        uint32_t k_src = (uint32_t)sport >> global_sport_shift;
        uint32_t word2 = (uint32_t)pad16 | (k_src << 16);

        uint32_t hash = rte_jhash_2words(word1, word2, 0);

        uint32_t idx = hash & (agg_ctx->buffer_size_entries - 1);
        if (idx >= agg_ctx->buffer_size_entries || !agg_ctx->buffer) {
            // Out of bounds or buffer not alloc, drop
            rte_pktmbuf_free(mbuf);
            continue;
        }
        
        struct aggregation_entry *entry = &agg_ctx->buffer[idx];
        
        // Pointer to payload
        char *payload_start = (char *)udp + sizeof(struct rte_udp_hdr) + AGGREGATION_PAD_BYTES;
        uint64_t *payload_ptr = (uint64_t *)payload_start;
        
        // Summation
        for (int k = 0; k < AGGREGATION_NUM_INTS; k++) {
            entry->values[k] += payload_ptr[k];
        }
        entry->count++;
        
        // Threshold Check
        if (entry->count >= global_limit) {
            // Reflect
            struct rte_ether_addr my_mac = ctx->port_macs[ctx->port_id];
            
            // Swap MACs
            eth->dst_addr = eth->src_addr;
            eth->src_addr = my_mac;
            
            // Swap IPs
            uint32_t tmp_ip = ip->dst_addr;
            ip->dst_addr = ip->src_addr;
            ip->src_addr = tmp_ip;
            ip->hdr_checksum = 0; // Required for HW calc
            
            // Prepare mbuf for TX Offload
            mbuf->l2_len = sizeof(struct rte_ether_hdr);
            mbuf->l3_len = sizeof(struct rte_ipv4_hdr);
            mbuf->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_UDP_CKSUM;
            
            // For UDP HW Checksum, we need the pseudo-header checksum in the UDP header
            udp->dgram_cksum = 0;
            
            // Copy aggregated values to payload
            memcpy(payload_ptr, entry->values, AGGREGATION_NUM_INTS * sizeof(uint64_t));
            
            // Reset entry
            entry->count = 0;
            memset(entry->values, 0, AGGREGATION_NUM_INTS * sizeof(uint64_t));
            
            // Send
            // Send 'global_limit' copies of the packet
            uint32_t to_send = global_limit;
            
            // Increase reference count for multiple sends
            // Current refcnt is 1. We want 'to_send' references.
            if (to_send > 1) {
                rte_pktmbuf_refcnt_update(mbuf, (uint16_t)(to_send - 1));
            }
            
            uint32_t sent_count = 0;
            struct rte_mbuf *tx_burst_mbufs[64];
            
            while (sent_count < to_send) {
                uint32_t burst_size = to_send - sent_count;
                if (burst_size > 64) burst_size = 64;
                
                for (uint32_t b = 0; b < burst_size; b++) {
                    tx_burst_mbufs[b] = mbuf;
                }
                
                uint16_t nb_tx = rte_eth_tx_burst(ctx->port_id, ctx->tx_queue_id, tx_burst_mbufs, (uint16_t)burst_size);
                
                if (unlikely(nb_tx < burst_size)) {
                    // Free valid references that were not accepted by the NIC
                    for (uint32_t k = nb_tx; k < burst_size; k++) {
                        rte_pktmbuf_free(mbuf);
                    }
                }
                
                sent_count += burst_size;
            }
        } else {
            // Drop packet (consumed in aggregation)
            rte_pktmbuf_free(mbuf);
        }
    }
}

static void dpdk_mapreduce_main_worker(void *arg) {
    struct dpdk_shared_rx_worker_ctx *ctx = (struct dpdk_shared_rx_worker_ctx *)arg;
    
    // Safety check mostly
    if (!ctx || !ctx->running) return;

    uint32_t lcore_id = rte_lcore_id();
    struct thread_aggregation_ctx *agg_ctx = &global_agg_ctx[lcore_id];

    printf("MapReduce MAIN worker core %u: port %u queue %u (Limit %u, Shift %u)\n",
           lcore_id, ctx->port_id, ctx->rx_queue_id, global_limit, global_sport_shift);

    uint64_t last_tsc = rte_get_timer_cycles();
    const uint64_t stats_hz = rte_get_timer_hz();
    struct rte_eth_stats port_deltas[RTE_MAX_ETHPORTS];

    while (*(ctx->running)) {
        // 1. Process Packets
        process_mapreduce_batch(ctx, agg_ctx);
        
        // 2. Check Timer & Print Stats
        uint64_t now = rte_get_timer_cycles();
        uint64_t elapsed = now - last_tsc;
        if (unlikely(elapsed >= STATS_INTERVAL * stats_hz)) {
            double secs = (double)elapsed / (double)stats_hz;
            
            for (uint16_t pid = 0; pid < global_num_ports; pid++) {
                 if (dpdk_stats_get_delta(pid, &g_port_stats_cache[pid], &port_deltas[pid]) < 0) {
                     memset(&port_deltas[pid], 0, sizeof(port_deltas[pid]));
                 }
            }

            dpdk_print_port_stats("MapReduce", global_port_ids, global_num_ports, secs, 0, global_show_per_port_stats, port_deltas);
            last_tsc = now;
        }
    }
}

static int dpdk_mapreduce_worker(void *arg) {
    struct dpdk_shared_rx_worker_ctx *ctx = (struct dpdk_shared_rx_worker_ctx *)arg;
    if (!ctx || !ctx->running) return 0;
    
    uint32_t lcore_id = rte_lcore_id();
    struct thread_aggregation_ctx *agg_ctx = &global_agg_ctx[lcore_id];
    
    printf("MapReduce worker core %u: port %u queue %u (Limit %u, Shift %u)\n",
           lcore_id, ctx->port_id, ctx->rx_queue_id, global_limit, global_sport_shift);

    while (*(ctx->running)) {
        process_mapreduce_batch(ctx, agg_ctx);
    }
    return 0;
}
