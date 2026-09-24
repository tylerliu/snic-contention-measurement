// SPDX-License-Identifier: BSD-3-Clause
#include <netinet/in.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_flow.h>
#include <signal.h>
#include "dpdk_common.h"
#include "generic/rte_cycles.h"

#define MAX_PAYLOAD_SIZE 1472  // Reasonable max to stay under MTU
#define PACKET_HEADERS_LEN (sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr))
#define MAX_PACKET_SIZE (PACKET_HEADERS_LEN + MAX_PAYLOAD_SIZE)
#define DEFAULT_BATCH_SIZE 10
#define BATCH_HISTORY_SIZE 65536
#define STATS_INTERVAL 1
#define DEFAULT_DST_PORT 3282
#define DEFAULT_SRC_PORT 12345
#define DEFAULT_SRC_IP "10.0.0.1"

// Global variables for worker threads
static struct rte_mempool *global_mbuf_pool = NULL;
static struct rte_ether_addr *global_src_macs = NULL;
static struct rte_ether_addr *global_tx_dst_macs = NULL; // per TX port
static struct in_addr *global_tx_dst_addrs = NULL; // per TX port
static struct in_addr *global_tx_src_addrs = NULL; // per TX port
static struct in_addr *global_rx_ip_addrs = NULL;  // per RX port
static uint16_t global_dest_port = DEFAULT_DST_PORT;
static uint16_t global_udp_src_port_base = DEFAULT_SRC_PORT;

static uint16_t *global_tx_port_ids = NULL;
static uint16_t global_num_tx_ports = 0;
// Note: In this generic logic, TX ports are assumed to be RX ports as well.
// We keep global_rx_port_ids for compatibility with common functions if needed,
// but for latency testing, Port X sends and Port X receives.
static uint16_t *global_rx_port_ids = NULL;
static uint16_t global_num_rx_ports = 0;

static volatile int global_running = 1;

static uint16_t global_payload_size = MAX_PAYLOAD_SIZE;
static uint32_t global_batch_size = DEFAULT_BATCH_SIZE;

// lcore role assignments
static uint16_t global_tx_lcores[RTE_MAX_LCORE];
static uint16_t global_tx_lcores_count = 0;

// Signal handler for graceful shutdown
static void signal_handler(int signum) {
    printf("\nReceived signal %d, shutting down...\n", signum);
    global_running = 0;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("  -p, --port PORT      Destination port (default: %d)\n", DEFAULT_DST_PORT);
    printf("  -s, --src-ip IP      Per-TX-port Source IP. IMPLIED: Also used as RX IP for ARP.\n");
    printf("  -d, --tx-dst-ip IP   Per-TX-port Destination IP.\n");
    printf("  -z, --size SIZE      Payload size in bytes (max: %d, min 6)\n", MAX_PAYLOAD_SIZE);
    printf("  -a, --device DEVICE  Device BDF. Implies both TX and RX.\n");
    printf("  -l, --lcores LCORES  Logical cores to use (e.g., 0-3, 0,2,4)\n");
    printf("  -f, --file-prefix P  DPDK file-prefix\n");
    printf("  --udp-src-port P     Base UDP source port (default: %d)\n", DEFAULT_SRC_PORT);
    printf("  --batch-size N       Batch size for ping-pong (default: %d)\n", DEFAULT_BATCH_SIZE);
    printf("  -M, --mbufs N        Number of mbufs in pool (default 65536)\n");
    printf("  -h, --help           Show this help\n");
}

static inline int get_tx_worker_index(void) {
    uint16_t lc = rte_lcore_id();
    for (uint16_t i = 0; i < global_tx_lcores_count; i++) {
        if (global_tx_lcores[i] == lc) return (int)i;
    }
    return 0;
}

