#include "monitoring_doca.h"

#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <doca_dpdk.h>
#include <doca_error.h>
#include <doca_flow.h>
#include <doca_log.h>
#include <netinet/in.h>
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip.h>

#include "device_manager.h"
#include "monitoring_config.h"

#include <rte_memcpy.h>

DOCA_LOG_REGISTER(MONITORING_DOCA);

namespace {

constexpr const char *kMonitoringDocaMode =
    "switch,hws,isolated,expert,hairpinq_num=4";
constexpr uint32_t kMonitoringDocaNrCounters = 128;
constexpr uint32_t kMonitoringPipePullTimeout = 10000;


inline void check_and_throw_doca(doca_error_t result,
                                 const char *potential_error_message) {
  if (result != DOCA_SUCCESS) {
    DOCA_LOG_ERR("%s: %s", potential_error_message,
                 doca_error_get_descr(result));
    throw std::runtime_error(potential_error_message);
  }
}

inline void check_and_warn_doca(doca_error_t result,
                                const char *potential_error_message) {
  if (result != DOCA_SUCCESS) {
    DOCA_LOG_WARN("%s: %s", potential_error_message,
                  doca_error_get_descr(result));
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
  ~GuardedFlowCfg() {
    if (cfg_ != nullptr) {
      check_and_warn_doca(doca_flow_cfg_destroy(cfg_), "doca_flow_cfg_destroy");
    }
  }

private:
  doca_flow_cfg *cfg_{nullptr};
};

struct GuardedPipeCfg {
  explicit GuardedPipeCfg(doca_flow_port *port) {
    check_and_throw_doca(doca_flow_pipe_cfg_create(&cfg_, port),
                         "doca_flow_pipe_cfg_create failed");
  }
  GuardedPipeCfg(const GuardedPipeCfg &) = delete;
  GuardedPipeCfg &operator=(const GuardedPipeCfg &) = delete;
  GuardedPipeCfg(GuardedPipeCfg &&) = default;
  GuardedPipeCfg &operator=(GuardedPipeCfg &&) = default;
  inline doca_flow_pipe_cfg *get() const { return cfg_; }
  ~GuardedPipeCfg() {
    if (cfg_ != nullptr) {
      check_and_warn_doca(doca_flow_pipe_cfg_destroy(cfg_),
                          "doca_flow_pipe_cfg_destroy");
    }
  }

private:
  doca_flow_pipe_cfg *cfg_{nullptr};
};

struct GuardedPortCfg {
  explicit GuardedPortCfg() {
    check_and_throw_doca(doca_flow_port_cfg_create(&cfg_),
                         "doca_flow_port_cfg_create failed");
  }
  GuardedPortCfg(const GuardedPortCfg &) = delete;
  GuardedPortCfg &operator=(const GuardedPortCfg &) = delete;
  inline doca_flow_port_cfg *get() const { return cfg_; }
  ~GuardedPortCfg() {
    if (cfg_ != nullptr) {
      check_and_warn_doca(doca_flow_port_cfg_destroy(cfg_),
                          "doca_flow_port_cfg_destroy");
    }
  }

private:
  doca_flow_port_cfg *cfg_{nullptr};
};

struct __attribute__((packed)) MonitoringEspHdr {
  rte_be32_t spi;
  rte_be32_t seq;
  uint8_t iv[8];
};

struct __attribute__((packed)) MonitoringEncapTemplate {
  rte_ether_hdr eth;
  rte_ipv4_hdr ip;
  MonitoringEspHdr esp;
};

static_assert(sizeof(MonitoringEncapTemplate) == 50,
              "Unexpected encap template size");

} // namespace

MonitoringDoca::MonitoringDoca(DeviceManager &device_manager,
                               const std::vector<uint16_t> &monitor_port_ids,
                               const std::vector<uint16_t> &host_port_ids,
                               uint16_t dpdk_nb_queues,
                               const MonitoringConfig &cfg,
                               const std::vector<uint16_t> &forward_port_ids)
    : host_branch_enabled_(cfg.enable_host_branch),
      monitor_dest_type_(cfg.monitoring_dest_type),
      forward_port_ids_(forward_port_ids),
      counters_enabled_(cfg.enable_counters),
      sampling_fraction_(cfg.sampling_fraction),
      monitor_port_ids_(monitor_port_ids), host_port_ids_(host_port_ids),
      control_pipes_(monitor_port_ids_.size(), nullptr),
      monitor_output_pipes_(monitor_port_ids_.size(), nullptr),
      sampling_pipes_(monitor_port_ids_.size(), nullptr),
      host_decrypt_pipes_(monitor_port_ids_.size(), nullptr),
      host_decap_pipes_(monitor_port_ids_.size(), nullptr),
      drop_pipes_(monitor_port_ids_.size(), nullptr),
      to_host_pipes_(monitor_port_ids_.size(), nullptr),
      host_encap_pipes_(monitor_port_ids_.size(), nullptr),
      drop_entries_(monitor_port_ids_.size(), nullptr),
      host_decrypt_entries_(monitor_port_ids_.size(), nullptr),
      host_decap_entries_(monitor_port_ids_.size(), nullptr),
      host_encap_entries_(monitor_port_ids_.size(), nullptr),
      rss_entries_(monitor_port_ids_.size(), nullptr),
      sampling_entries_(monitor_port_ids_.size()),
      to_host_entries_(monitor_port_ids_.size(), nullptr),
      rss_last_packets_(monitor_port_ids_.size(), 0),
      rss_last_bytes_(monitor_port_ids_.size(), 0),
      host_decrypt_last_packets_(monitor_port_ids_.size(), 0),
      host_decrypt_last_bytes_(monitor_port_ids_.size(), 0),
      host_decap_last_packets_(monitor_port_ids_.size(), 0),
      host_decap_last_bytes_(monitor_port_ids_.size(), 0),
      host_encap_last_packets_(monitor_port_ids_.size(), 0),
      host_encap_last_bytes_(monitor_port_ids_.size(), 0),
      drop_last_packets_(monitor_port_ids_.size(), 0),
      drop_last_bytes_(monitor_port_ids_.size(), 0),
      to_host_last_packets_(monitor_port_ids_.size(), 0),
      to_host_last_bytes_(monitor_port_ids_.size(), 0) {
  std::stringstream ports_msg;
  ports_msg << "monitor_port=";
  for (const auto &port_id : monitor_port_ids_) {
    if (&port_id != &monitor_port_ids_.front())
      ports_msg << ",";
    ports_msg << port_id;
  }
  ports_msg << " host_port=";
  if (host_branch_enabled_) {
    for (const auto &port_id : host_port_ids_) {
      if (&port_id != &host_port_ids_.front())
        ports_msg << ",";
      ports_msg << port_id;
    }
  } else {
    ports_msg << "none";
  }
  ports_msg << std::endl;
  DOCA_LOG_INFO("DOCA init: %s", ports_msg.str().c_str());
  initialize_flow(dpdk_nb_queues);

  // switch mode: start devices
  for (uint16_t port_id = 0; port_id < device_manager.get_port_count();
       ++port_id) {
    if (!device_manager.is_device_rep(port_id)) {
      doca_flow_port *port =
          start_port(device_manager.get_device(port_id), port_id);
      switch_ports_.push_back(port);
    } else {
      doca_flow_port *port =
          start_port_rep(device_manager.get_device_rep(port_id), port_id);
      switch_ports_.push_back(port);
    }
  }

  build_pipeline(cfg, dpdk_nb_queues);

  last_counter_timestamp_ = std::chrono::steady_clock::now();
}

MonitoringDoca::~MonitoringDoca() {
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
    sampling_entries_[i].clear();
  }

