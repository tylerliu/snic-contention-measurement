// SPDX-License-Identifier: BSD-3-Clause
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <signal.h>

#define AES_BLOCK_SIZE 16
#define AES_IV_SIZE 12
#define AES_TAG_SIZE 12
#define MAX_DATA_SIZE 1280
#define MAX_PACKET_SIZE (AES_IV_SIZE + MAX_DATA_SIZE + AES_TAG_SIZE)
#define MTU_SIZE 1500
#define BATCH_SIZE 64
#define STATS_INTERVAL 1
#define PARTIAL_DECRYPTION_TRAFFIC_PORT 3282
#define DEFAULT_SRC_PORT 12345
#define DEFAULT_SRC_IP "10.0.0.1"

// Global variables for worker threads
static struct rte_mempool *global_mbuf_pool = NULL;
static struct rte_ether_addr *global_src_macs = NULL;
static struct in_addr global_dst_addr, global_src_addr;
static uint16_t global_dest_port;
static uint16_t *global_port_ids = NULL;
static uint16_t global_num_ports = 0;
static volatile int global_running = 1;
static uint16_t num_queues = 0;

// Signal handler for graceful shutdown
static void signal_handler(int signum) {
    printf("\nReceived signal %d, shutting down...\n", signum);
    global_running = 0;
}

static int generate_iv(unsigned char *iv) {
    return RAND_bytes(iv, AES_IV_SIZE) == 1 ? 0 : -1;
}

static int generate_plaintext(int data_size, unsigned char *plaintext) {
    return RAND_bytes(plaintext, data_size) == 1 ? 0 : -1;
}

static int encrypt_data(EVP_CIPHER_CTX *ctx, const unsigned char *key, const unsigned char *iv,
                       const unsigned char *plaintext, int data_size,
                       unsigned char *ciphertext, unsigned char *tag) {
    int len;
    EVP_CIPHER_CTX_reset(ctx);
    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, key, iv) != 1) return -1;
    if (EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, data_size) != 1) return -1;
    if (EVP_EncryptFinal_ex(ctx, ciphertext + len, &len) != 1) return -1;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, AES_TAG_SIZE, tag) != 1) return -1;
    return 0;
}

static int fill_mbuf_with_packet(struct rte_mbuf *mbuf, EVP_CIPHER_CTX *ctx, const unsigned char *key, 
                                struct in_addr dst_addr, struct in_addr src_addr, 
                                struct rte_ether_addr *src_mac, uint16_t dest_port) {
    unsigned char iv[AES_IV_SIZE], plaintext[MAX_DATA_SIZE], ciphertext[MAX_DATA_SIZE], tag[AES_TAG_SIZE];
    int data_size = (rand() % MAX_DATA_SIZE) + 1;
    if (generate_iv(iv) != 0 || generate_plaintext(data_size, plaintext) != 0 || encrypt_data(ctx, key, iv, plaintext, data_size, ciphertext, tag) != 0) {
        return -1;
    }
    uint8_t *pkt = rte_pktmbuf_mtod(mbuf, uint8_t *);
    struct {
        struct rte_ether_hdr eth;
        struct rte_ipv4_hdr ip;
        struct rte_udp_hdr udp;
        uint8_t payload[MAX_PACKET_SIZE];
    } __attribute__((__packed__, aligned(2))) *frame = (void *)pkt;
    memset(frame, 0, sizeof(*frame));
    memset(frame->eth.dst_addr.addr_bytes, 0xff, 6);
    memcpy(frame->eth.src_addr.addr_bytes, src_mac->addr_bytes, 6);
    frame->eth.ether_type = htons(0x0800);
    frame->ip.version_ihl = 0x45;
    frame->ip.type_of_service = 0;
    frame->ip.total_length = htons(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + AES_IV_SIZE + data_size + AES_TAG_SIZE);
    frame->ip.packet_id = 0;
    frame->ip.fragment_offset = 0;
    frame->ip.time_to_live = 64;
    frame->ip.next_proto_id = IPPROTO_UDP;
    frame->ip.src_addr = src_addr.s_addr;
    frame->ip.dst_addr = dst_addr.s_addr;
    frame->udp.src_port = htons(DEFAULT_SRC_PORT);
    frame->udp.dst_port = htons(dest_port);
    frame->udp.dgram_len = htons(sizeof(struct rte_udp_hdr) + AES_IV_SIZE + data_size + AES_TAG_SIZE);
    frame->udp.dgram_cksum = 0;
    memcpy(frame->payload, iv, AES_IV_SIZE);
    memcpy(frame->payload + AES_IV_SIZE, ciphertext, data_size);
    memcpy(frame->payload + AES_IV_SIZE + data_size, tag, AES_TAG_SIZE);
    int pkt_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + AES_IV_SIZE + data_size + AES_TAG_SIZE;
    mbuf->data_len = pkt_size;
    mbuf->pkt_len = pkt_size;
    return 0;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS] <Destination IP>\n", prog);
    printf("  -p, --port PORT      Destination port (default: %d)\n", PARTIAL_DECRYPTION_TRAFFIC_PORT);
    printf("  -s, --src-ip IP      Source IP address (default: %s)\n", DEFAULT_SRC_IP);
    printf("  -a, --device DEVICE  Device to use (can specify multiple times)\n");
    printf("  -l, --lcores LCORES  Logical cores to use (e.g., 0-3, 0,2,4)\n");
    printf("  -h, --help           Show this help\n");
}

