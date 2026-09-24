#include "monitoring_doca.h"

#include <cstring>
#include <stdexcept>
#include <vector>
#include <sstream>

#include <doca_dpdk.h>
#include <doca_error.h>
#include <doca_flow.h>
#include <doca_log.h>
#include <rte_byteorder.h>

#include "monitoring_config.h"
#include "device_manager.h"

#include <rte_memcpy.h>

DOCA_LOG_REGISTER(MONITORING_DOCA);

namespace {

constexpr const char *kMonitoringDocaMode = "switch,hws,isolated,hairpinq_num=4";
constexpr uint32_t kMonitoringDocaNrCounters = 128;
constexpr uint32_t kMonitoringPipePullTimeout = 10000;
constexpr const char *kMonitoringPipeControlName = "MONITOR_CONTROL";

inline void check_and_throw_doca(doca_error_t result, const char* potential_error_message) {
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("%s: %s", potential_error_message, doca_error_get_descr(result));
        throw std::runtime_error(potential_error_message);
    }
}

inline void check_and_warn_doca(doca_error_t result, const char* potential_error_message) {
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_WARN("%s: %s", potential_error_message, doca_error_get_descr(result));
    }
}

struct GuardedFlowCfg {
	explicit GuardedFlowCfg() {
        doca_error_t result = doca_flow_cfg_create(&cfg_);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("doca_flow_cfg_create: %s", doca_error_get_descr(result));
            throw std::runtime_error("doca_flow_cfg_create failed");
        }
    }
	GuardedFlowCfg(const GuardedFlowCfg &) = delete;
	GuardedFlowCfg &operator=(const GuardedFlowCfg &) = delete;
	GuardedFlowCfg(GuardedFlowCfg &&) = default;
	GuardedFlowCfg &operator=(GuardedFlowCfg &&) = default;
	inline doca_flow_cfg *get() const { return cfg_; }
	~GuardedFlowCfg()
	{
		if (cfg_ != nullptr) {
            check_and_warn_doca(doca_flow_cfg_destroy(cfg_), "doca_flow_cfg_destroy");
		}
	}
private:
	doca_flow_cfg *cfg_{nullptr};
};

struct GuardedPipeCfg {
	explicit GuardedPipeCfg(doca_flow_port *port) {
        check_and_throw_doca(doca_flow_pipe_cfg_create(&cfg_, port), "doca_flow_pipe_cfg_create failed");
    }
	GuardedPipeCfg(const GuardedPipeCfg &) = delete;
	GuardedPipeCfg &operator=(const GuardedPipeCfg &) = delete;
	GuardedPipeCfg(GuardedPipeCfg &&) = default;
	GuardedPipeCfg &operator=(GuardedPipeCfg &&) = default;
	inline doca_flow_pipe_cfg *get() const { return cfg_; }
	~GuardedPipeCfg() {
		if (cfg_ != nullptr) {
			check_and_warn_doca(doca_flow_pipe_cfg_destroy(cfg_), "doca_flow_pipe_cfg_destroy");
		}
	}
private:
    doca_flow_pipe_cfg *cfg_{nullptr};
};

struct GuardedPortCfg {
	explicit GuardedPortCfg() {
        check_and_throw_doca(doca_flow_port_cfg_create(&cfg_), "doca_flow_port_cfg_create failed");
    }
	GuardedPortCfg(const GuardedPortCfg &) = delete;
	GuardedPortCfg &operator=(const GuardedPortCfg &) = delete;
	inline doca_flow_port_cfg *get() const { return cfg_; }
	~GuardedPortCfg()
	{
		if (cfg_ != nullptr) {
			check_and_warn_doca(doca_flow_port_cfg_destroy(cfg_), "doca_flow_port_cfg_destroy");
		}
	}
private:
	doca_flow_port_cfg *cfg_{nullptr};
};

} // namespace

