#ifndef MONITORING_APP_MONITORING_CONFIG_H
#define MONITORING_APP_MONITORING_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <array>

namespace litefs {
    class ArgsContext;
}

class MonitoringConfig {
public:
    enum class MonitoringDest {
        DPDK,
        NONE,
        DEVICE
    };

    MonitoringDest monitoring_dest_type = MonitoringDest::DPDK;
    std::vector<std::string> monitoring_dest_devices;
    MonitoringConfig();
    
    int parse(int argc, char **argv);
    bool validate(char *errbuf, size_t errbuf_len) const;
    void print() const;

    const std::vector<std::string>& monitor_devices() const { return monitor_devices_; }
    const std::vector<std::string>& host_reps() const { return host_reps_; }
    const std::string& app_name() const { return app_name_str_; }
    uint16_t tunnel_udp_port;
    bool enable_host_branch;
    // bool enable_dpdk_branch; // Replaced by monitoring_dest_type
    bool enable_counters;
    bool anti_replay;
    uint16_t decap_flow_count = 1;
    double sampling_fraction;

    struct IpsecEncapConfig {
        std::array<uint8_t, 4> src_ip;
        std::array<uint8_t, 4> dst_ip;
        std::array<uint8_t, 6> src_mac;
        std::array<uint8_t, 6> dst_mac;
        uint32_t spi;
        std::array<uint8_t, 32> key;     // 256-bit key
        uint32_t salt;
        uint64_t iv;       // 64-bit IV
    };

    struct IpsecDecapConfig {
        uint32_t spi;
        std::array<uint8_t, 32> key;    // 256-bit key
        uint32_t salt;
        uint64_t iv;      // 64-bit IV
        std::array<uint8_t, 6> src_mac;
        std::array<uint8_t, 6> dst_mac;
    };

    const std::vector<IpsecEncapConfig> &ipsec_encap_configs() const {
        return ipsec_encap_configs_;
    }
    const std::vector<IpsecDecapConfig> &ipsec_decap_configs() const {
        return ipsec_decap_configs_;
    }

private:
    std::vector<std::string> monitor_devices_;
    std::vector<std::string> host_reps_;
    std::string app_name_str_;
    std::vector<IpsecEncapConfig> ipsec_encap_configs_;
    std::vector<IpsecDecapConfig> ipsec_decap_configs_;
    void apply_config_to_app_cfg(const litefs::ArgsContext& ctx);
    void store_device_strings(const litefs::ArgsContext& ctx);
    void parse_ipsec_config(const litefs::ArgsContext& ctx);
    static std::array<uint8_t, 4> parse_ipv4_string(const std::string& ip_str);
    static std::array<uint8_t, 6> parse_mac_string(const std::string& mac_str);
    template<size_t N>
    static std::array<uint8_t, N> parse_hex_string(const std::string& hex_str);
};

// Template implementation
template<size_t N>
inline std::array<uint8_t, N> MonitoringConfig::parse_hex_string(const std::string& hex_str) {
    std::array<uint8_t, N> result = {0};
    
    // Remove 0x prefix if present
    const char* start = hex_str.c_str();
    if (hex_str.length() >= 2 && hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X')) {
        start += 2;
    }
    
    size_t len = strlen(start);
    size_t expected_len = N * 2;
    
    // For keys (32 bytes), require exact length
    if (N == 32) {
        if (hex_str.empty()) {
            throw std::runtime_error("Key string cannot be empty");
        }
        if (len != expected_len) {
            throw std::runtime_error("Key must be exactly 64 hex characters (32 bytes), got " + std::to_string(len));
        }
    } else if (len > expected_len) {
        throw std::runtime_error("Hex string too long for array size " + std::to_string(N) + ", got " + std::to_string(len) + " chars");
    }
    
    // Parse each pair of hex characters, starting from the end for proper alignment
    // (most significant bytes first for big-endian representation)
    size_t parse_len = (len < expected_len) ? len : expected_len;
    size_t offset = expected_len - parse_len;
    
    for (size_t i = 0; i < parse_len / 2; ++i) {
        char hex_byte[3] = {start[i * 2], start[i * 2 + 1], '\0'};
        char* endptr = nullptr;
        unsigned long byte_val = strtoul(hex_byte, &endptr, 16);
        if (endptr == hex_byte || *endptr != '\0') {
            throw std::runtime_error("Invalid hex character at position " + std::to_string(i * 2));
        }
        result[offset + i] = static_cast<uint8_t>(byte_val);
    }
    
    return result;
}

#endif /* MONITORING_APP_MONITORING_CONFIG_H */


