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
#include <rte_vxlan.h>
#include <signal.h>
#include "dpdk_common.h"
#include "generic/rte_cycles.h"
#ifdef ARM_HOST_DOCA
void arm_host_probe(void);
void arm_host_flow_start(struct rte_mempool *pool);
void arm_host_flow_stop(void);
#endif

#define MAX_PAYLOAD_SIZE 1472  // Reasonable max to stay under MTU
#define VXLAN_OUTER_HEADERS_LEN (sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(struct rte_vxlan_hdr))
#define INNER_PACKET_HEADERS_LEN (sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr))
#define MAX_PACKET_SIZE (VXLAN_OUTER_HEADERS_LEN + INNER_PACKET_HEADERS_LEN + MAX_PAYLOAD_SIZE)
#define BATCH_SIZE 64
#define STATS_INTERVAL 1
#define PARTIAL_DECRYPTION_TRAFFIC_PORT 3282
#define DEFAULT_SRC_PORT 12345
#define DEFAULT_SRC_IP "10.0.0.1"
#define DEFAULT_PAUSE_CALLS 0  // No throttling by default

// Global variables for worker threads
static struct rte_mempool *global_mbuf_pool = NULL;
static struct rte_ether_addr *global_src_macs = NULL;
static struct rte_ether_addr *global_tx_dst_macs = NULL; // per TX port (provided via CLI)
static struct in_addr *global_tx_dst_addrs = NULL; // per TX port
static struct in_addr *global_tx_src_addrs = NULL; // per TX port
static struct in_addr *global_rx_ip_addrs = NULL;  // per RX port
static uint16_t global_dest_port;
// VXLAN per-port outer destination (from -v IP:port)
static struct in_addr *global_vxlan_dst_addrs = NULL; // per TX port
static uint16_t *global_vxlan_udp_ports = NULL;       // per TX port
// legacy TX list removed
static uint16_t *global_tx_port_ids = NULL;
static uint16_t global_num_tx_ports = 0;
static uint16_t *global_rx_port_ids = NULL;
static uint16_t global_num_rx_ports = 0;
static volatile int global_running = 1;
static volatile int global_aux_polling_enabled = 0;
// legacy num_queues removed
static uint16_t tx_queues_per_port = 0;
static uint16_t rx_queues_per_port = 0;
static uint16_t global_payload_size = MAX_PAYLOAD_SIZE;
static uint32_t global_pause_calls = DEFAULT_PAUSE_CALLS;  // Number of rte_pause() calls per loop
static int global_show_per_port_stats = 0;

// Reflector device globals
// Reflector device globals
static const char *global_reflector_devices[64];
static uint16_t global_reflector_device_count = 0;
static uint16_t global_reflector_port_ids[64];
static int global_reflector_enabled = 0;

// lcore role assignments
static uint16_t global_tx_lcores[RTE_MAX_LCORE];
static uint16_t global_tx_lcores_count = 0;
static uint16_t global_rx_lcores[RTE_MAX_LCORE];
static uint16_t global_rx_lcores_count = 0;
static struct dpdk_shared_rx_worker_ctx global_rx_worker_ctx[RTE_MAX_LCORE];

static struct rte_eth_stats g_port_stats_cache[RTE_MAX_ETHPORTS];

// Signal handler for graceful shutdown
static void signal_handler(int signum) {
    printf("\nReceived signal %d, shutting down...\n", signum);
    global_running = 0;
}

