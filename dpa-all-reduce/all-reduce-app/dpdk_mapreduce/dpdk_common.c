// SPDX-License-Identifier: BSD-3-Clause
#include "dpdk_common.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <rte_flow.h>
#include <rte_vxlan.h>
#include <rte_ip.h>
#include <rte_udp.h>

struct rte_mempool *dpdk_create_mbuf_pool(const char *name, uint32_t num_mbufs, uint32_t cache_size) {
    const char *pool_name = name && name[0] ? name : "MBUF_POOL";
    uint32_t nb_mbuf = (num_mbufs == 0) ? 65536 : num_mbufs;
    uint32_t cache = (cache_size == 0) ? 512 : cache_size;
    struct rte_mempool *pool = rte_pktmbuf_pool_create(pool_name,
                                                      nb_mbuf,
                                                      cache,
                                                      0,
                                                      RTE_MBUF_DEFAULT_BUF_SIZE,
                                                      rte_socket_id());
    if (!pool) {
        fprintf(stderr, "Failed to create mbuf pool %s\n", pool_name);
    }
    return pool;
}

static inline int ends_with_ci(const char *s, const char *suffix) {
    size_t ls = strlen(s), lsf = strlen(suffix);
    if (lsf > ls) return 0;
    return strncasecmp(s + (ls - lsf), suffix, lsf) == 0;
}

static inline void extract_bdf_base(const char *spec, char *out, size_t out_sz) {
    size_t i = 0;
    while (spec[i] && spec[i] != ',' && i + 1 < out_sz) { out[i] = spec[i]; i++; }
    out[i] = '\0';
}

static int port_matches_bdf(uint16_t port_id, const char *bdf_spec) {
    struct rte_eth_dev_info info;
    memset(&info, 0, sizeof(info));
    rte_eth_dev_info_get(port_id, &info);
    if (!info.device) return 0;
    const char *dev_name = rte_dev_name(info.device);
    if (!dev_name) return 0;
    char bdf_only[64];
    extract_bdf_base(bdf_spec, bdf_only, sizeof(bdf_only));
    if (bdf_only[0] == '\0') return 0;
    if (ends_with_ci(dev_name, bdf_only)) return 1;
    if (strchr(bdf_only, ':') && strncmp(bdf_only, "0000:", 5) != 0) {
        char with_domain[72];
        size_t bl = strlen(bdf_only);
        size_t max_copy = sizeof(with_domain) - 1 - 5;
        if (bl > max_copy) bl = max_copy;
        memcpy(with_domain, "0000:", 5);
        memcpy(with_domain + 5, bdf_only, bl);
        with_domain[5 + bl] = '\0';
        if (ends_with_ci(dev_name, with_domain)) return 1;
    }
    if (strncmp(bdf_only, "0000:", 5) == 0) {
        const char *no_dom = bdf_only + 5;
        if (ends_with_ci(dev_name, no_dom)) return 1;
    }
    return 0;
}