// Worker function for packet generation
static int packet_worker(__rte_unused void *dummy) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        fprintf(stderr, "Failed to create cipher context\n");
        return -1;
    }
    
    unsigned char key[AES_BLOCK_SIZE] = {0};
    unsigned long long packets_this_second = 0, bytes_this_second = 0;
    const uint64_t stats_hz = rte_get_timer_hz();
    uint64_t last_tsc = rte_get_timer_cycles();
    
    uint16_t queue_id = rte_lcore_id() - 1; // Queue 0 for lcore 1, queue 1 for lcore 2, etc.
    uint16_t port_id = queue_id % global_num_ports; // Distribute across ports
    uint16_t port_queue_id = queue_id / global_num_ports; // Queue within each port
    
    printf("Worker thread %u started on core %u using port %u queue %u\n", 
           rte_lcore_id(), rte_get_next_lcore(-1, 1, 0), port_id, port_queue_id);
    
    while (global_running) {
        struct rte_mbuf *mbufs[BATCH_SIZE];
        uint16_t nb = 0;
        for (int i = 0; i < BATCH_SIZE; i++) {
            struct rte_mbuf *m = rte_pktmbuf_alloc(global_mbuf_pool);
            if (!m) continue;
            if (fill_mbuf_with_packet(m, ctx, key, global_dst_addr, global_src_addr, &global_src_macs[port_id], global_dest_port) != 0) {
                rte_pktmbuf_free(m);
                continue;
            }
            mbufs[nb++] = m;
        }
        uint16_t sent = 0;
        if (nb > 0) {
            sent = rte_eth_tx_burst(global_port_ids[port_id], port_queue_id, mbufs, nb);
            for (uint16_t i = sent; i < nb; i++) rte_pktmbuf_free(mbufs[i]);
        }
        packets_this_second += sent;
        bytes_this_second += sent * (sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + AES_IV_SIZE + MAX_DATA_SIZE + AES_TAG_SIZE);
        uint64_t now_tsc = rte_get_timer_cycles();
        uint64_t elapsed = now_tsc - last_tsc;
        if (elapsed >= STATS_INTERVAL * stats_hz) {
            printf("Core %u (Queue %u): %.1f packets/sec (%.1f MB/sec)\n", 
                   rte_lcore_id(), queue_id, (double)packets_this_second / elapsed * stats_hz, 
                   (((double)bytes_this_second) / elapsed * stats_hz) / (1 << 20));
            packets_this_second = 0;
            bytes_this_second = 0;
            last_tsc = now_tsc;
        }
    }
    
    EVP_CIPHER_CTX_free(ctx);
    return 0;
}