MonitoringDoca::MonitoringDoca(DeviceManager &device_manager,
                               const std::vector<uint16_t> &monitor_port_ids,
                               const std::vector<uint16_t> &host_port_ids,
                               uint16_t dpdk_nb_queues,
                               const MonitoringConfig &cfg,
                               const std::vector<uint16_t> &forward_port_ids)
    : main_dest_type_(cfg.main_dest_type),
      clone_dest_type_(cfg.clone_dest_type),
      enable_mirroring_(cfg.enable_mirroring),
      forward_port_ids_(forward_port_ids),
      counters_enabled_(cfg.enable_counters),
      monitor_port_ids_(monitor_port_ids),
      host_port_ids_(host_port_ids),
	  control_pipes_(monitor_port_ids_.size(), nullptr),
	  monitor_output_pipes_(monitor_port_ids_.size(), nullptr),
	  host_pipes_(monitor_port_ids_.size(), nullptr),
	  host_entries_(monitor_port_ids_.size(), nullptr),
	  rss_entries_(monitor_port_ids_.size(), nullptr),
	  clone_entries_(monitor_port_ids_.size(), nullptr),
	  rss_last_packets_(monitor_port_ids_.size(), 0),
	  rss_last_bytes_(monitor_port_ids_.size(), 0),
	  host_last_packets_(monitor_port_ids_.size(), 0),
	  host_last_bytes_(monitor_port_ids_.size(), 0),
	  clone_last_packets_(monitor_port_ids_.size(), 0),
	  clone_last_bytes_(monitor_port_ids_.size(), 0)
{
  std::stringstream ports_msg;
  ports_msg << "monitor_port=";
  for (const auto &port_id : monitor_port_ids_) {
    if (&port_id != &monitor_port_ids_.front())
      ports_msg << ",";
    ports_msg << port_id;
  }
  ports_msg << " host_port=";
  for (const auto &port_id : host_port_ids_) {
    if (&port_id != &host_port_ids_.front())
      ports_msg << ",";
    ports_msg << port_id;
  }
  ports_msg << std::endl;
  DOCA_LOG_INFO("DOCA init: %s", ports_msg.str().c_str());
  initialize_flow(dpdk_nb_queues);

	// switch mode: start devices
	for (uint16_t port_id = 0; port_id < device_manager.get_port_count(); ++port_id) {
		if (!device_manager.is_device_rep(port_id)) {
			doca_flow_port *port = start_port(device_manager.get_device(port_id), port_id);
			switch_ports_.push_back(port);
		} else {
			doca_flow_port *port = start_port_rep(device_manager.get_device_rep(port_id), port_id);
			switch_ports_.push_back(port);
		}
	}

    build_pipeline(cfg, dpdk_nb_queues);

    last_counter_timestamp_ = std::chrono::steady_clock::now();
}

MonitoringDoca::~MonitoringDoca()
{
	for (size_t i = 0; i < control_pipes_.size(); ++i) {	
		if (control_pipes_[i] != nullptr) {
			doca_flow_pipe_destroy(control_pipes_[i]);
			control_pipes_[i] = nullptr;
		}
	}

  for (size_t i = 0; i < monitor_output_pipes_.size(); ++i) {
    if (monitor_output_pipes_[i] != nullptr) {
      doca_flow_pipe_destroy(monitor_output_pipes_[i]);
      monitor_output_pipes_[i] = nullptr;
    }
  }

	for (size_t i = 0; i < host_pipes_.size(); ++i) {
		if (host_pipes_[i] != nullptr) {
			doca_flow_pipe_destroy(host_pipes_[i]);
			host_pipes_[i] = nullptr;
		}
	}
	for (uint16_t port_id = switch_ports_.size() - 1; port_id != (uint16_t) -1; --port_id) {
		doca_flow_port *port = switch_ports_[port_id];
		doca_flow_port_stop(port);
		DOCA_LOG_INFO("DOCA port stopped (port=%u)", port_id);
	}

    doca_flow_destroy();
    DOCA_LOG_INFO("DOCA flow destroyed");
}

