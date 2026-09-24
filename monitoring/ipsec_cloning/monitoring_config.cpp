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

    // Set tunnel_udp_port
    if (ctx.has("tunnel-udp-port")) {
        int value = static_cast<int>(ctx.int_value("tunnel-udp-port", 4789));
        if (value <= 0 || value > UINT16_MAX) {
            throw std::runtime_error("tunnel-udp-port must be between 1 and " + std::to_string(UINT16_MAX));
        }
        tunnel_udp_port = static_cast<uint16_t>(value);
    }


    // Set disable-host
    if (ctx.has("disable-host") && ctx.bool_value("disable-host", false)) {
        enable_host_branch = false;
    }

    // Set monitoring-dest
    if (ctx.has("monitor-dest")) {
        std::vector<std::string> dests = ctx.values("monitor-dest");
        // Remove empty strings
        dests.erase(std::remove_if(dests.begin(), dests.end(), 
            [](const std::string& s) { return s.empty(); }), dests.end());

        if (!dests.empty()) {
            // Check type based on first element
            std::string first = dests[0];
            if (first == "dpdk") {
                monitoring_dest_type = MonitoringDest::DPDK;
            } else if (first == "none") {
                monitoring_dest_type = MonitoringDest::NONE;
            } else {
                monitoring_dest_type = MonitoringDest::DEVICE;
                monitoring_dest_devices = dests;
            }
        }
    }

    // Normalize monitoring_dest_devices if needed (broadcast)
    if (monitoring_dest_type == MonitoringDest::DEVICE && !monitor_devices_.empty()) {
        if (monitoring_dest_devices.size() == 1 && monitor_devices_.size() > 1) {
            std::string device = monitoring_dest_devices[0];
            monitoring_dest_devices.resize(monitor_devices_.size(), device);
        }
    }

    // Set enable-counters
    if (ctx.has("enable-counters")) {
        enable_counters = true;
    }

    // Set anti-replay
    if (ctx.has("ipsec-decap-anti-replay")) {
        anti_replay = ctx.bool_value("ipsec-decap-anti-replay", true);
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
    : monitoring_dest_type(MonitoringDest::DPDK),
      tunnel_udp_port(4789),
      enable_host_branch(true),
      enable_counters(false),
      anti_replay(true),
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

        g_parser->add_option("tunnel-udp-port").alias("u")
            .help("Outer UDP destination port for mirrored traffic (VXLAN by default)")
            .default_value("4789");


        g_parser->add_option("disable-host")
            .help("Disable host branch forwarding")
            .argument_count('0');

        g_parser->add_option("monitor-dest").alias("d")
            .help("Monitoring destination: 'dpdk', 'none', or device identifier (e.g. 03:00.1)")
            .default_value("dpdk");

        g_parser->add_option("enable-counters")
            .help("Enable counters on monitoring control pipe")
            .argument_count('0');

        g_parser->add_option("ipsec-decap-anti-replay")
            .help("Enable anti-replay for IPsec decapsulation (true/false)")
            .default_value("true");

        g_parser->add_option("ipsec-encap-src-ip")
            .help("IPsec encap source IP address(es) - one per monitor device")
            .default_value("");

        g_parser->add_option("ipsec-encap-dst-ip")
            .help("IPsec encap destination IP address(es) - one per monitor device")
            .default_value("");

        g_parser->add_option("ipsec-encap-src-mac")
            .help("IPsec encap source MAC address(es) - one per monitor device (format: aa:bb:cc:dd:ee:ff)")
            .default_value("");

        g_parser->add_option("ipsec-encap-dst-mac")
            .help("IPsec encap destination MAC address(es) - one per monitor device (format: aa:bb:cc:dd:ee:ff)")
            .default_value("");

        g_parser->add_option("ipsec-encap-spi")
            .help("IPsec encap Security Parameter Index(es) - one per monitor device (format: 0x1001 or decimal)")
            .default_value("");

        g_parser->add_option("ipsec-encap-key")
            .help("IPsec encap encryption key(s) - one per monitor device (64 hex chars = 32 bytes = 256 bits)")
            .default_value("");

        g_parser->add_option("ipsec-encap-salt")
            .help("IPsec encap salt value(s) - one per monitor device (format: 0x11223344)")
            .default_value("");

        g_parser->add_option("ipsec-encap-iv")
            .help("IPsec encap implicit IV value(s) - one per monitor device (format: 0x1234567890abcdef)")
            .default_value("");

        g_parser->add_option("ipsec-decap-spi")
            .help("IPsec decap Security Parameter Index(es) - one per monitor device (format: 0x2001 or decimal)")
            .default_value("");

        g_parser->add_option("ipsec-decap-key")
            .help("IPsec decap decryption key(s) - one per monitor device (64 hex chars = 32 bytes = 256 bits)")
            .default_value("");

        g_parser->add_option("ipsec-decap-salt")
            .help("IPsec decap salt value(s) - one per monitor device (format: 0x22334455)")
            .default_value("");

        g_parser->add_option("ipsec-decap-iv")
            .help("IPsec decap implicit IV value(s) - one per monitor device (format: 0x0)")
            .default_value("");

        g_parser->add_option("ipsec-decap-src-mac")
            .help("IPsec decap source MAC address(es) - one per monitor device "
                  "(format: aa:bb:cc:dd:ee:ff)")
            .default_value("");

        g_parser->add_option("ipsec-decap-dst-mac")
            .help("IPsec decap destination MAC address(es) - one per monitor "
                  "device (format: aa:bb:cc:dd:ee:ff)")
            .default_value("");

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

        // Parse IPsec configuration
        parse_ipsec_config(g_context);

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
	if (!enable_host_branch && monitoring_dest_type == MonitoringDest::NONE) {
		if (errbuf != NULL && errbuf_len > 0)
			snprintf(errbuf, errbuf_len, "at least one forwarding branch must be enabled (host or monitor-dest != none)");
		return false;
	}

	// Validate monitoring dest count if type is DEVICE
    if (monitoring_dest_type == MonitoringDest::DEVICE) {
        if (monitoring_dest_devices.empty()) {
             if (errbuf != NULL && errbuf_len > 0)
                snprintf(errbuf, errbuf_len, "monitoring-dest type is DEVICE but no devices specified");
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

	// Require monitor and host pairing if host branch is enabled
	if (enable_host_branch && host_reps_.empty()) {
		if (monitor_devices_.size() != host_reps_.size()) {
			if (errbuf != NULL && errbuf_len > 0)
				snprintf(errbuf, errbuf_len, 
					"number of monitor devices (%zu) must equal number of host representors (%zu) when both branches are enabled",
					monitor_devices_.size(), host_reps_.size());
			return false;
		}
	}

    // Validate IPsec encap configuration
    if (!ipsec_encap_configs_.empty()) {
        if (ipsec_encap_configs_.size() != monitor_devices_.size()) {
            if (errbuf != NULL && errbuf_len > 0)
                snprintf(errbuf, errbuf_len, "number of IPsec encap entries (%zu) must equal number of monitor devices (%zu)",
                    ipsec_encap_configs_.size(), monitor_devices_.size());
            return false;
        }
    }

    // Validate IPsec decap configuration
    if (enable_host_branch) {
        if (ipsec_decap_configs_.size() != monitor_devices_.size()) {
            if (errbuf != NULL && errbuf_len > 0)
                snprintf(errbuf, errbuf_len, "number of IPsec decap entries (%zu) must equal number of monitor devices (%zu)",
                    ipsec_decap_configs_.size(), monitor_devices_.size());
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
	printf("  tunnel_udp_port : %u\n", tunnel_udp_port);
	printf("  enable_host     : %s\n", enable_host_branch ? "yes" : "no");
	printf("  monitor_dest    : ");
    if (monitoring_dest_type == MonitoringDest::DPDK) printf("dpdk\n");
    else if (monitoring_dest_type == MonitoringDest::NONE) printf("none\n");
    else {
        for (size_t i = 0; i < monitoring_dest_devices.size(); ++i) {
            printf("%s%s", i > 0 ? ", " : "", monitoring_dest_devices[i].c_str());
        }
        printf("\n");
    }
	printf("  enable_counters : %s\n", enable_counters ? "yes" : "no");
	printf("  enable_ipsec_encap : %s\n", (!ipsec_encap_configs_.empty()) ? "yes" : "no");
	if (!ipsec_encap_configs_.empty()) {
		printf("  ipsec_encap entries: %zu\n", ipsec_encap_configs_.size());
		for (size_t i = 0; i < ipsec_encap_configs_.size(); ++i) {
			const auto& cfg = ipsec_encap_configs_[i];
			printf("    [%zu]", i);
			// Convert IP array to in_addr for printing
			struct in_addr ip;
			uint32_t ip_val = (static_cast<uint32_t>(cfg.src_ip[0]) << 24) |
			                  (static_cast<uint32_t>(cfg.src_ip[1]) << 16) |
			                  (static_cast<uint32_t>(cfg.src_ip[2]) << 8) |
			                  static_cast<uint32_t>(cfg.src_ip[3]);
			ip.s_addr = rte_cpu_to_be_32(ip_val);
			printf(" src_ip=%s", inet_ntoa(ip));
			ip_val = (static_cast<uint32_t>(cfg.dst_ip[0]) << 24) |
			         (static_cast<uint32_t>(cfg.dst_ip[1]) << 16) |
			         (static_cast<uint32_t>(cfg.dst_ip[2]) << 8) |
			         static_cast<uint32_t>(cfg.dst_ip[3]);
			ip.s_addr = rte_cpu_to_be_32(ip_val);
			printf(" dst_ip=%s", inet_ntoa(ip));
			printf(" src_mac=%02x:%02x:%02x:%02x:%02x:%02x",
				cfg.src_mac[0], cfg.src_mac[1], cfg.src_mac[2],
				cfg.src_mac[3], cfg.src_mac[4], cfg.src_mac[5]);
			printf(" dst_mac=%02x:%02x:%02x:%02x:%02x:%02x",
				cfg.dst_mac[0], cfg.dst_mac[1], cfg.dst_mac[2],
				cfg.dst_mac[3], cfg.dst_mac[4], cfg.dst_mac[5]);
			uint32_t spi_val = cfg.spi;
			uint32_t salt_val = cfg.salt;
			uint64_t iv_val = cfg.iv;
			printf(" spi=0x%08x", spi_val);
			printf(" salt=0x%08x iv=0x%016llx", salt_val, static_cast<unsigned long long>(iv_val));
			printf("\n");
		}
	}
	if (!ipsec_decap_configs_.empty()) {
		printf("  ipsec_decap entries: %zu\n", ipsec_decap_configs_.size());
		for (size_t i = 0; i < ipsec_decap_configs_.size(); ++i) {
			const auto& cfg = ipsec_decap_configs_[i];
			uint32_t spi_val = cfg.spi;
			uint32_t salt_val = cfg.salt;
			uint64_t iv_val = cfg.iv;
			printf("    [%zu] spi=0x%08x", i, spi_val);
			printf(" salt=0x%08x iv=0x%016llx", salt_val, static_cast<unsigned long long>(iv_val));
			printf("\n");
                        printf(
                            "          src_mac=%02x:%02x:%02x:%02x:%02x:%02x",
                            cfg.src_mac[0], cfg.src_mac[1], cfg.src_mac[2],
                            cfg.src_mac[3], cfg.src_mac[4], cfg.src_mac[5]);
                        printf(" dst_mac=%02x:%02x:%02x:%02x:%02x:%02x",
                               cfg.dst_mac[0], cfg.dst_mac[1], cfg.dst_mac[2],
                               cfg.dst_mac[3], cfg.dst_mac[4], cfg.dst_mac[5]);
                        printf("\n");
                }
	}
	if (ipsec_encap_configs_.empty() && ipsec_decap_configs_.empty()) {
		printf("  ipsec: not enabled\n");
	}
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

void MonitoringConfig::parse_ipsec_config(const litefs::ArgsContext& ctx) {
	// IPsec is always enabled when host branch is enabled (no flag needed)

	// Parse IPsec Encapsulation Configuration
	std::vector<std::string> encap_src_ips, encap_dst_ips, encap_src_macs, encap_dst_macs;
	std::vector<std::string> encap_spis, encap_keys, encap_salts, encap_ivs;

	if (ctx.has("ipsec-encap-src-ip")) {
		encap_src_ips = ctx.values("ipsec-encap-src-ip");
		encap_src_ips.erase(std::remove_if(encap_src_ips.begin(), encap_src_ips.end(),
			[](const std::string& s) { return s.empty(); }), encap_src_ips.end());
	}

	if (ctx.has("ipsec-encap-dst-ip")) {
		encap_dst_ips = ctx.values("ipsec-encap-dst-ip");
		encap_dst_ips.erase(std::remove_if(encap_dst_ips.begin(), encap_dst_ips.end(),
			[](const std::string& s) { return s.empty(); }), encap_dst_ips.end());
	}

	if (ctx.has("ipsec-encap-src-mac")) {
		encap_src_macs = ctx.values("ipsec-encap-src-mac");
		encap_src_macs.erase(std::remove_if(encap_src_macs.begin(), encap_src_macs.end(),
			[](const std::string& s) { return s.empty(); }), encap_src_macs.end());
	}

	if (ctx.has("ipsec-encap-dst-mac")) {
		encap_dst_macs = ctx.values("ipsec-encap-dst-mac");
		encap_dst_macs.erase(std::remove_if(encap_dst_macs.begin(), encap_dst_macs.end(),
			[](const std::string& s) { return s.empty(); }), encap_dst_macs.end());
	}

	if (ctx.has("ipsec-encap-spi")) {
		encap_spis = ctx.values("ipsec-encap-spi");
		encap_spis.erase(std::remove_if(encap_spis.begin(), encap_spis.end(),
			[](const std::string& s) { return s.empty(); }), encap_spis.end());
	}

	if (ctx.has("ipsec-encap-key")) {
		encap_keys = ctx.values("ipsec-encap-key");
		encap_keys.erase(std::remove_if(encap_keys.begin(), encap_keys.end(),
			[](const std::string& s) { return s.empty(); }), encap_keys.end());
	}

	if (ctx.has("ipsec-encap-salt")) {
		encap_salts = ctx.values("ipsec-encap-salt");
		encap_salts.erase(std::remove_if(encap_salts.begin(), encap_salts.end(),
			[](const std::string& s) { return s.empty(); }), encap_salts.end());
	}

	if (ctx.has("ipsec-encap-iv")) {
		encap_ivs = ctx.values("ipsec-encap-iv");
		encap_ivs.erase(std::remove_if(encap_ivs.begin(), encap_ivs.end(),
			[](const std::string& s) { return s.empty(); }), encap_ivs.end());
	}

	// Determine expected size for encap
	size_t encap_expected_size = 0;
	if (!encap_src_ips.empty()) {
		encap_expected_size = encap_src_ips.size();
	} else if (!encap_dst_ips.empty()) {
		encap_expected_size = encap_dst_ips.size();
	} else if (!encap_spis.empty()) {
		encap_expected_size = encap_spis.size();
	}

	// Build IPsec Encap Config entries
	if (encap_expected_size > 0) {
		// Validate that all arrays have the same size
		if (!encap_src_ips.empty() && encap_src_ips.size() != encap_expected_size) {
			throw std::runtime_error("ipsec-encap-src-ip count (" + std::to_string(encap_src_ips.size()) +
				") does not match expected count (" + std::to_string(encap_expected_size) + ")");
		}
		if (!encap_dst_ips.empty() && encap_dst_ips.size() != encap_expected_size) {
			throw std::runtime_error("ipsec-encap-dst-ip count (" + std::to_string(encap_dst_ips.size()) +
				") does not match expected count (" + std::to_string(encap_expected_size) + ")");
		}
		if (!encap_src_macs.empty() && encap_src_macs.size() != encap_expected_size) {
			throw std::runtime_error("ipsec-encap-src-mac count (" + std::to_string(encap_src_macs.size()) +
				") does not match expected count (" + std::to_string(encap_expected_size) + ")");
		}
		if (!encap_dst_macs.empty() && encap_dst_macs.size() != encap_expected_size) {
			throw std::runtime_error("ipsec-encap-dst-mac count (" + std::to_string(encap_dst_macs.size()) +
				") does not match expected count (" + std::to_string(encap_expected_size) + ")");
		}
		if (!encap_spis.empty() && encap_spis.size() != encap_expected_size) {
			throw std::runtime_error("ipsec-encap-spi count (" + std::to_string(encap_spis.size()) +
				") does not match expected count (" + std::to_string(encap_expected_size) + ")");
		}
		if (!encap_keys.empty() && encap_keys.size() != encap_expected_size) {
			throw std::runtime_error("ipsec-encap-key count (" + std::to_string(encap_keys.size()) +
				") does not match expected count (" + std::to_string(encap_expected_size) + ")");
		}
		if (!encap_salts.empty() && encap_salts.size() != encap_expected_size) {
			throw std::runtime_error("ipsec-encap-salt count (" + std::to_string(encap_salts.size()) +
				") does not match expected count (" + std::to_string(encap_expected_size) + ")");
		}
		if (!encap_ivs.empty() && encap_ivs.size() != encap_expected_size) {
			throw std::runtime_error("ipsec-encap-iv count (" + std::to_string(encap_ivs.size()) +
				") does not match expected count (" + std::to_string(encap_expected_size) + ")");
		}

		ipsec_encap_configs_.clear();
		ipsec_encap_configs_.reserve(encap_expected_size);

		for (size_t i = 0; i < encap_expected_size; ++i) {
			IpsecEncapConfig cfg;
			
			if (i < encap_src_ips.size()) {
				cfg.src_ip = parse_ipv4_string(encap_src_ips[i]);
			} else {
				throw std::runtime_error("ipsec-encap-src-ip is required for all entries");
			}

			if (i < encap_dst_ips.size()) {
				cfg.dst_ip = parse_ipv4_string(encap_dst_ips[i]);
			} else {
				throw std::runtime_error("ipsec-encap-dst-ip is required for all entries");
			}

			if (i < encap_src_macs.size()) {
				cfg.src_mac = parse_mac_string(encap_src_macs[i]);
			} else {
				throw std::runtime_error("ipsec-encap-src-mac is required for all entries");
			}

			if (i < encap_dst_macs.size()) {
				cfg.dst_mac = parse_mac_string(encap_dst_macs[i]);
			} else {
				throw std::runtime_error("ipsec-encap-dst-mac is required for all entries");
			}

			if (i < encap_spis.size()) {
				cfg.spi = std::stoul(encap_spis[i], nullptr, 16);
			} else {
				throw std::runtime_error("ipsec-encap-spi is required for all entries");
			}

			if (i < encap_keys.size()) {
				cfg.key = parse_hex_string<32>(encap_keys[i]);
			} else {
				throw std::runtime_error("ipsec-encap-key is required for all entries");
			}

			if (i < encap_salts.size()) {
				cfg.salt = std::stoul(encap_salts[i], nullptr, 0);
			} else {
				cfg.salt = 0; // Default
			}

			if (i < encap_ivs.size()) {
				cfg.iv = std::stoull(encap_ivs[i], nullptr, 0);
			} else {
				cfg.iv = 0; // Default
			}

			ipsec_encap_configs_.push_back(cfg);
		}
	}

	// Parse IPsec Decapsulation Configuration
	std::vector<std::string> decap_spis, decap_keys, decap_salts, decap_ivs;
        std::vector<std::string> decap_src_macs, decap_dst_macs;

        if (ctx.has("ipsec-decap-spi")) {
          decap_spis = ctx.values("ipsec-decap-spi");
          decap_spis.erase(
              std::remove_if(decap_spis.begin(), decap_spis.end(),
                             [](const std::string &s) { return s.empty(); }),
              decap_spis.end());
        }

        if (ctx.has("ipsec-decap-key")) {
		decap_keys = ctx.values("ipsec-decap-key");
		decap_keys.erase(std::remove_if(decap_keys.begin(), decap_keys.end(),
			[](const std::string& s) { return s.empty(); }), decap_keys.end());
	}

	if (ctx.has("ipsec-decap-salt")) {
		decap_salts = ctx.values("ipsec-decap-salt");
		decap_salts.erase(std::remove_if(decap_salts.begin(), decap_salts.end(),
			[](const std::string& s) { return s.empty(); }), decap_salts.end());
	}

	if (ctx.has("ipsec-decap-iv")) {
		decap_ivs = ctx.values("ipsec-decap-iv");
		decap_ivs.erase(std::remove_if(decap_ivs.begin(), decap_ivs.end(),
			[](const std::string& s) { return s.empty(); }), decap_ivs.end());
	}

        if (ctx.has("ipsec-decap-src-mac")) {
          decap_src_macs = ctx.values("ipsec-decap-src-mac");
          decap_src_macs.erase(
              std::remove_if(decap_src_macs.begin(), decap_src_macs.end(),
                             [](const std::string &s) { return s.empty(); }),
              decap_src_macs.end());
        }

        if (ctx.has("ipsec-decap-dst-mac")) {
          decap_dst_macs = ctx.values("ipsec-decap-dst-mac");
          decap_dst_macs.erase(
              std::remove_if(decap_dst_macs.begin(), decap_dst_macs.end(),
                             [](const std::string &s) { return s.empty(); }),
              decap_dst_macs.end());
        }

        // Determine expected size for decap
        size_t decap_expected_size = 0;
	if (!decap_spis.empty()) {
		decap_expected_size = decap_spis.size();
	} else if (!decap_keys.empty()) {
		decap_expected_size = decap_keys.size();
	}

	// If host branch is enabled, decap configs are required
	if (enable_host_branch && decap_expected_size == 0) {
          throw std::runtime_error(
              "IPsec decap configuration is required when host branch is "
              "enabled. "
              "Please provide ipsec-decap-spi and ipsec-decap-key (and "
              "optionally ipsec-decap-salt, ipsec-decap-iv, src-mac, dst-mac) "
              "with one entry per monitor device.");
        }

        // Build IPsec Decap Config entries
	if (decap_expected_size > 0) {
		// Validate that all arrays have the same size
		if (!decap_spis.empty() && decap_spis.size() != decap_expected_size) {
			throw std::runtime_error("ipsec-decap-spi count (" + std::to_string(decap_spis.size()) +
				") does not match expected count (" + std::to_string(decap_expected_size) + ")");
		}
		if (!decap_keys.empty() && decap_keys.size() != decap_expected_size) {
			throw std::runtime_error("ipsec-decap-key count (" + std::to_string(decap_keys.size()) +
				") does not match expected count (" + std::to_string(decap_expected_size) + ")");
		}
		if (!decap_salts.empty() && decap_salts.size() != decap_expected_size) {
			throw std::runtime_error("ipsec-decap-salt count (" + std::to_string(decap_salts.size()) +
				") does not match expected count (" + std::to_string(decap_expected_size) + ")");
		}
		if (!decap_ivs.empty() && decap_ivs.size() != decap_expected_size) {
			throw std::runtime_error("ipsec-decap-iv count (" + std::to_string(decap_ivs.size()) +
				") does not match expected count (" + std::to_string(decap_expected_size) + ")");
		}
                if (!decap_src_macs.empty() &&
                    decap_src_macs.size() != decap_expected_size) {
                  throw std::runtime_error(
                      "ipsec-decap-src-mac count (" +
                      std::to_string(decap_src_macs.size()) +
                      ") does not match expected count (" +
                      std::to_string(decap_expected_size) + ")");
                }
                if (!decap_dst_macs.empty() &&
                    decap_dst_macs.size() != decap_expected_size) {
                  throw std::runtime_error(
                      "ipsec-decap-dst-mac count (" +
                      std::to_string(decap_dst_macs.size()) +
                      ") does not match expected count (" +
                      std::to_string(decap_expected_size) + ")");
                }

                ipsec_decap_configs_.clear();
                ipsec_decap_configs_.reserve(decap_expected_size);

		for (size_t i = 0; i < decap_expected_size; ++i) {
			IpsecDecapConfig cfg;
			
			if (i < decap_spis.size()) {
				cfg.spi = std::stoul(decap_spis[i], nullptr, 0);
			} else {
				throw std::runtime_error("ipsec-decap-spi is required for all entries");
			}

			if (i < decap_keys.size()) {
				cfg.key = parse_hex_string<32>(decap_keys[i]);
			} else {
				throw std::runtime_error("ipsec-decap-key is required for all entries");
			}

			if (i < decap_salts.size()) {
				cfg.salt = std::stoul(decap_salts[i], nullptr, 0);
			} else {
				cfg.salt = 0; // Default
			}

			if (i < decap_ivs.size()) {
				cfg.iv = std::stoull(decap_ivs[i], nullptr, 0);
			} else {
				cfg.iv = 0; // Default
			}

                        if (i < decap_src_macs.size()) {
                          cfg.src_mac = parse_mac_string(decap_src_macs[i]);
                        } else {
                          // Default to 0 if not provided, will be
                          // handled/warned later if needed
                          cfg.src_mac = {0, 0, 0, 0, 0, 0};
                        }

                        if (i < decap_dst_macs.size()) {
                          cfg.dst_mac = parse_mac_string(decap_dst_macs[i]);
                        } else {
                          // Default to 0 if not provided
                          cfg.dst_mac = {0, 0, 0, 0, 0, 0};
                        }

                        ipsec_decap_configs_.push_back(cfg);
                }
	}

	// Validate that counts match monitor devices count
	if (!ipsec_encap_configs_.empty() && ipsec_encap_configs_.size() != monitor_devices_.size()) {
		DOCA_LOG_WARN("IPsec encap config has %zu entries but %zu monitor devices. "
			"Each monitor device should have one IPsec encap entry.",
			ipsec_encap_configs_.size(), monitor_devices_.size());
	}
	if (!ipsec_decap_configs_.empty() && ipsec_decap_configs_.size() != monitor_devices_.size()) {
		DOCA_LOG_WARN("IPsec decap config has %zu entries but %zu monitor devices. "
			"Each monitor device should have one IPsec decap entry.",
			ipsec_decap_configs_.size(), monitor_devices_.size());
	}
}

