
#include "verbs_helper.h"
#include <cstring>

#include <infiniband/verbs.h>
#include <stdexcept>
#include <iostream>
#include <unistd.h>
#include <doca_log.h>

DOCA_LOG_REGISTER(LITEFS_VERBS_HELPER);

#define IBV_ACCESS_FLAGS (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ)

// Forward declarations for QP state transition helpers
static void qp_to_init(struct ibv_qp* qp, uint8_t port_num);
static void qp_to_rtr(struct ibv_qp* qp, const RdmaConnData& remote, uint8_t port_num, uint8_t gid_index, struct ibv_context* verbs_context);
static void qp_to_rts(struct ibv_qp* qp);

VerbsManager::VerbsManager(const std::string &ibv_device_name, uint8_t ib_port, uint8_t gid_index)
    : ibv_device_name_(ibv_device_name), ib_port_(ib_port), gid_index_(gid_index) {
    DOCA_LOG_INFO("Initializing RDMA device list...");
    int num_devices = 0;
    device_list_ = ibv_get_device_list(&num_devices);
    if (device_list_ == nullptr || num_devices == 0) {
        throw std::runtime_error("No RDMA devices found");
    }
    // Select device by name (if provided) or first
    const struct ibv_device *chosen = device_list_[0];
    if (!ibv_device_name_.empty()) {
        for (int i = 0; i < num_devices; ++i) {
            const char *name = ibv_get_device_name(device_list_[i]);
            if (name != nullptr && ibv_device_name_ == std::string(name)) {
                chosen = device_list_[i];
                break;
            }
        }
        DOCA_LOG_INFO("Selecting device by name: '%s'", ibv_device_name_.c_str());
    } else {
        DOCA_LOG_INFO("No device specified; using first device.");
        DOCA_LOG_INFO("First device: %s", ibv_get_device_name(device_list_[0]));
    }
    verbs_context_ = ibv_open_device(const_cast<struct ibv_device *>(chosen));
    if (!verbs_context_) {
        throw std::runtime_error("Failed to open RDMA device");
    }
    build_resources();
    create_qp();
    // Move QP to INIT early so RECVs can be posted before handshake
    DOCA_LOG_INFO("Transitioning QP to INIT on port %d...", (int)ib_port_);
    qp_to_init(conn_.qp, ib_port_);
}

VerbsManager::~VerbsManager() {
    if (conn_.qp) ibv_destroy_qp(conn_.qp);
    if (conn_.cq) ibv_destroy_cq(conn_.cq);
    for (auto* mr : conn_.mrs) {
        ibv_dereg_mr(mr);
    }
    if (conn_.pd) ibv_dealloc_pd(conn_.pd);
    if (verbs_context_) {
        ibv_close_device(verbs_context_);
        verbs_context_ = nullptr;
    }
    if (device_list_) {
        ibv_free_device_list(device_list_);
        device_list_ = nullptr;
    }
}

VerbsManager::VerbsManager(VerbsManager&& other) noexcept
    : verbs_context_(other.verbs_context_),
      device_list_(other.device_list_),
      ibv_device_name_(std::move(other.ibv_device_name_)),
      ib_port_(other.ib_port_),
      gid_index_(other.gid_index_),
      conn_(other.conn_),
      local_conn_data_(other.local_conn_data_),
      remote_conn_data_(other.remote_conn_data_) {
    // Reset the moved-from object
    other.verbs_context_ = nullptr;
    other.device_list_ = nullptr;
    other.conn_ = {};
    other.local_conn_data_ = {};
    other.remote_conn_data_ = {};
}