  for (size_t i = 0; i < sampling_pipes_.size(); ++i) {
    if (sampling_pipes_[i] != nullptr) {
      doca_flow_pipe_destroy(sampling_pipes_[i]);
      sampling_pipes_[i] = nullptr;
    }
  }

  for (size_t i = 0; i < host_decrypt_pipes_.size(); ++i) {
    if (host_decrypt_pipes_[i] != nullptr) {
      doca_flow_pipe_destroy(host_decrypt_pipes_[i]);
      host_decrypt_pipes_[i] = nullptr;
    }
  }

  for (size_t i = 0; i < host_decap_pipes_.size(); ++i) {
    if (host_decap_pipes_[i] != nullptr) {
      doca_flow_pipe_destroy(host_decap_pipes_[i]);
      host_decap_pipes_[i] = nullptr;
    }
  }

  for (size_t i = 0; i < drop_pipes_.size(); ++i) {
    if (drop_pipes_[i] != nullptr) {
      doca_flow_pipe_destroy(drop_pipes_[i]);
      drop_pipes_[i] = nullptr;
    }
  }

  for (size_t i = 0; i < to_host_pipes_.size(); ++i) {
    if (to_host_pipes_[i] != nullptr) {
      doca_flow_pipe_destroy(to_host_pipes_[i]);
      to_host_pipes_[i] = nullptr;
    }
  }

  for (size_t i = 0; i < host_encap_pipes_.size(); ++i) {
    if (host_encap_pipes_[i] != nullptr) {
      doca_flow_pipe_destroy(host_encap_pipes_[i]);
      host_encap_pipes_[i] = nullptr;
    }
  }
  for (uint16_t port_id = switch_ports_.size() - 1; port_id != (uint16_t)-1;
       --port_id) {
    doca_flow_port *port = switch_ports_[port_id];
    doca_flow_port_stop(port);
    DOCA_LOG_INFO("DOCA port stopped (port=%u)", port_id);
  }

  doca_flow_destroy();
  DOCA_LOG_INFO("DOCA flow destroyed");
}

int MonitoringDoca::print_stats(FILE *stream) {
  if (stream == nullptr || switch_ports_.empty())
    return -1;

  auto now = std::chrono::steady_clock::now();
  double elapsed =
      std::chrono::duration<double>(now - last_counter_timestamp_).count();
  if (elapsed <= 0.0)
    elapsed = 1.0;

  for (size_t i = 0; i < rss_entries_.size(); ++i) {
    if (rss_entries_[i] != nullptr) {
      struct doca_flow_resource_query query{};
      doca_error_t qres =
          doca_flow_resource_query_entry(rss_entries_[i], &query);
      if (qres == DOCA_SUCCESS) {
        uint64_t delta_packets =
            query.counter.total_pkts - rss_last_packets_[i];
        uint64_t delta_bytes = query.counter.total_bytes - rss_last_bytes_[i];
        rss_last_packets_[i] = query.counter.total_pkts;
        rss_last_bytes_[i] = query.counter.total_bytes;
        double packets_per_sec = delta_packets / elapsed;
        double mb_per_sec = (delta_bytes / (1024.0 * 1024.0)) / elapsed;
        fprintf(stream, "DOCA Monitor output pipe: %.2f pkt/s %.2f MB/s\n",
                packets_per_sec, mb_per_sec);
      } else if (qres != DOCA_ERROR_NOT_SUPPORTED) {
        DOCA_LOG_WARN("doca_flow_resource_query_entry failed: %s",
                      doca_error_get_descr(qres));
      }
    }
  }

  for (size_t i = 0; i < to_host_entries_.size(); ++i) {
    struct doca_flow_resource_query query{};
    doca_error_t qres =
        doca_flow_resource_query_entry(to_host_entries_[i], &query);
    if (qres == DOCA_SUCCESS) {
        uint64_t delta_packets =
            query.counter.total_pkts - to_host_last_packets_[i];
        uint64_t delta_bytes = query.counter.total_bytes - to_host_last_bytes_[i
      ];
        to_host_last_packets_[i] = query.counter.total_pkts;
        to_host_last_bytes_[i] = query.counter.total_bytes;
        double packets_per_sec = delta_packets / elapsed;
        double mb_per_sec = (delta_bytes / (1024.0 * 1024.0)) / elapsed;
        fprintf(stream, "DOCA To-Host pipe: %.2f pkt/s %.2f MB/s\n",
                packets_per_sec, mb_per_sec);
      } else if (qres != DOCA_ERROR_NOT_SUPPORTED) {
        DOCA_LOG_WARN("doca_flow_resource_query_entry(to_host) failed: %s",
                      doca_error_get_descr(qres));
      }
  }

  for (size_t i = 0; i < host_decrypt_entries_.size(); ++i) {
    struct doca_flow_resource_query query{};
    doca_error_t qres =
        doca_flow_resource_query_entry(host_decrypt_entries_[i], &query);
    if (qres == DOCA_SUCCESS) {
      uint64_t delta_packets =
          query.counter.total_pkts - host_decrypt_last_packets_[i];
      uint64_t delta_bytes =
          query.counter.total_bytes - host_decrypt_last_bytes_[i];
      host_decrypt_last_packets_[i] = query.counter.total_pkts;
      host_decrypt_last_bytes_[i] = query.counter.total_bytes;
      double packets_per_sec = delta_packets / elapsed;
      double mb_per_sec = (delta_bytes / (1024.0 * 1024.0)) / elapsed;
      fprintf(stream, "DOCA Host decrypt pipe: %.2f pkt/s %.2f MB/s\n",
              packets_per_sec, mb_per_sec);
    } else if (qres != DOCA_ERROR_NOT_SUPPORTED) {
      DOCA_LOG_WARN("doca_flow_resource_query_entry(decrypt) failed: %s",
                    doca_error_get_descr(qres));
    }
  }

  for (size_t i = 0; i < host_decap_entries_.size(); ++i) {
    struct doca_flow_resource_query query{};
    doca_error_t qres =
        doca_flow_resource_query_entry(host_decap_entries_[i], &query);
    if (qres == DOCA_SUCCESS) {
      uint64_t delta_packets =
          query.counter.total_pkts - host_decap_last_packets_[i];
      uint64_t delta_bytes =
          query.counter.total_bytes - host_decap_last_bytes_[i];
      host_decap_last_packets_[i] = query.counter.total_pkts;
      host_decap_last_bytes_[i] = query.counter.total_bytes;
      double packets_per_sec = delta_packets / elapsed;
      double mb_per_sec = (delta_bytes / (1024.0 * 1024.0)) / elapsed;
      fprintf(stream, "DOCA Host decap pipe: %.2f pkt/s %.2f MB/s\n",
              packets_per_sec, mb_per_sec);
    } else if (qres != DOCA_ERROR_NOT_SUPPORTED) {
      DOCA_LOG_WARN("doca_flow_resource_query_entry failed: %s",
                    doca_error_get_descr(qres));
    }
  }

  for (size_t i = 0; i < host_encap_entries_.size(); ++i) {
    struct doca_flow_resource_query query{};
    doca_error_t qres =
        doca_flow_resource_query_entry(host_encap_entries_[i], &query);
    if (qres == DOCA_SUCCESS) {
      uint64_t delta_packets =
          query.counter.total_pkts - host_encap_last_packets_[i];
      uint64_t delta_bytes =
          query.counter.total_bytes - host_encap_last_bytes_[i];
      host_encap_last_packets_[i] = query.counter.total_pkts;
      host_encap_last_bytes_[i] = query.counter.total_bytes;
      double packets_per_sec = delta_packets / elapsed;
      double mb_per_sec = (delta_bytes / (1024.0 * 1024.0)) / elapsed;
      fprintf(stream, "DOCA Host encap pipe: %.2f pkt/s %.2f MB/s\n",
              packets_per_sec, mb_per_sec);
    } else if (qres != DOCA_ERROR_NOT_SUPPORTED) {
      DOCA_LOG_WARN("doca_flow_resource_query_entry failed: %s",
                    doca_error_get_descr(qres));
    }
  }

  for (size_t i = 0; i < drop_entries_.size(); ++i) {
    struct doca_flow_resource_query query{};
    doca_error_t qres =
        doca_flow_resource_query_entry(drop_entries_[i], &query);
    if (qres == DOCA_SUCCESS) {
      uint64_t delta_packets =
          query.counter.total_pkts - drop_last_packets_[i];
      uint64_t delta_bytes =
          query.counter.total_bytes - drop_last_bytes_[i];
      drop_last_packets_[i] = query.counter.total_pkts;
      drop_last_bytes_[i] = query.counter.total_bytes;
      double packets_per_sec = delta_packets / elapsed;
      double mb_per_sec = (delta_bytes / (1024.0 * 1024.0)) / elapsed;
      fprintf(stream, "DOCA Drop pipe: %.2f pkt/s %.2f MB/s\n",
              packets_per_sec, mb_per_sec);
    } else if (qres != DOCA_ERROR_NOT_SUPPORTED) {
      DOCA_LOG_WARN("doca_flow_resource_query_entry(drop) failed: %s",
                    doca_error_get_descr(qres));
    }
  }

  last_counter_timestamp_ = now;
  return 0;
}

