#include "gcm_log_processor.h"
#include "lite_log.h"
#include "rte_mbuf_core.h"
#include "rte_memcpy.h"
#include <cstdint>
#include <doca_log.h>
#include <string.h>
#include <rte_mbuf.h>
#include <cassert>
#include <algorithm>

// GCM constants
#define GCM_128_KEY_SIZE 16
#define GCM_IV_SIZE 12
#define GCM_TAG_SIZE 16
// Use slightly smaller chunk size to fit tag if we were keeping it, but we strip it.
// We can use standard 64KB chunks or similar. reusing XTS constants roughly.
#define GCM_DATA_UNIT_SIZE 4096 
#define GCM_PER_OP_SIZE (GCM_DATA_UNIT_SIZE * 7)

DOCA_LOG_REGISTER(GCM_LOG_PROCESSOR);

static constexpr size_t kIvOffset = sizeof(struct rte_crypto_op) + sizeof(struct rte_crypto_sym_op);
static constexpr size_t kPrivateDataOffset = kIvOffset + GCM_IV_SIZE;

GcmLogProcessor::GcmLogProcessor(AesGcmDevice* gcm_dev, uint16_t num_workers)
    : gcm_dev_(gcm_dev), 
      thread_states_(), work_queues_(num_workers), completion_queues_(num_workers),
      next_worker_(0), session_pool_(nullptr) {
    
    num_workers_ = num_workers;

    // Create shared session pool
    session_pool_ = gcm_dev_->create_session_pool("gcm_sess_pool", 1024, 128);
    if (session_pool_ == nullptr) {
        DOCA_LOG_ERR("Failed to create session pool");
        exit(1);
    }

    // We need extra private area for our PendingUnit (and IV).
    // The AesGcmDevice helper hides the additional_private_area_len argument, so we cast to base.
    crypto_op_pool_ = static_cast<CryptoDevice*>(gcm_dev_)->create_crypto_op_pool("gcm_ops_pool", 2048, 128, GCM_IV_SIZE + sizeof(PendingUnit));

    if (crypto_op_pool_ == nullptr) {
        DOCA_LOG_ERR("Failed to create crypto op pool");
        exit(1);
    }
    
    mbuf_pool_ = rte_pktmbuf_pool_create("gcm_mbuf_pool", 4096, 256, 128, 32768, gcm_dev_->socket_id());
    if (mbuf_pool_ == nullptr) {
        DOCA_LOG_ERR("Failed to create mbuf pool");
        exit(1);
    }

    // We also need a pool for dummy tags since we are stripping them.
    // Mbufs can hold them if we append them to dst_mbuf. 
    // But we need to make sure dst_mbuf has room. 
    // 32768 is plenty for payload + 16 bytes.
    
    // Initialize per-thread state
    thread_states_.resize(num_workers_);
    work_queues_.resize(num_workers_);
    
    for (uint16_t i = 0; i < num_workers_; i++) {
        auto& state = thread_states_[i];
        state.queue_pair_id = i;
        state.inflight_units_count = 0;

        // Create shared session (same session for all threads)
        const uint8_t temp_key[GCM_128_KEY_SIZE] = {0}; 
        state.session_ = gcm_dev_->create_encrypt_session(
            session_pool_,
            temp_key,
            GCM_128_KEY_SIZE,
            GCM_TAG_SIZE,
            0); // AAD length 0
        
        if (state.session_ == nullptr) {
            DOCA_LOG_ERR("Failed to create GCM session");
            exit(1);
        }
        
        work_queues_[i].reset(new SpscRing<WriteWork>(16));
        state.work_queue_ = work_queues_[i].get();

        completion_queues_[i].reset(new SpscRing<uint64_t>(16));
        state.completion_queue_ = completion_queues_[i].get();
    }
    
    DOCA_LOG_INFO("GcmLogProcessor initialized with %u workers", num_workers_);
}

GcmLogProcessor::~GcmLogProcessor() {
    for (auto& state : thread_states_) {
        if (state.session_ != nullptr) {
            rte_cryptodev_sym_session_free(gcm_dev_->get_dev_id(), state.session_);
        }
    }
    if (session_pool_) rte_mempool_free(session_pool_);
    if (crypto_op_pool_) rte_mempool_free(crypto_op_pool_);
    if (mbuf_pool_) rte_mempool_free(mbuf_pool_);
}