VerbsManager& VerbsManager::operator=(VerbsManager&& other) noexcept {
    if (this != &other) {
        // Clean up current resources
        if (conn_.qp) ibv_destroy_qp(conn_.qp);
        if (conn_.cq) ibv_destroy_cq(conn_.cq);
        for (auto* mr : conn_.mrs) {
            ibv_dereg_mr(mr);
        }
        if (conn_.pd) ibv_dealloc_pd(conn_.pd);
        if (verbs_context_) {
            ibv_close_device(verbs_context_);
        }
        if (device_list_) {
            ibv_free_device_list(device_list_);
        }
        
        // Move from other
        ibv_device_name_ = std::move(other.ibv_device_name_);
        ib_port_ = other.ib_port_;
        gid_index_ = other.gid_index_;
        verbs_context_ = other.verbs_context_;
        device_list_ = other.device_list_;
        conn_ = other.conn_;
        local_conn_data_ = other.local_conn_data_;
        remote_conn_data_ = other.remote_conn_data_;
        
        // Reset the moved-from object
        other.verbs_context_ = nullptr;
        other.device_list_ = nullptr;
        other.conn_ = {};
        other.local_conn_data_ = {};
        other.remote_conn_data_ = {};
    }
    return *this;
}

void VerbsManager::build_resources() {
    DOCA_LOG_INFO("Allocating PD and creating CQ...");
    conn_.pd = ibv_alloc_pd(verbs_context_);
    if (!conn_.pd) {
        throw std::runtime_error("Failed to allocate protection domain");
    }

    conn_.cq = ibv_create_cq(verbs_context_, 128, nullptr, nullptr, 0);
    if (!conn_.cq) {
        throw std::runtime_error("Failed to create completion queue");
    }
}

void VerbsManager::create_qp() {
    DOCA_LOG_INFO("Creating RC QP...");
    struct ibv_qp_init_attr qp_init_attr = {};
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all = 1; // Generate completion for all sends
    qp_init_attr.send_cq = conn_.cq;
    qp_init_attr.recv_cq = conn_.cq;
    qp_init_attr.cap.max_send_wr = 512;
    qp_init_attr.cap.max_recv_wr = 512;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    qp_init_attr.cap.max_inline_data = 64;

    conn_.qp = ibv_create_qp(conn_.pd, &qp_init_attr);
    if (!conn_.qp) {
        throw std::runtime_error("Failed to create queue pair");
    }
}

void VerbsManager::SetLocalMr(uint64_t addr, uint32_t rkey) {
    local_conn_data_.mr_addr = addr;
    local_conn_data_.mr_rkey = rkey;
}

void VerbsManager::PrepareLocalConnData() {
    struct ibv_port_attr port_attr;
    if (ibv_query_port(verbs_context_, ib_port_, &port_attr)) {
        throw std::runtime_error("Failed to query port attributes");
    }
    local_conn_data_.lid = port_attr.lid;
    local_conn_data_.qp_num = conn_.qp->qp_num;
    if (ibv_query_gid(verbs_context_, ib_port_, gid_index_, &local_conn_data_.gid)) {
        throw std::runtime_error("Failed to query GID");
    }
}

void VerbsManager::SetRemoteConnData(const RdmaConnData& remote) {
    remote_conn_data_ = remote;
}

void VerbsManager::ConnectToRemote() {
    const uint8_t port_num = ib_port_;
    qp_to_init(conn_.qp, port_num);
    qp_to_rtr(conn_.qp, remote_conn_data_, port_num, gid_index_, verbs_context_);
    qp_to_rts(conn_.qp);
}

struct ibv_mr* VerbsManager::RegisterMemory(void* addr, size_t length) {
    struct ibv_mr* mr = ibv_reg_mr(conn_.pd, addr, length, IBV_ACCESS_FLAGS);
    if (!mr) {
        throw std::runtime_error("Failed to register memory region");
    }
    conn_.mrs.push_back(mr);
    return mr;
}

void VerbsManager::DeregisterMemory(struct ibv_mr* mr) {
    if (!mr) return;
    for (auto it = conn_.mrs.begin(); it != conn_.mrs.end(); ++it) {
        if (*it == mr) {
            ibv_dereg_mr(mr);
            conn_.mrs.erase(it);
            return;
        }
    }
}

void VerbsManager::PostRecv(struct ibv_mr* mr, void* local_addr, size_t len) {
    struct ibv_sge sge = { (uint64_t)local_addr, (uint32_t)len, mr->lkey };
    struct ibv_recv_wr wr = {}, *bad_wr = nullptr;

    wr.wr_id = (uint64_t)local_addr; // Use address as ID for simplicity
    wr.sg_list = &sge;
    wr.num_sge = 1;

    if (ibv_post_recv(conn_.qp, &wr, &bad_wr)) {
        throw std::runtime_error("Failed to post receive");
    }
}