int MonitoringDoca::print_stats(FILE *stream)
{
	if (stream == nullptr || switch_ports_.empty())
		return -1;

	auto now = std::chrono::steady_clock::now();
	double elapsed = std::chrono::duration<double>(now - last_counter_timestamp_).count();
	if (elapsed <= 0.0)
		elapsed = 1.0;

	for (size_t i = 0; i < rss_entries_.size(); ++i) {
		if (rss_entries_[i] != nullptr) {
			struct doca_flow_resource_query query{};
			doca_error_t qres = doca_flow_resource_query_entry(rss_entries_[i], &query);
			if (qres == DOCA_SUCCESS) {
				uint64_t delta_packets = query.counter.total_pkts - rss_last_packets_[i];
				uint64_t delta_bytes = query.counter.total_bytes - rss_last_bytes_[i];
				rss_last_packets_[i] = query.counter.total_pkts;
				rss_last_bytes_[i] = query.counter.total_bytes;
				double packets_per_sec = delta_packets / elapsed;
				double mb_per_sec = (delta_bytes / (1024.0 * 1024.0)) / elapsed;
				const char *dest_str = (main_dest_type_ == MonitoringConfig::MonitoringDest::RSS) ? "RSS (Main)" : 
										(clone_dest_type_ == MonitoringConfig::MonitoringDest::RSS) ? "RSS (Clone)" : "DEVICE";
				fprintf(stream, "DOCA %s pipe: %.2f pkt/s %.2f MB/s\n",
					dest_str, packets_per_sec, mb_per_sec);
			} else if (qres != DOCA_ERROR_NOT_SUPPORTED) {
				DOCA_LOG_WARN("doca_flow_resource_query_entry failed: %s",
							  doca_error_get_descr(qres));
			}
		}
	}

	for (size_t i = 0; i < host_entries_.size(); ++i) {
		if (host_entries_[i] == nullptr) continue;
		struct doca_flow_resource_query query{};
		doca_error_t qres = doca_flow_resource_query_entry(host_entries_[i], &query);
		if (qres == DOCA_SUCCESS) {
			uint64_t delta_packets = query.counter.total_pkts - host_last_packets_[i];
			uint64_t delta_bytes = query.counter.total_bytes - host_last_bytes_[i];
			host_last_packets_[i] = query.counter.total_pkts;
			host_last_bytes_[i] = query.counter.total_bytes;
			double packets_per_sec = delta_packets / elapsed;
			double mb_per_sec = (delta_bytes / (1024.0 * 1024.0)) / elapsed;
			const char *dest_str = (main_dest_type_ == MonitoringConfig::MonitoringDest::HOST) ? "HOST (Main)" : "HOST (Clone)";
			fprintf(stream,
				"DOCA %s pipe: %.2f pkt/s %.2f MB/s\n",
				dest_str,
				packets_per_sec,
				mb_per_sec);
		} else if (qres != DOCA_ERROR_NOT_SUPPORTED) {
			DOCA_LOG_WARN("doca_flow_resource_query_entry failed: %s", doca_error_get_descr(qres));
		}
	}

	last_counter_timestamp_ = now;
	return 0;
}

