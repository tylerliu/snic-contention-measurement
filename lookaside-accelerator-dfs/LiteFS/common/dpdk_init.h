#ifndef LITEFS_APP_LIFECYCLE_H
#define LITEFS_APP_LIFECYCLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <rte_eal.h>
#include <rte_mempool.h>

#ifdef __cplusplus
}
#endif

#include <string>
#include <memory>

#include "arg_parser/arg_parser.h"

/**
 * @brief Manages the lifecycle of DOCA and DPDK initialization using DOCA ARGP.
 *
 * This RAII class handles doca_argp init/destroy and ensures rte_eal_cleanup is called.
 */
class AppLifecycleManager {
private: 
    AppLifecycleManager(const std::string &app_name);
public:
    ~AppLifecycleManager();

    static void init(const std::string &app_name) {
        app_lifecycle_manager_.reset(new AppLifecycleManager(app_name));
    }

    static AppLifecycleManager& get_instance() {
        assert(app_lifecycle_manager_ != nullptr);
        return *app_lifecycle_manager_;
    }

    /**
     * @brief Parses the command line arguments, initializing DPDK EAL in the process.
     */
    void parse(int argc, char **argv);

    void set_use_argp_dpdk_program(bool enable);

    bool get_flag(const std::string &name) const;
    long long get_int(const std::string &name) const;
    std::string get_string(const std::string &name) const;

    AppLifecycleManager(const AppLifecycleManager&) = delete;
    AppLifecycleManager& operator=(const AppLifecycleManager&) = delete;
    AppLifecycleManager(AppLifecycleManager&&) = delete;
    AppLifecycleManager& operator=(AppLifecycleManager&&) = delete;

private:
    void initialize_dpdk();
    void apply_log_level() const;
    static int to_doca_log_level(const std::string &level);

public:
    litefs::ArgParser& parser() { return parser_; }
    const litefs::ArgsContext& context() const { return context_; }

private:
    litefs::ArgParser parser_;
    litefs::ArgsContext context_;
    std::string app_name_;

private:
    static std::unique_ptr<AppLifecycleManager> app_lifecycle_manager_;
};



/**
 * @brief A C++ RAII wrapper for a DPDK memory pool.
 */
class Mempool {
public:
    Mempool(const std::string &name, uint32_t num_bufs, uint16_t buf_size, int socket_id);
    ~Mempool();
    rte_mempool* get() { return pool_; }

    Mempool(const Mempool&) = delete;
    Mempool& operator=(const Mempool&) = delete;
    Mempool(Mempool&&) = delete;
    Mempool& operator=(Mempool&&) = delete;

private:
    rte_mempool *pool_;
};

/**
 * @brief A C++ RAII wrapper for a DPDK Ethernet Port.
 */
class EthernetPort {
public:
    EthernetPort(uint16_t port_id, uint16_t num_rx_queues, uint16_t num_tx_queues, Mempool &mp);
    ~EthernetPort();
    uint16_t get_port_id() const { return port_id_; }

    EthernetPort(const EthernetPort&) = delete;
    EthernetPort& operator=(const EthernetPort&) = delete;
    EthernetPort(EthernetPort&&) = delete;
    EthernetPort& operator=(EthernetPort&&) = delete;

private:
    uint16_t port_id_;
};

#endif // LITEFS_APP_LIFECYCLE_H