int main(int argc, char **argv) {
    int ret, opt, dest_port = PARTIAL_DECRYPTION_TRAFFIC_PORT;
    const char *dest_ip = NULL;
    const char *src_ip = DEFAULT_SRC_IP;
    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"src-ip", required_argument, 0, 's'},
        {"device", required_argument, 0, 'a'},
        {"lcores", required_argument, 0, 'l'},
        {0, 0, 0, 0}
    };
    
    // Collect DPDK arguments
    char *eal_argv[argc];
    int eal_argc = 0;
    eal_argv[eal_argc++] = argv[0]; // Program name
    
    // Parse our application arguments first
    while ((opt = getopt_long(argc, argv, "hp:s:a:l:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h': print_usage(argv[0]); return 0;
            case 'p': dest_port = atoi(optarg); break;
            case 's': src_ip = optarg; break;
            case 'a': 
                eal_argv[eal_argc++] = "-a";
                eal_argv[eal_argc++] = optarg;
                break;
            case 'l': 
                eal_argv[eal_argc++] = "-l";
                eal_argv[eal_argc++] = optarg;
                break;
            default: print_usage(argv[0]); return 1;
        }
    }
    
    if (optind >= argc) {
        fprintf(stderr, "Destination IP required\n");
        print_usage(argv[0]);
        return 1;
    }
    dest_ip = argv[optind];
    
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
    
    // Print information about all available ports
    for (uint16_t i = 0; i < num_ports; i++) {
        if (rte_eth_dev_is_valid_port(i)) {
            struct rte_ether_addr mac_addr;
            ret = rte_eth_macaddr_get(i, &mac_addr);
            if (ret == 0) {
                printf("Port %d: MAC %02x:%02x:%02x:%02x:%02x:%02x\n", 
                       i, mac_addr.addr_bytes[0], mac_addr.addr_bytes[1], 
                       mac_addr.addr_bytes[2], mac_addr.addr_bytes[3], 
                       mac_addr.addr_bytes[4], mac_addr.addr_bytes[5]);
            } else {
                printf("Port %d: MAC address unavailable\n", i);
            }
        }
    }
    
    // Use all available ports
    uint16_t num_ports_to_use = num_ports;
    printf("Using %d ports (0-%d)\n", num_ports_to_use, num_ports_to_use - 1);
    
    // Count available lcores for queue setup
    uint16_t total_lcores = rte_lcore_count();
    uint16_t main_lcore = rte_get_main_lcore();
    num_queues = total_lcores - 1; // Exclude main lcore
    
    printf("Total lcores: %d, Main lcore: %d, Worker lcores: %d\n", 
           total_lcores, main_lcore, num_queues);
    
    if (num_queues == 0) {
        fprintf(stderr, "No worker lcores available. Check -l option.\n");
        fprintf(stderr, "Available lcores: ");
        for (uint16_t i = 0; i < total_lcores; i++) {
            if (rte_lcore_is_enabled(i)) {
                printf("%d ", i);
            }
        }
        printf("\n");
        fprintf(stderr, "Try specifying cores with -l option, e.g.: -l 0-3\n");
        return 1;
    }
    
    // Calculate queues per port
    uint16_t queues_per_port = num_queues / num_ports_to_use;
    if (queues_per_port == 0) {
        fprintf(stderr, "Not enough queues (%d) for %d ports. Need at least %d queues.\n", 
                num_queues, num_ports_to_use, num_ports_to_use);
        return 1;
    }
    
    printf("Setting up %d TX queues per port (%d total) for %d worker cores\n", 
           queues_per_port, queues_per_port * num_ports_to_use, num_queues);
    
    // Configure and start all ports
    struct rte_eth_conf port_conf = { .rxmode = { .mtu = 1518 } };
    struct rte_ether_addr *src_macs = malloc(num_ports_to_use * sizeof(struct rte_ether_addr));
    
    for (uint16_t port_id = 0; port_id < num_ports_to_use; port_id++) {
        if (!rte_eth_dev_is_valid_port(port_id)) {
            fprintf(stderr, "Port %d is not valid\n", port_id);
            free(src_macs);
            return 1;
        }
        
        ret = rte_eth_dev_configure(port_id, 0, queues_per_port, &port_conf);
        if (ret < 0) {
            fprintf(stderr, "Failed to configure port %d with %d TX queues\n", port_id, queues_per_port);
            free(src_macs);
            return 1;
        }
        
        // Setup TX queues for this port
        struct rte_eth_txconf txq_conf = {0};
        for (uint16_t q = 0; q < queues_per_port; q++) {
            ret = rte_eth_tx_queue_setup(port_id, q, 1024, rte_eth_dev_socket_id(port_id), &txq_conf);
            if (ret < 0) {
                fprintf(stderr, "Failed to setup TX queue %d on port %d\n", q, port_id);
                free(src_macs);
                return 1;
            }
        }
        
        ret = rte_eth_dev_start(port_id);
        if (ret < 0) {
            fprintf(stderr, "Failed to start port %d (ret=%d). This may be due to:\n", port_id, ret);
            fprintf(stderr, "  - Device not properly bound to DPDK\n");
            fprintf(stderr, "  - Mellanox flow setup issues\n");
            fprintf(stderr, "  - Insufficient permissions\n");
            free(src_macs);
            return 1;
        }
        
        // Get MAC address from the port
        ret = rte_eth_macaddr_get(port_id, &src_macs[port_id]);
        if (ret < 0) {
            fprintf(stderr, "Failed to get MAC address from port %d\n", port_id);
            free(src_macs);
            return 1;
        }
        
        printf("Port %d: MAC %02x:%02x:%02x:%02x:%02x:%02x\n", 
               port_id, src_macs[port_id].addr_bytes[0], src_macs[port_id].addr_bytes[1], 
               src_macs[port_id].addr_bytes[2], src_macs[port_id].addr_bytes[3], 
               src_macs[port_id].addr_bytes[4], src_macs[port_id].addr_bytes[5]);
    }
    
    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", 8192, 256, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!mbuf_pool) {
        fprintf(stderr, "Failed to create mbuf pool\n");
        return 1;
    }
    
    struct in_addr dst_addr, src_addr;
    inet_aton(dest_ip, &dst_addr);
    inet_aton(src_ip, &src_addr);
    
    printf("DPDK AES-GCM Packet Generator\n");
    printf("  Destination: %s:%d\n", dest_ip, dest_port);
    printf("  Source IP: %s\n", src_ip);
    printf("  Number of ports: %d\n", num_ports_to_use);
    printf("  Cores: Multi-core (all available lcores)\n");
    printf("  Rate: Maximum (no sleep)\n");
    printf("  Batch size: %d\n", BATCH_SIZE);
    
    // Global variables for worker threads
    global_mbuf_pool = mbuf_pool;
    global_src_macs = src_macs; // Use the allocated array from above
    global_port_ids = malloc(num_ports_to_use * sizeof(uint16_t));
    global_num_ports = num_ports_to_use;

    for (uint16_t i = 0; i < num_ports_to_use; i++) {
        global_port_ids[i] = i;
    }

    global_dst_addr = dst_addr;
    global_src_addr = src_addr;
    global_dest_port = dest_port;
    
    // Launch worker threads on all available lcores except the main lcore
    ret = rte_eal_mp_remote_launch(packet_worker, NULL, SKIP_MAIN);
    if (ret < 0) {
        fprintf(stderr, "Failed to launch worker threads\n");
        return 1;
    }
    
    // Wait for worker threads to finish
    rte_eal_mp_wait_lcore();
    
    // Stop and close all ports
    printf("Stopping all ports...\n");
    for (uint16_t i = 0; i < global_num_ports; i++) {
        printf("Stopping port %d...\n", global_port_ids[i]);
        rte_eth_dev_stop(global_port_ids[i]);
        rte_eth_dev_close(global_port_ids[i]);
    }
    
    rte_eal_cleanup();
    free(global_src_macs);
    free(global_port_ids);
    return 0;
}
