#include "monitoring_config.h"

#include "arg_parser/arg_parser.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <stdexcept>
#include <memory>
#include <vector>
#include <string>
#include <iostream>
#include <cstdlib>

#include <doca_log.h>
#include <doca_version.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <arpa/inet.h>
#include <netinet/in.h>

extern "C" {
#include "dpdk_utils.h"
}

DOCA_LOG_REGISTER(MONITORING_CONFIG);

namespace {
    std::unique_ptr<litefs::ArgParser> g_parser;
    litefs::ArgsContext g_context;
}

void MonitoringConfig::apply_config_to_app_cfg(const litefs::ArgsContext& ctx) {
    // Device-based port IDs will be set after probing

    // Set monitor_udp_port
    if (ctx.has("monitor-udp-port")) {
        int value = static_cast<int>(ctx.int_value("monitor-udp-port", 4789));
        if (value <= 0 || value > UINT16_MAX) {
            throw std::runtime_error("monitor-udp-port must be between 1 and " + std::to_string(UINT16_MAX));
        }
        monitor_udp_port = static_cast<uint16_t>(value);
    }

    // Set main-dest
    if (ctx.has("main-dest")) {
        std::vector<std::string> dests = ctx.values("main-dest");
        dests.erase(std::remove_if(dests.begin(), dests.end(), 
            [](const std::string& s) { return s.empty(); }), dests.end());

        if (!dests.empty()) {
            std::string first = dests[0];
            if (first == "rss") main_dest_type = MonitoringDest::RSS;
            else if (first == "host") main_dest_type = MonitoringDest::HOST;
            else if (first == "drop") main_dest_type = MonitoringDest::DROP;
            else if (first == "device") {
                main_dest_type = MonitoringDest::DEVICE;
                if (dests.size() > 1) monitoring_dest_devices.assign(dests.begin() + 1, dests.end());
            } else {
                main_dest_type = MonitoringDest::DEVICE;
                monitoring_dest_devices = dests;
            }
        }
    }

    // Set clone-dest
    if (ctx.has("clone-dest")) {
        std::vector<std::string> dests = ctx.values("clone-dest");
        dests.erase(std::remove_if(dests.begin(), dests.end(), 
            [](const std::string& s) { return s.empty(); }), dests.end());

        if (!dests.empty()) {
            std::string first = dests[0];
            if (first == "rss") clone_dest_type = MonitoringDest::RSS;
            else if (first == "host") clone_dest_type = MonitoringDest::HOST;
            else if (first == "drop") clone_dest_type = MonitoringDest::DROP;
            else if (first == "device") {
                clone_dest_type = MonitoringDest::DEVICE;
                // Note: for simplicity, we assume one device list for both if needed, 
                // but usually clone is to RSS or HOST.
            } else {
                clone_dest_type = MonitoringDest::DEVICE;
            }
        }
    }

    // Normalize monitoring_dest_devices if needed (broadcast)
    if (main_dest_type == MonitoringDest::DEVICE && !monitor_devices_.empty()) {
        if (monitoring_dest_devices.size() == 1 && monitor_devices_.size() > 1) {
            std::string device = monitoring_dest_devices[0];
            monitoring_dest_devices.resize(monitor_devices_.size(), device);
        }
    }

    // Set enable-counters
    if (ctx.bool_value("enable-counters", false)) {
        enable_counters = true;
    }

    // Set disable-mirroring
    if (ctx.bool_value("disable-mirroring", false)) {
        enable_mirroring = false;
    }
}

void MonitoringConfig::store_device_strings(const litefs::ArgsContext& ctx) {
    // Store monitor devices (plural, supports multiple)
    if (ctx.has("monitor-devices")) {
        monitor_devices_ = ctx.values("monitor-devices");
        // Filter out empty strings
        monitor_devices_.erase(
            std::remove_if(monitor_devices_.begin(), monitor_devices_.end(),
                [](const std::string& s) { return s.empty(); }),
            monitor_devices_.end());
    }

    // Store host representors (plural, supports multiple)
    if (ctx.has("host-reps")) {
        host_reps_ = ctx.values("host-reps");
        // Filter out empty strings
        host_reps_.erase(
            std::remove_if(host_reps_.begin(), host_reps_.end(),
                [](const std::string& s) { return s.empty(); }),
            host_reps_.end());
    }
}

MonitoringConfig::MonitoringConfig()
    : main_dest_type(MonitoringDest::RSS),
      clone_dest_type(MonitoringDest::DROP),
      enable_mirroring(true),
      monitor_udp_port(4789),
      enable_counters(false),
      app_name_str_("monitoring_app")
{
}