int dpdk_configure_ports(uint16_t total_ports, const uint16_t *rx_port_ids,
                         uint16_t num_rx_ports, uint16_t rx_queues_per_port,
                         const uint16_t *tx_port_ids, uint16_t num_tx_ports,
                         uint16_t tx_queues_per_port,
                         struct rte_mempool *mbuf_pool,
                         struct rte_ether_addr **out_src_macs) {

  struct rte_eth_conf port_conf;
  memset(&port_conf, 0, sizeof(port_conf));
  port_conf.rxmode.mtu = 1518;
  port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
  port_conf.rx_adv_conf.rss_conf.rss_key = NULL;
  port_conf.rx_adv_conf.rss_conf.rss_key_len = 0;
  port_conf.rx_adv_conf.rss_conf.rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP;
  port_conf.rxmode.offloads = RTE_ETH_RX_OFFLOAD_RSS_HASH;
  port_conf.txmode.offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                              RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                              RTE_ETH_TX_OFFLOAD_OUTER_IPV4_CKSUM;

  struct rte_ether_addr *src_macs =
      malloc(total_ports * sizeof(struct rte_ether_addr));
  if (!src_macs) {
    fprintf(stderr, "Failed to allocate src_macs array\n");
    return -1;
  }

  for (uint16_t port_id = 0; port_id < total_ports; port_id++) {
    if (!rte_eth_dev_is_valid_port(port_id)) {
      continue;
    }

    uint16_t rxq = 0, txq = 0;
    if (rx_port_ids) {
      for (uint16_t i = 0; i < num_rx_ports; i++)
        if (rx_port_ids[i] == port_id) {
          rxq = rx_queues_per_port;
          break;
        }
    }
    if (tx_port_ids) {
      for (uint16_t i = 0; i < num_tx_ports; i++)
        if (tx_port_ids[i] == port_id) {
          txq = tx_queues_per_port;
          break;
        }
    }

    if (rxq == 0 && txq == 0)
      continue;

    if (rxq == 0)
      rxq = 1; // Dummy RX queue
    if (txq == 0)
      txq = 1; // Dummy TX queue

    int ret = rte_eth_dev_configure(port_id, rxq, txq, &port_conf);
    if (ret < 0) {
      fprintf(stderr, "Failed to configure port %d with RX %u / TX %u queues\n",
              port_id, rxq, txq);
      free(src_macs);
      return -1;
    }

    struct rte_eth_rxconf rxq_conf = {0};
    if (!mbuf_pool && rxq > 0) {
      fprintf(stderr, "Mbuf pool null but RX queues requested\n");
      free(src_macs);
      return -1;
    }

    for (uint16_t q = 0; q < rxq; q++) {
      ret = rte_eth_rx_queue_setup(port_id, q, 1024,
                                   rte_eth_dev_socket_id(port_id), &rxq_conf,
                                   mbuf_pool);
      if (ret < 0) {
        fprintf(stderr, "Failed to setup RX queue %u on port %u\n", q, port_id);
        free(src_macs);
        return -1;
      }
    }

    struct rte_eth_txconf txq_conf = {0};
    txq_conf.offloads = port_conf.txmode.offloads;
    for (uint16_t q = 0; q < txq; q++) {
      ret = rte_eth_tx_queue_setup(port_id, q, 1024,
                                   rte_eth_dev_socket_id(port_id), &txq_conf);
      if (ret < 0) {
        fprintf(stderr, "Failed to setup TX queue %u on port %u\n", q, port_id);
        free(src_macs);
        return -1;
      }
    }

    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
      fprintf(stderr, "Failed to start port %d\n", port_id);
      free(src_macs);
      return -1;
    }
    rte_eth_promiscuous_enable(port_id);

    ret = rte_eth_macaddr_get(port_id, &src_macs[port_id]);
    if (ret < 0) {
      fprintf(stderr, "Failed to get MAC address from port %d\n", port_id);
      free(src_macs);
      return -1;
    }

    printf("Port %d: MAC %02x:%02x:%02x:%02x:%02x:%02x\n", port_id,
           src_macs[port_id].addr_bytes[0], src_macs[port_id].addr_bytes[1],
           src_macs[port_id].addr_bytes[2], src_macs[port_id].addr_bytes[3],
           src_macs[port_id].addr_bytes[4], src_macs[port_id].addr_bytes[5]);
  }

  if (out_src_macs)
    *out_src_macs = src_macs;
  else
    free(src_macs);

  return 0;
}

int dpdk_match_ports_by_bdf(const char **devices, uint16_t device_count,
	uint16_t *out_port_ids, uint16_t *out_num_ports) {
    if (!out_port_ids || !out_num_ports) return -1;
    uint16_t num_ports = rte_eth_dev_count_avail();
    uint16_t matched = 0;
    uint8_t used[RTE_MAX_ETHPORTS];
    memset(used, 0, sizeof(used));
    for (uint16_t d = 0; d < device_count; d++) {
        for (uint16_t i = 0; i < num_ports; i++) {
            if (!used[i] && port_matches_bdf(i, devices[d])) {
                out_port_ids[matched++] = i;
                used[i] = 1;
                break;
            }
        }
    }
    *out_num_ports = matched;
    return matched ? 0 : -1;
}

