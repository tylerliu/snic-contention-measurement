// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdint.h>
#include <netinet/in.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ether.h>

#ifdef __cplusplus
extern "C" {
#endif

// Cacheline spacing helpers for lock-free counters
#ifndef COUNTER_CACHELINE_SIZE
#define COUNTER_CACHELINE_SIZE 64
#endif
#ifdef __cplusplus
static_assert((COUNTER_CACHELINE_SIZE % sizeof(uint64_t)) == 0,
              "COUNTER_CACHELINE_SIZE must be a multiple of sizeof(uint64_t)");
#else
_Static_assert((COUNTER_CACHELINE_SIZE % sizeof(uint64_t)) == 0, "COUNTER_CACHELINE_SIZE must be a multiple of sizeof(uint64_t)");
#endif
#define COUNTER_STRIDE_U64 (COUNTER_CACHELINE_SIZE / sizeof(uint64_t))

#include <rte_ip.h>
#include <rte_udp.h>

struct dpdk_port_sets {
	uint16_t *tx_port_ids;
	uint16_t num_tx_ports;
	uint16_t *rx_port_ids;
	uint16_t num_rx_ports;
};

struct rx_dest_stats {
    uint64_t pkts;
    uint64_t bytes;
} __rte_cache_aligned;

struct dpdk_runtime {
	struct rte_mempool *mbuf_pool;
	struct rte_ether_addr *src_macs; // sized to num_ports
	uint16_t num_ports;
};

struct dpdk_stats {
	uint64_t ipackets;
	uint64_t opackets;
	uint64_t ibytes;
	uint64_t obytes;
	uint64_t imissed;
	uint64_t ierrors;
	uint64_t oerrors;
	uint64_t rx_nombuf;
};

#define ARP_HRD_ETHER 0x0001
#define ARP_PRO_IPV4  0x0800
#define ARP_OP_REQUEST 0x0001
#define ARP_OP_REPLY   0x0002

struct arp_ipv4_payload {
	uint16_t hrd;
	uint16_t pro;
	uint8_t  hln;
	uint8_t  pln;
	uint16_t op;
	struct rte_ether_addr sha;
	uint32_t sip;
	struct rte_ether_addr tha;
	uint32_t tip;
} __attribute__((__packed__, aligned(2)));

int dpdk_init_eal_from_devices_and_lcores(
	int argc, char **argv,
	const char **tx_devices, uint16_t tx_device_count,
	const char **rx_devices, uint16_t rx_device_count,
	const char *tx_lcores_str, const char *rx_lcores_str,
	const char *file_prefix,
	char eal_args_buf[][128], int *out_argc);

int dpdk_match_ports_by_bdf(const char **devices, uint16_t device_count,
	uint16_t *out_port_ids, uint16_t *out_num_ports);

int dpdk_configure_ports(uint16_t total_ports,
	const uint16_t *rx_port_ids, uint16_t num_rx_ports, uint16_t rx_queues_per_port,
	const uint16_t *tx_port_ids, uint16_t num_tx_ports, uint16_t tx_queues_per_port,
	struct rte_mempool *mbuf_pool,
	struct rte_ether_addr **out_src_macs);

struct rte_mempool *dpdk_create_mbuf_pool(const char *name, uint32_t num_mbufs, uint32_t cache_size);

int dpdk_resolve_arp_for_port(uint16_t port_id,
	const struct rte_ether_addr *src_mac,
	struct in_addr src_ip, struct in_addr dst_ip,
	struct rte_ether_addr *out_dst_mac,
	struct rte_mempool *mbuf_pool,
	uint32_t timeout_ms);

// Tiny helpers
static inline int dpdk_is_mac_unresolved(const struct rte_ether_addr *addr) {
	for (int i = 0; i < RTE_ETHER_ADDR_LEN; i++) if (addr->addr_bytes[i] != 0x00) return 0;
	return 1;
}

// Parse lcore string like "0-3,5,7-8" into a list
int dpdk_parse_lcore_set(const char *str, uint16_t *out, uint16_t *out_count);

// Parse "IP:PORT" into addr and port (returns 0 on success)
int dpdk_parse_ip_port(const char *str, struct in_addr *out_ip, uint16_t *out_port);

// Create a VXLAN decap rte_flow on a port (ingress) matching outer dst IP/port
// Returns 0 on success and stores flow handle in *out_flow
int dpdk_install_vxlan_decap_flow(uint16_t port_id,
	struct in_addr outer_dst_ip,
	uint16_t outer_dst_port,
	struct rte_flow **out_flow,
	char *errbuf, size_t errbuf_len);

// Build EAL argv from devices and lcores (RX devices first, then TX devices, union of lcores)
// Returns number of arguments added, or -1 on error
// eal_argv must have space for at least (argc + 128) pointers
int dpdk_build_eal_argv(const char *prog_name,
	const char **tx_devices, uint16_t tx_device_count,
	const char **rx_devices, uint16_t rx_device_count,
	const char *tx_lcores_str, const char *rx_lcores_str,
	const char *file_prefix,
	char **eal_argv, int max_args);

// Filter lcores: remove main lcore, ensure TX and RX are disjoint
// Returns 0 on success, -1 on error
int dpdk_filter_lcores(uint16_t *tx_lcores, uint16_t *tx_count,
	uint16_t *rx_lcores, uint16_t *rx_count);

void dpdk_stats_cache_reset(struct dpdk_stats *cache, uint16_t count);

int dpdk_stats_get_delta(uint16_t port_id,
	struct dpdk_stats *snapshot,
	struct dpdk_stats *delta);

void dpdk_print_port_stats(const char *label,
	const uint16_t *port_ids,
	uint16_t port_count,
	double interval_secs,
	int is_tx_direction,
	int show_per_port,
	const struct dpdk_stats *deltas);

int dpdk_handle_arp_request(uint16_t port_id,
	struct rte_mbuf *mbuf,
	struct rte_ether_addr *port_macs,
	struct in_addr *port_ips,
	uint16_t tx_queue_id);

struct dpdk_shared_rx_worker_ctx {
	volatile int *running;
	uint16_t port_id;
	uint16_t rx_queue_id;
	uint16_t tx_queue_id;
	uint16_t filter_udp_port; // 0 = no filter
	int warn_on_mismatch;
	struct rte_ether_addr *port_macs; // optional
	struct in_addr *rx_ip_addrs;       // optional per port
	struct in_addr monitor_arp_ip;     // If set, look for ARP reply from this IP
	struct rte_ether_addr *monitor_arp_result; // Write resolved MAC here

	struct rx_dest_stats *dest_stats_1;
	uint16_t dest_port_1;
	struct rx_dest_stats *dest_stats_2;
	uint16_t dest_port_2;

    uint64_t *rx_non_matching_pkts; // Counter for packets not matching any configured port
    
    // Auxiliary ports to poll for ARP requests (e.g. TX-only ports)
    volatile int *aux_polling_enabled; // If NULL or points to 0, skip aux polling
    uint16_t *aux_rx_ports;
    uint16_t aux_rx_ports_count;
};

int dpdk_shared_rx_worker(void *arg);

#ifdef __cplusplus
}
#endif