int MonitoringConfig::parse(int argc, char **argv) {
    try {
        // Create parser
        g_parser.reset(new litefs::ArgParser(
            app_name_str_,
            "Monitoring application"));

        // Register options
        g_parser->add_option("monitor-devices").alias("m")
            .help("Monitor device(s) (e.g., p0, 03:00.0, auxiliary:mlx5_core.sf.4). Can be specified multiple times or as a list in YAML")
            .default_value("");

        g_parser->add_option("host-reps").alias("H")
            .help("Host representor device(s) (e.g., pf0hpf). Can be specified multiple times or as a list in YAML")
            .default_value("");

        g_parser->add_option("monitor-udp-port").alias("u")
            .help("UDP destination port for matching mirrored traffic")
            .default_value("4789");

        g_parser->add_option("main-dest").alias("d")
            .help("Main path destination: 'rss', 'host', 'drop', or 'device'")
            .default_value("rss");

        g_parser->add_option("clone-dest").alias("C")
            .help("Clone path destination: 'rss', 'host', 'drop', or 'device'")
            .default_value("drop");

        g_parser->add_option("disable-mirroring")
            .help("Disable mirroring path entirely")
            .argument_count('0');

        g_parser->add_option("enable-counters")
            .help("Enable counters on monitoring control pipe")
            .argument_count('0');

        g_parser->add_option("log-level")
            .help("Controls doca_log global level (DISABLE, CRITICAL, ERROR, WARNING, INFO, DEBUG, TRACE)")
            .default_value("INFO");

        // Handle version flag
        g_parser->add_option("version").alias("v")
            .help("Print version information")
            .argument_count('0');

        // Parse arguments
        g_context = g_parser->parse(argc, argv);

        // Check for version flag
        if (g_context.has("version")) {
            printf("DOCA SDK version: %s\n", doca_version());
            printf("DOCA runtime version: %s\n", doca_version_runtime());
            exit(EXIT_SUCCESS);
        }

        // Apply parsed values to cfg (overrides defaults)
        apply_config_to_app_cfg(g_context);

        // Store device strings
        store_device_strings(g_context);

        // Initialize DPDK EAL
        const std::vector<std::string> &dpdk_tokens = g_context.dpdk_args();
        std::vector<char*> eal_argv;
        eal_argv.reserve(dpdk_tokens.size() + 1);
        eal_argv.push_back(const_cast<char*>(app_name_str_.c_str()));
        for (const std::string &token : dpdk_tokens) {
            eal_argv.push_back(const_cast<char*>(token.c_str()));
        }
        
        std::cout << "EAL argv: ";
        for (const char* arg : eal_argv) {
            std::cout << arg << " ";
        }
        std::cout << std::endl;

        int rc = dpdk_init(static_cast<int>(eal_argv.size()), eal_argv.data());
        if (rc < 0) {
            DOCA_LOG_ERR("EAL initialization failed: %s", rte_strerror(rte_errno));
            return -1;
        }

        // Apply log level
        const std::string* level = g_context.first_value("log-level");
        if (level) {
            std::string upper(level->size(), '\0');
            std::transform(level->begin(), level->end(), upper.begin(),
                          [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

            uint32_t doca_level = DOCA_LOG_LEVEL_INFO;
            if (upper == "DISABLE" || upper == "NONE") {
                doca_level = DOCA_LOG_LEVEL_DISABLE;
            } else if (upper == "CRITICAL") {
                doca_level = DOCA_LOG_LEVEL_CRIT;
            } else if (upper == "ERROR") {
                doca_level = DOCA_LOG_LEVEL_ERROR;
            } else if (upper == "WARNING" || upper == "WARN") {
                doca_level = DOCA_LOG_LEVEL_WARNING;
            } else if (upper == "INFO") {
                doca_level = DOCA_LOG_LEVEL_INFO;
            } else if (upper == "DEBUG") {
                doca_level = DOCA_LOG_LEVEL_DEBUG;
            } else if (upper == "TRACE") {
                doca_level = DOCA_LOG_LEVEL_TRACE;
            } else {
                DOCA_LOG_WARN("Invalid log level: %s, using INFO", level->c_str());
            }
            doca_log_level_set_global_lower_limit(doca_level);
        }

        return 0;
    } catch (const litefs::ArgParserError &err) {
        DOCA_LOG_ERR("Failed to parse arguments: %s", err.what());
        return -1;
    } catch (const std::exception &err) {
        DOCA_LOG_ERR("Failed to initialize argument parser: %s", err.what());
        return -1;
    }
}

bool MonitoringConfig::validate(char *errbuf, size_t errbuf_len) const
{
	// Validate monitoring dest count if type is DEVICE
    if (main_dest_type == MonitoringDest::DEVICE) {
        if (monitoring_dest_devices.empty()) {
             if (errbuf != NULL && errbuf_len > 0)
                snprintf(errbuf, errbuf_len, "main-dest type is DEVICE but no devices specified");
            return false;
        }
        if (monitoring_dest_devices.size() != monitor_devices_.size()) {
            if (errbuf != NULL && errbuf_len > 0)
                snprintf(errbuf, errbuf_len, 
                    "number of monitoring destination devices (%zu) must equal 1 or number of monitor devices (%zu)",
                    monitoring_dest_devices.size(), monitor_devices_.size());
            return false;
        }
    }

	return true;
}

void MonitoringConfig::print() const
{
	printf("%s configuration:\n", app_name_str_.c_str());
	printf("  monitor_devices : ");
	if (monitor_devices_.empty()) {
		printf("(none)\n");
	} else {
		for (size_t i = 0; i < monitor_devices_.size(); ++i) {
			printf("%s%s", i > 0 ? ", " : "", monitor_devices_[i].c_str());
		}
		printf("\n");
	}
	printf("  host_reps       : ");
	if (host_reps_.empty()) {
		printf("(none)\n");
	} else {
		for (size_t i = 0; i < host_reps_.size(); ++i) {
			printf("%s%s", i > 0 ? ", " : "", host_reps_[i].c_str());
		}
		printf("\n");
	}
	printf("  monitor_udp_port: %u\n", monitor_udp_port);
	printf("  main_dest       : ");
    if (main_dest_type == MonitoringDest::RSS) printf("rss\n");
    else if (main_dest_type == MonitoringDest::HOST) printf("host\n");
    else if (main_dest_type == MonitoringDest::DROP) printf("drop\n");
    else if (main_dest_type == MonitoringDest::DEVICE) {
        if (monitoring_dest_devices.empty()) {
            printf("device (unconfigured)\n");
        } else {
            printf("device (");
            for (size_t i = 0; i < monitoring_dest_devices.size(); ++i) {
                printf("%s%s", i > 0 ? ", " : "", monitoring_dest_devices[i].c_str());
            }
            printf(")\n");
        }
    }
	printf("  clone_dest      : ");
    if (clone_dest_type == MonitoringDest::RSS) printf("rss\n");
    else if (clone_dest_type == MonitoringDest::HOST) printf("host\n");
    else if (clone_dest_type == MonitoringDest::DROP) printf("drop\n");
    else if (clone_dest_type == MonitoringDest::DEVICE) printf("device\n");
    printf("  enable_mirroring: %s\n", enable_mirroring ? "yes" : "no");
	printf("  enable_counters : %s\n", enable_counters ? "yes" : "no");
}

std::array<uint8_t, 4> MonitoringConfig::parse_ipv4_string(const std::string& ip_str) {
	struct in_addr addr;
	if (inet_aton(ip_str.c_str(), &addr) == 0) {
		throw std::runtime_error("Invalid IPv4 address: " + ip_str);
	}
	// Convert network byte order to array of bytes
	uint32_t ip = rte_be_to_cpu_32(addr.s_addr);
	std::array<uint8_t, 4> result;
	result[0] = (ip >> 24) & 0xff;
	result[1] = (ip >> 16) & 0xff;
	result[2] = (ip >> 8) & 0xff;
	result[3] = ip & 0xff;
	return result;
}

std::array<uint8_t, 6> MonitoringConfig::parse_mac_string(const std::string& mac_str) {
	std::array<uint8_t, 6> mac = {0};
	unsigned int bytes[6];
	int count = sscanf(mac_str.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
		&bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]);
	if (count != 6) {
		throw std::runtime_error("Invalid MAC address format: " + mac_str + " (expected aa:bb:cc:dd:ee:ff)");
	}
	for (int i = 0; i < 6; ++i) {
		mac[i] = static_cast<uint8_t>(bytes[i]);
	}
	return mac;
}

std::array<uint8_t, 3> MonitoringConfig::parse_hex_string(const std::string& hex_str) {
	std::array<uint8_t, 3> tun_id = {0};
	if (hex_str.empty()) {
		return tun_id;
	}
	// Handle 0x prefix
	const char* start = hex_str.c_str();
	if (hex_str.length() >= 2 && hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X')) {
		start += 2;
	}
	char* endptr = nullptr;
	unsigned long long_val = strtoul(start, &endptr, 16);
	if (endptr == start || *endptr != '\0') {
		throw std::runtime_error("Invalid hex string: " + hex_str);
	}
	uint32_t value = static_cast<uint32_t>(long_val) & 0x00ffffff; // Mask to 24 bits for VXLAN VNI
	// Store as 3 bytes (VXLAN VNI format)
	tun_id[0] = (value >> 16) & 0xff;
	tun_id[1] = (value >> 8) & 0xff;
	tun_id[2] = value & 0xff;
	return tun_id;
}

// VXLAN parsing functions removed