bool GcmLogProcessor::distribute_write_work(const void* src_header, void* dst_header,
                                            uint64_t submission_id) {
    WriteWork work = {
        .src_header = src_header,
        .dst_header = dst_header,
        .submission_id = submission_id
    };
    
    for (uint16_t i = 0; i < num_workers_; i++) {
        uint16_t worker_id = next_worker_;
        next_worker_ = (next_worker_ + 1) % num_workers_;
        if (work_queues_[worker_id]->try_push(work)) {
            return true;
        }
    }
    return false;
}

bool GcmLogProcessor::process_worker_queue(uint16_t thread_id) {
    auto& state = thread_states_[thread_id];
    WriteWork work;
    bool progress_made = false;
    while (state.waiting_units_.size() < 64 && state.work_queue_->try_pop(work)) {
        process_write_work(thread_id, work);
        progress_made = true;
    }
    size_t enqueued = submit_from_waiting_queue(thread_id);
    if (enqueued > 0) progress_made = true;
    return progress_made;
}

void GcmLogProcessor::process_write_work(uint16_t thread_id, const WriteWork& work) {
    auto& state = thread_states_[thread_id];
    
    const lite_log_write* write_log = reinterpret_cast<const lite_log_write*>(
        reinterpret_cast<const uint8_t*>(work.src_header) + sizeof(lite_log_header));
    
    uint32_t inode = write_log->ino;
    uint64_t file_offset = write_log->offset;
    size_t data_length = write_log->length;
    
    const uint8_t* src_data = reinterpret_cast<const uint8_t*>(work.src_header) + 
                               sizeof(lite_log_header) + sizeof(lite_log_write);
    uint8_t* dst_data = reinterpret_cast<uint8_t*>(work.dst_header) + 
                        sizeof(lite_log_header) + sizeof(lite_log_write);
    
    // Copy header plain
    memcpy(work.dst_header, work.src_header, sizeof(lite_log_header) + sizeof(lite_log_write));
    
    size_t remaining = data_length;
    uint32_t op_count = 0;
    
    // Calculate total ops needed
    // Since input is 16-byte aligned, we don't need skew handling
    uint32_t total_ops_needed = (remaining + GCM_PER_OP_SIZE - 1) / GCM_PER_OP_SIZE;
    
    // Bulk allocate ops
    struct rte_crypto_op* ops_bulk[total_ops_needed];
    if (rte_crypto_op_bulk_alloc(crypto_op_pool_, RTE_CRYPTO_OP_TYPE_SYMMETRIC, ops_bulk, total_ops_needed) == 0) {
        DOCA_LOG_ERR("Failed to bulk allocate %u crypto ops", total_ops_needed);
        exit(1);
    }

    // Bulk allocate mbufs (src and dst needed per op -> 2 * total_ops_needed)
    struct rte_mbuf* mbufs_bulk[total_ops_needed * 2];
    if (rte_pktmbuf_alloc_bulk(mbuf_pool_, mbufs_bulk, total_ops_needed * 2) < 0) {
         DOCA_LOG_ERR("Failed to bulk allocate %u mbufs", total_ops_needed * 2);
         // Cleanup ops before exit? Or just exit.
         exit(1);
    }
    
    for (uint32_t i = 0; i < total_ops_needed; ++i) {
        size_t op_len = (remaining < GCM_PER_OP_SIZE) ? remaining : GCM_PER_OP_SIZE;
        
        struct rte_crypto_op* op = ops_bulk[i];
        struct rte_mbuf* src_buf = mbufs_bulk[2 * i];
        struct rte_mbuf* dst_buf = mbufs_bulk[2 * i + 1];

        // Copy data to source mbuf
        rte_pktmbuf_reset(src_buf);
        rte_pktmbuf_append(src_buf, op_len);
        uint8_t* src_mbuf_ptr = rte_pktmbuf_mtod(src_buf, uint8_t*);
        rte_memcpy(src_mbuf_ptr, src_data, op_len);
        
        rte_pktmbuf_reset(dst_buf);
        // We need space for data + tag
        rte_pktmbuf_append(dst_buf, op_len + GCM_TAG_SIZE);
        
        op->sym->m_src = src_buf;
        op->sym->m_dst = dst_buf;
        op->sym->aead.data.offset = 0;
        op->sym->aead.data.length = op_len;
        op->sym->aead.digest.data = rte_pktmbuf_mtod_offset(dst_buf, uint8_t*, op_len);
        op->sym->aead.digest.phys_addr = rte_pktmbuf_iova_offset(dst_buf, op_len);
        
        // IV Generation: (Inode << 32) | (Block Index)
        // Ensure big-endian format for consistency
        uint64_t block_index = file_offset / 16;
        uint32_t ino_be = rte_cpu_to_be_32(inode);
        uint64_t blk_be = rte_cpu_to_be_64(block_index);
        
        uint8_t iv[GCM_IV_SIZE];
        memcpy(iv, &ino_be, 4);
        memcpy(iv + 4, &blk_be, 8);
        
        memcpy(rte_crypto_op_ctod_offset(op, uint8_t*, kIvOffset), iv, GCM_IV_SIZE);
        
        // Store metadata
        *(PendingUnit*)(rte_crypto_op_ctod_offset(op, uint8_t*, kPrivateDataOffset)) = {
            .submission_id = work.submission_id,
            .dest_base = dst_data,
            .dest_offset = 0, 
            .unit_len = op_len, 
        };

        rte_crypto_op_attach_sym_session(op, state.session_);
        state.waiting_units_.push_back(op);
        
        op_count++;
        remaining -= op_len;
        dst_data += op_len;
        src_data += op_len;
        // Advance file_offset to keep block index correct
        file_offset += op_len;
    }
    
    state.active_works_[work.submission_id] = {.total_ops = op_count, .completed_ops = 0};
}