static int fill_mbuf_with_packet(struct rte_mbuf *mbuf,
                                 struct in_addr inner_dst_addr,
                                 struct in_addr inner_src_addr,
                                 struct rte_ether_addr *src_mac,
                                 struct rte_ether_addr *dst_mac,
                                 uint16_t inner_dest_port,
                                 uint16_t inner_udp_src_port,
                                 uint64_t batch_id) {
    uint8_t *pkt = rte_pktmbuf_mtod(mbuf, uint8_t *);
    struct {
        struct rte_ether_hdr eth;
        struct rte_ipv4_hdr ip;
        struct rte_udp_hdr udp;
        uint8_t payload[MAX_PAYLOAD_SIZE];
    } __attribute__((__packed__, aligned(2))) *inner_frame;

    mbuf->ol_flags = 0;
    mbuf->l2_len = sizeof(struct rte_ether_hdr);
    mbuf->l3_len = sizeof(struct rte_ipv4_hdr);
    mbuf->l4_len = sizeof(struct rte_udp_hdr);
    mbuf->outer_l2_len = 0;
    mbuf->outer_l3_len = 0;

    uint32_t payload_size = global_payload_size;
    if (payload_size < 2) payload_size = 2; // Min 2 bytes for Batch ID

    uint32_t pkt_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                        sizeof(struct rte_udp_hdr) + payload_size;

    inner_frame = (void *)pkt;
    memset(inner_frame, 0, pkt_size);

    memcpy(inner_frame->eth.dst_addr.addr_bytes, dst_mac->addr_bytes, RTE_ETHER_ADDR_LEN);
    memcpy(inner_frame->eth.src_addr.addr_bytes, src_mac->addr_bytes, RTE_ETHER_ADDR_LEN);
    inner_frame->eth.ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    mbuf->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;

    inner_frame->ip.version_ihl = 0x45;
    inner_frame->ip.type_of_service = 0;
    inner_frame->ip.total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + payload_size);
    inner_frame->ip.time_to_live = 64;
    inner_frame->ip.next_proto_id = IPPROTO_UDP;
    inner_frame->ip.src_addr = inner_src_addr.s_addr;
    inner_frame->ip.dst_addr = inner_dst_addr.s_addr;
    inner_frame->ip.hdr_checksum = 0;

    inner_frame->udp.src_port = rte_cpu_to_be_16(inner_udp_src_port);
    inner_frame->udp.dst_port = rte_cpu_to_be_16(inner_dest_port);
    inner_frame->udp.dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + payload_size);
    inner_frame->udp.dgram_cksum = 0;

    // Embed 6-byte Hybrid Batch ID (Little-endian)
    inner_frame->payload[0] = (uint8_t)(batch_id & 0xFF);
    inner_frame->payload[1] = (uint8_t)((batch_id >> 8) & 0xFF);
    inner_frame->payload[2] = (uint8_t)((batch_id >> 16) & 0xFF);
    inner_frame->payload[3] = (uint8_t)((batch_id >> 24) & 0xFF);
    inner_frame->payload[4] = (uint8_t)((batch_id >> 32) & 0xFF);
    inner_frame->payload[5] = (uint8_t)((batch_id >> 40) & 0xFF);

    mbuf->data_len = pkt_size;
    mbuf->pkt_len = pkt_size;

    return 0;
}

// ...

static uint64_t read_batch_id_from_payload(const uint8_t *payload) {
    uint64_t id = 0;
    id |= (uint64_t)payload[0];
    id |= (uint64_t)payload[1] << 8;
    id |= (uint64_t)payload[2] << 16;
    id |= (uint64_t)payload[3] << 24;
    id |= (uint64_t)payload[4] << 32;
    id |= (uint64_t)payload[5] << 40;
    return id;
}

