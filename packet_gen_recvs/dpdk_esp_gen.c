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
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <rte_esp.h>

#define AES_BLOCK_SIZE 16
#define AES_IV_SIZE 12
#define AES_TAG_SIZE 16

#define MAX_PAYLOAD_SIZE 1472  // Reasonable max to stay under MTU
#define MAX_PACKET_SIZE 2048
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
static uint16_t global_dest_port;
// ESP per-port tunnel destination (from -T IP)
// ESP per-port tunnel destination (from -T IP)
static struct in_addr *global_esp_tunnel_ips = NULL; // per TX port
static uint32_t *global_esp_spis = NULL; // per TX port
static uint8_t *global_esp_keys = NULL; // per TX port, stride 32
static int *global_esp_key_lens = NULL; // per TX port
static uint32_t *global_esp_salts = NULL; // per TX port
static uint64_t *global_esp_ivs = NULL; // per TX port
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
static int global_vary_src_ip = 0;

// lcore role assignments
static uint16_t global_tx_lcores[RTE_MAX_LCORE];
static uint16_t global_tx_lcores_count = 0;
static uint16_t global_rx_lcores[RTE_MAX_LCORE];
static uint16_t global_rx_lcores_count = 0;
static struct dpdk_shared_rx_worker_ctx global_rx_worker_ctx[RTE_MAX_LCORE];

// Synchronization for ordered sequence generation
struct sync_point {
    volatile uint64_t ticket;
    uint8_t pad[RTE_CACHE_LINE_SIZE - sizeof(uint64_t)];
} __rte_cache_aligned;

static struct sync_point global_sync_tokens[RTE_MAX_ETHPORTS][RTE_MAX_LCORE];
static uint16_t global_threads_per_port[RTE_MAX_ETHPORTS] = {0};

static struct dpdk_stats g_port_stats_cache[RTE_MAX_ETHPORTS];

// Signal handler for graceful shutdown
static void signal_handler(int signum) {
    printf("\nReceived signal %d, shutting down...\n", signum);
    global_running = 0;
}

