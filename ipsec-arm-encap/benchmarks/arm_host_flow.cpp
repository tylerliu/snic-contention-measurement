#include "device_manager.h"
#include <doca_flow.h>
#include <rte_ethdev.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

static DeviceManager *devices;
static doca_flow_port *ports[4];
static doca_flow_pipe_entry *entries[2];
static void check(doca_error_t rc, const char *operation) {
    if (rc != DOCA_SUCCESS) {
        fprintf(stderr, "%s: %s\n", operation, doca_error_get_descr(rc));
        exit(1);
    }
}
#define CHECK(call) check((call), #call)

extern "C" void arm_host_probe(void) {
    try {
        devices = new DeviceManager;
        devices->add_device("03:00.0,dv_flow_en=2", "ARM PF0", true);
        devices->add_device("03:00.1,dv_flow_en=2", "ARM PF1", true);
        devices->add_device_rep("pf0hpf,dv_flow_en=2", "host PF0", true);
        devices->add_device_rep("pf1hpf,dv_flow_en=2", "host PF1", true);
    } catch (const std::exception &e) {
        fprintf(stderr, "probe: %s\n", e.what()); exit(1);
    }
}

extern "C" void arm_host_flow_start(struct rte_mempool *pool) {
    // The generator configures PF queues. Representors need only dummy queues.
    for (unsigned p = 2; p < 4; ++p) {
        rte_eth_conf cfg{};
        if (rte_eth_dev_configure(p, 1, 1, &cfg) ||
            rte_eth_rx_queue_setup(p, 0, 1024, rte_eth_dev_socket_id(p), nullptr, pool) ||
            rte_eth_tx_queue_setup(p, 0, 1024, rte_eth_dev_socket_id(p), nullptr) ||
            rte_eth_dev_start(p)) {
            fprintf(stderr, "representor %u setup failed\n", p); exit(1);
        }
    }
    doca_flow_cfg *cfg;
    CHECK(doca_flow_cfg_create(&cfg));
    CHECK(doca_flow_cfg_set_pipe_queues(cfg, 1));
    CHECK(doca_flow_cfg_set_mode_args(cfg, "switch,hws,isolated,expert,hairpinq_num=4"));
    CHECK(doca_flow_cfg_set_nr_counters(cfg, 16));
    uint16_t queue = 0;
    doca_flow_resource_rss_cfg rss{};
    rss.nr_queues = 1; rss.queues_array = &queue;
    CHECK(doca_flow_cfg_set_default_rss(cfg, &rss));
    CHECK(doca_flow_init(cfg));
    CHECK(doca_flow_cfg_destroy(cfg));
    for (unsigned p = 0; p < 4; ++p) {
        doca_flow_port_cfg *pc;
        CHECK(doca_flow_port_cfg_create(&pc));
        CHECK(doca_flow_port_cfg_set_port_id(pc, p));
        if (p < 2) CHECK(doca_flow_port_cfg_set_dev(pc, devices->get_device(p)));
        else CHECK(doca_flow_port_cfg_set_dev_rep(pc, devices->get_device_rep(p)));
        CHECK(doca_flow_port_cfg_set_actions_mem_size(pc, 16 * DOCA_FLOW_MAX_ENTRY_ACTIONS_MEM_SIZE));
        CHECK(doca_flow_port_start(pc, &ports[p]));
        CHECK(doca_flow_port_cfg_destroy(pc));
    }
    for (unsigned p = 0; p < 2; ++p) {
        auto *sw = doca_flow_port_switch_get(ports[p]);
        doca_flow_pipe_cfg *pc;
        CHECK(doca_flow_pipe_cfg_create(&pc, sw));
        CHECK(doca_flow_pipe_cfg_set_name(pc, "ARM_HOST_EGRESS"));
        CHECK(doca_flow_pipe_cfg_set_type(pc, DOCA_FLOW_PIPE_BASIC));
        CHECK(doca_flow_pipe_cfg_set_is_root(pc, true));
        CHECK(doca_flow_pipe_cfg_set_domain(pc, DOCA_FLOW_PIPE_DOMAIN_EGRESS));
        doca_flow_match match{};
        memset(match.outer.eth.dst_mac, 0xff, 6);
        CHECK(doca_flow_pipe_cfg_set_match(pc, &match, nullptr));
        doca_flow_monitor mon{};
        mon.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
        CHECK(doca_flow_pipe_cfg_set_monitor(pc, &mon));
        doca_flow_fwd fwd{}, miss{};
        fwd.type = DOCA_FLOW_FWD_PORT; fwd.port_id = p + 2;
        miss.type = DOCA_FLOW_FWD_DROP;
        doca_flow_pipe *pipe;
        CHECK(doca_flow_pipe_create(pc, &fwd, &miss, &pipe));
        CHECK(doca_flow_pipe_cfg_destroy(pc));
        const uint8_t mac[6] = {0x58,0xa2,0xe1,0x53,0x19,static_cast<uint8_t>(0xd6+p)};
        memcpy(match.outer.eth.dst_mac, mac, 6);
        CHECK(doca_flow_pipe_add_entry(0, pipe, &match, nullptr, nullptr, nullptr, 0, nullptr, &entries[p]));
        CHECK(doca_flow_entries_process(sw, 0, 10000, 1));
        if (doca_flow_pipe_entry_get_status(entries[p]) != DOCA_FLOW_ENTRY_STATUS_SUCCESS) {
            fprintf(stderr, "egress entry failed\n"); exit(1);
        }
        printf("ARM PF%u -> egress dst-MAC -> host representor %u ready\n", p, p+2);
    }
}

extern "C" void arm_host_flow_stop(void) {
    for (unsigned p = 0; p < 2; ++p) {
        doca_flow_resource_query q{};
        CHECK(doca_flow_resource_query_entry(entries[p], &q));
        printf("EGRESS PF%u packets=%llu bytes=%llu\n", p,
            (unsigned long long)q.counter.total_pkts, (unsigned long long)q.counter.total_bytes);
    }
    for (int p = 3; p >= 0; --p) CHECK(doca_flow_port_stop(ports[p]));
    doca_flow_destroy();
    // Device handles deliberately outlive ethdev stop/close in the generator.
}