static int latency_worker(__rte_unused void *dummy) {
    uint16_t worker_idx = (uint16_t)get_tx_worker_index();
    uint16_t port_id = global_tx_port_ids ? global_tx_port_ids[worker_idx % (global_num_tx_ports ? global_num_tx_ports : 1)] : 0;

    printf("Latency Worker thread %u started on core %u using port %u\n",
           rte_lcore_id(), rte_get_next_lcore(-1, 1, 0), port_id);
    fflush(stdout);

    // Dynamic allocation for batch array to support large sizes if user overrides
    // But we use MAX_BATCH_SIZE for stack arrays?
    // Let's use flexible burst buffers
    #define MAX_BURST_SIZE 64
    struct rte_mbuf *mbufs[MAX_BURST_SIZE];
    struct rte_mbuf *rx_mbufs[MAX_BURST_SIZE];

    uint64_t *batch_history = calloc(BATCH_HISTORY_SIZE, sizeof(uint64_t));
    uint64_t *batch_history_ids = calloc(BATCH_HISTORY_SIZE, sizeof(uint64_t));
    if (!batch_history || !batch_history_ids) {
        printf("Worker %u: Failed to allocate history\n", rte_lcore_id());
        return -1;
    }

    uint64_t hz = rte_get_timer_hz();
    uint16_t queue_id = worker_idx;
    uint16_t current_batch_id = 0;

    if (global_tx_dst_addrs[port_id].s_addr == 0 || global_tx_src_addrs[port_id].s_addr == 0) {
        printf("Worker %u Warning: Missing Source or Dst IP for port %u. Packets might be malformed.\n", rte_lcore_id(), port_id);
    }

    // Stats
    double rtt_sum_us = 0;
    uint32_t batches_completed = 0; // Full batches
    uint32_t batches_partial = 0;
    uint32_t batches_empty = 0;
    uint32_t stale_packets = 0;     // Technically "mismatch" can now be "valid late packet"
    uint32_t late_packets = 0;      // Valid but from prev batch
    uint64_t total_sent = 0;
    uint64_t total_recv = 0;
    uint64_t last_report_time = rte_get_timer_cycles();
    uint64_t report_interval = hz;

    while (global_running) {
        uint32_t target_batch_size = global_batch_size;

        // Advance Batch ID
        current_batch_id++; // Wraps at 65535 naturally
        // Randomize UDP source port per batch as requested
        // Base + [0..10000] approx range
        uint16_t worker_udp_port = global_udp_src_port_base + (rte_rand() % 10000);

        // Generate 6-byte Hybrid Batch ID
        // Bytes 0-1: Counter (current_batch_current)
        // Bytes 2-5: Random
        uint64_t random_part = (uint64_t)rte_rand() & 0xFFFFFFFF;
        uint64_t full_batch_id = (current_batch_id & 0xFFFF) | (random_part << 16);


        // Split Batch: Head (N-1) and Tail (1)
        uint32_t head_size = (target_batch_size > 0) ? target_batch_size - 1 : 0;
        uint32_t tail_size = (target_batch_size > 0) ? 1 : 0;

        // 1. Send Head
        uint32_t packets_sent_head = 0;
        while (packets_sent_head < head_size && global_running) {
             uint32_t burst = head_size - packets_sent_head;
             if (burst > MAX_BURST_SIZE) burst = MAX_BURST_SIZE;

             if (rte_pktmbuf_alloc_bulk(global_mbuf_pool, mbufs, burst) != 0) {
                 rte_pause();
                 continue;
             }

             for (uint32_t i = 0; i < burst; i++) {
                 fill_mbuf_with_packet(mbufs[i],
                                  global_tx_dst_addrs[port_id],
                                  global_tx_src_addrs[port_id],
                                  &global_src_macs[port_id],
                                  &global_tx_dst_macs[port_id],
                                  global_dest_port,
                                  worker_udp_port,
                                  full_batch_id);
             }

             uint16_t nb_tx = rte_eth_tx_burst(port_id, queue_id, mbufs, burst);
             if (nb_tx < burst) {
                 for (uint16_t i = nb_tx; i < burst; i++) rte_pktmbuf_free(mbufs[i]);
             }

             packets_sent_head += nb_tx;
             total_sent += nb_tx; // Count head packets

             if (nb_tx == 0) rte_delay_us_block(10);
        }

        // 2. Pause
        // User requested "rte_pause some time". Let's do a busy loop of pauses.
        // Approx delay? Let's just do a fixed number of pauses to separate them.
        for (int k = 0; k < 20000; k++) rte_pause();

        // 3. Timestamp (NOW, before sending tail)
        batch_history_ids[current_batch_id & 0xFFFF] = full_batch_id;

        // 4. Send Tail (Last Packet)
        uint32_t packets_sent_tail = 0;
        while (packets_sent_tail < tail_size && global_running) {
             // We only have 1 packet to send usually
             struct rte_mbuf *mbuf = rte_pktmbuf_alloc(global_mbuf_pool);
             if (mbuf == NULL) {
                 rte_pause();
                 continue;
             }
             fill_mbuf_with_packet(mbuf,
                              global_tx_dst_addrs[port_id],
                              global_tx_src_addrs[port_id],
                              &global_src_macs[port_id],
                              &global_tx_dst_macs[port_id],
                              global_dest_port,
                              worker_udp_port,
                              full_batch_id);


             batch_history[current_batch_id & 0xFFFF] = rte_rdtsc();
             uint16_t nb_tx = rte_eth_tx_burst(port_id, queue_id, &mbuf, 1);
             if (nb_tx < 1) {
                 rte_pktmbuf_free(mbuf);
                 rte_delay_us_block(10);
                 continue;
             }
             packets_sent_tail += nb_tx;
             total_sent += nb_tx;
        }

        uint32_t packets_sent_this_batch = packets_sent_head + packets_sent_tail;

        // Receive Loop
        uint32_t packets_received_this_cycle = 0; // Packets matching CURRENT batch
        uint64_t min_rtt = UINT64_MAX;
        uint64_t start_wait = rte_rdtsc();
        uint64_t timeout_cycles = hz / 500; // 2ms timeout

        // We wait for expected number of packets for THIS batch.
        // But we might receive packets from previous batches.

        while (packets_received_this_cycle < packets_sent_this_batch &&
               (rte_rdtsc() - start_wait < timeout_cycles) && global_running) {

            uint16_t nb_rx = rte_eth_rx_burst(port_id, queue_id, rx_mbufs, MAX_BURST_SIZE);
            for (uint16_t i = 0; i < nb_rx; i++) {
                struct rte_mbuf *m = rx_mbufs[i];
                struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

                if (eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
                     dpdk_handle_arp_request(port_id, m, global_src_macs, global_rx_ip_addrs, queue_id);
                     continue;
                }

                int processed = 0;
                if (eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
                     struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
                     if (ip->next_proto_id == IPPROTO_UDP) {
                         int ip_hdr_len = (ip->version_ihl & 0xF) * 4;
                         struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((uint8_t *)ip + ip_hdr_len);
                         uint8_t *payload = (uint8_t *)(udp + 1);

                         // Validation: Payload size >= 2
                          if (rte_pktmbuf_pkt_len(m) >= sizeof(struct rte_ether_hdr) + ip_hdr_len + sizeof(struct rte_udp_hdr) + 6) {
                              uint64_t rx_id = read_batch_id_from_payload(payload);
                              uint16_t rx_idx = (uint16_t)(rx_id & 0xFFFF);
                              uint64_t send_ts = batch_history[rx_idx];

                              // Verify full ID matches history for this index
                              if (send_ts != 0 && batch_history_ids[rx_idx] == rx_id) {
                                  uint64_t t_now = rte_rdtsc();
                                  uint64_t rtt_cycles = t_now - send_ts;
                                  // Check validity (under 1s)
                                  if (rtt_cycles < hz) {
                                      uint64_t rtt_val = rtt_cycles;
                                      if (rx_id == full_batch_id) {
                                          if (rtt_val < min_rtt) min_rtt = rtt_val;
                                          packets_received_this_cycle++;
                                      } else {
                                          late_packets++;
                                      }

                                      processed = 1;
                                  } else {
                                      // Too old
                                      stale_packets++;
                                  }
                              } else {
                                  // Unknown ID or cleared history or mismatch
                                  stale_packets++;
                              }
                          }
                      }
                 }
                 rte_pktmbuf_free(m);
            }
            total_recv += nb_rx;
        }

        if (packets_received_this_cycle > 0) {
            double rtt_us = (double)min_rtt * 1000000.0 / hz;
            rtt_sum_us += rtt_us;
            if (packets_received_this_cycle >= packets_sent_this_batch) batches_completed++;
            else batches_partial++;
        } else {
            batches_empty++;
        }

        for (int k = 0; k < 20000; k++) rte_pause();

        // Report Interval
        if (rte_rdtsc() - last_report_time > report_interval) {
            uint32_t total_batches = batches_completed + batches_partial;
            if (total_batches > 0) {
                 double avg_min_rtt = rtt_sum_us / total_batches;
                 printf("Core %u [%u Batches (F:%u P:%u E:%u)]: Avg Min RTT: %.2f us, Pkts Sent/Recv: %lu/%lu, Late: %u, Stale: %u\n",
                        rte_lcore_id(), total_batches, batches_completed, batches_partial, batches_empty,
                        avg_min_rtt, total_sent, total_recv, late_packets, stale_packets);
            } else {
                 printf("Core %u [0 Batches]: Empty: %u, Stale: %u\n",
                        rte_lcore_id(), batches_empty, stale_packets);
            }
            fflush(stdout);

            // Reset
            rtt_sum_us = 0;
            batches_completed = 0;
            batches_partial = 0;
            batches_empty = 0;
            total_sent = 0;
            total_recv = 0;
            stale_packets = 0;
            late_packets = 0;
            last_report_time = rte_get_timer_cycles();
        }

        rte_delay_us_block(100);
    }
    free(batch_history);
    free(batch_history_ids);
    return 0;
}