int dpdk_parse_lcore_set(const char *str, uint16_t *out, uint16_t *out_count) {
    if (!str || !out || !out_count) return -1;
    char buf[256];
    memset(buf, 0, sizeof(buf));
    strncpy(buf, str, sizeof(buf) - 1);
    char *saveptr = NULL;
    char *token = strtok_r(buf, ",", &saveptr);
    uint16_t count = 0;
    while (token && count < 256) {
        int a = -1, b = -1;
        if (sscanf(token, "%d-%d", &a, &b) == 2) {
            if (a > b || a < 0 || b >= RTE_MAX_LCORE) return -1;
            for (int v = a; v <= b && count < 256; v++) out[count++] = (uint16_t)v;
        } else if (sscanf(token, "%d", &a) == 1) {
            if (a < 0 || a >= RTE_MAX_LCORE) return -1;
            out[count++] = (uint16_t)a;
        } else {
            return -1;
        }
        token = strtok_r(NULL, ",", &saveptr);
    }
    *out_count = count;
    return 0;
}

int dpdk_parse_ip_port(const char *str, struct in_addr *out_ip, uint16_t *out_port) {
    if (!str || !out_ip || !out_port) return -1;
    char buf[128]; memset(buf, 0, sizeof(buf));
    strncpy(buf, str, sizeof(buf) - 1);
    char *colon = strchr(buf, ':');
    if (!colon) return -1;
    *colon = '\0';
    struct in_addr ip = {0};
    if (!inet_aton(buf, &ip)) return -1;
    long p = strtol(colon + 1, NULL, 10);
    if (p <= 0 || p > 65535) return -1;
    *out_ip = ip;
    *out_port = (uint16_t)p;
    return 0;
}

int dpdk_install_vxlan_decap_flow(uint16_t port_id,
    struct in_addr outer_dst_ip,
    uint16_t outer_dst_port,
    struct rte_flow **out_flow,
    char *errbuf, size_t errbuf_len) {
    if (!out_flow) return -1;
    *out_flow = NULL;
    struct rte_flow_attr attr; memset(&attr, 0, sizeof(attr));
    attr.ingress = 1; attr.egress = 0; attr.transfer = 0; attr.group = 0; attr.priority = 0;

    // Pattern: ETH / IPv4 dst=outer_dst_ip / UDP dst=outer_dst_port / VXLAN (I flag)
    struct rte_flow_item_eth eth_any; memset(&eth_any, 0, sizeof(eth_any));

    struct rte_flow_item_ipv4 ip4; memset(&ip4, 0, sizeof(ip4));
    ip4.hdr.dst_addr = outer_dst_ip.s_addr;
    ip4.hdr.next_proto_id = IPPROTO_UDP;
    struct rte_flow_item_ipv4 ip4_mask; memset(&ip4_mask, 0, sizeof(ip4_mask));
    ip4_mask.hdr.dst_addr = 0xFFFFFFFFu;
    ip4_mask.hdr.next_proto_id = 0xFF;

    struct rte_flow_item_udp udp; memset(&udp, 0, sizeof(udp));
    udp.hdr.dst_port = rte_cpu_to_be_16(outer_dst_port);
    struct rte_flow_item_udp udp_mask; memset(&udp_mask, 0, sizeof(udp_mask));
    udp_mask.hdr.dst_port = 0xFFFF;

    struct rte_flow_item_vxlan vx_spec_hdr; memset(&vx_spec_hdr, 0, sizeof(vx_spec_hdr));
    vx_spec_hdr.hdr.flags = 0x08; // I flag set
    struct rte_flow_item_vxlan vx_mask_hdr = rte_flow_item_vxlan_mask;

    struct rte_flow_item pattern[6]; memset(pattern, 0, sizeof(pattern));
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;  pattern[0].spec = &eth_any; pattern[0].mask = &eth_any;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4; pattern[1].spec = &ip4;      pattern[1].mask = &ip4_mask;
    pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;  pattern[2].spec = &udp;      pattern[2].mask = &udp_mask;
    pattern[3].type = RTE_FLOW_ITEM_TYPE_VXLAN;pattern[3].spec = &vx_spec_hdr; pattern[3].mask = &vx_mask_hdr;
    pattern[4].type = RTE_FLOW_ITEM_TYPE_END;

    // Get number of RX queues for this port
    // Query device info to get maximum supported RX queues
    struct rte_eth_dev_info dev_info;
    memset(&dev_info, 0, sizeof(dev_info));
    rte_eth_dev_info_get(port_id, &dev_info);
    uint16_t num_rx_queues = dev_info.nb_rx_queues;
    // nb_rx_queues is the maximum supported, but we want all configured queues
    // Since the port is already configured, we'll use the maximum supported
    // (which should be >= configured). If 0, fall back to 1.
    if (num_rx_queues == 0) {
        num_rx_queues = 1; // at least 1
    }
    // Cap at RTE_MAX_QUEUES_PER_PORT to avoid array overflow
    if (num_rx_queues > RTE_MAX_QUEUES_PER_PORT) {
        num_rx_queues = RTE_MAX_QUEUES_PER_PORT;
    }

    struct rte_flow_action_rss rss_id; memset(&rss_id, 0, sizeof(rss_id));
    rss_id.key = NULL;
    rss_id.key_len = 0;
    rss_id.types = RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP;
    uint16_t queues[RTE_MAX_QUEUES_PER_PORT] = {0};
    // Populate all RX queue indices (0, 1, 2, ..., num_rx_queues-1)
    for (uint16_t i = 0; i < num_rx_queues; i++) {
        queues[i] = i;
    }
    rss_id.queue = queues;
    rss_id.queue_num = num_rx_queues;

    struct rte_flow_action actions[3]; memset(actions, 0, sizeof(actions));
    actions[0].type = RTE_FLOW_ACTION_TYPE_VXLAN_DECAP; actions[0].conf = NULL;
    // Keep default RX pipeline after decapsulation
    actions[1].type = RTE_FLOW_ACTION_TYPE_RSS; actions[1].conf = &rss_id; // TODO rss
    actions[2].type = RTE_FLOW_ACTION_TYPE_END;

    struct rte_flow_error error; memset(&error, 0, sizeof(error));
    struct rte_flow *flow = rte_flow_create(port_id, &attr, pattern, actions, &error);
    if (!flow) {
        if (errbuf && errbuf_len) snprintf(errbuf, errbuf_len, "type=%d msg=%s", error.type, error.message ? error.message : "(none)");
        return -1;
    }
    *out_flow = flow;
    return 0;
}

