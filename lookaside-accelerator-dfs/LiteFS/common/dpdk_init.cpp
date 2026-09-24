#include "dpdk_init.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <doca_error.h>
#include <doca_log.h>
#include <rte_ethdev.h>
#include <rte_errno.h>

std::unique_ptr<AppLifecycleManager> AppLifecycleManager::app_lifecycle_manager_ = nullptr;

AppLifecycleManager::AppLifecycleManager(const std::string &app_name)
    : parser_(app_name, app_name + " arguments"), app_name_(app_name) {
    parser_.add_option("log-level")
        .help("Controls doca_log global level (DISABLE, CRITICAL, ERROR, WARNING, INFO, DEBUG, TRACE)")
        .default_value("INFO");
}

AppLifecycleManager::~AppLifecycleManager() {
    if (rte_eal_get_runtime_dir() != nullptr) {
        rte_eal_cleanup();
    }
}

void AppLifecycleManager::parse(int argc, char **argv) {
    try {
        context_ = parser_.parse(argc, argv);
    } catch (const litefs::ArgParserError &err) {
        throw std::runtime_error(err.what());
    }

    initialize_dpdk();
    apply_log_level();
}

void AppLifecycleManager::set_use_argp_dpdk_program(bool enable) {
    (void)enable;  // Compatibility no-op.
}

bool AppLifecycleManager::get_flag(const std::string &name) const {
    if (!context_.has(name)) {
        return false;
    }
    try {
        return context_.bool_value(name, false);
    } catch (const litefs::ArgParserError &err) {
        throw std::runtime_error(err.what());
    }
}

long long AppLifecycleManager::get_int(const std::string &name) const {
    if (!context_.has(name)) {
        throw std::runtime_error("Missing required int option: --" + name);
    }
    try {
        return context_.int_value(name, 0);
    } catch (const litefs::ArgParserError &err) {
        throw std::runtime_error(err.what());
    }
}

std::string AppLifecycleManager::get_string(const std::string &name) const {
    if (!context_.has(name)) {
        throw std::runtime_error("Missing required string option: --" + name);
    }
    const std::string* value = context_.first_value(name);
    if (!value) {
        throw std::runtime_error("Missing required string option: --" + name);
    }
    return *value;
}

void AppLifecycleManager::initialize_dpdk() {
    const std::vector<std::string> &dpdk_tokens = context_.dpdk_args();
    if (dpdk_tokens.empty()) {
        return;
    }

    std::vector<char*> argv;
    argv.reserve(dpdk_tokens.size() + 1);
    argv.push_back(const_cast<char*>(app_name_.c_str()));
    for (const std::string &token : dpdk_tokens) {
        argv.push_back(const_cast<char*>(token.c_str()));
    }

    // print the argv
    std::cout << "DPDK argv: ";
    for (const char *token : argv) {
        std::cout << token << " ";
    }
    std::cout << std::endl;

    int rc = rte_eal_init(static_cast<int>(argv.size()), argv.data());
    if (rc < 0) {
        throw std::runtime_error(std::string("rte_eal_init failed: ") + rte_strerror(rte_errno));
    }
}

void AppLifecycleManager::apply_log_level() const {
    const std::string* level = context_.first_value("log-level");
    if (!level) {
        return;
    }
    (void)doca_log_level_set_global_lower_limit(static_cast<uint32_t>(to_doca_log_level(*level)));
}

int AppLifecycleManager::to_doca_log_level(const std::string &level) {
    std::string upper(level.size(), '\0');
    std::transform(level.begin(), level.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    if (upper == "DISABLE" || upper == "NONE") {
        return DOCA_LOG_LEVEL_DISABLE;
    } else if (upper == "CRITICAL" || upper == "FATAL") {
        return DOCA_LOG_LEVEL_CRIT;
    } else if (upper == "ERROR") {
        return DOCA_LOG_LEVEL_ERROR;
    } else if (upper == "WARNING" || upper == "WARN") {
        return DOCA_LOG_LEVEL_WARNING;
    } else if (upper == "DEBUG") {
        return DOCA_LOG_LEVEL_DEBUG;
    } else if (upper == "TRACE") {
        return DOCA_LOG_LEVEL_TRACE;
    }

    return DOCA_LOG_LEVEL_INFO;
}

Mempool::Mempool(const std::string &name, uint32_t num_bufs, uint16_t buf_size, int socket_id) {
    uint16_t real_buf_size = (buf_size == 0) ? RTE_MBUF_DEFAULT_BUF_SIZE : buf_size;
    pool_ = rte_pktmbuf_pool_create(name.c_str(), num_bufs, 0, 0, real_buf_size, socket_id);
    if (pool_ == nullptr) {
        throw std::runtime_error("Mempool creation failed: " + std::string(rte_strerror(rte_errno)));
    }
}

Mempool::~Mempool() {
    if (pool_ != nullptr) {
        rte_mempool_free(pool_);
    }
}

// EthernetPort implementation
EthernetPort::EthernetPort(uint16_t port_id, uint16_t num_rx_queues, uint16_t num_tx_queues, Mempool &mp)
    : port_id_(port_id) {

    if (!rte_eth_dev_is_valid_port(port_id_)) {
        throw std::runtime_error("Error: Port " + std::to_string(port_id_) + " is not a valid port.");
    }

    // Default port configuration
    struct rte_eth_conf port_conf;
    memset(&port_conf, 0, sizeof(port_conf));
    port_conf.rxmode.max_lro_pkt_size = RTE_ETHER_MAX_LEN;

    int ret = rte_eth_dev_configure(port_id_, num_rx_queues, num_tx_queues, &port_conf);
    if (ret != 0) throw std::runtime_error("Failed to configure port " + std::to_string(port_id_));

    uint16_t nb_rxd = 1024;
    uint16_t nb_txd = 1024;
    ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id_, &nb_rxd, &nb_txd);
    if (ret != 0) throw std::runtime_error("Failed to adjust descriptors for port " + std::to_string(port_id_));

    int socket_id = rte_eth_dev_socket_id(port_id_);
    for (uint16_t q = 0; q < num_rx_queues; q++) {
        ret = rte_eth_rx_queue_setup(port_id_, q, nb_rxd, socket_id, NULL, mp.get());
        if (ret != 0) throw std::runtime_error("Failed to setup RX queue " + std::to_string(q));
    }

    for (uint16_t q = 0; q < num_tx_queues; q++) {
        ret = rte_eth_tx_queue_setup(port_id_, q, nb_txd, socket_id, NULL);
        if (ret != 0) throw std::runtime_error("Failed to setup TX queue " + std::to_string(q));
    }

    ret = rte_eth_dev_start(port_id_);
    if (ret != 0) throw std::runtime_error("Failed to start port " + std::to_string(port_id_));

    ret = rte_eth_promiscuous_enable(port_id_);
    if (ret != 0) throw std::runtime_error("Failed to enable promiscuous mode on port " + std::to_string(port_id_));
}

EthernetPort::~EthernetPort() {
    std::cout << "Stopping port " << port_id_ << std::endl;
    rte_eth_dev_stop(port_id_);
    rte_eth_dev_close(port_id_);
}