int main(int argc, char **argv) {
    int ret, opt;
    const char *per_tx_src_ips[64]; memset(per_tx_src_ips, 0, sizeof(per_tx_src_ips)); uint16_t per_tx_src_count = 0;
    const char *per_tx_dst_ips[64]; memset(per_tx_dst_ips, 0, sizeof(per_tx_dst_ips)); uint16_t per_tx_dst_count = 0;
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"src-ip", required_argument, 0, 's'},
        {"tx-dst-ip", required_argument, 0, 'd'},
        {"size", required_argument, 0, 'z'},
        {"device", required_argument, 0, 'a'},
        {"lcores", required_argument, 0, 'l'},
        {"file-prefix", required_argument, 0, 'f'},
        {"udp-src-port", required_argument, 0, 3},
        {"batch-size", required_argument, 0, 4},
        {"mbufs", required_argument, 0, 'M'},
        {0, 0, 0, 0}
    };

    char *eal_argv[argc + 128];
    int eal_argc = 0;
    eal_argv[eal_argc++] = argv[0];

    const char *file_prefix = NULL;
    const char *tx_devices[64]; memset(tx_devices, 0, sizeof(tx_devices));
    uint16_t tx_device_count = 0;
    const char *tx_lcores_str = NULL;

    while ((opt = getopt_long(argc, argv, "hp:s:d:z:a:l:f:R:M:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h': print_usage(argv[0]); return 0;
            case 'p': global_dest_port = atoi(optarg); break;
            case 's':
                if (per_tx_src_count < 64) per_tx_src_ips[per_tx_src_count++] = optarg;
                break;
            case 'd':
                if (per_tx_dst_count < 64) per_tx_dst_ips[per_tx_dst_count++] = optarg;
                break;
            case 'z':
                global_payload_size = atoi(optarg);
                if (global_payload_size > MAX_PAYLOAD_SIZE) global_payload_size = MAX_PAYLOAD_SIZE;
                break;
            case 'a':
                if (tx_device_count < 64) tx_devices[tx_device_count++] = optarg;
                break;
            case 'l':
                tx_lcores_str = optarg;
                break;
            // Ignore -R arguments gracefully
            case 'R':
            case 1: // --rx-ip
                // silently ignore
                break;
            case 'f':
                file_prefix = optarg;
                break;
            case 'M': break;
            case 3:
                global_udp_src_port_base = (uint16_t)atoi(optarg);
                break;
            case 4:
                global_batch_size = (uint32_t)atoi(optarg);
                break;
            default: print_usage(argv[0]); return 1;
        }
    }

    // Build EAL arguments: pass tx_devices as -a
    eal_argc = dpdk_build_eal_argv(argv[0], tx_devices, tx_device_count,
		NULL, 0, tx_lcores_str, NULL,
		file_prefix, eal_argv, argc + 128);

    if (eal_argc < 0) {
        fprintf(stderr, "Failed to build EAL arguments\n");
        return 1;
    }

    printf("EAL arguments (%d): ", eal_argc);
    for (int i = 0; i < eal_argc; i++) printf("%s ", eal_argv[i]);
    printf("\n");
    fflush(stdout);

    ret = rte_eal_init(eal_argc, eal_argv);
    if (ret < 0) {
        fprintf(stderr, "Failed to init DPDK EAL\n");
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    uint16_t num_ports = rte_eth_dev_count_avail();
    if (num_ports == 0) {
        fprintf(stderr, "No DPDK ports available.\n");
        return 1;
    }

    if (tx_device_count == 0) {
        global_num_tx_ports = num_ports;
        global_tx_port_ids = malloc(global_num_tx_ports * sizeof(uint16_t));
        for (uint16_t i = 0; i < num_ports; i++) global_tx_port_ids[i] = i;
    } else {
        uint16_t matched[64];
        uint16_t m = 0;
        dpdk_match_ports_by_bdf(tx_devices, tx_device_count, matched, &m);
        global_num_tx_ports = m;
        global_tx_port_ids = malloc(global_num_tx_ports * sizeof(uint16_t));
        for (uint16_t i = 0; i < m; i++) global_tx_port_ids[i] = matched[i];
    }

    // For Latency Config: RX Ports = TX Ports
    global_num_rx_ports = global_num_tx_ports;
    global_rx_port_ids = global_tx_port_ids; // Alias

    // Lcores
    if (tx_lcores_str) {
        dpdk_parse_lcore_set(tx_lcores_str, global_tx_lcores, &global_tx_lcores_count);
    } else {
         unsigned int lc = rte_get_next_lcore(-1, 1, 0);
         if (lc < RTE_MAX_LCORE && lc != rte_get_main_lcore()) {
             global_tx_lcores[global_tx_lcores_count++] = (uint16_t)lc;
         }
    }

    if (global_tx_lcores_count == 0) {
        fprintf(stderr, "No worker lcores available. Please use -l.\n");
        return 1;
    }

    // Filter out main lcore from worker list
    uint16_t main_lcore = rte_get_main_lcore();
    uint16_t filtered_count = 0;
    for (uint16_t i = 0; i < global_tx_lcores_count; i++) {
        if (global_tx_lcores[i] != main_lcore) {
            global_tx_lcores[filtered_count++] = global_tx_lcores[i];
        } else {
            printf("Notice: Core %u is main lcore, excluding from worker pool.\n", global_tx_lcores[i]);
        }
    }
    global_tx_lcores_count = filtered_count;

    if (global_tx_lcores_count == 0) {
        fprintf(stderr, "No worker lcores available after filtering main lcore.\n");
        return 1;
    }

    struct rte_mempool *mbuf_pool = dpdk_create_mbuf_pool("MBUF_POOL", 65536, 512);
    if (!mbuf_pool) return 1;
    global_mbuf_pool = mbuf_pool;

    global_tx_src_addrs = calloc(num_ports, sizeof(struct in_addr));
    global_tx_dst_addrs = calloc(num_ports, sizeof(struct in_addr));
    global_tx_dst_macs = calloc(num_ports, sizeof(struct rte_ether_addr));
    global_rx_ip_addrs = calloc(num_ports, sizeof(struct in_addr));

    for (uint16_t i = 0; i < global_num_tx_ports; i++) {
        uint16_t pid = global_tx_port_ids[i];
        if (i < per_tx_src_count) {
             inet_aton(per_tx_src_ips[i], &global_tx_src_addrs[pid]);
             // Use same IP for RX for ARP
             global_rx_ip_addrs[pid] = global_tx_src_addrs[pid];
        }
        if (i < per_tx_dst_count) inet_aton(per_tx_dst_ips[i], &global_tx_dst_addrs[pid]);
    }

    // Configure ports with N queues, where N = number of workers.
    // This allows each worker to use a dedicated queue (queue_id = worker_idx).
    uint16_t queues_per_port = global_tx_lcores_count;
    if (queues_per_port < 1) queues_per_port = 1;

    dpdk_configure_ports(num_ports, global_rx_port_ids, global_num_rx_ports, queues_per_port,
                         global_tx_port_ids, global_num_tx_ports, queues_per_port,
                         mbuf_pool, &global_src_macs);

    // Resolve ARP (Main thread uses Queue 0 temporarily - hopefully safe before workers start)
    for (uint16_t i = 0; i < global_num_tx_ports; i++) {
         uint16_t pid = global_tx_port_ids[i];
         if (global_tx_dst_addrs[pid].s_addr != 0 && global_tx_src_addrs[pid].s_addr != 0) {
             printf("Resolving ARP for port %u...\n", pid);
             fflush(stdout);
             int arp_rc = dpdk_resolve_arp_for_port(pid, &global_src_macs[pid],
                                       global_tx_src_addrs[pid], global_tx_dst_addrs[pid],
                                       &global_tx_dst_macs[pid], mbuf_pool, 1000);
             if (arp_rc == 0) {
                 printf("Resolved MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                        global_tx_dst_macs[pid].addr_bytes[0], global_tx_dst_macs[pid].addr_bytes[1],
                        global_tx_dst_macs[pid].addr_bytes[2], global_tx_dst_macs[pid].addr_bytes[3],
                        global_tx_dst_macs[pid].addr_bytes[4], global_tx_dst_macs[pid].addr_bytes[5]);
             } else {
                 printf("Failed to resolve ARP for port %u. Sending blindly/using zero MAC.\n", pid);
             }
             fflush(stdout);
         }
    }

    // Launch Workers
    for (uint16_t i = 0; i < global_tx_lcores_count; i++) {
        if (rte_eal_remote_launch(latency_worker, NULL, global_tx_lcores[i]) != 0) {
             fprintf(stderr, "Error: Failed to launch worker on core %u\n", global_tx_lcores[i]);
        }
    }

    rte_eal_mp_wait_lcore();
    return 0;
}