static inline int lcore_in_list(uint16_t lc, const uint16_t *list, uint16_t count) {
    for (uint16_t i = 0; i < count; i++) if (list[i] == lc) return 1;
    return 0;
}

int dpdk_build_eal_argv(const char *prog_name,
	const char **tx_devices, uint16_t tx_device_count,
	const char **rx_devices, uint16_t rx_device_count,
	const char *tx_lcores_str, const char *rx_lcores_str,
	const char *file_prefix,
	char **eal_argv, int max_args) {
    if (!prog_name || !eal_argv || max_args < 1) return -1;
    
    int argc = 0;
    eal_argv[argc++] = (char *)prog_name;
    
    // Add file-prefix
    if (file_prefix && argc + 2 < max_args) {
        eal_argv[argc++] = (char *)"--file-prefix";
        eal_argv[argc++] = (char *)file_prefix;
    }
    
    // Add RX devices first (to preserve low port IDs)
    for (uint16_t i = 0; i < rx_device_count && argc + 2 < max_args; i++) {
        eal_argv[argc++] = (char *)"-a";
        eal_argv[argc++] = (char *)rx_devices[i];
    }
    
    // Add TX devices that are not already in RX list (case-insensitive comparison)
    for (uint16_t i = 0; i < tx_device_count && argc + 2 < max_args; i++) {
        int found = 0;
        for (uint16_t j = 0; j < rx_device_count; j++) {
            if (strcasecmp(tx_devices[i], rx_devices[j]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            eal_argv[argc++] = (char *)"-a";
            eal_argv[argc++] = (char *)tx_devices[i];
        }
    }
    
    // Union lcores (TX and RX)
    if ((tx_lcores_str || rx_lcores_str) && argc + 2 < max_args) {
        uint8_t mask[RTE_MAX_LCORE];
        memset(mask, 0, sizeof(mask));
        uint16_t tmp[256], tmp_n = 0;
        
        if (tx_lcores_str && dpdk_parse_lcore_set(tx_lcores_str, tmp, &tmp_n) == 0) {
            for (uint16_t i = 0; i < tmp_n; i++) if (tmp[i] < RTE_MAX_LCORE) mask[tmp[i]] = 1;
        }
        tmp_n = 0;
        if (rx_lcores_str && dpdk_parse_lcore_set(rx_lcores_str, tmp, &tmp_n) == 0) {
            for (uint16_t i = 0; i < tmp_n; i++) if (tmp[i] < RTE_MAX_LCORE) mask[tmp[i]] = 1;
        }
        
        // Serialize mask into compact ranges
        char lcores_str[1024] = {0};
        char buf[32];
        int first = 1;
        for (uint16_t i = 0; i < RTE_MAX_LCORE; ) {
            if (!mask[i]) { i++; continue; }
            uint16_t start = i;
            while (i < RTE_MAX_LCORE && mask[i]) i++;
            uint16_t end = i - 1;
            if (!first) strncat(lcores_str, ",", sizeof(lcores_str) - strlen(lcores_str) - 1);
            if (start == end) {
                snprintf(buf, sizeof(buf), "%u", start);
            } else {
                snprintf(buf, sizeof(buf), "%u-%u", start, end);
            }
            strncat(lcores_str, buf, sizeof(lcores_str) - strlen(lcores_str) - 1);
            first = 0;
        }
        
        if (lcores_str[0] != '\0') {
            // Need to store the string somewhere - caller should provide buffer
            static char lcores_buf[1024];
            strncpy(lcores_buf, lcores_str, sizeof(lcores_buf) - 1);
            lcores_buf[sizeof(lcores_buf) - 1] = '\0';
            eal_argv[argc++] = (char *)"-l";
            eal_argv[argc++] = lcores_buf;
        }
    }
    
    return argc;
}

int dpdk_filter_lcores(uint16_t *tx_lcores, uint16_t *tx_count,
	uint16_t *rx_lcores, uint16_t *rx_count) {
    if (!tx_lcores || !tx_count || !rx_lcores || !rx_count) return -1;
    
    uint16_t main_lcore = rte_get_main_lcore();
    
    // Filter RX lcores: remove main lcore and disabled lcores
    uint16_t rx_out = 0;
    for (uint16_t i = 0; i < *rx_count; i++) {
        uint16_t lc = rx_lcores[i];
        if (lc == main_lcore) continue;
        if (!rte_lcore_is_enabled(lc)) continue;
        rx_lcores[rx_out++] = lc;
    }
    *rx_count = rx_out;
    
    // Filter TX lcores: remove main lcore, disabled lcores, and any in RX list
    uint16_t tx_out = 0;
    for (uint16_t i = 0; i < *tx_count; i++) {
        uint16_t lc = tx_lcores[i];
        if (lc == main_lcore) continue;
        if (!rte_lcore_is_enabled(lc)) continue;
        if (lcore_in_list(lc, rx_lcores, *rx_count)) continue;
        tx_lcores[tx_out++] = lc;
    }
    *tx_count = tx_out;
    
    return 0;
}

void dpdk_stats_cache_reset(struct rte_eth_stats *cache, uint16_t count) {
	if (!cache) return;
	for (uint16_t i = 0; i < count; i++) {
		memset(&cache[i], 0, sizeof(cache[i]));
	}
}

int dpdk_stats_get_delta(uint16_t port_id,
	struct rte_eth_stats *snapshot,
	struct rte_eth_stats *delta) {
	if (!snapshot || !delta) return -1;

	struct rte_eth_stats curr;
	memset(&curr, 0, sizeof(curr));
	int ret = rte_eth_stats_get(port_id, &curr);
	if (ret != 0) {
		return ret;
	}

	memset(delta, 0, sizeof(*delta));
	delta->ipackets = curr.ipackets - snapshot->ipackets;
	delta->opackets = curr.opackets - snapshot->opackets;
	delta->ibytes = curr.ibytes - snapshot->ibytes;
	delta->obytes = curr.obytes - snapshot->obytes;
	delta->imissed = curr.imissed - snapshot->imissed;
	delta->ierrors = curr.ierrors - snapshot->ierrors;
	delta->oerrors = curr.oerrors - snapshot->oerrors;
	delta->rx_nombuf = curr.rx_nombuf - snapshot->rx_nombuf;
	for (uint16_t i = 0; i < RTE_ETHDEV_QUEUE_STAT_CNTRS; i++) {
		delta->q_ipackets[i] = curr.q_ipackets[i] - snapshot->q_ipackets[i];
		delta->q_opackets[i] = curr.q_opackets[i] - snapshot->q_opackets[i];
		delta->q_ibytes[i] = curr.q_ibytes[i] - snapshot->q_ibytes[i];
		delta->q_obytes[i] = curr.q_obytes[i] - snapshot->q_obytes[i];
	}

	*snapshot = curr;
	return 0;
}

void dpdk_print_port_stats(const char *label,
	const uint16_t *port_ids,
	uint16_t port_count,
	double interval_secs,
	int is_tx_direction,
	int show_per_port,
	const struct rte_eth_stats *deltas) {
	if (!label || !port_ids || !deltas || port_count == 0 || interval_secs <= 0.0) return;

	double total_pps = 0.0;
	double total_mbs = 0.0;
	uint16_t active_ports = 0;
	uint64_t total_missed = 0;
	uint64_t total_nombuf = 0;

	for (uint16_t i = 0; i < port_count; i++) {
		uint16_t pid = port_ids[i];
		if (pid >= RTE_MAX_ETHPORTS) continue;

		const struct rte_eth_stats *delta = &deltas[pid];

		uint64_t pkt_delta = is_tx_direction ? delta->opackets : delta->ipackets;
		uint64_t byte_delta = is_tx_direction ? delta->obytes : delta->ibytes;
		double pps = pkt_delta / interval_secs;
		double mbs = (byte_delta / interval_secs) / (1 << 20);

		if (!is_tx_direction) {
			total_missed += delta->imissed;
			total_nombuf += delta->rx_nombuf;
		}

		if (show_per_port) {
			printf("%s port %u: %.1f pps, %.1f MB/s", label, pid, pps, mbs);
			if (!is_tx_direction && (delta->imissed || delta->rx_nombuf)) {
				printf(" (missed=%" PRIu64 ", no_mbuf=%" PRIu64 ")", delta->imissed, delta->rx_nombuf);
			}
			printf("\n");
		}

		total_pps += pps;
		total_mbs += mbs;
		active_ports++;
	}

	if (active_ports == 0) return;

	double avg_pps = total_pps / active_ports;
	double avg_mbs = total_mbs / active_ports;
	if (is_tx_direction) {
		printf("TOTAL %s: %.1f pps, %.1f MB/s | per-port avg: %.1f pps, %.1f MB/s (%u ports)\n",
		       label, total_pps, total_mbs, avg_pps, avg_mbs, active_ports);
	} else {
		printf("TOTAL %s: %.1f pps, %.1f MB/s | per-port avg: %.1f pps, %.1f MB/s (%u ports) [missed=%" PRIu64 ", no_mbuf=%" PRIu64 "]\n",
		       label, total_pps, total_mbs, avg_pps, avg_mbs, active_ports, total_missed, total_nombuf);
	}
}

int dpdk_handle_arp_request(uint16_t port_id,
	struct rte_mbuf *mbuf,
	struct rte_ether_addr *port_macs,
	struct in_addr *port_ips,
	uint16_t tx_queue_id) {
	if (!mbuf || !port_macs || !port_ips) return 0;
	struct in_addr target_ip = port_ips[port_id];
	if (target_ip.s_addr == 0) return 0;

	struct rte_ether_hdr *eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
	if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) return 0;

	struct arp_ipv4_payload *arp = (struct arp_ipv4_payload *)(eth + 1);
	if (arp->op != rte_cpu_to_be_16(ARP_OP_REQUEST)) return 0;
	if (arp->tip != target_ip.s_addr) return 0;

	struct rte_ether_addr our_mac = port_macs[port_id];
	struct rte_ether_addr requester = eth->src_addr;
	struct rte_ether_addr orig_sha = arp->sha;
	uint32_t orig_sip = arp->sip;

	eth->dst_addr = requester;
	eth->src_addr = our_mac;

	arp->op = rte_cpu_to_be_16(ARP_OP_REPLY);
	arp->sha = our_mac;
	arp->sip = target_ip.s_addr;
	arp->tha = orig_sha;
	arp->tip = orig_sip;

	mbuf->data_len = sizeof(struct rte_ether_hdr) + sizeof(struct arp_ipv4_payload);
	mbuf->pkt_len = mbuf->data_len;

	struct rte_mbuf *tx_buf = mbuf;
	if (rte_eth_tx_burst(port_id, tx_queue_id, &tx_buf, 1) != 1) {
		// If TX fails, drop; still considered handled if we drained RX queue
		rte_pktmbuf_free(mbuf);
	}
	return 1;
}

#define DPDK_RX_WORKER_BURST 64

static inline int dpdk_packet_matches_filter(struct rte_mbuf *mbuf, uint16_t filter_port) {
	if (filter_port == 0) return 1;

	const uint32_t min_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr);
	if (mbuf->pkt_len < min_len) return 0;

	struct rte_ether_hdr *eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
	if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) return 0;

	struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
	if ((ip->version_ihl >> 4) != 4) return 0;
	if (ip->next_proto_id != IPPROTO_UDP) return 0;

	uint16_t ihl_bytes = (uint16_t)((ip->version_ihl & RTE_IPV4_HDR_IHL_MASK) * 4);
	if (ihl_bytes < sizeof(struct rte_ipv4_hdr)) return 0;

	const uint8_t *ip_end = (const uint8_t *)ip + ihl_bytes;
	if ((const uint8_t *)eth + mbuf->pkt_len < ip_end + sizeof(struct rte_udp_hdr)) return 0;

	const struct rte_udp_hdr *udp = (const struct rte_udp_hdr *)ip_end;
	return udp->dst_port == rte_cpu_to_be_16(filter_port);
}

