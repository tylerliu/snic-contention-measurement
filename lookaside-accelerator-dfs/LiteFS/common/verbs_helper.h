#ifndef LITEFS_VERBS_HELPER_H
#define LITEFS_VERBS_HELPER_H

#include <string>
#include <vector>
#include <infiniband/verbs.h>

// Structure to hold all the resources for a single RDMA connection
struct RdmaConnection {
    struct ibv_pd* pd = nullptr;
    struct ibv_cq* cq = nullptr;
    struct ibv_qp* qp = nullptr;
    std::vector<ibv_mr*> mrs;
};

// Structure to exchange connection info over TCP
struct RdmaConnData {
    uint32_t qp_num;
    uint16_t lid;
    union ibv_gid gid;
    // Add memory region info if needed for one-sided ops
    uint64_t mr_addr;
    uint32_t mr_rkey;
};


class VerbsManager {
public:
    // Initializes verbs by opening the specified RDMA device by name (or the first available if empty).
    // Optionally specify IB port number (1-based) and GID index (for RoCE).
    VerbsManager(const std::string &ibv_device_name = "", uint8_t ib_port = 1, uint8_t gid_index = 0);
    ~VerbsManager();

    // Prepare local RDMA connection data (LID, GID, QP number, etc.)
    void PrepareLocalConnData();

    // Provide remote RDMA connection data obtained externally (e.g., via TCP K/V)
    void SetRemoteConnData(const RdmaConnData& remote);

    // Transition QP to RTR/RTS using the provided remote data
    void ConnectToRemote();

    // Register a memory region
    struct ibv_mr* RegisterMemory(void* addr, size_t length);
    void DeregisterMemory(struct ibv_mr* mr);

    void SetLocalMr(uint64_t addr, uint32_t rkey);

    // Post a receive request to the queue
    void PostRecv(struct ibv_mr* mr, void* local_addr, size_t len);

    // Post a one-sided RDMA WRITE; optional remote_offset
    void PostWrite(struct ibv_mr* local_mr, size_t len, const RdmaConnData& remote_conn_data, uint64_t remote_offset = 0);
    
    // Post a one-sided RDMA WRITE using an address within the MR (offset write)
    void PostWrite(struct ibv_mr* mr_for_lkey, void* local_addr_within_mr, size_t len, const RdmaConnData& remote_conn_data, uint64_t remote_offset = 0);

    // Post a two-sided RDMA SEND using the MR's base address
    void PostSend(struct ibv_mr* local_mr, size_t len, uint32_t flags = IBV_SEND_SIGNALED);

    // Post a two-sided RDMA SEND using an address within the MR (offset send)
    void PostSend(struct ibv_mr* mr_for_lkey, void* local_addr_within_mr, size_t len, uint32_t flags = IBV_SEND_SIGNALED);

    // Poll the completion queue
    int PollCompletion(struct ibv_wc* wc);

    const RdmaConnData& GetLocalConnData() const { return local_conn_data_; }
    const RdmaConnData& GetRemoteConnData() const { return remote_conn_data_; }

    // Disable copy and assign
    VerbsManager(const VerbsManager&) = delete;
    VerbsManager& operator=(const VerbsManager&) = delete;
    
    // Enable move semantics
    VerbsManager(VerbsManager&& other) noexcept;
    VerbsManager& operator=(VerbsManager&& other) noexcept;

private:
    void build_resources();
    void create_qp();
    // removed: TCP-based exchange; K/V handled by caller

    struct ibv_context* verbs_context_ = nullptr;
    struct ibv_device** device_list_ = nullptr; // kept for cleanup
    std::string ibv_device_name_;
    uint8_t ib_port_ = 1;
    uint8_t gid_index_ = 0;

    RdmaConnection conn_{};
    RdmaConnData local_conn_data_{};
    RdmaConnData remote_conn_data_{};
};

#endif // LITEFS_VERBS_HELPER_H