static int fill_mbuf_with_esp_tunnel_packet(struct rte_mbuf *mbuf,
                                            EVP_CIPHER_CTX *ctx,
                                            struct in_addr src_ip,
                                            struct in_addr dst_ip,
                                            struct in_addr tunnel_ip,
                                            struct rte_ether_addr *src_mac,
                                            struct rte_ether_addr *dst_mac,
                                            uint16_t udp_src_port,
                                            uint16_t udp_dst_port,
                                            uint32_t seq,
                                            uint32_t spi,
                                            uint32_t salt,
                                            uint64_t esp_iv) {
    uint32_t payload_len = global_payload_size;
    uint8_t plaintext[MAX_PAYLOAD_SIZE + 64];

    // Construct Inner Packet (IP + UDP + Payload)
    struct rte_ipv4_hdr *inner_ip = (struct rte_ipv4_hdr *)plaintext;
    struct rte_udp_hdr *inner_udp = (struct rte_udp_hdr *)(inner_ip + 1);
    uint8_t *inner_data = (uint8_t *)(inner_udp + 1);

    // IP
    inner_ip->version_ihl = 0x45;
    inner_ip->type_of_service = 0;
    inner_ip->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + payload_len);
    inner_ip->packet_id = 0;
    inner_ip->fragment_offset = 0;
    inner_ip->time_to_live = 64;
    inner_ip->next_proto_id = IPPROTO_UDP;
    inner_ip->src_addr = src_ip.s_addr;
    inner_ip->dst_addr = dst_ip.s_addr;
    inner_ip->hdr_checksum = 0;
    inner_ip->hdr_checksum = rte_ipv4_cksum(inner_ip);

    // UDP
    inner_udp->src_port = rte_cpu_to_be_16(udp_src_port);
    inner_udp->dst_port = rte_cpu_to_be_16(udp_dst_port);
    inner_udp->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + payload_len);
    inner_udp->dgram_cksum = 0;

    // Payload
    memset(inner_data, 0xA5, payload_len);

    uint16_t inner_headers_len = sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr);

    // Calculate Padding for ESP
    int esp_payload_len = inner_headers_len + payload_len;
    int pad_len = 0;
    while ((esp_payload_len + pad_len + 2) % 4 != 0) {
        pad_len++;
    }

    uint8_t *pad_ptr = inner_data + payload_len;
    for (int i = 0; i < pad_len; i++) {
        pad_ptr[i] = i + 1;
    }

    uint8_t *trailer = pad_ptr + pad_len;
    trailer[0] = pad_len;
    trailer[1] = IPPROTO_IPIP; // Tunnel Mode (IPv4)

    int full_plaintext_len = esp_payload_len + pad_len + 2;

    unsigned char iv[AES_IV_SIZE];
    // Optimized IV generation using sequence number and salt
    uint32_t salt_be = rte_cpu_to_be_32(salt);
    memcpy(iv, &salt_be, 4);

    uint64_t iv_val;
    if (esp_iv != 0) {
        iv_val = esp_iv; // Static IV if specified
    } else {
        // Use sequence number mixed with salt for unique but fast IV
        iv_val = (uint64_t)seq ^ ((uint64_t)salt_be << 32);
    }
    uint64_t iv_be = rte_cpu_to_be_64(iv_val);
    memcpy(iv + 4, &iv_be, 8);

    unsigned char ciphertext[MAX_PAYLOAD_SIZE + 64];
    unsigned char tag[AES_TAG_SIZE];

    // Reuse context, only set IV
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, NULL, iv) != 1) return -1;

    // Construct AAD (SPI + Sequence Number)
    uint8_t aad[8];
    uint32_t spi_be = rte_cpu_to_be_32(spi);
    uint32_t seq_be = rte_cpu_to_be_32(seq);
    memcpy(aad, &spi_be, 4);
    memcpy(aad + 4, &seq_be, 4);

    int len;
    int outlen;
    // Feed AAD
    if (EVP_EncryptUpdate(ctx, NULL, &outlen, aad, sizeof(aad)) != 1) return -1;

    // Encrypt
    if (EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, full_plaintext_len) != 1) return -1;
    if (EVP_EncryptFinal_ex(ctx, ciphertext + len, &len) != 1) return -1;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, AES_TAG_SIZE, tag) != 1) return -1;

    uint8_t *pkt = rte_pktmbuf_mtod(mbuf, uint8_t *);
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)pkt;
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    struct rte_esp_hdr *esp = (struct rte_esp_hdr *)(ip + 1);
    uint8_t *pkt_iv_ptr = (uint8_t *)(esp + 1);
    uint8_t *esp_data = pkt_iv_ptr + 8;
    uint8_t *esp_icv = esp_data + full_plaintext_len;

    memcpy(eth->dst_addr.addr_bytes, dst_mac->addr_bytes, RTE_ETHER_ADDR_LEN);
    memcpy(eth->src_addr.addr_bytes, src_mac->addr_bytes, RTE_ETHER_ADDR_LEN);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    ip->version_ihl = 0x45;
    ip->type_of_service = 0;
    uint16_t total_len = sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_esp_hdr) + 8 + full_plaintext_len + AES_TAG_SIZE;
    ip->total_length = rte_cpu_to_be_16(total_len);
    ip->packet_id = 0;
    ip->fragment_offset = 0;
    ip->time_to_live = 64;
    ip->next_proto_id = IPPROTO_ESP;
    ip->src_addr = src_ip.s_addr; // Use Inner Src as Outer Src (Device IP)
    ip->dst_addr = tunnel_ip.s_addr;
    ip->hdr_checksum = 0; // Offload will handle or 0

    esp->spi = spi_be;
    esp->seq = seq_be;

    memcpy(pkt_iv_ptr, iv + 4, 8);
    memcpy(esp_data, ciphertext, full_plaintext_len);
    memcpy(esp_icv, tag, AES_TAG_SIZE);

    mbuf->data_len = sizeof(struct rte_ether_hdr) + total_len;
    mbuf->pkt_len = mbuf->data_len;
    mbuf->l2_len = sizeof(struct rte_ether_hdr);
    mbuf->l3_len = sizeof(struct rte_ipv4_hdr);
    mbuf->ol_flags = RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;

    return 0;
}

