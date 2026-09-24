#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>
#include <vector>
#include <memory>
#include <stdexcept>
#include <cassert>

extern "C" {

#include <doca_log.h>
#include <doca_error.h>

}

#include "monitoring_config.h"
#include "monitoring_dpdk.h"
#include "monitoring_doca.h"
#include "device_manager.h"

DOCA_LOG_REGISTER(MONITORING_MAIN);

namespace {

std::atomic<bool> g_keep_running{true};

void handle_signal(int)
{
	g_keep_running.store(false);
}

} // namespace

int main(int argc, char **argv)
{
	MonitoringConfig config;
	std::unique_ptr<MonitoringDpdk> dpdk;
	std::unique_ptr<MonitoringDoca> doca;
	char errbuf[256];
	int exit_code = EXIT_SUCCESS;
	struct doca_log_backend *sdk_log = NULL;
	doca_error_t log_result;

	log_result = doca_log_backend_create_standard();
	if (log_result != DOCA_SUCCESS) {
		fprintf(stderr, "monitoring_app: failed to create standard log backend (%s)\n",
			doca_error_get_descr(log_result));
		return EXIT_FAILURE;
	}

	log_result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (log_result != DOCA_SUCCESS) {
		fprintf(stderr, "%s: failed to create SDK log backend (%s)\n",
			config.app_name().c_str(), doca_error_get_descr(log_result));
		return EXIT_FAILURE;
	}

	log_result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (log_result != DOCA_SUCCESS) {
		fprintf(stderr, "%s: failed to set SDK log level (%s)\n",
			config.app_name().c_str(), doca_error_get_descr(log_result));
		return EXIT_FAILURE;
	}

	if (config.parse(argc, argv) != 0) {
		fprintf(stderr, "%s: failed to parse arguments\n", config.app_name().c_str());
		return EXIT_FAILURE;
	}

	if (!config.validate(errbuf, sizeof(errbuf))) {
		fprintf(stderr, "%s: invalid configuration: %s\n", config.app_name().c_str(), errbuf);
		return EXIT_FAILURE;
	}

	config.print();

	// Probe devices using device manager
	DeviceManager device_manager;
	std::vector<uint16_t> monitor_port_ids;
	std::vector<uint16_t> host_port_ids;
    std::vector<uint16_t> forward_port_ids;

	try {
		for (const auto &dev : config.monitor_devices()) {
			monitor_port_ids.push_back(device_manager.add_device(dev, "monitor", true));
		}
		for (const auto &dev : config.host_reps()) {
			host_port_ids.push_back(device_manager.add_device_rep(dev, "host"));
		}
        if (config.monitoring_dest_type == MonitoringConfig::MonitoringDest::DEVICE) {
            for (const auto &dev : config.monitoring_dest_devices) {
                forward_port_ids.push_back(device_manager.add_device_rep(dev, "forward_dest"));
            }
        } else {
             // Fill with invalid IDs if not device forwarding, to match size logic if needed,
             // though DOCA class handles logic based on type.
             // But let's pass an empty vector or handle it in DOCA. 
             // Actually DOCA expects valid IDs if type is DEVICE.
        }
	} catch (const std::exception &ex) {
		DOCA_LOG_ERR("Failed to probe devices: %s", ex.what());
		return EXIT_FAILURE;
	}

	std::vector<uint64_t> worker_packets_last;
	std::vector<uint64_t> worker_bytes_last;
	const std::chrono::milliseconds stats_interval(1000);
	auto last_snapshot = std::chrono::high_resolution_clock::time_point{};

	std::signal(SIGINT, handle_signal);
	std::signal(SIGTERM, handle_signal);

	try {
		try {
			dpdk.reset(new MonitoringDpdk(device_manager));
		} catch (const std::exception &ex) {
			fprintf(stderr, "%s: failed to initialize DPDK context: %s\n",
				config.app_name().c_str(),
				ex.what());
			throw std::runtime_error("failed to initialize DPDK context");
		}

		try {
			doca.reset(new MonitoringDoca(device_manager,
							  monitor_port_ids,
							  host_port_ids,
						      dpdk->queue_count(),
						      config,
                              forward_port_ids));
		} catch (const std::exception &ex) {
			DOCA_LOG_ERR("failed to initialize DOCA context: %s", ex.what());
			throw;
		}

		if (config.monitoring_dest_type == MonitoringConfig::MonitoringDest::DPDK) {
            if (dpdk->start_workers() != 0) {
                DOCA_LOG_ERR("failed to start DPDK workers");
                throw std::runtime_error("failed to start DPDK workers");
            }
        }

		last_snapshot = std::chrono::high_resolution_clock::now();

		const auto &initial_workers = dpdk->worker_info();
		worker_packets_last.resize(initial_workers.size());
		worker_bytes_last.resize(initial_workers.size());
		for (size_t i = 0; i < initial_workers.size(); ++i) {
			worker_packets_last[i] = initial_workers[i].stats.packets;
			worker_bytes_last[i] = initial_workers[i].stats.bytes;
		}

		DOCA_LOG_INFO("%s initialized", config.app_name().c_str());

		while (g_keep_running.load(std::memory_order_relaxed)) {

			const std::vector<MonitoringDpdk::monitoring_dpdk_worker_info> &workers = dpdk->worker_info();

			auto now = std::chrono::high_resolution_clock::now();
			double elapsed = std::chrono::duration<double>(now - last_snapshot).count();
			last_snapshot = now;
			if (elapsed <= 0.0)
				elapsed = 1.0;

			printf("\n=== monitoring snapshot ===\n");

			printf("Worker statistics:\n");
			assert(workers.size() == worker_packets_last.size());
			if (workers.size() > 0) {
				for (unsigned int i = 0; i < workers.size(); ++i) {
					uint64_t packets = workers[i].stats.packets;
					uint64_t bytes = workers[i].stats.bytes;

					uint64_t delta_packets = packets - worker_packets_last[i];
					uint64_t delta_bytes = bytes - worker_bytes_last[i];

					worker_packets_last[i] = packets;
					worker_bytes_last[i] = bytes;

					double packets_per_sec = delta_packets / elapsed;
					double mb_per_sec = (delta_bytes / elapsed) / (1024.0 * 1024.0);

					printf("  lcore=%u queue=%u rate=%.2f pkt/s %.2f MB/s\n",
						workers[i].lcore_id,
						workers[i].queue_id,
						packets_per_sec,
						mb_per_sec);
				}
			} else {
				printf("  <no worker data available>\n");
			}

			for (uint16_t port_id = 0; port_id < device_manager.get_dpdk_port_count(); ++port_id) {
				if (!device_manager.is_device_rep(port_id)) {
					(void)dpdk->print_port_stats(port_id, stdout);
				}
			}

			if (doca != nullptr)
				doca->print_stats(stdout);

			std::this_thread::sleep_for(stats_interval);
		}
	} catch (const std::exception &ex) {
		DOCA_LOG_ERR("exception: %s", ex.what());
		exit_code = EXIT_FAILURE;
	}

	// finally; clean up
	g_keep_running.store(false, std::memory_order_relaxed);
	if (dpdk != nullptr)
		dpdk->stop_workers();
	doca.reset();
	return exit_code;
}