void MonitoringDoca::initialize_flow(uint16_t nb_queues) {
  if (nb_queues == 0 && main_dest_type_ == MonitoringConfig::MonitoringDest::RSS) {
    DOCA_LOG_WARN("no DPDK queues available; RSS pipe will not be built");
    return;
  }

	GuardedFlowCfg cfg_guard{};

	doca_error_t result = doca_flow_cfg_set_pipe_queues(cfg_guard.get(), nb_queues);
	check_and_throw_doca(result, "doca_flow_cfg_set_pipe_queues failed");

	result = doca_flow_cfg_set_mode_args(cfg_guard.get(), kMonitoringDocaMode);
	check_and_throw_doca(result, "doca_flow_cfg_set_mode_args failed");

	result = doca_flow_cfg_set_nr_counters(cfg_guard.get(), kMonitoringDocaNrCounters);
	check_and_throw_doca(result, "doca_flow_cfg_set_nr_counters failed");

  if (enable_mirroring_) {
    result = doca_flow_cfg_set_nr_shared_resource(
        cfg_guard.get(),
        monitor_port_ids_.size() + 1, // +1 for 0 reserved for default no mirror
        DOCA_FLOW_SHARED_RESOURCE_MIRROR);
    check_and_throw_doca(result,
                         "doca_flow_cfg_set_nr_shared_resource(mirror) failed");
  }

	std::vector<uint16_t> rss_queues(nb_queues);
	for (uint16_t i = 0; i < nb_queues; ++i)
		rss_queues[i] = i;

	struct doca_flow_resource_rss_cfg rss_cfg{};
	rss_cfg.nr_queues = nb_queues;
	rss_cfg.queues_array = rss_queues.data();

	result = doca_flow_cfg_set_default_rss(cfg_guard.get(), &rss_cfg);
	check_and_throw_doca(result, "doca_flow_cfg_set_default_rss failed");

	result = doca_flow_init(cfg_guard.get());
	check_and_throw_doca(result, "doca_flow_init failed");

	DOCA_LOG_INFO("DOCA flow initialized (queues=%u)", nb_queues);
}

doca_flow_port *MonitoringDoca::start_port(struct doca_dev *dev, uint16_t port_id) {
	GuardedPortCfg cfg_guard;

	doca_error_t result = doca_flow_port_cfg_set_port_id(cfg_guard.get(), port_id);
	check_and_throw_doca(result, "doca_flow_port_cfg_set_port_id failed");

	result = doca_flow_port_cfg_set_dev(cfg_guard.get(), dev);
	check_and_throw_doca(result, "doca_flow_port_cfg_set_dev failed");

	result = doca_flow_port_cfg_set_priv_data_size(cfg_guard.get(), 0);
	check_and_throw_doca(result, "doca_flow_port_cfg_set_priv_data_size failed");

	result = doca_flow_port_cfg_set_actions_mem_size(
		cfg_guard.get(), (16 * DOCA_FLOW_MAX_ENTRY_ACTIONS_MEM_SIZE));
	check_and_throw_doca(result, "doca_flow_port_cfg_set_actions_mem_size failed");

	doca_flow_port *port = nullptr;
	result = doca_flow_port_start(cfg_guard.get(), &port);
	check_and_throw_doca(result, "doca_flow_port_start failed");

	DOCA_LOG_INFO("DOCA port started (port=%u)", port_id);
	return port;
}

doca_flow_port *MonitoringDoca::start_port_rep(struct doca_dev_rep *dev_rep, uint16_t port_id) {
	GuardedPortCfg cfg_guard;

	doca_error_t result = doca_flow_port_cfg_set_port_id(cfg_guard.get(), port_id);
	check_and_throw_doca(result, "doca_flow_port_cfg_set_port_id failed");

	result = doca_flow_port_cfg_set_dev_rep(cfg_guard.get(), dev_rep);
	check_and_throw_doca(result, "doca_flow_port_cfg_set_dev_rep failed");

	result = doca_flow_port_cfg_set_actions_mem_size(
		cfg_guard.get(), (16 * DOCA_FLOW_MAX_ENTRY_ACTIONS_MEM_SIZE));
	check_and_throw_doca(result, "doca_flow_port_cfg_set_actions_mem_size failed");

	doca_flow_port *port = nullptr;
	result = doca_flow_port_start(cfg_guard.get(), &port);
	check_and_throw_doca(result, "doca_flow_port_start failed");

	DOCA_LOG_INFO("DOCA port started (port=%u)", port_id);
	return port;
}

