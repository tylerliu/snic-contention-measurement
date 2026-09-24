#include "monitoring_dpdk.h"

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
	struct rte_mbuf *burst[MONITORING_DPDK_BURST_SIZE];

	while (g_dpdk_workers_keep_running.load(std::memory_order_relaxed)) {
		uint64_t bytes = 0;
		uint64_t packets = 0;
		for (uint16_t port_id : worker->polled_ports) {
			uint16_t received = rte_eth_rx_burst(port_id, worker->queue_id, burst, MONITORING_DPDK_BURST_SIZE);

			if (received == 0) {
				continue;
			}

			for (uint16_t i = 0; i < received; ++i) {
				bytes += rte_pktmbuf_pkt_len(burst[i]);
				rte_pktmbuf_free(burst[i]);
			}

			packets += received;
		}

		worker->stats.packets += packets;
		worker->stats.bytes += bytes;
        if (packets == 0) {
		    rte_pause();
		}
	}

	return 0;
}

MonitoringDpdk::MonitoringDpdk(DeviceManager &device_manager)
	: nb_ports_(device_manager.get_dpdk_port_count())
{
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