static int install_flow_all_to_queue0(uint16_t port_id, uint16_t queue_id) {
    struct rte_flow_attr attr; memset(&attr, 0, sizeof(attr));
    attr.ingress = 1;
    
    struct rte_flow_item pattern[2]; memset(pattern, 0, sizeof(pattern));
    struct rte_flow_item_eth eth_spec; memset(&eth_spec, 0, sizeof(eth_spec));
    struct rte_flow_item_eth eth_mask; memset(&eth_mask, 0, sizeof(eth_mask));
    
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[0].spec = &eth_spec;
    pattern[0].mask = &eth_mask;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_END;
    
    struct rte_flow_action actions[2]; memset(actions, 0, sizeof(actions));
    struct rte_flow_action_queue queue_conf;
    queue_conf.index = queue_id;
    
    actions[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    actions[0].conf = &queue_conf;
    actions[1].type = RTE_FLOW_ACTION_TYPE_END;
    
    struct rte_flow_error error;
    struct rte_flow *flow = rte_flow_create(port_id, &attr, pattern, actions, &error);
    if (!flow) {
        fprintf(stderr, "Failed to create flow: %s\n", error.message);
        return -1;
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
                                 int use_vxlan,
                                 struct in_addr outer_dst_addr,
                                 uint16_t outer_udp_dest_port) {
    uint8_t *pkt = rte_pktmbuf_mtod(mbuf, uint8_t *);
    struct {
        struct rte_ether_hdr eth;
        struct rte_ipv4_hdr ip;
        struct rte_udp_hdr udp;
        uint8_t payload[MAX_PAYLOAD_SIZE];
    } __attribute__((__packed__, aligned(2))) *inner_frame;

    memset(pkt, 0, MAX_PACKET_SIZE);
    mbuf->ol_flags = 0;
    mbuf->l2_len = sizeof(struct rte_ether_hdr);
    mbuf->l3_len = sizeof(struct rte_ipv4_hdr);
    mbuf->l4_len = sizeof(struct rte_udp_hdr);
    mbuf->outer_l2_len = 0;
    mbuf->outer_l3_len = 0;

    uint32_t payload_size = global_payload_size;
    uint16_t base_packet_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                               sizeof(struct rte_udp_hdr) + payload_size;
    uint32_t pkt_size = 0;

    if (use_vxlan) {
        struct rte_ether_hdr *outer_eth = (struct rte_ether_hdr *)pkt;
        memcpy(outer_eth->dst_addr.addr_bytes, dst_mac->addr_bytes, RTE_ETHER_ADDR_LEN);
        memcpy(outer_eth->src_addr.addr_bytes, src_mac->addr_bytes, RTE_ETHER_ADDR_LEN);
        outer_eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

        struct rte_ipv4_hdr *outer_ip = (struct rte_ipv4_hdr *)(outer_eth + 1);
        outer_ip->version_ihl = 0x45;
        outer_ip->type_of_service = 0;
        outer_ip->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
                                                  sizeof(struct rte_udp_hdr) +
                                                  sizeof(struct rte_vxlan_hdr) +
                                                  base_packet_len);
        outer_ip->packet_id = 0;
        outer_ip->fragment_offset = 0;
        outer_ip->time_to_live = 64;
        outer_ip->next_proto_id = IPPROTO_UDP;
        outer_ip->src_addr = inner_src_addr.s_addr;
        outer_ip->dst_addr = (outer_dst_addr.s_addr != 0) ? outer_dst_addr.s_addr : inner_dst_addr.s_addr;
        outer_ip->hdr_checksum = 0;

        struct rte_udp_hdr *outer_udp = (struct rte_udp_hdr *)(outer_ip + 1);
        outer_udp->src_port = htons(inner_udp_src_port);
        outer_udp->dst_port = rte_cpu_to_be_16(outer_udp_dest_port);
        outer_udp->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) +
                                                sizeof(struct rte_vxlan_hdr) +
                                                base_packet_len);
        outer_udp->dgram_cksum = 0;

        struct rte_vxlan_hdr *vxlan_hdr = (struct rte_vxlan_hdr *)(outer_udp + 1);
        memset(vxlan_hdr, 0, sizeof(*vxlan_hdr));
        vxlan_hdr->flags = 0x08; // I flag set, VNI=0 by default

        inner_frame = (void *)(vxlan_hdr + 1);
        pkt_size = VXLAN_OUTER_HEADERS_LEN + base_packet_len;

        mbuf->ol_flags |= RTE_MBUF_F_TX_OUTER_IPV4 | RTE_MBUF_F_TX_OUTER_IP_CKSUM | RTE_MBUF_F_TX_TUNNEL_VXLAN;
        mbuf->outer_l2_len = sizeof(struct rte_ether_hdr);
        mbuf->outer_l3_len = sizeof(struct rte_ipv4_hdr);
    } else {
        inner_frame = (void *)pkt;
        pkt_size = base_packet_len;
        memcpy(inner_frame->eth.dst_addr.addr_bytes, dst_mac->addr_bytes, RTE_ETHER_ADDR_LEN);
        memcpy(inner_frame->eth.src_addr.addr_bytes, src_mac->addr_bytes, RTE_ETHER_ADDR_LEN);
        inner_frame->eth.ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    }

    mbuf->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;

    if (use_vxlan) {
        // Inner Ethernet header (re-use src/dst MACs)
        memcpy(inner_frame->eth.dst_addr.addr_bytes, dst_mac->addr_bytes, RTE_ETHER_ADDR_LEN);
        memcpy(inner_frame->eth.src_addr.addr_bytes, src_mac->addr_bytes, RTE_ETHER_ADDR_LEN);
        inner_frame->eth.ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    }

    inner_frame->ip.version_ihl = 0x45;
    inner_frame->ip.type_of_service = 0;
    inner_frame->ip.total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + payload_size);
    inner_frame->ip.packet_id = 0;
    inner_frame->ip.fragment_offset = 0;
    inner_frame->ip.time_to_live = 64;
    inner_frame->ip.next_proto_id = IPPROTO_UDP;
    inner_frame->ip.src_addr = inner_src_addr.s_addr;
    inner_frame->ip.dst_addr = inner_dst_addr.s_addr;
    inner_frame->ip.hdr_checksum = 0;

    inner_frame->udp.src_port = rte_cpu_to_be_16(inner_udp_src_port);
    inner_frame->udp.dst_port = rte_cpu_to_be_16(inner_dest_port);
    inner_frame->udp.dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + payload_size);
    inner_frame->udp.dgram_cksum = 0; // Leave inner checksum disabled

    mbuf->data_len = pkt_size;
    mbuf->pkt_len = pkt_size;

    return 0;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("  -p, --port PORT      Destination port (default: %d)\n", PARTIAL_DECRYPTION_TRAFFIC_PORT);
    printf("  -s, --src-ip IP      Per-TX-port Source IP. \n");
    printf("  -d, --tx-dst-ip IP   Per-TX-port Destination IP.\n");
    printf("  -D, --dst-mac MAC    Per-TX-port Destination MAC (bypass ARP).\n");
    printf("  -z, --size SIZE      Payload size in bytes (max: %d)\n", MAX_PAYLOAD_SIZE);
    printf("  -a, --device DEVICE  Device to use (BDF or Auxiliary name, can specify multiple times)\n");
    printf("  -l, --lcores LCORES  Logical cores to use (e.g., 0-3, 0,2,4)\n");
    printf("  -t, --throttle PAUSE Number of rte_pause() calls per loop for rate control (default: %d)\n", DEFAULT_PAUSE_CALLS);
    printf("  -f, --file-prefix P  DPDK file-prefix\n");
    printf("  -R, --rx-device DEV  Receiving device (BDF or Auxiliary name, repeat to add multiple)\n");
    printf("  -L, --rx-lcores SET  Receiving lcores (e.g., 16-18,20). Subset of -l.\n");
    printf("      --rx-ip IP       Per-RX-port IP (repeat in --rx-device order).\n");
    printf("      --reflector-device DEV  Device to use as a reflector (swaps MAC/IP and reflects)\n");
    printf("  -v, --vxlan IP:PORT  Per-TX-port VXLAN outer dst (repeat per -a)\n");
    printf("  -V IP:PORT           Per-RX-port VXLAN decap dst (repeat per --rx-device)\n");
    printf("  -M, --mbufs N        Number of mbufs in pool (default 65536)\n");
    printf("  -S, --per-port-stats Show per-port statistics (default: totals only)\n");
    printf("  -h, --help           Show this help\n");
    printf("\nThis generator creates fixed-size packets.\n");
    printf("Destination IPs must be provided via -d/--tx-dst-ip (repeat per TX port as needed).\n");
    printf("Throttling: Use -t option to control sending rate. Higher values = lower rate.\n");
    printf("Example: -t 1000 for moderate throttling, -t 10000 for heavy throttling.\n");
}

static inline int lcore_in_list(uint16_t lc, const uint16_t *list, uint16_t count) {
    for (uint16_t i = 0; i < count; i++) if (list[i] == lc) return 1;
    return 0;
}

// index_in_lcore_list removed


// BDF matching helpers moved to dpdk_common

static inline uint16_t ceil_div(uint16_t a, uint16_t b) {
    return (uint16_t)((a + b - 1) / b);
}

static void reset_port_stats_cache(void) {
    dpdk_stats_cache_reset(g_port_stats_cache, RTE_MAX_ETHPORTS);
}

static void dpdk_build_arp_request(struct rte_mbuf *m, const struct rte_ether_addr *src_mac, struct in_addr src_ip, struct in_addr dst_ip) {
    uint8_t *pkt = rte_pktmbuf_mtod(m, uint8_t *);
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)pkt;
    struct arp_ipv4_payload *arp = (struct arp_ipv4_payload *)(eth + 1);
    memset(eth, 0, sizeof(*eth));
    memset(arp, 0, sizeof(*arp));
    memset(eth->dst_addr.addr_bytes, 0xff, 6);
    memcpy(eth->src_addr.addr_bytes, src_mac->addr_bytes, 6);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

    arp->hrd = rte_cpu_to_be_16(ARP_HRD_ETHER);
    arp->pro = rte_cpu_to_be_16(ARP_PRO_IPV4);
    arp->hln = RTE_ETHER_ADDR_LEN;
    arp->pln = sizeof(uint32_t);
    arp->op  = rte_cpu_to_be_16(ARP_OP_REQUEST);
    memcpy(&arp->sha, src_mac, sizeof(struct rte_ether_addr));
    arp->sip = src_ip.s_addr;
    memset(&arp->tha, 0x00, sizeof(struct rte_ether_addr));
    arp->tip = dst_ip.s_addr;

    m->data_len = sizeof(*eth) + sizeof(*arp);
    m->pkt_len = m->data_len;
}