void MonitoringDoca::create_control_pipe(uint16_t port_pair_idx)
{
	GuardedPipeCfg cfg_guard{switch_ports_[monitor_port_ids_[port_pair_idx]]};

	doca_error_t result = doca_flow_pipe_cfg_set_name(cfg_guard.get(), kMonitoringPipeControlName);
	check_and_throw_doca(result, "doca_flow_pipe_cfg_set_name(control) failed");

	result = doca_flow_pipe_cfg_set_type(cfg_guard.get(), DOCA_FLOW_PIPE_CONTROL);
	check_and_throw_doca(result, "doca_flow_pipe_cfg_set_type(control) failed");

	result = doca_flow_pipe_cfg_set_is_root(cfg_guard.get(), true);
	check_and_throw_doca(result, "doca_flow_pipe_cfg_set_is_root(control) failed");

	result = doca_flow_pipe_create(cfg_guard.get(), NULL, NULL, &control_pipes_[port_pair_idx]);
	check_and_throw_doca(result, "doca_flow_pipe_create(control) failed");

	DOCA_LOG_INFO("pipeline control pipe created");
}

void MonitoringDoca::build_monitor_output_pipe(uint16_t nb_queues,
                                    uint16_t port_pair_idx) {
  if ((main_dest_type_ != MonitoringConfig::MonitoringDest::RSS &&
       main_dest_type_ != MonitoringConfig::MonitoringDest::DEVICE) &&
      (clone_dest_type_ != MonitoringConfig::MonitoringDest::RSS &&
       clone_dest_type_ != MonitoringConfig::MonitoringDest::DEVICE))
    return;
                                          
  doca_flow_match match{};
  doca_flow_actions actions{};
  struct doca_flow_actions *actions_arr[1] = {&actions};
  doca_flow_fwd fwd{};
  doca_flow_monitor monitor{};

  GuardedPipeCfg cfg_guard{switch_ports_[monitor_port_ids_[port_pair_idx]]};

  doca_error_t result =
      doca_flow_pipe_cfg_set_name(cfg_guard.get(), "MONITOR_OUTPUT_PIPE");
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_name(output) failed");

  result = doca_flow_pipe_cfg_set_is_root(cfg_guard.get(), false);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_is_root(output) failed");

  result = doca_flow_pipe_cfg_set_match(cfg_guard.get(), &match, NULL);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_match(output) failed");

  result = doca_flow_pipe_cfg_set_actions(cfg_guard.get(), actions_arr, NULL,
                                          NULL, 1);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_actions(output) failed");

  if (counters_enabled_) {
    monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
    result = doca_flow_pipe_cfg_set_monitor(cfg_guard.get(), &monitor);
    if (result != DOCA_SUCCESS) {
      if (result != DOCA_ERROR_NOT_SUPPORTED) {
        DOCA_LOG_ERR("doca_flow_pipe_cfg_set_monitor(output): %s",
                   doca_error_get_descr(result));
        throw std::runtime_error("failed to set output pipe monitor");
      }
    }
  }

  std::vector<uint16_t> rss_queues(nb_queues);
  for (uint16_t i = 0; i < nb_queues; ++i)
    rss_queues[i] = i;

  if (main_dest_type_ == MonitoringConfig::MonitoringDest::RSS ||
      clone_dest_type_ == MonitoringConfig::MonitoringDest::RSS) {
      fwd.type = DOCA_FLOW_FWD_RSS;
      fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
      fwd.rss.inner_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
      fwd.rss.nr_queues = nb_queues;
      fwd.rss.queues_array = rss_queues.data();
  } else if (main_dest_type_ == MonitoringConfig::MonitoringDest::DEVICE ||
             clone_dest_type_ == MonitoringConfig::MonitoringDest::DEVICE) {
      if (port_pair_idx >= forward_port_ids_.size()) {
          throw std::runtime_error("forward_port_ids index out of bounds");
      }
      fwd.type = DOCA_FLOW_FWD_PORT;
      fwd.port_id = forward_port_ids_[port_pair_idx];
  }

  result = doca_flow_pipe_create(cfg_guard.get(), &fwd, NULL,
                                 &monitor_output_pipes_[port_pair_idx]);
  check_and_throw_doca(result, "doca_flow_pipe_create(output) failed");

  result = doca_flow_pipe_add_entry(0, monitor_output_pipes_[port_pair_idx], &match,
                                    &actions, NULL, &fwd, 0, NULL,
                                    &rss_entries_[port_pair_idx]);
  check_and_throw_doca(result, "doca_flow_pipe_add_entry(output) failed");

  result =
      doca_flow_entries_process(switch_ports_[monitor_port_ids_[port_pair_idx]],
                                0, kMonitoringPipePullTimeout, 1);
  check_and_throw_doca(result, "doca_flow_entries_process(output) failed");

  DOCA_LOG_INFO("pipeline monitor output pipe created");
}

