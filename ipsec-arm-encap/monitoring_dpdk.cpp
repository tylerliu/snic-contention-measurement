#include "monitoring_dpdk.h"
#include "esp_layout.h"

#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <stdexcept>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_launch.h>
#include <rte_pause.h>
#include <rte_common.h>
#include <rte_flow.h>
#include "decap_metadata.h"

#include <doca_error.h>
#include <doca_log.h>

#include "dpdk_utils.h"

DOCA_LOG_REGISTER(MONITORING_DPDK);

namespace {
std::atomic<bool> g_dpdk_workers_keep_running{false};
}

void MonitoringDpdk::monitoring_dpdk_log_devices(void)
{
	uint16_t port_id;
	uint16_t avail = rte_eth_dev_count_avail();

	DOCA_LOG_INFO("DPDK detected %u available Ethernet ports", avail);

	RTE_ETH_FOREACH_DEV(port_id) {
		struct rte_ether_addr mac = {{0}};
		struct rte_eth_dev_info dev_info;
		char name[RTE_ETH_NAME_MAX_LEN] = {0};

		rte_eth_dev_get_name_by_port(port_id, name);
		rte_eth_dev_info_get(port_id, &dev_info);
		rte_eth_macaddr_get(port_id, &mac);

		char mac_str[RTE_ETHER_ADDR_FMT_SIZE];
		rte_ether_format_addr(mac_str, sizeof(mac_str), &mac);

		DOCA_LOG_INFO("  port %u (%s): driver=%s mac=%s", port_id, name[0] ? name : "unknown",
			      dev_info.driver_name ? dev_info.driver_name : "n/a",
			      mac_str);
	}
}