// Poll all configured RX ports once and answer ARP requests for their configured IPs.
// (Deleted service_rx_arp_requests as it is now handled by shared RX workers or dead if do_rx=false)

static int resolve_arp_for_port(uint16_t port_id, const struct rte_ether_addr *src_mac,
                                struct in_addr src_ip, struct in_addr dst_ip,
                                struct rte_ether_addr *out_dst_mac,
                                struct rte_mempool *mbuf_pool,
                                uint32_t timeout_ms) {
    struct rte_mbuf *m = rte_pktmbuf_alloc(mbuf_pool);
    if (!m) return -1;
    
    dpdk_build_arp_request(m, src_mac, src_ip, dst_ip);

    int tx_ret = rte_eth_tx_burst(port_id, 0, &m, 1);
    if (tx_ret != 1) {
        fprintf(stderr, "Failed to send ARP request on port %u (ret=%d)\n", port_id, tx_ret);
        rte_pktmbuf_free(m);
        return -1;
    }

    const uint64_t start = rte_get_timer_cycles();
    const uint64_t hz = rte_get_timer_hz();
    struct rte_mbuf *rx[8];
    while (((rte_get_timer_cycles() - start) * 1000 / hz) < timeout_ms) {
        // 1) Check for ARP reply on the TX port we're resolving for
        const uint16_t n_rx = rte_eth_rx_burst(port_id, 0, rx, 8);
        for (uint16_t i = 0; i < n_rx; i++) {
            struct rte_mbuf *rm = rx[i];
            struct rte_ether_hdr *reth = rte_pktmbuf_mtod(rm, struct rte_ether_hdr *);
            if (reth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
                struct arp_ipv4_payload *r = (struct arp_ipv4_payload *)(reth + 1);
                if (r->op == rte_cpu_to_be_16(ARP_OP_REPLY) && r->sip == dst_ip.s_addr && r->tip == src_ip.s_addr) {
                    memcpy(out_dst_mac, &r->sha, sizeof(struct rte_ether_addr));
                    rte_pktmbuf_free(rm);
                    for (uint16_t j = i + 1; j < n_rx; j++) rte_pktmbuf_free(rx[j]);
                    return 0;
                }
            }
            rte_pktmbuf_free(rm);
        }
    }
    return -2;
}

static inline int get_tx_worker_index(void) {
    uint16_t lc = rte_lcore_id();
    for (uint16_t i = 0; i < global_tx_lcores_count; i++) {
        if (global_tx_lcores[i] == lc) return (int)i;
    }
    return 0;
}

// Worker function for packet generation
static int packet_worker(__rte_unused void *dummy) {
    uint16_t worker_idx = (uint16_t)get_tx_worker_index();
    uint16_t port_id = global_tx_port_ids ? global_tx_port_ids[worker_idx % (global_num_tx_ports ? global_num_tx_ports : 1)] : 0;
    uint16_t port_queue_id = (global_num_tx_ports ? (worker_idx / global_num_tx_ports) : 0);
    
    printf("Worker thread %u started on core %u using port %u queue %u\n", 
           rte_lcore_id(), rte_get_next_lcore(-1, 1, 0), port_id, port_queue_id);
    
    // Maintain a small pool of ready-to-send mbufs across iterations
    struct rte_mbuf *pending[BATCH_SIZE];
    uint16_t pending_count = 0;
    while (global_running) {
        // Refill pending up to BATCH_SIZE
        while (pending_count < BATCH_SIZE) {
            struct rte_mbuf *m = rte_pktmbuf_alloc(global_mbuf_pool);
            if (!m) break;
            struct in_addr src_ip_i = global_tx_src_addrs[port_id];
            struct in_addr dst_ip_i = global_tx_dst_addrs[port_id];
            int use_vxlan = (global_vxlan_dst_addrs &&
                             global_vxlan_udp_ports &&
                             global_vxlan_dst_addrs[port_id].s_addr != 0 &&
                             global_vxlan_udp_ports[port_id] != 0);
            struct in_addr outer_dst = use_vxlan ? global_vxlan_dst_addrs[port_id] : dst_ip_i;
            uint16_t outer_udp_port = use_vxlan ? global_vxlan_udp_ports[port_id] : global_dest_port;
            uint16_t udp_src_port = (uint16_t)(DEFAULT_SRC_PORT + port_queue_id);
            if (fill_mbuf_with_packet(m, dst_ip_i, src_ip_i,
                                      &global_src_macs[port_id], &global_tx_dst_macs[port_id], global_dest_port,
                                      udp_src_port, use_vxlan, outer_dst, outer_udp_port) != 0) {
                rte_pktmbuf_free(m);
                break;
            }
            pending[pending_count] = m;
            pending_count++;
        }

        // Transmit pending mbufs
        uint16_t sent = 0;
        if (pending_count > 0) {
            sent = rte_eth_tx_burst(port_id, port_queue_id, pending, pending_count);
            pending_count -= sent;
            if (sent == 0) rte_pause();
        }
        
        if (pending_count && sent)
            memmove(pending, pending + sent, pending_count * sizeof(pending[0]));
        // Throttling using rte_pause() calls
        for (uint32_t i = 0; i < global_pause_calls; i++) {
            rte_pause();
        }
        
    }
    // Drain any remaining pending mbufs on exit
    for (uint16_t i = 0; i < pending_count; i++) rte_pktmbuf_free(pending[i]);
    return 0;
}