void MonitoringDoca::setup_mirror(uint16_t port_pair_idx) {
  doca_flow_mirror_target target{};
  target.fwd.type = DOCA_FLOW_FWD_PIPE;
  
  if (clone_dest_type_ == MonitoringConfig::MonitoringDest::DROP) {
      target.fwd.type = DOCA_FLOW_FWD_DROP;
  } else if (clone_dest_type_ == MonitoringConfig::MonitoringDest::HOST) {
      if (host_pipes_[port_pair_idx] == nullptr)
          throw std::runtime_error("host pipe for clone not available");
      target.fwd.next_pipe = host_pipes_[port_pair_idx];
  } else {
      // RSS or DEVICE
      if (monitor_output_pipes_[port_pair_idx] == nullptr)
          throw std::runtime_error("monitor output pipe for clone not available");
      target.fwd.next_pipe = monitor_output_pipes_[port_pair_idx];
  }

  doca_flow_resource_mirror_cfg mirror_cfg{};
  mirror_cfg.nr_targets = 1;
  mirror_cfg.target = &target;

  doca_flow_shared_resource_cfg shared_cfg{};
  shared_cfg.mirror_cfg = mirror_cfg;

  doca_error_t result = doca_flow_shared_resource_set_cfg(
      DOCA_FLOW_SHARED_RESOURCE_MIRROR,
      port_pair_idx + 1, // +1 for 0 reserved for default no mirror
      &shared_cfg);
  check_and_throw_doca(result, "doca_flow_shared_resource_set_cfg(mirror) failed");

  uint32_t mirror_ids[1] = {(uint32_t)(port_pair_idx + 1)};
  result = doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_MIRROR,
						mirror_ids,
						1,
						switch_ports_[monitor_port_ids_[port_pair_idx]]);
  check_and_throw_doca(result, "doca_flow_shared_resources_bind(mirror) failed");

  DOCA_LOG_INFO("pipeline mirror configured (id=%u)", port_pair_idx + 1);
}

