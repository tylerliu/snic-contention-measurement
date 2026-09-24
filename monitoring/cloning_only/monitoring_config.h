#ifndef MONITORING_APP_MONITORING_CONFIG_H
#define MONITORING_APP_MONITORING_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <array>

namespace litefs {
	class ArgsContext;
}

class MonitoringConfig {

public:
    enum class MonitoringDest {
        RSS,
        HOST,
        DROP,
        DEVICE
    };

    MonitoringDest main_dest_type = MonitoringDest::RSS;
    MonitoringDest clone_dest_type = MonitoringDest::DROP;
    bool enable_mirroring = true;
    std::vector<std::string> monitoring_dest_devices;

	MonitoringConfig();
	
	int parse(int argc, char **argv);
	bool validate(char *errbuf, size_t errbuf_len) const;
	void print() const;

	const std::vector<std::string>& monitor_devices() const { return monitor_devices_; }
	const std::vector<std::string>& host_reps() const { return host_reps_; }
	const std::string& app_name() const { return app_name_str_; }
	uint16_t monitor_udp_port;
	bool enable_counters;

	// Removed VXLAN encap configurations

private:
	std::vector<std::string> monitor_devices_;
	std::vector<std::string> host_reps_;
	std::string app_name_str_;
	void apply_config_to_app_cfg(const litefs::ArgsContext& ctx);
	void store_device_strings(const litefs::ArgsContext& ctx);
	static std::array<uint8_t, 4> parse_ipv4_string(const std::string& ip_str);
	static std::array<uint8_t, 6> parse_mac_string(const std::string& mac_str);
	static std::array<uint8_t, 3> parse_hex_string(const std::string& hex_str);
};

#endif /* MONITORING_APP_MONITORING_CONFIG_H */