static int fill_mbuf_with_packet(struct rte_mbuf *mbuf,
                                 struct in_addr inner_dst_addr,
                                 struct in_addr inner_src_addr,
                                 struct rte_ether_addr *src_mac,
                                 struct rte_ether_addr *dst_mac,
                                 uint16_t inner_dest_port,
                                 uint16_t inner_udp_src_port) {
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
    uint32_t pkt_size = base_packet_len;

    inner_frame = (void *)pkt;
    memcpy(inner_frame->eth.dst_addr.addr_bytes, dst_mac->addr_bytes, RTE_ETHER_ADDR_LEN);
    memcpy(inner_frame->eth.src_addr.addr_bytes, src_mac->addr_bytes, RTE_ETHER_ADDR_LEN);
    inner_frame->eth.ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    mbuf->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;

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
    printf("      --dst-mac MAC    Per-TX-port destination Ethernet MAC (repeat; skips ARP).\n");
    printf("      --vary-src-ip   Give each TX worker a distinct outer source IP and inner UDP source port.\n");
    printf("  -z, --size SIZE      Payload size in bytes (max: %d)\n", MAX_PAYLOAD_SIZE);
    printf("  -a, --device DEVICE  Device to use (can specify multiple times)\n");
    printf("  -l, --lcores LCORES  Logical cores to use (e.g., 0-3, 0,2,4)\n");
    printf("  -t, --throttle PAUSE Number of rte_pause() calls per loop for rate control (default: %d)\n", DEFAULT_PAUSE_CALLS);
    printf("  -f, --file-prefix P  DPDK file-prefix\n");
    printf("  -R, --rx-device DEV  Receiving device BDF (repeat to add multiple)\n");
    printf("  -L, --rx-lcores SET  Receiving lcores (e.g., 16-18,20). Subset of -l.\n");
    printf("      --rx-ip IP       Per-RX-port IP (repeat in --rx-device order).\n");
    printf("  -T, --tunnel-ip IP   Per-TX-port ESP Tunnel Destination IP (enables ESP)\n");
    printf("      --spi SPI        ESP SPI (default: 1000)\n");
    printf("      --key KEY        AES Key (hex string, 128 or 256 bit)\n");
    printf("      --salt SALT      ESP Salt (32-bit hex/int, default 0)\n");
    printf("      --iv IV          ESP Implicit IV (64-bit hex/int, default random)\n");
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
    // For ordered sequence generation, we use a single shared queue (0)
    uint16_t port_queue_id = 0;

    // Calculate rank within this port's thread group
    uint16_t rank = 0;
    uint16_t threads_for_this_port = global_threads_per_port[port_id];
    // Re-calculate rank based on worker_idx and port assignment logic
    // Logic: worker_idx % num_ports == port_index.
    // rank = worker_idx / num_ports.
    if (global_num_tx_ports > 0) {
        rank = worker_idx / global_num_tx_ports;
    }

    printf("Worker thread %u started on core %u using port %u queue %u (Rank %u/%u)\n",
           rte_lcore_id(), rte_get_next_lcore(-1, 1, 0), port_id, port_queue_id, rank, threads_for_this_port);

    // Maintain a small pool of ready-to-send mbufs across iterations
    struct rte_mbuf *pending[BATCH_SIZE];
    uint16_t pending_count = 0;

    // Give each worker its own provider cipher and EVP context.
    OSSL_LIB_CTX *libctx = OSSL_LIB_CTX_new();
    EVP_CIPHER *cipher = libctx ? EVP_CIPHER_fetch(libctx,
        global_esp_key_lens[port_id] == 32 ? "AES-256-GCM" : "AES-128-GCM", NULL) : NULL;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!libctx || !cipher || !ctx ||
        EVP_EncryptInit_ex(ctx, cipher, NULL, &global_esp_keys[port_id * 32], NULL) != 1) {
        fprintf(stderr, "Private OpenSSL initialization failed on lcore %u\n", rte_lcore_id());
        EVP_CIPHER_CTX_free(ctx);
        EVP_CIPHER_free(cipher);
        OSSL_LIB_CTX_free(libctx);
        global_running = 0;
        return -1;
    }
    EVP_CIPHER_CTX_set_padding(ctx, 0);

    // Stateless sequence number initialization
    // T0 starts at 0, T1 starts at BATCH_SIZE, etc.
    // But we iterate in batches.
    // Batch 0: T0 sends [0..63], T1 sends [64..127]...
    // So initial seq = rank * BATCH_SIZE
    uint64_t seq_counter = (uint64_t)rank * BATCH_SIZE;
    uint64_t batch_idx = 0;
    int use_esp = (global_esp_tunnel_ips && global_esp_tunnel_ips[port_id].s_addr != 0);

    while (global_running) {
        // Refill pending up to BATCH_SIZE
        while (pending_count < BATCH_SIZE) {
            struct rte_mbuf *m = rte_pktmbuf_alloc(global_mbuf_pool);
            if (!m) break;
            struct in_addr src_ip_i = global_tx_src_addrs[port_id];
            struct in_addr dst_ip_i = global_tx_dst_addrs[port_id];
            if (global_vary_src_ip)
                src_ip_i.s_addr = rte_cpu_to_be_32(rte_be_to_cpu_32(src_ip_i.s_addr) + rank);

            uint16_t udp_src_port = (uint16_t)(DEFAULT_SRC_PORT + (global_vary_src_ip ? rank : port_queue_id));

            int ret = 0;
            if (use_esp) {
                ret = fill_mbuf_with_esp_tunnel_packet(m, ctx, src_ip_i, dst_ip_i,
                                                       global_esp_tunnel_ips[port_id],
                                                       &global_src_macs[port_id],
                                                       &global_tx_dst_macs[port_id],
                                                       udp_src_port, global_dest_port,
                                                       (uint32_t)seq_counter++,
                                                       global_esp_spis[port_id],
                                                       global_esp_salts[port_id],
                                                       global_esp_ivs[port_id]);
            } else {
                ret = fill_mbuf_with_packet(m, dst_ip_i, src_ip_i,
                                          &global_src_macs[port_id], &global_tx_dst_macs[port_id], global_dest_port,
                                          udp_src_port);
            }

            if (ret != 0) {
                rte_pktmbuf_free(m);
                break;
            }
            pending[BATCH_SIZE - pending_count - 1] = m;
            pending_count++;
        }

        // Wait for our turn
        if (use_esp && threads_for_this_port > 1) {
            volatile uint64_t *my_ticket = &global_sync_tokens[port_id][rank].ticket;
            while (*my_ticket != batch_idx && global_running) {
                rte_pause();
            }
        }

        // Transmit pending mbufs
        uint16_t sent = 0;
        while (pending_count > 0) {
            // Use Queue 0 for all threads
            sent = rte_eth_tx_burst(port_id, 0, pending, pending_count);
            pending_count -= sent;
            if (sent == 0) rte_pause();
        }

        // Pass token to next thread
        if (use_esp && threads_for_this_port > 1) {
            uint16_t next_rank = (rank + 1) % threads_for_this_port;
            // If we are the last rank, we increment the batch index for the next round (Rank 0)
            uint64_t next_val = (next_rank == 0) ? (batch_idx + 1) : batch_idx;

            // Ensure strict ordering: Write barrier before updating next token
            rte_smp_wmb();
            global_sync_tokens[port_id][next_rank].ticket = next_val;

            // Update our own sequence counter for next batch
            // We skip (threads_per_port - 1) batches of size BATCH_SIZE
            // Current seq was: batch_idx * (N * 64) + rank * 64
            // Next seq should be: (batch_idx + 1) * (N * 64) + rank * 64
            // Delta is N * 64
            seq_counter += (uint64_t)threads_for_this_port * BATCH_SIZE;
            batch_idx++;
        } else {
             // Single thread case, just increment
             // seq_counter already incremented in loop? No, loop increments by 1 per packet.
             // Wait, loop uses seq_counter++ per packet.
             // So after loop, seq_counter is at end of batch.
             // But for multi-thread, we calculated start seq.
             // Ah, inside the loop: (uint32_t)seq_counter++
             // So seq_counter increases by 64.
             // For multi-thread, we need to jump ahead to skip other threads' ranges.
             // We used 64 sequence numbers.
             // Next batch starts at current + (N-1)*64.
             if (use_esp && threads_for_this_port > 1) {
                 seq_counter += (uint64_t)(threads_for_this_port - 1) * BATCH_SIZE;
             }
        }

        // Throttling using rte_pause() calls
        for (uint32_t i = 0; i < global_pause_calls; i++) {
            rte_pause();
        }

    }
    // Drain any remaining pending mbufs on exit
    for (uint16_t i = 0; i < pending_count; i++) rte_pktmbuf_free(pending[i]);
    EVP_CIPHER_CTX_free(ctx);
    EVP_CIPHER_free(cipher);
    OSSL_LIB_CTX_free(libctx);
    return 0;
}