void MonitoringDoca::create_host_pipe(const MonitoringConfig &/*cfg*/, uint16_t port_pair_idx)
{
	if ((main_dest_type_ != MonitoringConfig::MonitoringDest::HOST &&
         clone_dest_type_ != MonitoringConfig::MonitoringDest::HOST) || 
        switch_ports_[monitor_port_ids_[port_pair_idx]] == nullptr)
		return;

	GuardedPipeCfg cfg_guard{switch_ports_[monitor_port_ids_[port_pair_idx]]};

	doca_flow_match match{};
	doca_flow_actions actions{};
	struct doca_flow_actions *actions_arr[1] = {&actions};

	doca_flow_fwd fwd{};
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = UINT16_MAX; // changeable

	doca_flow_fwd fwd_miss{};
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	doca_flow_monitor monitor{};
	if (counters_enabled_)
		monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

	doca_error_t result = doca_flow_pipe_cfg_set_name(cfg_guard.get(), "HOST_PIPE");
	check_and_throw_doca(result, "doca_flow_pipe_cfg_set_name(host_pipe) failed");

	result = doca_flow_pipe_cfg_set_domain(cfg_guard.get(), DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
	check_and_throw_doca(result, "doca_flow_pipe_cfg_set_domain(host_pipe) failed");

	result = doca_flow_pipe_cfg_set_type(cfg_guard.get(), DOCA_FLOW_PIPE_BASIC);
	check_and_throw_doca(result, "doca_flow_pipe_cfg_set_type(host_pipe) failed");

	result = doca_flow_pipe_cfg_set_is_root(cfg_guard.get(), false);
	check_and_throw_doca(result, "doca_flow_pipe_cfg_set_is_root(host_pipe) failed");

	result = doca_flow_pipe_cfg_set_match(cfg_guard.get(), &match, NULL);
	check_and_throw_doca(result, "doca_flow_pipe_cfg_set_match(host_pipe) failed");

	result = doca_flow_pipe_cfg_set_actions(cfg_guard.get(), actions_arr, NULL, NULL, 1);
	check_and_throw_doca(result, "doca_flow_pipe_cfg_set_actions(host_pipe) failed");

	if (counters_enabled_) {
		result = doca_flow_pipe_cfg_set_monitor(cfg_guard.get(), &monitor);
        if (result != DOCA_SUCCESS && result != DOCA_ERROR_NOT_SUPPORTED)
		    check_and_throw_doca(result, "doca_flow_pipe_cfg_set_monitor(host_pipe) failed");
	}

	result = doca_flow_pipe_create(cfg_guard.get(), &fwd, &fwd_miss, &host_pipes_[port_pair_idx]);
	check_and_throw_doca(result, "doca_flow_pipe_create(host_pipe) failed");

	fwd.port_id = host_port_ids_[port_pair_idx];
	result = doca_flow_pipe_add_entry(0,
					host_pipes_[port_pair_idx],
					&match,
					&actions,
					NULL,
					&fwd,
					0,
					NULL,
					&host_entries_[port_pair_idx]);
	check_and_throw_doca(result, "doca_flow_pipe_add_entry(host_pipe) failed");

	result = doca_flow_entries_process(switch_ports_[monitor_port_ids_[port_pair_idx]], 0, kMonitoringPipePullTimeout, 1);
	check_and_throw_doca(result, "doca_flow_entries_process(host_pipe) failed");

	DOCA_LOG_INFO("pipeline host entry installed (forward to port %u)", fwd.port_id);
}

void MonitoringDoca::install_default_drop(uint16_t port_pair_idx)
{
	if (control_pipes_[port_pair_idx] == nullptr)
		throw std::runtime_error("control pipe not available");

	doca_flow_match match{};
	doca_flow_fwd fwd{};
	fwd.type = DOCA_FLOW_FWD_DROP;

	struct doca_flow_pipe_entry *entry = NULL;
	doca_error_t result = doca_flow_pipe_control_add_entry(
		0,
		7,
		control_pipes_[port_pair_idx],
		&match,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		&fwd,
		NULL,
		&entry);
	check_and_throw_doca(result, "doca_flow_pipe_control_add_entry(default_drop) failed");

	result = doca_flow_entries_process(switch_ports_[monitor_port_ids_[port_pair_idx]], 0, kMonitoringPipePullTimeout, 1);
	check_and_throw_doca(result, "doca_flow_entries_process(default_drop) failed");

	DOCA_LOG_INFO("pipeline default drop entry installed");
}

void MonitoringDoca::add_monitor_rule(const MonitoringConfig &cfg, uint16_t port_pair_idx)
{
	if (control_pipes_[port_pair_idx] == nullptr)
		throw std::runtime_error("control pipe not available");

	doca_flow_match match{};
	match.parser_meta.port_id = monitor_port_ids_[port_pair_idx];
	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
	match.outer.udp.l4_port.dst_port = rte_cpu_to_be_16(cfg.monitor_udp_port);

	doca_flow_fwd fwd{};
	if (main_dest_type_ == MonitoringConfig::MonitoringDest::HOST) {
		if (host_pipes_[port_pair_idx] == nullptr)
			throw std::runtime_error("host pipe for main destination not available");
		fwd.type = DOCA_FLOW_FWD_PIPE;
		fwd.next_pipe = host_pipes_[port_pair_idx];
	} else if (main_dest_type_ == MonitoringConfig::MonitoringDest::DROP) {
		fwd.type = DOCA_FLOW_FWD_DROP;
	} else {
        // RSS or DEVICE
        if (monitor_output_pipes_[port_pair_idx] == nullptr)
			throw std::runtime_error("monitor output pipe for main destination not available");
        fwd.type = DOCA_FLOW_FWD_PIPE;
        fwd.next_pipe = monitor_output_pipes_[port_pair_idx];
    }

	doca_flow_monitor monitor{};
	if (enable_mirroring_) {
		monitor.shared_mirror_id = port_pair_idx + 1; // Always set mirror id to ensure hardware mapping
        DOCA_LOG_INFO("Using mirror id %u for port pair %u", monitor.shared_mirror_id, port_pair_idx);
    }

	struct doca_flow_pipe_entry *entry = NULL;
	doca_error_t result = doca_flow_pipe_control_add_entry(
		0,
		0,
		control_pipes_[port_pair_idx],
		&match,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		&monitor,
		&fwd,
		NULL,
		&entry);
	check_and_throw_doca(result, "doca_flow_pipe_control_add_entry(monitor) failed");

	result = doca_flow_entries_process(switch_ports_[monitor_port_ids_[port_pair_idx]], 0, kMonitoringPipePullTimeout, 1);
	check_and_throw_doca(result, "doca_flow_entries_process(monitor) failed");

	DOCA_LOG_INFO("pipeline monitor rule installed (udp dst port %u)", cfg.monitor_udp_port);
}

// host_encap and encap_control rules removed

void MonitoringDoca::build_pipeline(const MonitoringConfig &cfg, uint16_t nb_queues)
{
	for (size_t i = 0; i < monitor_port_ids_.size(); ++i) {
		if (switch_ports_[monitor_port_ids_[i]] == nullptr)
			throw std::runtime_error("pipeline build requires monitor port");

        if (main_dest_type_ == MonitoringConfig::MonitoringDest::HOST || 
            clone_dest_type_ == MonitoringConfig::MonitoringDest::HOST) {
			create_host_pipe(cfg, i);
		} 
        
        if (main_dest_type_ == MonitoringConfig::MonitoringDest::RSS || 
            main_dest_type_ == MonitoringConfig::MonitoringDest::DEVICE ||
            clone_dest_type_ == MonitoringConfig::MonitoringDest::RSS || 
            clone_dest_type_ == MonitoringConfig::MonitoringDest::DEVICE) {
			build_monitor_output_pipe(nb_queues, i);
		}

        if (enable_mirroring_) {
            setup_mirror(i);
        }

		create_control_pipe(i);
		install_default_drop(i);
		add_monitor_rule(cfg, i);
	}

	DOCA_LOG_INFO("pipeline build complete (ports=%zu, queues=%u, main_dest=%s, clone_dest=%s)",
		      monitor_port_ids_.size(),
		      nb_queues,
		      main_dest_type_ == MonitoringConfig::MonitoringDest::RSS ? "rss" :
              main_dest_type_ == MonitoringConfig::MonitoringDest::HOST ? "host" :
              main_dest_type_ == MonitoringConfig::MonitoringDest::DROP ? "drop" : "device",
              enable_mirroring_ ? (clone_dest_type_ == MonitoringConfig::MonitoringDest::RSS ? "rss" :
              clone_dest_type_ == MonitoringConfig::MonitoringDest::HOST ? "host" :
              clone_dest_type_ == MonitoringConfig::MonitoringDest::DROP ? "drop" : "device") : "disabled");
}
