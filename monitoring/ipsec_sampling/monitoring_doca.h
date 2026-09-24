#ifndef MONITORING_APP_MONITORING_DOCA_H
#define MONITORING_APP_MONITORING_DOCA_H

#ifdef __cplusplus

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "monitoring_config.h"
class DeviceManager;
struct doca_flow_port;
struct doca_flow_pipe;
struct doca_flow_pipe_entry;
struct doca_flow_entry;

class MonitoringDoca {
public:
  MonitoringDoca(DeviceManager &device_manager,
                 const std::vector<uint16_t> &monitor_port_ids,
                 const std::vector<uint16_t> &host_port_ids,
                 uint16_t dpdk_nb_queues, const MonitoringConfig &cfg,
                 const std::vector<uint16_t> &forward_port_ids);
  ~MonitoringDoca();

  MonitoringDoca(const MonitoringDoca &) = delete;
  MonitoringDoca &operator=(const MonitoringDoca &) = delete;
  MonitoringDoca(MonitoringDoca &&) = delete;
  MonitoringDoca &operator=(MonitoringDoca &&) = delete;

  int print_stats(FILE *stream);

private:
  void initialize_pipeline(const MonitoringConfig &cfg);
  void initialize_flow(uint16_t nb_queues);
  doca_flow_port *start_port(struct doca_dev *dev, uint16_t port_id);
  doca_flow_port *start_port_rep(struct doca_dev_rep *dev_rep,
                                 uint16_t port_id);
  void stop_port(doca_flow_port *&port, uint16_t port_id);
  void build_pipeline(const MonitoringConfig &cfg, uint16_t nb_queues);

  void create_control_pipe(uint16_t port_pair_idx);
  void build_monitor_output_pipe(uint16_t nb_queues, uint16_t port_pair_idx);
  void create_sampling_pipe(uint16_t port_pair_idx);
  void setup_mirror(uint16_t port_pair_idx);
  void setup_ipsec_sa_encap(const MonitoringConfig &cfg,
                            uint16_t port_pair_idx);
  void setup_ipsec_sa_decap(const MonitoringConfig &cfg,
                            uint16_t port_pair_idx);
  void create_host_decrypt_pipe(const MonitoringConfig &cfg, uint16_t port_pair_idx);
  void create_host_decap_pipe(const MonitoringConfig &cfg,
                              uint16_t port_pair_idx);
  void create_drop_pipe(uint16_t port_pair_idx);
  void create_to_host_pipe(uint16_t port_pair_idx);
  void install_default_drop(uint16_t port_pair_idx);
  void add_monitor_rule(const MonitoringConfig &cfg, uint16_t port_pair_idx);
  void create_host_encap_pipe(const MonitoringConfig &cfg,
                              uint16_t port_pair_idx);
  void add_encap_control_rule(const MonitoringConfig &cfg,
                              uint16_t port_pair_idx);

  static constexpr const char *kMonitoringPipeControlName = "CONTROL_PIPE";
  static constexpr const char *kMonitoringPipeHostDecryptName = "HOST_DECRYPT_PIPE";
  static constexpr const char *kMonitoringPipeHostDecapName = "HOST_DECAP_PIPE";
  static constexpr const char *kMonitoringPipeHostEncapName = "HOST_ENCAP_PIPE";
  static constexpr const char *kMonitoringPipeToHostName = "TO_HOST_PIPE";

  bool host_branch_enabled_;
  MonitoringConfig::MonitoringDest monitor_dest_type_;
  std::vector<uint16_t> forward_port_ids_;
  bool counters_enabled_;
  double sampling_fraction_;

  std::vector<doca_flow_port *> switch_ports_;
  std::vector<uint16_t> monitor_port_ids_;
  std::vector<uint16_t> host_port_ids_;
  std::vector<doca_flow_pipe *> control_pipes_;
  std::vector<doca_flow_pipe *> monitor_output_pipes_; // Renamed from rss_pipes_
  std::vector<doca_flow_pipe *> sampling_pipes_;
  std::vector<doca_flow_pipe *> host_decrypt_pipes_;
  std::vector<doca_flow_pipe *> host_decap_pipes_;
  std::vector<doca_flow_pipe *> drop_pipes_;
  std::vector<doca_flow_pipe *> to_host_pipes_;
  std::vector<doca_flow_pipe *> host_encap_pipes_;
  std::vector<doca_flow_pipe_entry *> drop_entries_;
  std::vector<doca_flow_pipe_entry *> host_decrypt_entries_;
  std::vector<doca_flow_pipe_entry *> host_decap_entries_;
  std::vector<doca_flow_pipe_entry *> host_encap_entries_;
  std::vector<doca_flow_pipe_entry *> rss_entries_; // Re-used for monitor output stats
  std::vector<std::vector<doca_flow_pipe_entry *>> sampling_entries_;
  std::vector<doca_flow_pipe_entry *> to_host_entries_;

  std::chrono::steady_clock::time_point last_counter_timestamp_{};
  std::vector<uint64_t> rss_last_packets_;
  std::vector<uint64_t> rss_last_bytes_;
  std::vector<uint64_t> host_decrypt_last_packets_;
  std::vector<uint64_t> host_decrypt_last_bytes_;
  std::vector<uint64_t> host_decap_last_packets_;
  std::vector<uint64_t> host_decap_last_bytes_;
  std::vector<uint64_t> host_encap_last_packets_;
  std::vector<uint64_t> host_encap_last_bytes_;
  std::vector<uint64_t> drop_last_packets_;
  std::vector<uint64_t> drop_last_bytes_;
  std::vector<uint64_t> to_host_last_packets_;
  std::vector<uint64_t> to_host_last_bytes_;
};

#endif // __cplusplus

#endif /* MONITORING_APP_MONITORING_DOCA_H */