size_t GcmLogProcessor::submit_from_waiting_queue(uint16_t thread_id) {
    auto& state = thread_states_[thread_id];
    size_t inflight_limit = CRYPTO_DEFAULT_NB_DESCRIPTORS;
    size_t submittable = std::min(inflight_limit - state.inflight_units_count, state.waiting_units_.size());
    if (submittable == 0) return 0;
    
    struct rte_crypto_op *ops[CRYPTO_DEFAULT_NB_DESCRIPTORS];
    auto it = state.waiting_units_.cbegin();
    for (size_t i = 0; i < submittable; i++) ops[i] = *it++;
    
    size_t enqueued = rte_cryptodev_enqueue_burst(gcm_dev_->get_dev_id(), state.queue_pair_id, ops, submittable);
    state.inflight_units_count += enqueued;
    for (size_t i = 0; i < enqueued; i++) state.waiting_units_.pop_front();
    
    return enqueued;
}

bool GcmLogProcessor::worker_poll(uint16_t thread_id) {
    auto& state = thread_states_[thread_id];
    struct rte_crypto_op* completed_op[64];
    bool progress = false;
    
    size_t dequeued = rte_cryptodev_dequeue_burst(gcm_dev_->get_dev_id(), state.queue_pair_id, completed_op, 64);
    if (dequeued > 0) progress = true;
    
    struct rte_mbuf* mbufs_to_free[64 * 2]; // src and dst per op
    unsigned int mbufs_count = 0;

    for (size_t i = 0; i < dequeued; i++) {
        PendingUnit& unit = *(PendingUnit*)(rte_crypto_op_ctod_offset(completed_op[i], uint8_t*, kPrivateDataOffset));
        
        if (completed_op[i]->status != RTE_CRYPTO_OP_STATUS_SUCCESS) {
            DOCA_LOG_ERR("GCM op failed for submission %lu status %d", unit.submission_id, completed_op[i]->status);
        }
        
        // Copy ONLY the data length, ignoring the tag at the end
        // unit.unit_len was set to op_len (data size)
        memcpy(unit.dest_base, 
               rte_pktmbuf_mtod(completed_op[i]->sym->m_dst, uint8_t*), 
               unit.unit_len);
        
        mbufs_to_free[mbufs_count++] = completed_op[i]->sym->m_src;
        mbufs_to_free[mbufs_count++] = completed_op[i]->sym->m_dst;
        
        state.inflight_units_count--;
        
        auto work_it = state.active_works_.find(unit.submission_id);
        if (work_it != state.active_works_.end()) {
            work_it->second.completed_ops++;
        }
    }
    
    if (dequeued > 0) {
        // Bulk free mbufs
        rte_pktmbuf_free_bulk(mbufs_to_free, mbufs_count);
        // Bulk free ops (cast to void** for raw mempool put, or use rte_mempool_put_bulk if they are from same pool)
        // Since they are from crypto_op_pool_, we can use rte_mempool_put_bulk.
        // rte_crypto_op_free just calls rte_mempool_put.
        rte_mempool_put_bulk(crypto_op_pool_, (void**)completed_op, dequeued);
    }
    
    for (auto it = state.active_works_.begin(); it != state.active_works_.end(); ) {
        if (it->second.completed_ops >= it->second.total_ops) {
            if (completion_queues_[thread_id]->try_push(it->first)) {
                state.active_works_.erase(it++);
                progress = true;
                continue;
            }
        }
        it++;
    }
    
    return progress;
}