int MonitoringDpdk::monitoring_dpdk_worker_loop(void *opaque)
{
    auto *worker = reinterpret_cast<monitoring_dpdk_worker_info *>(opaque);
    auto &owner = *worker->owner;
    rte_mbuf *burst[MONITORING_DPDK_BURST_SIZE];
    rte_mbuf *tx[MONITORING_DPDK_BURST_SIZE];
    uint64_t parsed = 0, malformed = 0, unauthenticated = 0, replay = 0;
    uint64_t transmitted = 0, tx_dropped = 0, tx_bytes = 0, linearized = 0;
    std::vector<uint64_t> sa_packets(owner.inbound_sas_.size(), 0);
    while (g_dpdk_workers_keep_running.load(std::memory_order_relaxed)) {
        uint64_t packets = 0, bytes = 0;
        for (uint16_t port : worker->polled_ports) {
            uint16_t count = rte_eth_rx_burst(port, worker->queue_id, burst,
                                             MONITORING_DPDK_BURST_SIZE);
            uint16_t ready = 0;
            uint32_t lengths[MONITORING_DPDK_BURST_SIZE];
            for (uint16_t i = 0; i < count; ++i) {
                rte_mbuf *m = burst[i];
                bytes += rte_pktmbuf_pkt_len(m);
                InboundSa *sa = nullptr;
                // Only the successful hardware-authentication pipe supplies this marker.
                size_t sa_index = 0;
                if (m->ol_flags & owner.metadata_rx_flag_) {
                    uint32_t pair = rte_flow_dynf_metadata_get(m) - decap_authenticated_mark(0);
                    if (pair < owner.inbound_sas_.size() / owner.flow_count_) {
                        sa_index = size_t(pair) * owner.flow_count_;
                        if (owner.inbound_sas_[sa_index]->rx_port == port)
                            sa = owner.inbound_sas_[sa_index].get();
                    }
                }
                if (!sa) {
                    ++unauthenticated;
                    rte_pktmbuf_free(m);
                    continue;
                }
                EspLayout layout;
                if (m->nb_segs > 1) ++linearized;
                if (rte_pktmbuf_linearize(m) != 0 ||
                    !parse_esp_layout(rte_pktmbuf_mtod(m, const uint8_t *),
                                      rte_pktmbuf_pkt_len(m), layout) ||
                    uint32_t(layout.spi - sa->config.spi) >= owner.flow_count_) {
                    ++malformed;
                    rte_pktmbuf_free(m);
                    continue;
                }
                sa_index += layout.spi - sa->config.spi;
                sa = owner.inbound_sas_[sa_index].get();
                ++sa_packets[sa_index];
                bool accepted = true;
                if (owner.anti_replay_) {
                    accepted = sa->replay.accept(layout.seq);
                }
                if (!accepted) {
                    ++replay;
                    rte_pktmbuf_free(m);
                    continue;
                }
                ++parsed;
                // Reuse the received buffer: remove outer headers, retain room
                // immediately before the inner IP header for an untagged L2 header.
                auto *data = reinterpret_cast<uint8_t *>(
                    rte_pktmbuf_adj(m, layout.plaintext - RTE_ETHER_HDR_LEN));
                uint32_t new_len = RTE_ETHER_HDR_LEN + layout.inner_len;
                if (!data || rte_pktmbuf_trim(m, rte_pktmbuf_pkt_len(m) - new_len) != 0) {
                    ++malformed;
                    rte_pktmbuf_free(m);
                    continue;
                }
                std::memcpy(data, sa->config.dst_mac.data(), RTE_ETHER_ADDR_LEN);
                std::memcpy(data + RTE_ETHER_ADDR_LEN, sa->config.src_mac.data(), RTE_ETHER_ADDR_LEN);
                data[12] = 0x08; data[13] = 0x00;
#ifdef ISOLATION_FRESH_TX_BUFFER
                rte_mbuf *fresh = rte_pktmbuf_alloc(m->pool);
                char *fresh_data = fresh ? rte_pktmbuf_append(fresh, new_len) : nullptr;
                if (!fresh_data) {
                    if (fresh) rte_pktmbuf_free(fresh);
                    rte_pktmbuf_free(m);
                    ++tx_dropped;
                    continue;
                }
                std::memcpy(fresh_data, data, new_len);
                rte_pktmbuf_free(m);
                m = fresh;
                data = reinterpret_cast<uint8_t *>(fresh_data);
#endif
                // Do not reuse RX checksum/security flags as TX offloads.
#ifdef ISOLATION_NO_TX_METADATA
                m->ol_flags = 0;
#else
                m->ol_flags = owner.metadata_tx_flag_;
#endif
                m->packet_type = 0;
                m->tx_offload = 0;
                m->l2_len = RTE_ETHER_HDR_LEN;
                m->l3_len = (data[RTE_ETHER_HDR_LEN] & 15) * 4;
#ifndef ISOLATION_NO_TX_METADATA
                rte_flow_dynf_metadata_set(m, sa->host_port);
#endif
                lengths[ready] = new_len;
                tx[ready++] = m;
            }
            if (ready) {
                uint16_t sent = 0;
                // Retain ownership of the unsent suffix; only abandon it on shutdown.
                while (sent < ready && g_dpdk_workers_keep_running.load(std::memory_order_relaxed)) {
                    uint16_t accepted = rte_eth_tx_burst(port, worker->queue_id,
                                                        tx + sent, ready - sent);
                    sent += accepted;
                    if (sent < ready) rte_pause();
                }
                transmitted += sent;
                for (uint16_t i = 0; i < sent; ++i) tx_bytes += lengths[i];
                tx_dropped += ready - sent;
                for (uint16_t i = sent; i < ready; ++i) rte_pktmbuf_free(tx[i]);
            }
            packets += count;
        }
        __atomic_fetch_add(&worker->stats.packets, packets, __ATOMIC_RELAXED);
        __atomic_fetch_add(&worker->stats.bytes, bytes, __ATOMIC_RELAXED);
        if (packets == 0) rte_pause();
    }
    for (size_t i = 0; i < sa_packets.size(); ++i)
        if (sa_packets[i])
            DOCA_LOG_INFO("RSS SA=%zu queue=%u packets=%lu", i, worker->queue_id, sa_packets[i]);
    DOCA_LOG_INFO("linearized queue=%u packets=%lu", worker->queue_id, linearized);
    DOCA_LOG_INFO("ESP totals queue=%u accepted=%lu malformed=%lu unauthenticated=%lu replay=%lu tx=%lu tx_drop=%lu tx_bytes=%lu",
        worker->queue_id, parsed, malformed, unauthenticated, replay, transmitted, tx_dropped, tx_bytes);
    return 0;
}
MonitoringDpdk::MonitoringDpdk(DeviceManager &device_manager, const MonitoringConfig &cfg,
    const std::vector<uint16_t> &monitor_ports, const std::vector<uint16_t> &host_ports)
	: nb_ports_(device_manager.get_dpdk_port_count())
{
    anti_replay_ = cfg.anti_replay;
    flow_count_ = cfg.decap_flow_count;
    if (cfg.enable_host_branch) {
        if (monitor_ports.size() != cfg.ipsec_decap_configs().size() ||
            monitor_ports.size() != host_ports.size())
            throw std::runtime_error("inbound SA/port count mismatch");
        for (size_t i = 0; i < monitor_ports.size(); ++i) {
            std::unique_ptr<InboundSa> sa(new InboundSa);
            sa->config = cfg.ipsec_decap_configs()[i];
            sa->rx_port = monitor_ports[i];
            sa->host_port = host_ports[i];
            sa->authenticated_mark = decap_authenticated_mark(i);
            bool nonzero = false;
            for (auto b : sa->config.dst_mac) nonzero |= b != 0;
            if (!nonzero || (sa->config.dst_mac[0] & 1))
                throw std::runtime_error("inbound destination MAC must be a nonzero unicast host MAC");
            for (const auto &other : inbound_sas_)
                if (other->rx_port != sa->rx_port && other->config.dst_mac == sa->config.dst_mac)
                    throw std::runtime_error("inbound destination MACs must uniquely identify host ports");
            for (uint16_t flow = 0; flow < flow_count_; ++flow) {
                std::unique_ptr<InboundSa> instance(new InboundSa);
                instance->config = sa->config;
                instance->config.spi += flow;
                instance->rx_port = sa->rx_port;
                instance->host_port = sa->host_port;
                instance->authenticated_mark = sa->authenticated_mark;
                inbound_sas_.push_back(std::move(instance));
            }
        }
    }
    dpdk_cfg_.port_config.enable_mbuf_metadata = true;
	for (uint16_t i = 0; i < nb_ports_; ++i) {
		if (!device_manager.is_device_rep(i)) {
			polled_ports_.push_back(i);
		}
	}

	dpdk_cfg_.reserve_main_thread = true;
	dpdk_cfg_.port_config.nb_ports = nb_ports_;

	doca_error_t dres = dpdk_queues_and_ports_init(&dpdk_cfg_);
	if (dres != DOCA_SUCCESS) {
		DOCA_LOG_ERR("dpdk_queues_and_ports_init failed: %s", doca_error_get_descr(dres));
		throw std::runtime_error("dpdk_queues_and_ports_init failed");
	}

	nb_ports_ = dpdk_cfg_.port_config.nb_ports;
	nb_queues_ = dpdk_cfg_.port_config.nb_queues;
    if (rte_flow_dynf_metadata_register() != 0)
        throw std::runtime_error("failed to register packet metadata");
    metadata_rx_flag_ = RTE_MBUF_DYNFLAG_RX_METADATA;
    metadata_tx_flag_ = RTE_MBUF_DYNFLAG_TX_METADATA;

	if (nb_queues_ == 0) {
		DOCA_LOG_ERR("no worker lcores available; DPDK requires at least one worker");
		throw std::runtime_error("no worker lcores available; DPDK requires at least one worker");
	}

	monitoring_dpdk_log_devices();

	DOCA_LOG_INFO("DPDK initialized (ports=%u, queues=%u)",
		      nb_ports_,
		      nb_queues_);

	g_dpdk_workers_keep_running.store(false, std::memory_order_relaxed);

	prev_ts_.resize(nb_ports_, std::chrono::high_resolution_clock::time_point{});
	port_stats_.resize(nb_ports_);
	for (uint16_t i = 0; i < nb_ports_; ++i) {
		port_stats_[i].packets = 0;
		port_stats_[i].bytes = 0;
		port_stats_[i].queue_packets.resize(nb_queues_);
		port_stats_[i].queue_bytes.resize(nb_queues_);
	}
}

