#ifndef LFS_XTS_LOG_PROCESSOR_H
#define LFS_XTS_LOG_PROCESSOR_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>
#include <memory>
#include <set>

#include "file_system/log_processor.h"
#include "common/dpdk_offloads.h"
#include "common/ring_buffer.h"

/**
 * @brief A LogProcessor that performs AES-XTS encryption on WRITE log data.
 * This implementation uses parallel worker threads with per-thread queue pairs.
 */
class XtsLogProcessor : public LogProcessor {
public:
    explicit XtsLogProcessor(AesXtsDevice* xts_dev, uint16_t num_workers = 4);
    ~XtsLogProcessor() override;

    size_t submit_chunk(const void* src_chunk, size_t src_chunk_size) override;
    bool poll_ready() override;
    size_t estimate_processed_size(size_t src_chunk_size) const override { return src_chunk_size; }

private:
    struct WriteWork {
        const void* src_header;
        void* dst_header;
        uint64_t submission_id;
    };

private:
    struct PendingUnit {
        uint64_t submission_id;
        void* dest_base;
        size_t dest_offset;
        size_t unit_len;
    };

    struct ActiveWork {
        uint32_t total_ops;
        uint32_t completed_ops;
    };

    struct PerThreadState {
        uint16_t queue_pair_id;
        std::deque<struct rte_crypto_op *> waiting_units_;
        size_t inflight_units_count;
        std::map<uint64_t, ActiveWork> active_works_;
        SpscRing<WriteWork>* work_queue_;
        SpscRing<uint64_t>* completion_queue_;
        struct rte_cryptodev_sym_session* session_;  // Shared across threads
    };

    /**
     * @brief Distribute write work to a worker queue.
     * @param src_header Pointer to the source header.
     * @param dst_header Pointer to the destination header.
     * @param submission_id The submission ID.
     * @return True if the work was distributed, false otherwise.
     */
    bool distribute_write_work(const void* src_header, void* dst_header,
                               uint64_t submission_id);
    bool process_worker_queue(uint16_t thread_id) override;
    void process_write_work(uint16_t thread_id, const WriteWork& work);
    bool worker_poll(uint16_t thread_id) override;
    size_t submit_from_waiting_queue(uint16_t thread_id);

    static inline uint32_t calculate_num_ops(uint32_t data_length, size_t first_unit_skew);

private:
    AesXtsDevice* xts_dev_;
    
    // Per-thread state
    std::vector<PerThreadState> thread_states_;
    std::vector<std::unique_ptr<SpscRing<WriteWork>>> work_queues_;
    
    // Completion notifications (all workers -> main thread)
    std::vector<std::unique_ptr<SpscRing<uint64_t>>> completion_queues_;  // one for each worker
    std::set<uint64_t> completed_submissions_;

    // Dest Buffer Reservations
    struct DestBufferReservation {
        uint64_t submission_id;
        void* dest_buffer;
        size_t ending_offset;
    };
    std::deque<DestBufferReservation> dest_buffer_reservations_;
    
    uint16_t next_worker_;  // Round-robin distribution counter
    
    // Shared pools (used for session creation and crypto ops)
    struct rte_mempool* session_pool_;
    struct rte_mempool* crypto_op_pool_;
    struct rte_mempool* mbuf_pool_;
};

#endif // LFS_XTS_LOG_PROCESSOR_H