void VerbsManager::PostWrite(struct ibv_mr* local_mr, size_t len, const RdmaConnData& remote_conn_data, uint64_t remote_offset) {
    PostWrite(local_mr, (void*)local_mr->addr, len, remote_conn_data, remote_offset);
}

void VerbsManager::PostWrite(struct ibv_mr* mr_for_lkey, void* local_addr_within_mr, size_t len, const RdmaConnData& remote_conn_data, uint64_t remote_offset) {
    struct ibv_sge sge = { (uint64_t)local_addr_within_mr, (uint32_t)len, mr_for_lkey->lkey };
    struct ibv_send_wr wr = {}, *bad_wr = nullptr;

    wr.wr_id = (uint64_t)local_addr_within_mr;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote_conn_data.mr_addr + remote_offset;
    wr.wr.rdma.rkey = remote_conn_data.mr_rkey;

    if (ibv_post_send(conn_.qp, &wr, &bad_wr)) {
        throw std::runtime_error("Failed to post RDMA write (offset addr)");
    }
}

void VerbsManager::PostSend(struct ibv_mr* local_mr, size_t len, uint32_t flags) {
    PostSend(local_mr, (void*)local_mr->addr, len, flags);
}

void VerbsManager::PostSend(struct ibv_mr* mr_for_lkey, void* local_addr_within_mr, size_t len, uint32_t flags) {
    struct ibv_sge sge = { (uint64_t)local_addr_within_mr, (uint32_t)len, mr_for_lkey->lkey };
    struct ibv_send_wr wr = {}, *bad_wr = nullptr;

    wr.wr_id = (uint64_t)local_addr_within_mr;
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = flags;

    if (ibv_post_send(conn_.qp, &wr, &bad_wr)) {
        throw std::runtime_error("Failed to post RDMA send");
    }
}

int VerbsManager::PollCompletion(struct ibv_wc* wc) {
    return ibv_poll_cq(conn_.cq, 1, wc);
}

// Internal helpers to transition QP states
static void qp_to_init(struct ibv_qp* qp, uint8_t port_num) {
    struct ibv_qp_attr attr = {};
    attr.qp_state        = IBV_QPS_INIT;
    attr.port_num        = port_num;
    attr.pkey_index      = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(qp, &attr, flags)) {
        throw std::runtime_error("Failed to modify QP to INIT");
    }
}

static void qp_to_rtr(struct ibv_qp* qp, const RdmaConnData& remote, uint8_t port_num, uint8_t gid_index, struct ibv_context* verbs_context) {
    // Query port attributes to get the active MTU
    struct ibv_port_attr port_attr;
    if (ibv_query_port(verbs_context, port_num, &port_attr)) {
        throw std::runtime_error("Failed to query port attributes for MTU selection");
    }

    // Convert MTU enum to bytes for logging (enum values: 1=256, 2=512, 3=1024, 4=2048, 5=4096)
    uint32_t mtu_bytes = 256 * (1 << (port_attr.active_mtu - 1));
    DOCA_LOG_INFO("Selected port MTU: enum=%u, bytes=%u", port_attr.active_mtu, mtu_bytes);
    
    struct ibv_qp_attr attr = {};
    attr.qp_state           = IBV_QPS_RTR;
    attr.path_mtu           = port_attr.active_mtu;
    attr.dest_qp_num        = remote.qp_num;
    attr.rq_psn             = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer      = 12;

    attr.ah_attr.is_global     = 1;
    attr.ah_attr.port_num      = port_num;
    attr.ah_attr.sl            = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.grh.hop_limit = 64; // more robust for multi-hop
    attr.ah_attr.grh.sgid_index= gid_index;
    attr.ah_attr.grh.dgid      = remote.gid;

    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(qp, &attr, flags)) {
        throw std::runtime_error("Failed to modify QP to RTR");
    }
}

static void qp_to_rts(struct ibv_qp* qp) {
    struct ibv_qp_attr attr = {};
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = 0;
    attr.max_rd_atomic = 1;
    int flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(qp, &attr, flags)) {
        throw std::runtime_error("Failed to modify QP to RTS");
    }
}