int dpdk_shared_rx_worker(void *arg) {
	struct dpdk_shared_rx_worker_ctx *ctx = (struct dpdk_shared_rx_worker_ctx *)arg;
	if (!ctx || !ctx->running) return 0;

	printf("RX worker core %u receiving on port %u queue %u\n",
	       rte_lcore_id(), ctx->port_id, ctx->rx_queue_id);

	while (*(ctx->running)) {
		struct rte_mbuf *mbufs[DPDK_RX_WORKER_BURST];
        
        // Poll main port
		uint16_t received = rte_eth_rx_burst(ctx->port_id, ctx->rx_queue_id, mbufs, DPDK_RX_WORKER_BURST);
        if (received > 0) {
            for (uint16_t i = 0; i < received; i++) {
                struct rte_mbuf *mbuf = mbufs[i];
                if (dpdk_handle_arp_request(ctx->port_id, mbuf, ctx->port_macs, ctx->rx_ip_addrs, ctx->tx_queue_id)) {
                    continue;
                }
                struct rte_ether_hdr *eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
                uint16_t ether_type = rte_be_to_cpu_16(eth->ether_type);

                // Check for ARP reply monitoring
                if (ether_type == RTE_ETHER_TYPE_ARP && ctx->monitor_arp_result) {
                    struct arp_ipv4_payload *arp = (struct arp_ipv4_payload *)(eth + 1);
                    if (arp->op == rte_cpu_to_be_16(ARP_OP_REPLY) && arp->sip == ctx->monitor_arp_ip.s_addr) {
                         *ctx->monitor_arp_result = arp->sha;
                    }
                }

                int ipv4_or_ipv6 = (ether_type == RTE_ETHER_TYPE_IPV4) || (ether_type == RTE_ETHER_TYPE_IPV6);
                if (ctx->warn_on_mismatch && ctx->filter_udp_port != 0 &&
                    ipv4_or_ipv6 && !dpdk_packet_matches_filter(mbuf, ctx->filter_udp_port)) {
                    printf("Non-matching packet on port %u q%u: ether_type=0x%04x len=%u filter_port=%u\n",
                   ctx->port_id, ctx->rx_queue_id, ether_type, mbuf->pkt_len, ctx->filter_udp_port);
                }
                rte_pktmbuf_free(mbuf);
            }
        }
        
        // Poll auxiliary ports (queue 0, ARP only)
        if (ctx->aux_rx_ports_count > 0 && ctx->aux_polling_enabled && *ctx->aux_polling_enabled) {
            for (uint16_t k = 0; k < ctx->aux_rx_ports_count; k++) {
                uint16_t aux_pid = ctx->aux_rx_ports[k];
                received = rte_eth_rx_burst(aux_pid, 0, mbufs, DPDK_RX_WORKER_BURST);
                for (uint16_t i = 0; i < received; i++) {
                    struct rte_mbuf *mbuf = mbufs[i];
                    // Only handle ARP, drop everything else silently
                    if (!dpdk_handle_arp_request(aux_pid, mbuf, ctx->port_macs, ctx->rx_ip_addrs, 0)) { // Use tx_queue 0 for aux
                        rte_pktmbuf_free(mbuf);
                    }
                }
            }
        }
        
        if (received == 0 && (!ctx->aux_rx_ports_count)) {
            rte_pause();
        }
	}
    return 0;
}