int main(int argc, char **argv) {
    int ret, opt, dest_port = PARTIAL_DECRYPTION_TRAFFIC_PORT;
    // Per-TX device overrides
    const char *per_tx_src_ips[64]; memset(per_tx_src_ips, 0, sizeof(per_tx_src_ips)); uint16_t per_tx_src_count = 0;
    const char *per_tx_dst_ips[64]; memset(per_tx_dst_ips, 0, sizeof(per_tx_dst_ips)); uint16_t per_tx_dst_count = 0;
    const char *per_tx_dst_macs[64]; memset(per_tx_dst_macs, 0, sizeof(per_tx_dst_macs)); uint16_t per_tx_dst_mac_count = 0;
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"src-ip", required_argument, 0, 's'},
        {"tx-dst-ip", required_argument, 0, 'd'},
        {"dst-mac", required_argument, 0, 'D'},
        {"size", required_argument, 0, 'z'},
        {"device", required_argument, 0, 'a'},
        {"lcores", required_argument, 0, 'l'},
        {"throttle", required_argument, 0, 't'},
        {"file-prefix", required_argument, 0, 'f'},
        {"rx-device", required_argument, 0, 'R'},
        {"rx-lcores", required_argument, 0, 'L'},
        {"rx-ip", required_argument, 0, 1},
        {"reflector-device", required_argument, 0, 2},
        {"vxlan", required_argument, 0, 'v'},
        {"vxlan-decap", required_argument, 0, 'V'},
        {"mbufs", required_argument, 0, 'M'},
        {"per-port-stats", no_argument, 0, 'S'},
        {0, 0, 0, 0}
    };
    
    // Collect DPDK arguments and app arguments
    char *eal_argv[argc + 128];
    int eal_argc = 0;
    eal_argv[eal_argc++] = argv[0]; // Program name

    const char *file_prefix = NULL;
    // We will build EAL -a list from both TX (-a) and RX (-R) devices
    const char *tx_devices[64]; memset(tx_devices, 0, sizeof(tx_devices));
    uint16_t tx_device_count = 0;
    const char *rx_devices[64]; memset(rx_devices, 0, sizeof(rx_devices));
    uint16_t rx_device_count = 0;
    const char *rx_ip_strs[64]; memset(rx_ip_strs, 0, sizeof(rx_ip_strs));
    uint16_t rx_ip_count = 0;
    // TX lcores string and RX lcores string (to be merged for EAL -l)
    const char *tx_lcores_str = NULL;
    const char *rx_lcores_str = NULL;
    
    // Parse our application arguments first
    const char *per_tx_vxlan[64]; memset(per_tx_vxlan, 0, sizeof(per_tx_vxlan));
    const char *per_rx_vxlan[64]; memset(per_rx_vxlan, 0, sizeof(per_rx_vxlan));
    while ((opt = getopt_long(argc, argv, "hp:s:d:D:z:a:l:t:f:R:L:v:V:M:S", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h': print_usage(argv[0]); return 0;
            case 'p': dest_port = atoi(optarg); break;
            case 's':
                if (per_tx_src_count < 64) per_tx_src_ips[per_tx_src_count++] = optarg;
                break;
            case 'd':
                if (per_tx_dst_count < 64) per_tx_dst_ips[per_tx_dst_count++] = optarg;
                break;
            case 'D':
                if (per_tx_dst_mac_count < 64) per_tx_dst_macs[per_tx_dst_mac_count++] = optarg;
                break;
            case 'z': 
                global_payload_size = atoi(optarg);
                if (global_payload_size == 0 || global_payload_size > MAX_PAYLOAD_SIZE) {
                    fprintf(stderr, "Invalid payload size. Must be 1-%d bytes\n", MAX_PAYLOAD_SIZE);
                    return 1;
                }
                break;
            case 'a': 
                if (tx_device_count < 64) tx_devices[tx_device_count++] = optarg;
                break;
            case 'l': 
                tx_lcores_str = optarg;
                break;
            case 't':
                {
                    int pause_calls = atoi(optarg);
                    if (pause_calls < 0) {
                        fprintf(stderr, "Invalid throttle value. Must be >= 0\n");
                        return 1;
                    }
                    global_pause_calls = (uint32_t)pause_calls;
                }
                break;
            case 'f':
                file_prefix = optarg;
                break;
            case 'M':
                /* parsed later */
                break;
            case 'R':
                if (rx_device_count < 64) rx_devices[rx_device_count++] = optarg;
                break;
            case 'L':
                rx_lcores_str = optarg;
                break;
            case 'v':
                if (tx_device_count > 0) per_tx_vxlan[tx_device_count - 1] = optarg;
                else fprintf(stderr, "Warning: -v ignored (must follow -a)\n");
                break;
            case 'V':
                if (rx_device_count > 0) per_rx_vxlan[rx_device_count - 1] = optarg;
                else fprintf(stderr, "Warning: -V ignored (must follow -R)\n");
                break;
            case 1: // --rx-ip
                if (rx_ip_count < 64) rx_ip_strs[rx_ip_count++] = optarg;
                break;
            case 2: // --reflector-device
                if (global_reflector_device_count < 64) {
                    global_reflector_devices[global_reflector_device_count++] = optarg;
                    global_reflector_enabled = 1;
                }
                break;
            case 'S':
                global_show_per_port_stats = 1;
                break;
            default: print_usage(argv[0]); return 1;
        }
    }
    
    
    // Build EAL arguments using common function
    // Build EAL arguments using common function
    // We pass reflector devices as part of rx_devices list to ensure they are added to -a list
    const char *eal_allowed_devices[128];
    uint16_t eal_allowed_count = 0;
    for (uint16_t i = 0; i < rx_device_count; i++) eal_allowed_devices[eal_allowed_count++] = rx_devices[i];
    for (uint16_t i = 0; i < global_reflector_device_count; i++) eal_allowed_devices[eal_allowed_count++] = global_reflector_devices[i];
    
    eal_argc = dpdk_build_eal_argv(argv[0], tx_devices, tx_device_count,
		eal_allowed_devices, eal_allowed_count, tx_lcores_str, rx_lcores_str,
		file_prefix, eal_argv, argc + 128);
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
    
    // Initialize DPDK EAL with collected arguments
#ifdef ARM_HOST_DOCA
    for (int i = 1; i + 1 < eal_argc; ++i)
        if (!strcmp(eal_argv[i], "-a")) eal_argv[++i] = "0000:00:00.0";
#endif
    ret = rte_eal_init(eal_argc, eal_argv);
    if (ret < 0) {
        fprintf(stderr, "Failed to init DPDK EAL\n");
        return 1;
    }
    
#ifdef ARM_HOST_DOCA
    arm_host_probe();
#endif
    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Check available ports
    uint16_t num_ports = rte_eth_dev_count_avail();
    printf("Available DPDK ports: %d\n", num_ports);
    
    if (num_ports == 0) {
        fprintf(stderr, "No DPDK ports available. Check device binding.\n");
        return 1;
    }
    
    
    // TX ports: if -a devices provided, restrict TX to those; otherwise use all ports
    if (tx_device_count == 0 && !global_reflector_enabled) {
        global_num_tx_ports = num_ports;
        global_tx_port_ids = malloc(global_num_tx_ports * sizeof(uint16_t));
        for (uint16_t i = 0; i < num_ports; i++) global_tx_port_ids[i] = i;
    } else {
        uint16_t matched[64];
        uint16_t m = 0;
        if (dpdk_match_ports_by_bdf(tx_devices, tx_device_count, matched, &m) != 0 || m == 0) {
            fprintf(stderr, "No TX ports matched -a device list.\n");
            return 1;
        }
        global_num_tx_ports = m;
        global_tx_port_ids = malloc(global_num_tx_ports * sizeof(uint16_t));
        for (uint16_t i = 0; i < m; i++) global_tx_port_ids[i] = matched[i];
    }
    
    int do_rx = (rx_device_count > 0) && (rx_lcores_str != NULL);

    // Build RX port list if requested (only when do_rx)
    if (!do_rx) {
        global_num_rx_ports = 0;
        global_rx_port_ids = NULL;
    } else {
        global_rx_port_ids = malloc(num_ports * sizeof(uint16_t));
        global_num_rx_ports = 0;
        if (dpdk_match_ports_by_bdf(rx_devices, rx_device_count, global_rx_port_ids, &global_num_rx_ports) != 0 || global_num_rx_ports == 0) {
            fprintf(stderr, "No RX ports matched --rx-device list.\n");
            return 1;
        }
        // Map --rx-ip strings to RX ports in given order
        if (rx_ip_count > 0 && rx_ip_count != global_num_rx_ports) {
            fprintf(stderr, "Number of --rx-ip values (%u) does not match number of RX ports (%u). Provide one per --rx-device.\n", rx_ip_count, global_num_rx_ports);
            return 1;
        }
        if (rx_ip_count == 0) {
            fprintf(stderr, "RX ports specified but no --rx-ip provided. Receiver needs IPs to answer ARP.\n");
            return 1;
        }
        global_rx_ip_addrs = calloc(num_ports ? num_ports : 1, sizeof(struct in_addr));
        for (uint16_t i = 0; i < global_num_rx_ports; i++) {
            uint16_t pid = global_rx_port_ids[i];
            struct in_addr ip = {0};
            inet_aton(rx_ip_strs[i], &ip);
            global_rx_ip_addrs[pid] = ip;
        }
    }

    // Build TX and RX lcore sets
    if (do_rx) {
        if (dpdk_parse_lcore_set(rx_lcores_str, global_rx_lcores, &global_rx_lcores_count) != 0) {
            fprintf(stderr, "Invalid --rx-lcores format.\n");
            return 1;
        }
    } else {
        global_rx_lcores_count = 0;
    }
    
    // Build TX lcores: if tx_lcores_str provided, parse it; otherwise use all available lcores
    if (tx_lcores_str) {
        if (dpdk_parse_lcore_set(tx_lcores_str, global_tx_lcores, &global_tx_lcores_count) != 0) {
            fprintf(stderr, "Invalid -l/--lcores format.\n");
            return 1;
        }
    } else {
        // Use all available lcores except main and RX lcores
        global_tx_lcores_count = 0;
        uint16_t main_lcore = rte_get_main_lcore();
        unsigned int lc = rte_get_next_lcore(-1, 1, 0);
        while (lc < RTE_MAX_LCORE) {
            if ((uint16_t)lc != main_lcore && !lcore_in_list((uint16_t)lc, global_rx_lcores, global_rx_lcores_count)) {
                global_tx_lcores[global_tx_lcores_count++] = (uint16_t)lc;
            }
            lc = rte_get_next_lcore(lc, 1, 0);
        }
    }
    
    // Filter lcores: exclude main lcore and ensure RX/TX sets are disjoint
    if (dpdk_filter_lcores(global_tx_lcores, &global_tx_lcores_count, global_rx_lcores, &global_rx_lcores_count) != 0) {
        fprintf(stderr, "Failed to filter lcores\n");
        return 1;
    }
    
    if (global_tx_lcores_count == 0 && global_rx_lcores_count == 0 && !global_reflector_enabled) {
        fprintf(stderr, "No worker lcores available.\n");
        return 1;
    }
    printf("Main lcore: %u, TX workers: %u, RX workers: %u\n", rte_get_main_lcore(), global_tx_lcores_count, global_rx_lcores_count);
    
    // Calculate TX/RX queues per port
    tx_queues_per_port = (global_tx_lcores_count == 0 || global_num_tx_ports == 0) ? 0 : ceil_div(global_tx_lcores_count, global_num_tx_ports);
    rx_queues_per_port = (!do_rx || global_rx_lcores_count == 0 || global_num_rx_ports == 0) ? 0 : ceil_div(global_rx_lcores_count, global_num_rx_ports);
    if (tx_queues_per_port == 0 && rx_queues_per_port == 0) { fprintf(stderr, "No TX or RX queues requested.\n"); return 1; }
    if (tx_queues_per_port) printf("Setting up %u TX queues/port for %u TX workers across %u ports\n", tx_queues_per_port, global_tx_lcores_count, global_num_tx_ports);
    if (rx_queues_per_port) printf("Setting up %u RX queues/port for %u RX workers across %u ports\n", rx_queues_per_port, global_rx_lcores_count, global_num_rx_ports);
    
    // Create mbuf pool early (needed for RX queue setup and ARP)
    uint32_t mbuf_pool_size = 65536;
    // Re-parse for -M/--mbufs
    optind = 1;
    while ((opt = getopt_long(argc, argv, "hp:s:d:D:z:a:l:t:f:R:L:v:V:M:S", long_options, NULL)) != -1) {
        if (opt == 'M') {
            long v = strtol(optarg, NULL, 10);
            if (v > 0) mbuf_pool_size = (uint32_t)v;
        }
    }
    struct rte_mempool *mbuf_pool = dpdk_create_mbuf_pool("MBUF_POOL", mbuf_pool_size, 512);
    if (!mbuf_pool) { return 1; }
    global_mbuf_pool = mbuf_pool;

    // Prepare per-port IP settings (needed before port config for ARP)
    // Already built per-port src/dst IPs earlier

    // Allocate caches and per-port addressing arrays
    global_tx_dst_macs = calloc(num_ports ? num_ports : 1, sizeof(struct rte_ether_addr));
    global_tx_src_addrs = calloc(num_ports ? num_ports : 1, sizeof(struct in_addr));
    global_tx_dst_addrs = calloc(num_ports ? num_ports : 1, sizeof(struct in_addr));
    // Allocate VXLAN arrays
    global_vxlan_dst_addrs = calloc(num_ports ? num_ports : 1, sizeof(struct in_addr));
    global_vxlan_udp_ports = calloc(num_ports ? num_ports : 1, sizeof(uint16_t));
    // Populate per-port source/destination IPs in TX device order
    for (uint16_t i = 0; i < global_num_tx_ports; i++) {
        uint16_t pid = global_tx_port_ids[i];
        if (pid >= num_ports) continue;
        struct in_addr s = (struct in_addr){0};
        struct in_addr d = (struct in_addr){0};
        if (i < per_tx_src_count) inet_aton(per_tx_src_ips[i], &s);
        if (i < per_tx_dst_count) inet_aton(per_tx_dst_ips[i], &d);
        global_tx_src_addrs[pid] = s;
        global_tx_dst_addrs[pid] = d;
        // Parse VXLAN outer dst from -v IP:PORT
        if (per_tx_vxlan[i]) {
            const char *vs = per_tx_vxlan[i];
            char buf[128]; memset(buf, 0, sizeof(buf));
            strncpy(buf, vs, sizeof(buf) - 1);
            char *colon = strchr(buf, ':');
            if (!colon) { fprintf(stderr, "Invalid -v value '%s' (expected IP:PORT)\n", vs); return 1; }
            *colon = '\0';
            struct in_addr vip = {0};
            if (!inet_aton(buf, &vip)) { fprintf(stderr, "Invalid VXLAN IP '%s'\n", buf); return 1; }
            int vport = atoi(colon + 1);
            if (vport <= 0 || vport > 65535) { fprintf(stderr, "Invalid VXLAN port '%s'\n", colon + 1); return 1; }
            global_vxlan_dst_addrs[pid] = vip;
            global_vxlan_udp_ports[pid] = (uint16_t)vport;
        }
    }

    // Populate per-port destination MACs (if provided via -D)
    for (uint16_t i = 0; i < global_num_tx_ports; i++) {
        uint16_t pid = global_tx_port_ids[i];
        if (pid >= num_ports) continue;
        if (i < per_tx_dst_mac_count) {
            struct rte_ether_addr mac;
            if (rte_ether_unformat_addr(per_tx_dst_macs[i], &mac) != 0) {
                fprintf(stderr, "Invalid MAC address format: %s\n", per_tx_dst_macs[i]);
                return 1;
            }
            global_tx_dst_macs[pid] = mac;
        }
    }

    // Validate that each TX port has a destination IP (must be provided via -d)
    for (uint16_t i = 0; i < global_num_tx_ports; i++) {
        uint16_t pid = global_tx_port_ids[i];
        if (pid >= num_ports) {
            fprintf(stderr, "Internal error: TX port id %u out of range (num_ports=%u)\n", pid, num_ports);
            return 1;
        }
        if (global_tx_dst_addrs[pid].s_addr == 0) {
            fprintf(stderr, "Missing -d/--tx-dst-ip for TX port index %u (port id %u). Provide one per -a TX device.\n", i, pid);
            return 1;
        }
    }
    // Validate TX src IP matches RX IP for ports that are both TX and RX
    if (do_rx && global_rx_ip_addrs) {
        for (uint16_t i = 0; i < global_num_tx_ports; i++) {
            uint16_t pid = global_tx_port_ids[i];
            // Check if this port is also in RX list
            int is_rx_port = 0;
            for (uint16_t j = 0; j < global_num_rx_ports; j++) {
                if (global_rx_port_ids[j] == pid) {
                    is_rx_port = 1;
                    break;
                }
            }
            if (is_rx_port && global_rx_ip_addrs[pid].s_addr != 0) {
                if (global_tx_src_addrs[pid].s_addr != global_rx_ip_addrs[pid].s_addr) {
                    char tx_ip_str[INET_ADDRSTRLEN], rx_ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &global_tx_src_addrs[pid], tx_ip_str, sizeof(tx_ip_str));
                    inet_ntop(AF_INET, &global_rx_ip_addrs[pid], rx_ip_str, sizeof(rx_ip_str));
                    fprintf(stderr, "Warning: Port %u is both TX and RX, but TX src IP (%s) != RX IP (%s). "
                            "ARP responses will use RX IP.\n",
                            pid, tx_ip_str, rx_ip_str);
                }
            }
        }
    }

    // Configure and start all ports
    if (dpdk_configure_ports(num_ports, global_rx_port_ids, global_num_rx_ports, rx_queues_per_port, global_tx_port_ids, global_num_tx_ports, tx_queues_per_port, mbuf_pool, &global_src_macs)) return 1;
