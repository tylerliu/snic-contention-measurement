#ifndef MONITORING_APP_MONITORING_DPDK_H
#define MONITORING_APP_MONITORING_DPDK_H

#include <cstdint>
#include <cstdio>
#include <vector>
#include <chrono>

#include <rte_mbuf.h>

#include "device_manager.h"
#include "dpdk_utils.h"

#define MONITORING_DPDK_BURST_SIZE 32
#define MONITORING_DPDK_DEFAULT_QUEUES 1


class MonitoringDpdk {
	public:
		struct monitoring_dpdk_worker_stats {
			uint64_t packets;
			uint64_t bytes;
		};
		struct monitoring_dpdk_worker_info {
			unsigned int lcore_id;
			std::vector<uint16_t> polled_ports;
			uint16_t queue_id;
			struct monitoring_dpdk_worker_stats stats;
		};
public:
	MonitoringDpdk(DeviceManager &device_manager);
	~MonitoringDpdk();

	MonitoringDpdk(const MonitoringDpdk &) = delete;
	MonitoringDpdk &operator=(const MonitoringDpdk &) = delete;
	MonitoringDpdk(MonitoringDpdk &&) = delete;
	MonitoringDpdk &operator=(MonitoringDpdk &&) = delete;

	int start_workers();
	void stop_workers();

	const std::vector<monitoring_dpdk_worker_info> &worker_info() const { return workers_; } 
	int print_port_stats(uint16_t port_id, FILE *stream);

	uint16_t queue_count() const { return nb_queues_; }

private:

    void monitoring_dpdk_log_devices();
    static int monitoring_dpdk_worker_loop(void *opaque);

private:

	struct monitoring_dpdk_port_stats {
		uint64_t packets;
		uint64_t bytes;
		std::vector<uint64_t> queue_packets;
		std::vector<uint64_t> queue_bytes;
	};

	uint16_t nb_queues_;
	uint16_t nb_ports_;
	std::vector<uint16_t> polled_ports_;
	struct application_dpdk_config dpdk_cfg_{};
	std::vector<monitoring_dpdk_worker_info> workers_;
	std::vector<std::chrono::high_resolution_clock::time_point> prev_ts_;
	std::vector<monitoring_dpdk_port_stats> port_stats_;
};
#endif /* MONITORING_APP_MONITORING_DPDK_H */