size_t GcmLogProcessor::submit_chunk(const void* src_chunk, size_t src_chunk_size) {
    assert(!buffer_queue_.empty());
    auto& current_buffer = buffer_queue_.back();
    if (current_buffer.reserved + src_chunk_size > current_buffer.capacity) {
        DOCA_LOG_ERR("Buffer overflow in GCM submit");
        exit(1);
    }
    
    const uint8_t* src_ptr = static_cast<const uint8_t*>(src_chunk);
    size_t src_offset = 0;
    
    while (src_offset < src_chunk_size) {
        void* dest_ptr = static_cast<uint8_t*>(current_buffer.dest_buffer) + current_buffer.reserved;
        const auto* src_header = reinterpret_cast<const lite_log_header*>(src_ptr + src_offset);
        if (src_header->entry_size < sizeof(lite_log_header)) break;
        if (current_buffer.reserved + src_header->entry_size > current_buffer.capacity) break;
        
        if (src_header->op_type == LITE_LOG_OP_WRITE) {
            uint64_t submission_id = next_async_id_++;
            if (!distribute_write_work(src_header, dest_ptr, submission_id)) break;
            
            src_offset += src_header->entry_size;
            current_buffer.reserved += src_header->entry_size;
            dest_ptr = static_cast<uint8_t*>(dest_ptr) + src_header->entry_size;
            dest_buffer_reservations_.emplace_back(DestBufferReservation{submission_id, current_buffer.dest_buffer, current_buffer.reserved});
        } else {
            memcpy(dest_ptr, src_ptr + src_offset, src_header->entry_size);
            src_offset += src_header->entry_size;
            if (current_buffer.written == current_buffer.reserved) {
                current_buffer.written += src_header->entry_size;
            } else {
                dest_buffer_reservations_.back().ending_offset += src_header->entry_size;
            }
            current_buffer.reserved += src_header->entry_size;
        }
    }
    return src_offset;
}

bool GcmLogProcessor::poll_ready() {
    bool progress = worker_poll(0);
    
    uint64_t submission_id;
    for (uint16_t i = 0; i < num_workers_; i++) {
        while (completion_queues_[i]->try_pop(submission_id)) {
            completed_submissions_.insert(submission_id);
            progress = true;
        }
    }
    
    while (!completed_submissions_.empty()) {
        auto it = completed_submissions_.begin();
        if (*it != completed_count_) break;
        
        DestBufferReservation& res = dest_buffer_reservations_.front();
        assert(res.submission_id == *it);
        
        for (auto& buf : buffer_queue_) {
            if (buf.dest_buffer == res.dest_buffer) {
                buf.written = res.ending_offset;
                break;
            }
        }
        dest_buffer_reservations_.pop_front();
        completed_count_++;
        completed_submissions_.erase(it);
    }
    
    if (!progress) progress = process_worker_queue(0);
    return progress;
}