void MonitoringDoca::initialize_flow(uint16_t nb_queues) {
  if (nb_queues == 0 && monitor_dest_type_ == MonitoringConfig::MonitoringDest::DPDK) {
    DOCA_LOG_WARN("no DPDK queues available; RSS pipe will not be built");
    return;
  }

  GuardedFlowCfg cfg_guard{};

  doca_error_t result =
      doca_flow_cfg_set_pipe_queues(cfg_guard.get(), nb_queues);
  check_and_throw_doca(result, "doca_flow_cfg_set_pipe_queues failed");

  result = doca_flow_cfg_set_mode_args(cfg_guard.get(), kMonitoringDocaMode);
  check_and_throw_doca(result, "doca_flow_cfg_set_mode_args failed");

  result =
      doca_flow_cfg_set_nr_counters(cfg_guard.get(), kMonitoringDocaNrCounters);
  check_and_throw_doca(result, "doca_flow_cfg_set_nr_counters failed");

  if (monitor_dest_type_ != MonitoringConfig::MonitoringDest::NONE) {
    result = doca_flow_cfg_set_nr_shared_resource(
        cfg_guard.get(),
        monitor_port_ids_.size() + 1, // +1 for 0 reserved for default no mirror
        DOCA_FLOW_SHARED_RESOURCE_MIRROR);
    check_and_throw_doca(result,
                         "doca_flow_cfg_set_nr_shared_resource(mirror) failed");
  }

  // Allocate IPsec SA resources if host branch is enabled
  // Need 2 SAs per port (encap + decap), using IDs: port_idx*2+1 for encap,
  // port_idx*2+2 for decap Index 0 is reserved (similar to mirror)
  if (host_branch_enabled_) {
    uint32_t max_sa_id = monitor_port_ids_.size() * 2 +
                         1; // Highest SA ID is (size-1)*2+2, +1 for index 0
    result = doca_flow_cfg_set_nr_shared_resource(
        cfg_guard.get(),
        max_sa_id, // Allocate enough for port_idx*2+2 (e.g., for 2 ports: IDs
                   // 0(reserved),1,2,3,4 = 5 total)
        DOCA_FLOW_SHARED_RESOURCE_IPSEC_SA);
    check_and_throw_doca(
        result, "doca_flow_cfg_set_nr_shared_resource(ipsec_sa) failed");
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

doca_flow_port *MonitoringDoca::start_port(struct doca_dev *dev,
                                           uint16_t port_id) {
  GuardedPortCfg cfg_guard;

  doca_error_t result =
      doca_flow_port_cfg_set_port_id(cfg_guard.get(), port_id);
  check_and_throw_doca(result, "doca_flow_port_cfg_set_port_id failed");

  result = doca_flow_port_cfg_set_dev(cfg_guard.get(), dev);
  check_and_throw_doca(result, "doca_flow_port_cfg_set_dev failed");

  result = doca_flow_port_cfg_set_priv_data_size(cfg_guard.get(), 0);
  check_and_throw_doca(result, "doca_flow_port_cfg_set_priv_data_size failed");

  result = doca_flow_port_cfg_set_actions_mem_size(
      cfg_guard.get(), (16 * DOCA_FLOW_MAX_ENTRY_ACTIONS_MEM_SIZE));
  check_and_throw_doca(result,
                       "doca_flow_port_cfg_set_actions_mem_size failed");

  doca_flow_port *port = nullptr;
  result = doca_flow_port_start(cfg_guard.get(), &port);
  check_and_throw_doca(result, "doca_flow_port_start failed");

  DOCA_LOG_INFO("DOCA port started (port=%u)", port_id);
  return port;
}

doca_flow_port *MonitoringDoca::start_port_rep(struct doca_dev_rep *dev_rep,
                                               uint16_t port_id) {
  GuardedPortCfg cfg_guard;

  doca_error_t result =
      doca_flow_port_cfg_set_port_id(cfg_guard.get(), port_id);
  check_and_throw_doca(result, "doca_flow_port_cfg_set_port_id failed");

  result = doca_flow_port_cfg_set_dev_rep(cfg_guard.get(), dev_rep);
  check_and_throw_doca(result, "doca_flow_port_cfg_set_dev_rep failed");

  result = doca_flow_port_cfg_set_actions_mem_size(
      cfg_guard.get(), (16 * DOCA_FLOW_MAX_ENTRY_ACTIONS_MEM_SIZE));
  check_and_throw_doca(result,
                       "doca_flow_port_cfg_set_actions_mem_size failed");

  doca_flow_port *port = nullptr;
  result = doca_flow_port_start(cfg_guard.get(), &port);
  check_and_throw_doca(result, "doca_flow_port_start failed");

  DOCA_LOG_INFO("DOCA port started (port=%u)", port_id);
  return port;
}

void MonitoringDoca::create_control_pipe(uint16_t port_pair_idx) {
  GuardedPipeCfg cfg_guard{switch_ports_[monitor_port_ids_[port_pair_idx]]};

  doca_error_t result =
      doca_flow_pipe_cfg_set_name(cfg_guard.get(), kMonitoringPipeControlName);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_name(control) failed");

  result = doca_flow_pipe_cfg_set_domain(cfg_guard.get(),
                                         DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_domain(control) failed");

  result = doca_flow_pipe_cfg_set_type(cfg_guard.get(), DOCA_FLOW_PIPE_CONTROL);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_type(control) failed");

  result = doca_flow_pipe_cfg_set_is_root(cfg_guard.get(), true);
  check_and_throw_doca(result,
                       "doca_flow_pipe_cfg_set_is_root(control) failed");

  result = doca_flow_pipe_create(cfg_guard.get(), NULL, NULL,
                                 &control_pipes_[port_pair_idx]);
  check_and_throw_doca(result, "doca_flow_pipe_create(control) failed");

  DOCA_LOG_INFO("pipeline control pipe created");
}

void MonitoringDoca::build_monitor_output_pipe(uint16_t nb_queues,
                                               uint16_t port_pair_idx) {
  if (monitor_dest_type_ == MonitoringConfig::MonitoringDest::NONE)
    return;

  GuardedPipeCfg cfg_guard{switch_ports_[monitor_port_ids_[port_pair_idx]]};

  doca_flow_match match{};
  doca_flow_actions actions{};
  struct doca_flow_actions *actions_arr[1] = {&actions};
  doca_flow_fwd fwd{};
  doca_flow_monitor monitor{};

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
      DOCA_LOG_ERR("doca_flow_pipe_cfg_set_monitor(output): %s",
                   doca_error_get_descr(result));
      throw std::runtime_error("failed to set output pipe monitor");
    }
  }

  std::vector<uint16_t> rss_queues(nb_queues);
  for (uint16_t i = 0; i < nb_queues; ++i)
    rss_queues[i] = i;

  if (monitor_dest_type_ == MonitoringConfig::MonitoringDest::DPDK) {
      fwd.type = DOCA_FLOW_FWD_RSS;
      fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
      // fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
      fwd.rss.inner_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
      fwd.rss.nr_queues = nb_queues;
      fwd.rss.queues_array = rss_queues.data();
  } else if (monitor_dest_type_ == MonitoringConfig::MonitoringDest::DEVICE) {
      if (port_pair_idx >= forward_port_ids_.size()) {
          throw std::runtime_error("forward_port_ids index out of bounds");
      }
      fwd.type = DOCA_FLOW_FWD_PORT;
      fwd.port_id = forward_port_ids_[port_pair_idx];
  }

  result = doca_flow_pipe_create(cfg_guard.get(), &fwd, NULL,
                                 &monitor_output_pipes_[port_pair_idx]);
  check_and_throw_doca(result, "doca_flow_pipe_create(output) failed");

  result = doca_flow_pipe_add_entry(0, monitor_output_pipes_[port_pair_idx],
                                    &match, &actions, NULL, &fwd, 0, NULL,
                                    &rss_entries_[port_pair_idx]);
  check_and_throw_doca(result, "doca_flow_pipe_add_entry(output) failed");

  result =
      doca_flow_entries_process(switch_ports_[monitor_port_ids_[port_pair_idx]],
                                0, kMonitoringPipePullTimeout, 1);
  check_and_throw_doca(result, "doca_flow_entries_process(output) failed");

  DOCA_LOG_INFO("pipeline monitor output pipe created");
}

void MonitoringDoca::create_sampling_pipe(uint16_t port_pair_idx) {
  GuardedPipeCfg cfg_guard{switch_ports_[monitor_port_ids_[port_pair_idx]]};

  doca_flow_match match{};
  doca_flow_actions actions{};
  struct doca_flow_actions *actions_arr[1] = {&actions};
  doca_flow_fwd fwd_miss{};
  doca_flow_fwd fwd_hit{};

  doca_error_t result =
      doca_flow_pipe_cfg_set_name(cfg_guard.get(), "SAMPLING_PIPE");
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_name(sampling) failed");

  result = doca_flow_pipe_cfg_set_type(cfg_guard.get(), DOCA_FLOW_PIPE_HASH);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_type(sampling) failed");

  result = doca_flow_pipe_cfg_set_hash_map_algorithm(cfg_guard.get(), DOCA_FLOW_PIPE_HASH_MAP_ALGORITHM_RANDOM);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_hash_map_algorithm(sampling) failed");

  result = doca_flow_pipe_cfg_set_nr_entries(cfg_guard.get(), 256);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_nr_entries(sampling) failed");

  result = doca_flow_pipe_cfg_set_is_root(cfg_guard.get(), false);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_is_root(sampling) failed");

  result = doca_flow_pipe_cfg_set_actions(cfg_guard.get(), actions_arr, NULL,
                                          NULL, 1);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_actions(sampling) failed");

  fwd_miss.type = DOCA_FLOW_FWD_DROP;
  fwd_hit.type = DOCA_FLOW_FWD_CHANGEABLE;

  result = doca_flow_pipe_create(cfg_guard.get(), &fwd_hit, &fwd_miss,
                                 &sampling_pipes_[port_pair_idx]);
  check_and_throw_doca(result, "doca_flow_pipe_create(sampling) failed");

  // Forward to Monitor Output Pipe
  fwd_hit.type = DOCA_FLOW_FWD_PIPE;
  fwd_hit.next_pipe = monitor_output_pipes_[port_pair_idx];

  // Sampling entries logic
  int count = static_cast<int>(std::round(256.0 * sampling_fraction_));
  if (count > 256) count = 256;

  DOCA_LOG_INFO("pipeline sampling pipe created with %d/256 entries to monitor output pipe", count);

  for (int i = 0; i < 256; ++i) {
	struct doca_flow_pipe_entry *entry = nullptr;
	
	result = doca_flow_pipe_hash_add_entry(0, sampling_pipes_[port_pair_idx], i,
									&actions, NULL, i < count ? &fwd_hit : &fwd_miss, i == 255 ? DOCA_FLOW_NO_WAIT : DOCA_FLOW_WAIT_FOR_BATCH, NULL,
									&entry);
	check_and_throw_doca(result, "doca_flow_pipe_hash_add_entry(sampling) failed");
	sampling_entries_[port_pair_idx].push_back(entry);
  }

  result =
      doca_flow_entries_process(switch_ports_[monitor_port_ids_[port_pair_idx]],
                                0, kMonitoringPipePullTimeout, sampling_entries_[port_pair_idx].size());
  check_and_throw_doca(result, "doca_flow_entries_process(sampling) failed");

  DOCA_LOG_INFO("pipeline sampling pipe created with %zu entries", sampling_entries_[port_pair_idx].size());
}

void MonitoringDoca::setup_mirror(uint16_t port_pair_idx) {
  if (monitor_dest_type_ == MonitoringConfig::MonitoringDest::NONE)
    return;

  doca_flow_mirror_target target{};
  target.fwd.type = DOCA_FLOW_FWD_PIPE;
  if (sampling_pipes_[port_pair_idx] != nullptr) {
    target.fwd.next_pipe = sampling_pipes_[port_pair_idx];
  } else {
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
  check_and_throw_doca(result,
                       "doca_flow_shared_resource_set_cfg(mirror) failed");

  uint32_t mirror_ids[1] = {(uint32_t)(port_pair_idx + 1)};
  result = doca_flow_shared_resources_bind(
      DOCA_FLOW_SHARED_RESOURCE_MIRROR, mirror_ids, 1,
      switch_ports_[monitor_port_ids_[port_pair_idx]]);
  check_and_throw_doca(result,
                       "doca_flow_shared_resources_bind(mirror) failed");

  DOCA_LOG_INFO("pipeline mirror configured (id=%u)", port_pair_idx + 1);
}

void MonitoringDoca::setup_ipsec_sa_encap(const MonitoringConfig &cfg,
                                          uint16_t port_pair_idx) {
  if (!host_branch_enabled_ ||
      port_pair_idx >= cfg.ipsec_encap_configs().size())
    return;

  const auto &ipsec_cfg = cfg.ipsec_encap_configs()[port_pair_idx];

  doca_flow_shared_resource_cfg shared_cfg{};
  memset(&shared_cfg, 0, sizeof(shared_cfg));

  shared_cfg.ipsec_sa_cfg.icv_len =
      DOCA_FLOW_CRYPTO_ICV_LENGTH_16; // Default to 16 bytes
  shared_cfg.ipsec_sa_cfg.salt = ipsec_cfg.salt;
  shared_cfg.ipsec_sa_cfg.implicit_iv = ipsec_cfg.iv;
  shared_cfg.ipsec_sa_cfg.key_cfg.key_type =
      DOCA_FLOW_CRYPTO_KEY_256; // 256-bit key
  shared_cfg.ipsec_sa_cfg.key_cfg.key =
      reinterpret_cast<uint32_t *>(const_cast<uint8_t *>(ipsec_cfg.key.data()));
  shared_cfg.ipsec_sa_cfg.sn_initial = 0;
  shared_cfg.ipsec_sa_cfg.esn_en = false;
  shared_cfg.ipsec_sa_cfg.sn_offload_type =
      DOCA_FLOW_CRYPTO_SN_OFFLOAD_INC; // Hardware sequence number increment

  // Use port_pair_idx * 2 + 1 as SA ID for encap (index 0 reserved,
  // alternating: *2+1 for encap, *2+2 for decap)
  uint32_t sa_id = port_pair_idx * 2 + 1;
  doca_error_t result = doca_flow_shared_resource_set_cfg(
      DOCA_FLOW_SHARED_RESOURCE_IPSEC_SA, sa_id, &shared_cfg);
  check_and_throw_doca(result,
                       "doca_flow_shared_resource_set_cfg(ipsec_encap) failed");

  // Bind SA to port
  uint32_t sa_ids[1] = {sa_id};
  result = doca_flow_shared_resources_bind(
      DOCA_FLOW_SHARED_RESOURCE_IPSEC_SA, sa_ids, 1,
      switch_ports_[monitor_port_ids_[port_pair_idx]]);
  check_and_throw_doca(result,
                       "doca_flow_shared_resources_bind(ipsec_encap) failed");

  DOCA_LOG_INFO("IPsec encap SA configured (id=%u, port_pair=%u)", sa_id,
                port_pair_idx);
}

void MonitoringDoca::setup_ipsec_sa_decap(const MonitoringConfig &cfg,
                                          uint16_t port_pair_idx) {
  if (!host_branch_enabled_ ||
      port_pair_idx >= cfg.ipsec_decap_configs().size())
    return;

  const auto &ipsec_cfg = cfg.ipsec_decap_configs()[port_pair_idx];

  doca_flow_shared_resource_cfg shared_cfg{};
  memset(&shared_cfg, 0, sizeof(shared_cfg));

  shared_cfg.ipsec_sa_cfg.icv_len =
      DOCA_FLOW_CRYPTO_ICV_LENGTH_16; // Default to 16 bytes
  shared_cfg.ipsec_sa_cfg.salt = ipsec_cfg.salt;
  shared_cfg.ipsec_sa_cfg.implicit_iv = ipsec_cfg.iv;
  shared_cfg.ipsec_sa_cfg.key_cfg.key_type =
      DOCA_FLOW_CRYPTO_KEY_256; // 256-bit key
  shared_cfg.ipsec_sa_cfg.key_cfg.key =
      reinterpret_cast<uint32_t *>(const_cast<uint8_t *>(ipsec_cfg.key.data()));
  shared_cfg.ipsec_sa_cfg.sn_initial = 0;
  shared_cfg.ipsec_sa_cfg.esn_en = false;
  shared_cfg.ipsec_sa_cfg.sn_offload_type =
      DOCA_FLOW_CRYPTO_SN_OFFLOAD_AR; // Hardware anti-replay
  shared_cfg.ipsec_sa_cfg.win_size = DOCA_FLOW_CRYPTO_REPLAY_WIN_SIZE_256;

  // Use port_pair_idx * 2 + 2 as SA ID for decap (index 0 reserved,
  // alternating: *2+1 for encap, *2+2 for decap)
  uint32_t sa_id = port_pair_idx * 2 + 2;
  doca_error_t result = doca_flow_shared_resource_set_cfg(
      DOCA_FLOW_SHARED_RESOURCE_IPSEC_SA, sa_id, &shared_cfg);
  check_and_throw_doca(result,
                       "doca_flow_shared_resource_set_cfg(ipsec_decap) failed");

  // Bind SA to port
  uint32_t sa_ids[1] = {sa_id};
  result = doca_flow_shared_resources_bind(
      DOCA_FLOW_SHARED_RESOURCE_IPSEC_SA, sa_ids, 1,
      switch_ports_[monitor_port_ids_[port_pair_idx]]);
  check_and_throw_doca(result,
                       "doca_flow_shared_resources_bind(ipsec_decap) failed");

  DOCA_LOG_INFO("IPsec decap SA configured (id=%u, port_pair=%u)", sa_id,
                port_pair_idx);
}

void MonitoringDoca::create_drop_pipe(uint16_t port_pair_idx) {
  if (!host_branch_enabled_ ||
      switch_ports_[monitor_port_ids_[port_pair_idx]] == nullptr)
    return;

  GuardedPipeCfg cfg_guard{switch_ports_[monitor_port_ids_[port_pair_idx]]};

  doca_error_t result =
      doca_flow_pipe_cfg_set_name(cfg_guard.get(), "DROP_PIPE");
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_name(drop) failed");

  result = doca_flow_pipe_cfg_set_domain(cfg_guard.get(),
                                         DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_domain(drop) failed");

  result = doca_flow_pipe_cfg_set_type(cfg_guard.get(), DOCA_FLOW_PIPE_BASIC);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_type(drop) failed");

  result = doca_flow_pipe_cfg_set_is_root(cfg_guard.get(), false);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_is_root(drop) failed");

  doca_flow_match match{};
  result = doca_flow_pipe_cfg_set_match(cfg_guard.get(), &match, NULL);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_match(drop) failed");

  doca_flow_fwd fwd{};
  fwd.type = DOCA_FLOW_FWD_DROP;

  doca_flow_monitor monitor{};
  if (counters_enabled_) {
    monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
    result = doca_flow_pipe_cfg_set_monitor(cfg_guard.get(), &monitor);
    check_and_throw_doca(result, "doca_flow_pipe_cfg_set_monitor(drop) failed");
  }

  result = doca_flow_pipe_create(cfg_guard.get(), &fwd, NULL,
                                 &drop_pipes_[port_pair_idx]);
  check_and_throw_doca(result, "doca_flow_pipe_create(drop) failed");

  result = doca_flow_pipe_add_entry(0, drop_pipes_[port_pair_idx], &match, NULL,
                                    NULL, &fwd, 0, NULL, &drop_entries_[port_pair_idx]);
  check_and_throw_doca(result, "doca_flow_pipe_add_entry(drop) failed");

  result =
      doca_flow_entries_process(switch_ports_[monitor_port_ids_[port_pair_idx]],
                                0, kMonitoringPipePullTimeout, 1);
  check_and_throw_doca(result, "doca_flow_entries_process(drop) failed");

  DOCA_LOG_INFO("pipeline drop pipe created");
}

void MonitoringDoca::create_to_host_pipe(uint16_t port_pair_idx) {
  if (!host_branch_enabled_ ||
      switch_ports_[monitor_port_ids_[port_pair_idx]] == nullptr)
    return;

  GuardedPipeCfg cfg_guard{switch_ports_[monitor_port_ids_[port_pair_idx]]};

  doca_error_t result =
      doca_flow_pipe_cfg_set_name(cfg_guard.get(), "TO_HOST_PIPE");
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_name(to_host) failed");

  result = doca_flow_pipe_cfg_set_domain(cfg_guard.get(),
                                         DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_domain(to_host) failed");

  result = doca_flow_pipe_cfg_set_type(cfg_guard.get(), DOCA_FLOW_PIPE_BASIC);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_type(to_host) failed");

  result = doca_flow_pipe_cfg_set_is_root(cfg_guard.get(), false);
  check_and_throw_doca(result,
                       "doca_flow_pipe_cfg_set_is_root(to_host) failed");

  doca_flow_monitor monitor{};
  if (counters_enabled_)
    monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

  doca_flow_match match{};
  result = doca_flow_pipe_cfg_set_match(cfg_guard.get(), &match, NULL);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_match(to_host) failed");

  if (counters_enabled_) {
    result = doca_flow_pipe_cfg_set_monitor(cfg_guard.get(), &monitor);
    check_and_throw_doca(result,
                         "doca_flow_pipe_cfg_set_monitor(to_host) failed");
  }

  doca_flow_fwd fwd{};
  fwd.type = DOCA_FLOW_FWD_PORT;
  fwd.port_id = host_port_ids_[port_pair_idx];

  result = doca_flow_pipe_create(cfg_guard.get(), &fwd, NULL,
                                 &to_host_pipes_[port_pair_idx]);
  check_and_throw_doca(result, "doca_flow_pipe_create(to_host) failed");

  result = doca_flow_pipe_add_entry(0, to_host_pipes_[port_pair_idx], &match,
                                    NULL, NULL, &fwd, 0, NULL,
                                    &to_host_entries_[port_pair_idx]);
  check_and_throw_doca(result, "doca_flow_pipe_add_entry(to_host) failed");

  result =
      doca_flow_entries_process(switch_ports_[monitor_port_ids_[port_pair_idx]],
                                0, kMonitoringPipePullTimeout, 1);
  check_and_throw_doca(result, "doca_flow_entries_process(to_host) failed");

  DOCA_LOG_INFO("pipeline to_host pipe created");
}

void MonitoringDoca::create_host_decrypt_pipe(const MonitoringConfig &cfg,
                                                uint16_t port_pair_idx) {
  if (!host_branch_enabled_ ||
      switch_ports_[monitor_port_ids_[port_pair_idx]] == nullptr)
    return;

  if (port_pair_idx >= cfg.ipsec_decap_configs().size()) {
    // No decap config for this port, skip decrypt pipe
    DOCA_LOG_ERR(
        "No IPsec decap config for port_pair_idx %u, skipping decrypt pipe",
        port_pair_idx);
    throw std::runtime_error("No IPsec decap config for port_pair_idx " +
                             std::to_string(port_pair_idx));
  }

  const auto &ipsec_cfg = cfg.ipsec_decap_configs()[port_pair_idx];
  GuardedPipeCfg cfg_guard{switch_ports_[monitor_port_ids_[port_pair_idx]]};

  doca_flow_match match{};
  // Match on ESP SPI
  match.tun.type = DOCA_FLOW_TUN_ESP;
  match.tun.esp_spi = UINT32_MAX; // Will be set in entry

  doca_flow_actions actions{};
  // Use crypto decrypt action with shared IPsec SA
  actions.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_DECRYPT;
  actions.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_IPSEC_SA;
  actions.crypto.crypto_id =
      UINT32_MAX; // Will be set in entry (port_pair_idx * 2 + 2)
  actions.crypto.ipsec_sa.sn_en = cfg.anti_replay; // Hardware sequence number increment

  struct doca_flow_actions *actions_arr[1] = {&actions};

  // Forward to Decap Pipe (Stage 2)
  doca_flow_fwd fwd{};
  fwd.type = DOCA_FLOW_FWD_PIPE;
  fwd.next_pipe = host_decap_pipes_[port_pair_idx];

  // Configure pipe
  doca_error_t result =
      doca_flow_pipe_cfg_set_name(cfg_guard.get(), kMonitoringPipeHostDecryptName);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_name(decrypt) failed");

  result = doca_flow_pipe_cfg_set_domain(cfg_guard.get(),
                                         DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS);
  check_and_throw_doca(result,
                       "doca_flow_pipe_cfg_set_domain(decrypt) failed");

  result = doca_flow_pipe_cfg_set_type(cfg_guard.get(), DOCA_FLOW_PIPE_BASIC);
  check_and_throw_doca(result,
                       "doca_flow_pipe_cfg_set_type(decrypt) failed");

  result = doca_flow_pipe_cfg_set_is_root(cfg_guard.get(), false);
  check_and_throw_doca(result,
                       "doca_flow_pipe_cfg_set_is_root(decrypt) failed");

  result = doca_flow_pipe_cfg_set_match(cfg_guard.get(), &match, NULL);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_match(decrypt) failed");

  result = doca_flow_pipe_cfg_set_actions(cfg_guard.get(), actions_arr, NULL, NULL,
                                          1);
  check_and_throw_doca(result,
                       "doca_flow_pipe_cfg_set_actions(decrypt) failed");

  doca_flow_monitor monitor{};
  if (counters_enabled_) {
    monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
    result = doca_flow_pipe_cfg_set_monitor(cfg_guard.get(), &monitor);
    check_and_throw_doca(result,
                         "doca_flow_pipe_cfg_set_monitor(decrypt) failed");
  }

  result = doca_flow_pipe_create(cfg_guard.get(), &fwd, NULL,
                                 &host_decrypt_pipes_[port_pair_idx]);
  check_and_throw_doca(result, "doca_flow_pipe_create(decrypt) failed");

  // Add entry for this port pair
  match.tun.esp_spi = rte_cpu_to_be_32(ipsec_cfg.spi);
  actions.crypto.crypto_id = port_pair_idx * 2 + 2; // Decap SA ID

  result = doca_flow_pipe_add_entry(
      0, host_decrypt_pipes_[port_pair_idx], &match, &actions, NULL,
      &fwd, 0, NULL, &host_decrypt_entries_[port_pair_idx]);
  check_and_throw_doca(result, "doca_flow_pipe_add_entry(decrypt) failed");

  // Trigger processing
  result = doca_flow_entries_process(
      switch_ports_[monitor_port_ids_[port_pair_idx]], 0,
      kMonitoringPipePullTimeout, 1);
  check_and_throw_doca(result, "doca_flow_entries_process(decrypt) failed");

  DOCA_LOG_INFO("pipeline host decrypt pipe created (SPI=0x%08x)",
                ipsec_cfg.spi);
}

void MonitoringDoca::create_host_decap_pipe(const MonitoringConfig &cfg,
                                            uint16_t port_pair_idx) {
  if (!host_branch_enabled_ ||
      switch_ports_[monitor_port_ids_[port_pair_idx]] == nullptr)
    return;

  if (port_pair_idx >= cfg.ipsec_decap_configs().size()) {
    // No decap config for this port, skip decap pipe
    DOCA_LOG_ERR(
        "No IPsec decap config for port_pair_idx %u, skipping decap pipe",
        port_pair_idx);
    throw std::runtime_error("No IPsec decap config for port_pair_idx " +
                             std::to_string(port_pair_idx));
  }

  GuardedPipeCfg cfg_guard{switch_ports_[monitor_port_ids_[port_pair_idx]]};

  doca_flow_match match{};
  match.parser_meta.ipsec_syndrome = 0xff; // ipsec syndrome
  if (cfg.anti_replay) {
    match.parser_meta.ipsec_ar_syndrome = 0xff; // anti-replay syndrome
  }

  doca_flow_actions actions{};
  // Add decap reformat for ESP tunnel mode (IPv4)
  actions.has_crypto_encap = true;
  actions.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_DECAP;
  actions.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_ESP_TUNNEL;
  actions.crypto_encap.icv_size = 16; // ICV length in bytes
  actions.crypto_encap.data_size = sizeof(rte_ether_hdr);
  memset(actions.crypto_encap.encap_data, 0xff, sizeof(actions.crypto_encap.encap_data));

  // Forward to To-Host Pipe
  doca_flow_fwd fwd{};
  fwd.type = DOCA_FLOW_FWD_PIPE;
  fwd.next_pipe = to_host_pipes_[port_pair_idx];
  
  // Drop on miss (bad syndrome)
  doca_flow_fwd miss_fwd{};
  miss_fwd.type = DOCA_FLOW_FWD_PIPE;
  miss_fwd.next_pipe = drop_pipes_[port_pair_idx];

  // Configure pipe
  doca_error_t result =
      doca_flow_pipe_cfg_set_name(cfg_guard.get(), kMonitoringPipeHostDecapName);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_name(decap) failed");

  result = doca_flow_pipe_cfg_set_domain(cfg_guard.get(),
                                         DOCA_FLOW_PIPE_DOMAIN_SECURE_INGRESS);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_domain(decap) failed");

  result = doca_flow_pipe_cfg_set_type(cfg_guard.get(), DOCA_FLOW_PIPE_BASIC);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_type(decap) failed");

  result = doca_flow_pipe_cfg_set_is_root(cfg_guard.get(), false);
  doca_flow_pipe_cfg_set_is_root(cfg_guard.get(), false);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_is_root(decap) failed");

  result = doca_flow_pipe_cfg_set_match(cfg_guard.get(), &match, NULL);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_match(decap) failed");

  struct doca_flow_actions *actions_arr[1] = {&actions};

  result = doca_flow_pipe_cfg_set_actions(cfg_guard.get(), actions_arr, NULL, NULL,
                                          1);
  check_and_throw_doca(result, "doca_flow_pipe_cfg_set_actions(decap) failed");

  doca_flow_monitor monitor{};
  if (counters_enabled_) {
    monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
    result = doca_flow_pipe_cfg_set_monitor(cfg_guard.get(), &monitor);
    check_and_throw_doca(result,
                         "doca_flow_pipe_cfg_set_monitor(decap) failed");
  }

  result = doca_flow_pipe_create(cfg_guard.get(), &fwd, &miss_fwd,
                                 &host_decap_pipes_[port_pair_idx]);
  check_and_throw_doca(result, "doca_flow_pipe_create(decap) failed");

  match.parser_meta.ipsec_syndrome = 0;
  match.parser_meta.ipsec_ar_syndrome = 0;

  // Build decap_data (new L2 header)
  rte_ether_hdr eth_hdr;
  const auto &encap_cfg = cfg.ipsec_encap_configs()[port_pair_idx];
  rte_memcpy(eth_hdr.src_addr.addr_bytes, encap_cfg.dst_mac.data(), RTE_ETHER_ADDR_LEN); // Swap for return path
  rte_memcpy(eth_hdr.dst_addr.addr_bytes, encap_cfg.src_mac.data(), RTE_ETHER_ADDR_LEN);
  eth_hdr.ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

  memcpy(actions.crypto_encap.encap_data, &eth_hdr, sizeof(eth_hdr));

  result = doca_flow_pipe_add_entry(
      0, host_decap_pipes_[port_pair_idx], &match, &actions, NULL,
      &fwd, 0, NULL, &host_decap_entries_[port_pair_idx]);
  check_and_throw_doca(result, "doca_flow_pipe_add_entry(decap) failed");

  // Trigger processing
  result = doca_flow_entries_process(
      switch_ports_[monitor_port_ids_[port_pair_idx]], 0,
      kMonitoringPipePullTimeout, 1);
  check_and_throw_doca(result, "doca_flow_entries_process(decap) failed");

  DOCA_LOG_INFO("pipeline host decap pipe created (id=%u)", port_pair_idx + 1);
}

void MonitoringDoca::install_default_drop(uint16_t port_pair_idx) {
  if (control_pipes_[port_pair_idx] == nullptr)
    throw std::runtime_error("control pipe not available");

  doca_flow_match match{};
  doca_flow_fwd fwd{};
  fwd.type = DOCA_FLOW_FWD_DROP;

  struct doca_flow_pipe_entry *entry = NULL;
  doca_error_t result = doca_flow_pipe_control_add_entry(
      0, 7, control_pipes_[port_pair_idx], &match, NULL, NULL, NULL, NULL, NULL,
      NULL, &fwd, NULL, &entry);
  check_and_throw_doca(result,
                       "doca_flow_pipe_control_add_entry(default_drop) failed");

  result =
      doca_flow_entries_process(switch_ports_[monitor_port_ids_[port_pair_idx]],
                                0, kMonitoringPipePullTimeout, 1);
  check_and_throw_doca(result,
                       "doca_flow_entries_process(default_drop) failed");

  DOCA_LOG_INFO("pipeline default drop entry installed");
}

void MonitoringDoca::add_monitor_rule(const MonitoringConfig & /*cfg*/,
                                      uint16_t port_pair_idx) {
  if (control_pipes_[port_pair_idx] == nullptr)
    throw std::runtime_error("control pipe not available");

  doca_flow_match match{};
  match.parser_meta.port_id = monitor_port_ids_[port_pair_idx];
  match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;

  // Match on ESP if IPsec is enabled, otherwise match on UDP (ESP)
  match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_ESP;
  match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
  match.outer.ip4.next_proto = IPPROTO_ESP;

  doca_flow_fwd fwd{};
  if (host_branch_enabled_) {
    if (host_decrypt_pipes_[port_pair_idx] == nullptr)
      throw std::runtime_error("host decrypt pipe not available");
    fwd.type = DOCA_FLOW_FWD_PIPE;
    fwd.next_pipe = host_decrypt_pipes_[port_pair_idx];
  } else {
    fwd.type = DOCA_FLOW_FWD_DROP;
  }

  doca_flow_monitor monitor{};
  if (monitor_dest_type_ != MonitoringConfig::MonitoringDest::NONE)
    monitor.shared_mirror_id =
        port_pair_idx + 1; // +1 for 0 reserved for default no mirror

  struct doca_flow_pipe_entry *entry = NULL;
  doca_error_t result = doca_flow_pipe_control_add_entry(
      0, 0, control_pipes_[port_pair_idx], &match, NULL, NULL, NULL, NULL, NULL,
      &monitor, &fwd, NULL, &entry);
  check_and_throw_doca(result,
                       "doca_flow_pipe_control_add_entry(monitor) failed");

  result =
      doca_flow_entries_process(switch_ports_[monitor_port_ids_[port_pair_idx]],
                                0, kMonitoringPipePullTimeout, 1);
  check_and_throw_doca(result, "doca_flow_entries_process(monitor) failed");

  DOCA_LOG_INFO("pipeline monitor rule installed (ESP protocol)");
}

void MonitoringDoca::create_host_encap_pipe(const MonitoringConfig &cfg,
                                            uint16_t port_pair_idx) {
  if (!host_branch_enabled_ ||
      switch_ports_[monitor_port_ids_[port_pair_idx]] == nullptr)
    return;

  if (port_pair_idx >= cfg.ipsec_encap_configs().size()) {
    // No encap config for this port, skip encap pipe
    DOCA_LOG_ERR(
        "No IPsec encap config for port_pair_idx %u, skipping encap pipe",
        port_pair_idx);
    throw std::runtime_error("No IPsec encap config for port_pair_idx " +
                             std::to_string(port_pair_idx));
  }

  const auto &ipsec_cfg = cfg.ipsec_encap_configs()[port_pair_idx];
  GuardedPipeCfg cfg_guard{switch_ports_[monitor_port_ids_[port_pair_idx]]};

  doca_flow_match match{};
  // Match on metadata (port_id) to intercept packets from host
  match.parser_meta.port_id = host_port_ids_[port_pair_idx];
  match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;

  doca_flow_actions actions{};
  // Use crypto encrypt action with shared IPsec SA
  actions.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_ENCRYPT;
  actions.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_IPSEC_SA;
  actions.crypto.crypto_id =
      UINT32_MAX; // Will be set in entry (port_pair_idx + 1)
  actions.crypto.ipsec_sa.sn_en = true; // Hardware sequence number increment

  // Add encap reformat for ESP tunnel mode (IPv4)
  actions.has_crypto_encap = true;
  actions.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_ENCAP;
  actions.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_ESP_TUNNEL;
  actions.crypto_encap.icv_size = 16;  // ICV length in bytes
  actions.crypto_encap.data_size = sizeof(MonitoringEncapTemplate);

  memset(actions.crypto_encap.encap_data, 0xff,
         sizeof(actions.crypto_encap.encap_data)); // encap header is changeable

  struct doca_flow_actions *actions_arr[1] = {&actions};

  doca_flow_fwd fwd{};
  fwd.type = DOCA_FLOW_FWD_PORT;
  fwd.port_id = monitor_port_ids_[port_pair_idx];

  doca_flow_fwd fwd_miss{};
  fwd_miss.type = DOCA_FLOW_FWD_DROP;

  doca_flow_monitor monitor{};
  if (counters_enabled_)
    monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

  doca_error_t result =
      doca_flow_pipe_cfg_set_name(cfg_guard.get(), "HOST_ENCAP");
  check_and_throw_doca(result,
                       "doca_flow_pipe_cfg_set_name(host_encap) failed");

  result = doca_flow_pipe_cfg_set_domain(cfg_guard.get(),
                                         DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS);
  check_and_throw_doca(result,
                       "doca_flow_pipe_cfg_set_domain(host_encap) failed");

  result = doca_flow_pipe_cfg_set_type(cfg_guard.get(), DOCA_FLOW_PIPE_BASIC);
  check_and_throw_doca(result,
                       "doca_flow_pipe_cfg_set_type(host_encap) failed");

  result = doca_flow_pipe_cfg_set_is_root(cfg_guard.get(), true);
  check_and_throw_doca(result,
                       "doca_flow_pipe_cfg_set_is_root(host_encap) failed");

  result = doca_flow_pipe_cfg_set_match(cfg_guard.get(), &match, NULL);
  check_and_throw_doca(result,
                       "doca_flow_pipe_cfg_set_match(host_encap) failed");

  result = doca_flow_pipe_cfg_set_actions(cfg_guard.get(), actions_arr, NULL,
                                          NULL, 1);
  check_and_throw_doca(result,
                       "doca_flow_pipe_cfg_set_actions(host_encap) failed");

  if (counters_enabled_) {
    result = doca_flow_pipe_cfg_set_monitor(cfg_guard.get(), &monitor);
    check_and_throw_doca(result,
                         "doca_flow_pipe_cfg_set_monitor(host_encap) failed");
  }

  result = doca_flow_pipe_create(cfg_guard.get(), &fwd, &fwd_miss,
                                 &host_encap_pipes_[port_pair_idx]);
  check_and_throw_doca(result, "doca_flow_pipe_create(host_encap) failed");

  // Add entry with specific values
  // Update encap_data with actual dst MAC, dst IP, and SPI
  MonitoringEncapTemplate encap_template{};
  rte_memcpy(encap_template.eth.src_addr.addr_bytes, ipsec_cfg.src_mac.data(),
             RTE_ETHER_ADDR_LEN);
  rte_memcpy(encap_template.eth.dst_addr.addr_bytes, ipsec_cfg.dst_mac.data(),
             RTE_ETHER_ADDR_LEN);
  encap_template.eth.ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

  encap_template.ip.version_ihl = 0x45;
  encap_template.ip.type_of_service = 0x00;
  encap_template.ip.total_length = 0;
  encap_template.ip.packet_id = 0;
  encap_template.ip.fragment_offset = 0;
  encap_template.ip.time_to_live = 0;
  encap_template.ip.next_proto_id = IPPROTO_ESP;
  encap_template.ip.hdr_checksum = 0;
  rte_memcpy(&encap_template.ip.src_addr, ipsec_cfg.src_ip.data(),
             sizeof(encap_template.ip.src_addr));
  memset(&encap_template.ip.dst_addr, 0xff, sizeof(encap_template.ip.dst_addr));

  encap_template.esp.spi = rte_cpu_to_be_32(ipsec_cfg.spi);
  encap_template.esp.seq = 0;
  memset(encap_template.esp.iv, 0, sizeof(encap_template.esp.iv));

  memcpy(actions.crypto_encap.encap_data, &encap_template,
         sizeof(encap_template));
  actions.crypto.crypto_id = port_pair_idx * 2 + 1; // Encap SA ID

  result = doca_flow_pipe_add_entry(0, host_encap_pipes_[port_pair_idx], &match,
                                    &actions, NULL, NULL, 0, NULL,
                                    &host_encap_entries_[port_pair_idx]);
  check_and_throw_doca(result, "doca_flow_pipe_add_entry(host_encap) failed");

  result =
      doca_flow_entries_process(switch_ports_[monitor_port_ids_[port_pair_idx]],
                                0, kMonitoringPipePullTimeout, 1);
  check_and_throw_doca(result, "doca_flow_entries_process(host_encap) failed");

  DOCA_LOG_INFO("pipeline host encap entry installed (SPI=0x%08x)",
                ipsec_cfg.spi);
}

void MonitoringDoca::add_encap_control_rule(const MonitoringConfig & /*cfg*/,
                                            uint16_t port_pair_idx) {
  if (control_pipes_[port_pair_idx] == nullptr)
    throw std::runtime_error("control pipe not available");

  doca_flow_match match{};
  match.parser_meta.port_id = host_port_ids_[port_pair_idx];
  match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;

  doca_flow_fwd fwd{};
  if (host_encap_pipes_[port_pair_idx] == nullptr)
    throw std::runtime_error("host encap pipe not available");
  // Forward to host encap pipe
  fwd.type = DOCA_FLOW_FWD_PIPE;
  fwd.next_pipe = host_encap_pipes_[port_pair_idx];

  struct doca_flow_pipe_entry *entry = NULL;
  doca_error_t result = doca_flow_pipe_control_add_entry(
      0,
      1, // higher priority than the default drop rule
      control_pipes_[port_pair_idx], &match, NULL, NULL, NULL, NULL, NULL, NULL,
      &fwd, NULL, &entry);
  check_and_throw_doca(
      result, "doca_flow_pipe_control_add_entry(encap_control) failed");

  result =
      doca_flow_entries_process(switch_ports_[monitor_port_ids_[port_pair_idx]],
                                0, kMonitoringPipePullTimeout, 1);
  check_and_throw_doca(result,
                       "doca_flow_entries_process(encap_control) failed");

  DOCA_LOG_INFO("pipeline encap control rule installed");
}

void MonitoringDoca::build_pipeline(const MonitoringConfig &cfg,
                                    uint16_t nb_queues) {
  for (size_t i = 0; i < monitor_port_ids_.size(); ++i) {
    if (switch_ports_[monitor_port_ids_[i]] == nullptr)
      throw std::runtime_error("pipeline build requires monitor port");

    if (host_branch_enabled_) {
      // Setup IPsec SAs before creating pipes
      if (!cfg.ipsec_decap_configs().empty())
        setup_ipsec_sa_decap(cfg, i);
      if (!cfg.ipsec_encap_configs().empty())
        setup_ipsec_sa_encap(cfg, i);

      create_drop_pipe(i);
      create_to_host_pipe(i);
      create_host_decap_pipe(cfg, i);
      create_host_decrypt_pipe(cfg, i);
    }
    if (monitor_dest_type_ != MonitoringConfig::MonitoringDest::NONE) {
      build_monitor_output_pipe(nb_queues, i);
      create_sampling_pipe(i);
      setup_mirror(i);
    }

    create_control_pipe(i);
    install_default_drop(i);
    add_monitor_rule(cfg, i);

    if (host_branch_enabled_ && !cfg.ipsec_encap_configs().empty()) {
      create_host_encap_pipe(cfg, i);
      add_encap_control_rule(cfg, i);
    }
  }

  DOCA_LOG_INFO("pipeline build complete (ports=%zu, queues=%u, "
                "host_branch=%s, monitor_dest_type=%d)",
                monitor_port_ids_.size(), nb_queues,
                host_branch_enabled_ ? "on" : "off",
                (int)monitor_dest_type_);
}