MonitoringDpdk::~MonitoringDpdk()
{

	DOCA_LOG_INFO("DPDK shutdown");

	stop_workers();

	dpdk_queues_and_ports_fini(&dpdk_cfg_);
}

int MonitoringDpdk::start_workers()
{
	if (!workers_.empty()) {
		DOCA_LOG_WARN("DPDK workers already started");
		return 0;
	}

	workers_.resize(nb_queues_);

	unsigned int idx = 0;
	unsigned int lcore_id;

	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		if (idx >= nb_queues_)
			break;

		monitoring_dpdk_worker_info *worker = &workers_[idx];
		worker->owner = this;
		worker->lcore_id = lcore_id;
		worker->queue_id = static_cast<uint16_t>(idx);
		worker->polled_ports = polled_ports_;
		worker->stats.packets = 0;
		worker->stats.bytes = 0;
		++idx;
	}

	if (idx != nb_queues_) {
		DOCA_LOG_ERR("insufficient worker lcores: need %u, found %u", nb_queues_, idx);
		workers_.clear();
		return -1;
	}

	g_dpdk_workers_keep_running.store(true, std::memory_order_relaxed);

	for (unsigned int i = 0; i < workers_.size(); ++i) {
		monitoring_dpdk_worker_info *worker = &workers_[i];
		int rc = rte_eal_remote_launch(monitoring_dpdk_worker_loop, worker, worker->lcore_id);
		if (rc != 0) {
			DOCA_LOG_ERR("failed to launch worker on lcore %u (rc=%d)", worker->lcore_id, rc);
			stop_workers();
			return -1;
		}
	}

	DOCA_LOG_INFO("launched %lu DPDK workers", workers_.size());
	return 0;
}