static int parse_hex_key(const char *hex, uint8_t *key, int max_len) {
    int str_len = strlen(hex);
    if (str_len > max_len * 2) return -1;
    if (str_len % 2 != 0) return -1;

    int len = str_len / 2;
    for (int i = 0; i < len; i++) {
        sscanf(hex + 2*i, "%02hhx", &key[i]);
    }
    return len;
}

int main(int argc, char **argv) {
    int ret, opt, dest_port = PARTIAL_DECRYPTION_TRAFFIC_PORT;
    // Per-TX device overrides
    const char *per_tx_src_ips[64]; memset(per_tx_src_ips, 0, sizeof(per_tx_src_ips)); uint16_t per_tx_src_count = 0;
    const char *per_tx_dst_ips[64]; memset(per_tx_dst_ips, 0, sizeof(per_tx_dst_ips)); uint16_t per_tx_dst_count = 0;
    const char *per_tx_tunnel_ips[64]; memset(per_tx_tunnel_ips, 0, sizeof(per_tx_tunnel_ips)); uint16_t per_tx_tunnel_count = 0;
    const char *per_tx_spis[64]; memset(per_tx_spis, 0, sizeof(per_tx_spis)); uint16_t per_tx_spi_count = 0;
    const char *per_tx_keys[64]; memset(per_tx_keys, 0, sizeof(per_tx_keys)); uint16_t per_tx_key_count = 0;
    const char *per_tx_salts[64]; memset(per_tx_salts, 0, sizeof(per_tx_salts)); uint16_t per_tx_salt_count = 0;
    const char *per_tx_ivs[64]; memset(per_tx_ivs, 0, sizeof(per_tx_ivs)); uint16_t per_tx_iv_count = 0;
    const char *per_tx_dst_macs[64]; memset(per_tx_dst_macs, 0, sizeof(per_tx_dst_macs)); uint16_t per_tx_dst_mac_count = 0;
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"src-ip", required_argument, 0, 's'},
        {"tx-dst-ip", required_argument, 0, 'd'},
        {"size", required_argument, 0, 'z'},
        {"device", required_argument, 0, 'a'},
        {"lcores", required_argument, 0, 'l'},
        {"throttle", required_argument, 0, 't'},
        {"file-prefix", required_argument, 0, 'f'},
        {"rx-device", required_argument, 0, 'R'},
        {"rx-lcores", required_argument, 0, 'L'},
        {"rx-ip", required_argument, 0, 1},
        {"tunnel-ip", required_argument, 0, 'T'},
        {"spi", required_argument, 0, 1001},
        {"key", required_argument, 0, 1002},
        {"salt", required_argument, 0, 1003},
        {"iv", required_argument, 0, 1004},
        {"dst-mac", required_argument, 0, 1006},
        {"vary-src-ip", no_argument, 0, 1007},
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
    // Parse our application arguments first
    while ((opt = getopt_long(argc, argv, "hp:s:d:z:a:l:t:f:R:L:T:M:S", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h': print_usage(argv[0]); return 0;
            case 'p': dest_port = atoi(optarg); break;
            case 's':
                if (per_tx_src_count < 64) per_tx_src_ips[per_tx_src_count++] = optarg;
                break;
            case 'd':
                if (per_tx_dst_count < 64) per_tx_dst_ips[per_tx_dst_count++] = optarg;
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
            case 'T':
                if (per_tx_tunnel_count < 64) per_tx_tunnel_ips[per_tx_tunnel_count++] = optarg;
                break;
            case 1001: // --spi
                if (per_tx_spi_count < 64) per_tx_spis[per_tx_spi_count++] = optarg;
                break;
            case 1002: // --key
                if (per_tx_key_count < 64) per_tx_keys[per_tx_key_count++] = optarg;
                break;
            case 1003: // --salt
                if (per_tx_salt_count < 64) per_tx_salts[per_tx_salt_count++] = optarg;
                break;
            case 1004: // --iv
                if (per_tx_iv_count < 64) per_tx_ivs[per_tx_iv_count++] = optarg;
                break;
            case 1007: // --vary-src-ip
                global_vary_src_ip = 1;
                break;
            case 1006: // --dst-mac
                if (per_tx_dst_mac_count < 64) per_tx_dst_macs[per_tx_dst_mac_count++] = optarg;
                break;
            case 1: // --rx-ip
                if (rx_ip_count < 64) rx_ip_strs[rx_ip_count++] = optarg;
                break;
            case 'S':
                global_show_per_port_stats = 1;
                break;
            default: print_usage(argv[0]); return 1;
        }
    }


    // Build EAL arguments using common function
    eal_argc = dpdk_build_eal_argv(argv[0], tx_devices, tx_device_count,
		rx_devices, rx_device_count, tx_lcores_str, rx_lcores_str,
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
    ret = rte_eal_init(eal_argc, eal_argv);
    if (ret < 0) {
        fprintf(stderr, "Failed to init DPDK EAL\n");
        return 1;
    }

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
    if (tx_device_count == 0) {
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

    if (global_tx_lcores_count == 0 && global_rx_lcores_count == 0) {
        fprintf(stderr, "No worker lcores available.\n");
        return 1;
    }
    printf("Main lcore: %u, TX workers: %u, RX workers: %u\n", rte_get_main_lcore(), global_tx_lcores_count, global_rx_lcores_count);

    // Calculate TX/RX queues per port
    // For ordered sequence generation, we MUST use 1 TX queue per port shared by all threads.
    tx_queues_per_port = (global_tx_lcores_count == 0 || global_num_tx_ports == 0) ? 0 : 1;
    rx_queues_per_port = (!do_rx || global_rx_lcores_count == 0 || global_num_rx_ports == 0) ? 0 : ceil_div(global_rx_lcores_count, global_num_rx_ports);
    if (tx_queues_per_port == 0 && rx_queues_per_port == 0) { fprintf(stderr, "No TX or RX queues requested.\n"); return 1; }
    if (tx_queues_per_port) printf("Setting up %u TX queues/port (shared) for %u TX workers across %u ports\n", tx_queues_per_port, global_tx_lcores_count, global_num_tx_ports);
    if (rx_queues_per_port) printf("Setting up %u RX queues/port for %u RX workers across %u ports\n", rx_queues_per_port, global_rx_lcores_count, global_num_rx_ports);

    // Calculate threads per port and initialize sync tokens
    memset(global_threads_per_port, 0, sizeof(global_threads_per_port));
    for (uint16_t i = 0; i < global_tx_lcores_count; i++) {
        uint16_t port_id = global_tx_port_ids[i % (global_num_tx_ports ? global_num_tx_ports : 1)];
        if (port_id < RTE_MAX_ETHPORTS) {
            global_threads_per_port[port_id]++;
        }
    }

    for (uint16_t p = 0; p < RTE_MAX_ETHPORTS; p++) {
        if (global_threads_per_port[p] > 0) {
            // Initialize token ring: Thread 0 gets 0, others get UINT64_MAX (blocked)
            // But wait, we need to know which thread index maps to which rank.
            // The worker determines its rank based on lcore index.
            // We'll initialize all to UINT64_MAX, and then set rank 0's token to 0.
            for (uint16_t t = 0; t < RTE_MAX_LCORE; t++) {
                global_sync_tokens[p][t].ticket = UINT64_MAX;
            }
            global_sync_tokens[p][0].ticket = 0;
        }
    }

    // Create mbuf pool early (needed for RX queue setup and ARP)
    uint32_t mbuf_pool_size = 65536;
    // Re-parse for -M/--mbufs
    optind = 1;
    while ((opt = getopt_long(argc, argv, "hp:s:d:z:a:l:t:f:R:L:T:M:S", long_options, NULL)) != -1) {
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
    // Allocate ESP Tunnel IP array
    global_esp_tunnel_ips = calloc(num_ports ? num_ports : 1, sizeof(struct in_addr));
    global_esp_spis = calloc(num_ports ? num_ports : 1, sizeof(uint32_t));
    global_esp_keys = calloc(num_ports ? num_ports : 1, 32); // Max 32 bytes per key
    global_esp_key_lens = calloc(num_ports ? num_ports : 1, sizeof(int));
    global_esp_salts = calloc(num_ports ? num_ports : 1, sizeof(uint32_t));
    global_esp_ivs = calloc(num_ports ? num_ports : 1, sizeof(uint64_t));
    if (per_tx_dst_mac_count && per_tx_dst_mac_count != global_num_tx_ports) {
        fprintf(stderr, "Provide one --dst-mac per TX port.\n");
        return 1;
    }

    // Populate per-port source/destination IPs in TX device order
    for (uint16_t i = 0; i < global_num_tx_ports; i++) {
        uint16_t pid = global_tx_port_ids[i];
        if (pid >= num_ports) continue;
        if (per_tx_dst_mac_count &&
            rte_ether_unformat_addr(per_tx_dst_macs[i], &global_tx_dst_macs[pid]) != 0) {
            fprintf(stderr, "Invalid --dst-mac for TX port %u.\n", i);
            return 1;
        }
        struct in_addr s = (struct in_addr){0};
        struct in_addr d = (struct in_addr){0};
        if (i < per_tx_src_count) inet_aton(per_tx_src_ips[i], &s);
        if (i < per_tx_dst_count) inet_aton(per_tx_dst_ips[i], &d);
        global_tx_src_addrs[pid] = s;
        global_tx_dst_addrs[pid] = d;

        // Parse ESP Tunnel IP from -T
        if (i < per_tx_tunnel_count) {
            struct in_addr t = {0};
            inet_aton(per_tx_tunnel_ips[i], &t);
            global_esp_tunnel_ips[pid] = t;
        }

        // Parse ESP SPI
        if (i < per_tx_spi_count) {
            global_esp_spis[pid] = strtoul(per_tx_spis[i], NULL, 0);
        } else {
            global_esp_spis[pid] = 1000; // Default
        }

        // Parse ESP Key
        if (i < per_tx_key_count) {
            int len = parse_hex_key(per_tx_keys[i], &global_esp_keys[pid * 32], 32);
            if (len < 0) {
                fprintf(stderr, "Invalid key format for port %u\n", pid);
                return 1;
            }
            global_esp_key_lens[pid] = len;
        } else {
            // Default key (all zeros, 16 bytes)
            memset(&global_esp_keys[pid * 32], 0, 16);
            global_esp_key_lens[pid] = 16;
        }

        // Parse ESP Salt
        if (i < per_tx_salt_count) {
            global_esp_salts[pid] = strtoul(per_tx_salts[i], NULL, 0);
        } else {
            global_esp_salts[pid] = 0;
        }

        // Parse ESP IV
        if (i < per_tx_iv_count) {
            global_esp_ivs[pid] = strtoull(per_tx_ivs[i], NULL, 0);
        } else {
            global_esp_ivs[pid] = 0;
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
    dpdk_configure_ports(num_ports, global_rx_port_ids, global_num_rx_ports, rx_queues_per_port, global_tx_port_ids, global_num_tx_ports, tx_queues_per_port, mbuf_pool, &global_src_macs);

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
    printf("  Port  SrcMAC               SrcIP          DstIP          DstMAC\n");
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
    struct dpdk_stats port_deltas[RTE_MAX_ETHPORTS];

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