#ifdef ARM_HOST_DOCA
    arm_host_flow_start(mbuf_pool);
#endif

    // Configure reflector ports if enabled
    if (global_reflector_enabled) {
        uint16_t matched[64];
        uint16_t m = 0;
        
        if (dpdk_match_ports_by_bdf(global_reflector_devices, global_reflector_device_count, matched, &m) != 0 || m == 0) {
            fprintf(stderr, "No reflector devices matched.\n");
            return 1;
        }
        
        for (uint16_t i = 0; i < m; i++) {
            uint16_t port_id = matched[i];
            global_reflector_port_ids[i] = port_id;
            
            // Configure 1 Hairpin RX, 1 Hairpin TX queue
            struct rte_eth_conf port_conf = {0};
            port_conf.rxmode.mtu = 1518;
            
            rte_eth_dev_configure(port_id, 1, 1, &port_conf);
            
            // Setup Hairpin RX Queue 0
            struct rte_eth_hairpin_conf hairpin_rx_conf;
            memset(&hairpin_rx_conf, 0, sizeof(hairpin_rx_conf));
            hairpin_rx_conf.peer_count = 1;
            hairpin_rx_conf.peers[0].port = port_id;
            hairpin_rx_conf.peers[0].queue = 0; // Bind to TX Queue 0
            
            if (rte_eth_rx_hairpin_queue_setup(port_id, 0, 1024, &hairpin_rx_conf) != 0) {
                fprintf(stderr, "Failed to setup hairpin RX queue on port %u\n", port_id);
                return 1;
            }

            // Setup Hairpin TX Queue 0
            struct rte_eth_hairpin_conf hairpin_tx_conf;
            memset(&hairpin_tx_conf, 0, sizeof(hairpin_tx_conf));
            hairpin_tx_conf.peer_count = 1;
            hairpin_tx_conf.peers[0].port = port_id;
            hairpin_tx_conf.peers[0].queue = 0; // Bind to RX Queue 0
            
            if (rte_eth_tx_hairpin_queue_setup(port_id, 0, 1024, &hairpin_tx_conf) != 0) {
                fprintf(stderr, "Failed to setup hairpin TX queue on port %u\n", port_id);
                return 1;
            }

            rte_eth_dev_start(port_id);
            
            // Bind Hairpin Queues
            if (rte_eth_hairpin_bind(port_id, port_id) != 0) {
                fprintf(stderr, "Failed to bind hairpin queues on port %u\n", port_id);
                return 1;
            }

            rte_eth_promiscuous_enable(port_id);
            
            // Install flow to steer everything to Queue 0
            install_flow_all_to_queue0(port_id, 0);
            
            printf("Reflector port %u configured with Hairpin queues.\n", port_id);
        }
    }

    reset_port_stats_cache();

    if (per_tx_dst_count != global_num_tx_ports) {
        fprintf(stderr, "Number of -d/--tx-dst-ip values (%u) does not match number of TX ports (%u). Provide one per -a TX device.\n", per_tx_dst_count, global_num_tx_ports);
        return 1;
    }
    if (per_tx_src_count != global_num_tx_ports) {
        fprintf(stderr, "Number of -s/--src-ip values (%u) does not match number of TX ports (%u). Provide one per -a TX device.\n", per_tx_src_count, global_num_tx_ports);
        return 1;
    }
    // per-port src/dst IPs already populated earlier
    
    if (global_num_rx_ports > 0) {
        printf("DPDK Simple Packet Gen+Recv\n");
    } else {
        printf("DPDK Simple Packet Gen\n");
    }
    printf("  UDP dest port: %d\n", dest_port);
    printf("  TX ports: %u\n", global_num_tx_ports);
    printf("  RX ports: %u\n", global_num_rx_ports);
    printf("\n");
    // Phase 2 (Moved First): Launch RX workers (but aux polling is initially disabled via global flag)
    if (do_rx) {
        uint16_t *orphan_tx_ports = calloc(num_ports, sizeof(uint16_t));
        uint16_t orphan_count = 0;
        for (uint16_t i = 0; i < global_num_tx_ports; i++) {
            uint16_t tx_pid = global_tx_port_ids[i];
            int is_rx = 0;
            for (uint16_t j = 0; j < global_num_rx_ports; j++) {
                if (global_rx_port_ids[j] == tx_pid) { is_rx = 1; break; }
            }
            if (!is_rx) {
                orphan_tx_ports[orphan_count++] = tx_pid;
            }
        }

        for (uint16_t i = 0; i < global_rx_lcores_count; i++) {
            uint16_t worker_idx = i;
            uint16_t port_id = global_rx_port_ids ? global_rx_port_ids[worker_idx % (global_num_rx_ports ? global_num_rx_ports : 1)] : 0;
            uint16_t queue_id = (global_num_rx_ports ? (worker_idx / global_num_rx_ports) : 0);
            struct dpdk_shared_rx_worker_ctx *ctx = &global_rx_worker_ctx[i];
            memset(ctx, 0, sizeof(*ctx));
            ctx->running = &global_running;
            ctx->port_id = port_id;
            ctx->rx_queue_id = queue_id;
            ctx->tx_queue_id = 0;
            ctx->filter_udp_port = global_dest_port;
            ctx->warn_on_mismatch = 1;
            ctx->port_macs = global_src_macs;
            ctx->rx_ip_addrs = global_rx_ip_addrs;
            
            if (global_tx_dst_addrs && global_tx_dst_addrs[port_id].s_addr != 0) {
                ctx->monitor_arp_ip = global_tx_dst_addrs[port_id];
                ctx->monitor_arp_result = &global_tx_dst_macs[port_id];
            }

            ctx->aux_polling_enabled = &global_aux_polling_enabled;
            ctx->aux_rx_ports = calloc(orphan_count, sizeof(uint16_t)); 
            ctx->aux_rx_ports_count = 0;
            for (uint16_t k = 0; k < orphan_count; k++) {
                if (k % global_rx_lcores_count == i) {
                    ctx->aux_rx_ports[ctx->aux_rx_ports_count++] = orphan_tx_ports[k];
                }
            }
            
            rte_eal_remote_launch(dpdk_shared_rx_worker, ctx, global_rx_lcores[i]);
        }
        free(orphan_tx_ports);
        rte_delay_ms(100); 
    }

    // Phase 1: Resolve Orphan Ports (Main thread poll)
    // Aux polling is disabled, so Main thread can safely poll orphan ports.
    // RX workers are running, so they can reply to ARP requests on destination ports.
    for (uint16_t port_id = 0; port_id < num_ports; port_id++) {
        int is_tx_port = 0;
        for (uint16_t i = 0; i < global_num_tx_ports; i++) if (global_tx_port_ids[i] == port_id) { is_tx_port = 1; break; }
        int is_rx_port = 0;
        if (do_rx) {
            for (uint16_t j = 0; j < global_num_rx_ports; j++) if (global_rx_port_ids[j] == port_id) { is_rx_port = 1; break; }
        }
        
        if (is_tx_port && !is_rx_port && dpdk_is_mac_unresolved(&global_tx_dst_macs[port_id])) {
            struct in_addr sip = global_tx_src_addrs ? global_tx_src_addrs[port_id] : (struct in_addr){0};
            struct in_addr dip = global_tx_dst_addrs ? global_tx_dst_addrs[port_id] : (struct in_addr){0};
            int ar = resolve_arp_for_port(port_id, &global_src_macs[port_id], sip, dip, &global_tx_dst_macs[port_id], mbuf_pool, 500);
            if (ar == 0) {
                char dip_str[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &dip, dip_str, sizeof(dip_str));
                printf("  ARP resolved on port %u: %s -> %02x:%02x:%02x:%02x:%02x:%02x\n", port_id, dip_str,
                       global_tx_dst_macs[port_id].addr_bytes[0], global_tx_dst_macs[port_id].addr_bytes[1], global_tx_dst_macs[port_id].addr_bytes[2],
                       global_tx_dst_macs[port_id].addr_bytes[3], global_tx_dst_macs[port_id].addr_bytes[4], global_tx_dst_macs[port_id].addr_bytes[5]);
            } else {
                char dip_str[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &dip, dip_str, sizeof(dip_str));
                fprintf(stderr, "ARP resolution timed out on port %u for %s. Leaving DstMAC unresolved.\n", port_id, dip_str);
                memset(&global_tx_dst_macs[port_id], 0xff, RTE_ETHER_ADDR_LEN);
            }
        }
    }
    
    // Enable Aux Polling for runtime
    global_aux_polling_enabled = 1;

    // Phase 3: Resolve Overlapping Ports (using RX worker snoop)
    for (uint16_t port_id = 0; port_id < num_ports; port_id++) {
        int is_tx_port = 0;
        for (uint16_t i = 0; i < global_num_tx_ports; i++) if (global_tx_port_ids[i] == port_id) { is_tx_port = 1; break; }
        int is_rx_port = 0;
        if (do_rx) {
            for (uint16_t j = 0; j < global_num_rx_ports; j++) if (global_rx_port_ids[j] == port_id) { is_rx_port = 1; break; }
        }

        // Only handle overlapping ports here
        if (is_tx_port && is_rx_port && dpdk_is_mac_unresolved(&global_tx_dst_macs[port_id])) {
            struct in_addr sip = global_tx_src_addrs ? global_tx_src_addrs[port_id] : (struct in_addr){0};
            struct in_addr dip = global_tx_dst_addrs ? global_tx_dst_addrs[port_id] : (struct in_addr){0};
            
            int ar = -1;
            int timeout_ms = 500;
            int elapsed = 0;
            while (elapsed < timeout_ms) {
                if (!dpdk_is_mac_unresolved(&global_tx_dst_macs[port_id])) {
                    ar = 0; break;
                }
                struct rte_mbuf *m = rte_pktmbuf_alloc(mbuf_pool);
                if (m) {
                    dpdk_build_arp_request(m, &global_src_macs[port_id], sip, dip);
                    if (rte_eth_tx_burst(port_id, 0, &m, 1) != 1) rte_pktmbuf_free(m);
                }
                rte_delay_ms(100);
                elapsed += 100;
            }
            
            if (ar == 0) {
                char dip_str[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &dip, dip_str, sizeof(dip_str));
                printf("  ARP resolved on port %u: %s -> %02x:%02x:%02x:%02x:%02x:%02x\n", port_id, dip_str,
                       global_tx_dst_macs[port_id].addr_bytes[0], global_tx_dst_macs[port_id].addr_bytes[1], global_tx_dst_macs[port_id].addr_bytes[2],
                       global_tx_dst_macs[port_id].addr_bytes[3], global_tx_dst_macs[port_id].addr_bytes[4], global_tx_dst_macs[port_id].addr_bytes[5]);
            } else {
                char dip_str[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &dip, dip_str, sizeof(dip_str));
                fprintf(stderr, "ARP resolution timed out on port %u for %s. Leaving DstMAC unresolved.\n", port_id, dip_str);
                memset(&global_tx_dst_macs[port_id], 0xff, RTE_ETHER_ADDR_LEN);
            }
        }
    }

    printf("Per-TX-port addressing and MACs:\n");
    printf("  Port  SrcMAC               SrcIP          DstIP          DstMAC(ARP)\n");
    for (uint16_t i = 0; i < global_num_tx_ports; i++) {
        uint16_t pid = global_tx_port_ids[i];
        if (pid >= num_ports || !global_tx_src_addrs || !global_tx_dst_addrs || !global_src_macs || !global_tx_dst_macs) {
            continue;
        }
        char src_ip_str[INET_ADDRSTRLEN] = {0};
        char dst_ip_str[INET_ADDRSTRLEN] = {0};
        if (!inet_ntop(AF_INET, &global_tx_src_addrs[pid], src_ip_str, sizeof(src_ip_str))) {
            strncpy(src_ip_str, "0.0.0.0", sizeof(src_ip_str) - 1);
        }
        if (!inet_ntop(AF_INET, &global_tx_dst_addrs[pid], dst_ip_str, sizeof(dst_ip_str))) {
            strncpy(dst_ip_str, "0.0.0.0", sizeof(dst_ip_str) - 1);
        }
        struct rte_ether_addr dst_mac = global_tx_dst_macs[pid];
        int unresolved = 1;
        for (int b = 0; b < RTE_ETHER_ADDR_LEN; b++) if (dst_mac.addr_bytes[b] != 0x00) { unresolved = 0; break; }
        if (unresolved) {
            printf("  %-4u  %02x:%02x:%02x:%02x:%02x:%02x  %-15s  %-15s  %s\n",
                   pid,
                   global_src_macs[pid].addr_bytes[0], global_src_macs[pid].addr_bytes[1], global_src_macs[pid].addr_bytes[2],
                   global_src_macs[pid].addr_bytes[3], global_src_macs[pid].addr_bytes[4], global_src_macs[pid].addr_bytes[5],
                   src_ip_str, dst_ip_str, "unresolved");
        } else {
            printf("  %-4u  %02x:%02x:%02x:%02x:%02x:%02x  %-15s  %-15s  %02x:%02x:%02x:%02x:%02x:%02x\n",
                   pid,
                   global_src_macs[pid].addr_bytes[0], global_src_macs[pid].addr_bytes[1], global_src_macs[pid].addr_bytes[2],
                   global_src_macs[pid].addr_bytes[3], global_src_macs[pid].addr_bytes[4], global_src_macs[pid].addr_bytes[5],
                   src_ip_str, dst_ip_str,
                   dst_mac.addr_bytes[0], dst_mac.addr_bytes[1], dst_mac.addr_bytes[2],
                   dst_mac.addr_bytes[3], dst_mac.addr_bytes[4], dst_mac.addr_bytes[5]);
        }
    }
    if (global_num_rx_ports > 0) {
        printf("\nPer-RX-port MACs and IPs:\n");
        printf("  Port  MAC                  IP\n");
        for (uint16_t i = 0; i < global_num_rx_ports; i++) {
            uint16_t pid = global_rx_port_ids[i];
            if (pid >= num_ports || !global_src_macs) continue;
            char rx_ip_str[INET_ADDRSTRLEN] = {0};
            int have_ip = 0;
            if (global_rx_ip_addrs && global_rx_ip_addrs[pid].s_addr != 0) {
                if (inet_ntop(AF_INET, &global_rx_ip_addrs[pid], rx_ip_str, sizeof(rx_ip_str))) {
                    have_ip = 1;
                }
            }
            if (!have_ip) strncpy(rx_ip_str, "n/a", sizeof(rx_ip_str) - 1);
            printf("  %-4u  %02x:%02x:%02x:%02x:%02x:%02x  %-15s\n",
                   pid,
                   global_src_macs[pid].addr_bytes[0], global_src_macs[pid].addr_bytes[1], global_src_macs[pid].addr_bytes[2],
                   global_src_macs[pid].addr_bytes[3], global_src_macs[pid].addr_bytes[4], global_src_macs[pid].addr_bytes[5],
                   rx_ip_str);
        }
    }
    // If RX ports present and -V provided per RX port, install decap flows
    if (global_num_rx_ports > 0) {
        for (uint16_t i = 0; i < global_num_rx_ports; i++) {
            if (!per_rx_vxlan[i]) continue;

            uint16_t pid = global_rx_port_ids[i];
            struct in_addr ip = {0}; uint16_t port = 0; char err[128];
            if (dpdk_parse_ip_port(per_rx_vxlan[i], &ip, &port) != 0) {
                fprintf(stderr, "Invalid -V value '%s' (expected IP:PORT)\n", per_rx_vxlan[i]);
                continue;
            }
            struct rte_flow *flow = NULL;
            if (dpdk_install_vxlan_decap_flow(pid, ip, port, &flow, err, sizeof(err)) != 0) {
                fprintf(stderr, "Failed to install RX VXLAN decap on port %u: %s\n", pid, err);
            } else {
                printf("Installed RX VXLAN decap on port %u for %s:%u\n", pid, inet_ntoa(ip), port);
            }
        }
    }
    printf("  Payload size: %d bytes (fixed)\n", global_payload_size);
    printf("  Total packet size: %zu bytes\n", global_payload_size + sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr));
    printf("  TX workers: %u, RX workers: %u\n", global_tx_lcores_count, global_rx_lcores_count);
    printf("  Rate: %s\n", global_pause_calls > 0 ? "Throttled" : "Maximum (no throttling)");
    printf("  Throttle: %u rte_pause() calls per loop\n", global_pause_calls);
    printf("  Batch size: %d\n", BATCH_SIZE);
    
    // Global variables for worker threads
    global_mbuf_pool = mbuf_pool;

    global_dest_port = dest_port;
    
    // (RX workers and ARP resolution moved earlier)
    // Launch TX workers on remaining lcores
    for (uint16_t i = 0; i < global_tx_lcores_count; i++) {
        rte_eal_remote_launch(packet_worker, NULL, global_tx_lcores[i]);
    }
    // Main-thread aggregation loop: compute deltas for TX/RX and print totals + per-thread averages
    uint64_t last_tsc = rte_get_timer_cycles();
    const uint64_t stats_hz = rte_get_timer_hz();
    struct rte_eth_stats port_deltas[RTE_MAX_ETHPORTS];

    while (global_running) {
        uint64_t now = rte_get_timer_cycles();
        uint64_t elapsed = now - last_tsc;
        if (elapsed >= STATS_INTERVAL * stats_hz) {
            double secs = (double)elapsed / (double)stats_hz;
            
            // Calculate deltas for all ports first (atomic snapshot per interval)
            for (uint16_t pid = 0; pid < num_ports; pid++) {
                if (dpdk_stats_get_delta(pid, &g_port_stats_cache[pid], &port_deltas[pid]) < 0) {
                     memset(&port_deltas[pid], 0, sizeof(port_deltas[pid]));
                }
            }

            if (global_num_tx_ports > 0) {
                dpdk_print_port_stats("TX", global_tx_port_ids, global_num_tx_ports, secs, 1, global_show_per_port_stats, port_deltas);
            }
            if (global_num_rx_ports > 0) {
                dpdk_print_port_stats("RX", global_rx_port_ids, global_num_rx_ports, secs, 0, global_show_per_port_stats, port_deltas);
            }
            last_tsc = now;
        }
        rte_pause();
    }
    // Wait for worker threads to finish
    rte_eal_mp_wait_lcore();
#ifdef ARM_HOST_DOCA
    arm_host_flow_stop();
#endif
    
    // Stop and close all ports
    printf("Stopping all ports...\n");
    for (uint16_t i = 0; i < num_ports; i++) {
        printf("Stopping port %d...\n", i);
        rte_eth_dev_stop(i);
        rte_eth_dev_close(i);
    }
    
    rte_eal_cleanup();
    free(global_src_macs);
    if (global_tx_port_ids) free(global_tx_port_ids);
    if (global_rx_port_ids) free(global_rx_port_ids);
    return 0;
}