void MonitoringDpdk::stop_workers()
{
	if (workers_.empty())
		return;

	g_dpdk_workers_keep_running.store(false, std::memory_order_relaxed);

	rte_eal_mp_wait_lcore();

	workers_.clear();
}

int MonitoringDpdk::print_port_stats(uint16_t port_id, FILE *stream)
{
	struct rte_eth_stats stats;
	struct rte_eth_dev_info dev_info;
	int ret;

	const auto now = std::chrono::high_resolution_clock::now();
	double elapsed = 0.0;

	if (stream == NULL)
		stream = stdout;

	ret = rte_eth_stats_get(port_id, &stats);
	if (ret != 0)
		return ret;

	ret = rte_eth_dev_info_get(port_id, &dev_info);
	if (ret != 0)
		return ret;

	elapsed = std::chrono::duration<double>(now - prev_ts_[port_id]).count();

	double pkt_rate = 0.0;
	double mb_rate = 0.0;
	if (elapsed > 0.0) {
		uint64_t diff_pkts = (stats.ipackets >= port_stats_[port_id].packets) ?
			(stats.ipackets - port_stats_[port_id].packets) : 0;
		uint64_t diff_bytes = (stats.ibytes >= port_stats_[port_id].bytes) ?
			(stats.ibytes - port_stats_[port_id].bytes) : 0;
		pkt_rate = diff_pkts / elapsed;
		mb_rate = (diff_bytes / elapsed) / (1024.0 * 1024.0);
	}

	fprintf(stream, "\nPort %u statistics:\n", port_id);
	fprintf(stream,
		"  RX rate: %.2f pkt/s %.2f MB/s (missed=%" PRIu64 " errors=%" PRIu64 " no_mbuf=%" PRIu64 ")\n",
		pkt_rate,
		mb_rate,
		stats.imissed,
		stats.ierrors,
		stats.rx_nombuf);

	uint32_t max_rx_queues = RTE_MIN(dev_info.nb_rx_queues, (uint32_t)RTE_ETHDEV_QUEUE_STAT_CNTRS);

	for (uint32_t q = 0; q < max_rx_queues; ++q) {
		double q_pkt_rate = 0.0;
		double q_mb_rate = 0.0;
		if (elapsed > 0.0) {
			uint64_t diff_pkts = (stats.q_ipackets[q] >= port_stats_[port_id].queue_packets[q]) ?
				(stats.q_ipackets[q] - port_stats_[port_id].queue_packets[q]) : 0;
			uint64_t diff_bytes = (stats.q_ibytes[q] >= port_stats_[port_id].queue_bytes[q]) ?
				(stats.q_ibytes[q] - port_stats_[port_id].queue_bytes[q]) : 0;
			q_pkt_rate = diff_pkts / elapsed;
			q_mb_rate = (diff_bytes / elapsed) / (1024.0 * 1024.0);
		}

		fprintf(stream,
			"  RX queue %2u: rate=%.2f pkt/s %.2f MB/s (errors=%" PRIu64 ")\n",
			q,
			q_pkt_rate,
			q_mb_rate,
			stats.q_errors[q]);
	}

	port_stats_[port_id].packets = stats.ipackets;
	port_stats_[port_id].bytes = stats.ibytes;
	prev_ts_[port_id] = now;
	for (uint32_t q = 0; q < max_rx_queues; ++q) {
		port_stats_[port_id].queue_packets[q] = stats.q_ipackets[q];
		port_stats_[port_id].queue_bytes[q] = stats.q_ibytes[q];
	}

	return 0;
}