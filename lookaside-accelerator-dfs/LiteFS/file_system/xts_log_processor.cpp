#include "xts_log_processor.h"
#include "lite_log.h"
#include "rte_mbuf_core.h"
#include "rte_memcpy.h"
#include <cstdint>
#include <doca_log.h>
#include <string.h>
#include <rte_mbuf.h>
#include <cassert>
#include <algorithm>

#define XTS_128_KEY_SIZE 16
#define XTS_DATA_UNIT_SIZE 4096
#define XTS_PER_OP_SIZE (XTS_DATA_UNIT_SIZE * 7)

DOCA_LOG_REGISTER(XTS_LOG_PROCESSOR);

static constexpr size_t kIvOffset = sizeof(struct rte_crypto_op) + sizeof(struct rte_crypto_sym_op);
static constexpr size_t kPrivateDataOffset = kIvOffset + XTS_128_KEY_SIZE;

// Helper: Calculate XTS tweak: (offset / 4096)
static inline void xts_tweak_advance(uint8_t tweak[16], uint32_t n) {
    if((n & (XTS_DATA_UNIT_SIZE - 1)) != 0) {
        DOCA_LOG_WARN("XTS tweak advance is not aligned to data unit boundary: %u", n);
        exit(1);
    }
    uint64_t advance_blocks = n / XTS_DATA_UNIT_SIZE;
    // it is actually a big endian addition to the tweak
    uint8_t carry = 0;
    for (size_t i = 0; i < 8; i++) {
        uint16_t sum = (uint16_t)tweak[15 - i] + (uint16_t)((advance_blocks >> (8 * i)) & 0xFF) + carry;
        tweak[15 - i] = sum & 0xFF;
        carry = sum >> 8;
    }
    tweak[15 - 8] += carry;
}

// Helper: initialize tweak/iv for XTS (use inode and block index). Canonical XTS starts with tweak = sector_num || inode
static inline void xts_init_tweak(uint8_t tweak[16], uint32_t ino) {
    // XTS recommended: tweak = (inode << 32) | inode (big endian)
    *(uint64_t*)(tweak + 0) = __builtin_bswap64((uint64_t)ino << 32);
    *(uint64_t*)(tweak + 8) = __builtin_bswap64((uint64_t)ino << 32);
}

XtsLogProcessor::XtsLogProcessor(AesXtsDevice* xts_dev, uint16_t num_workers)
    : xts_dev_(xts_dev), 
      thread_states_(), work_queues_(num_workers), completion_queues_(num_workers),
      next_worker_(0), session_pool_(nullptr) {
    
    num_workers_ = num_workers;

    // Create shared session pool
    session_pool_ = xts_dev_->create_session_pool("xts_sess_pool", 1024, 128);
    if (session_pool_ == nullptr) {
        DOCA_LOG_ERR("Failed to create session pool");
        exit(1);
    }

    // Create op and mbuf pools
    crypto_op_pool_ = xts_dev_->create_crypto_op_pool("xts_ops_pool", 2048, 128, sizeof(PendingUnit));
    if (crypto_op_pool_ == nullptr) {
        DOCA_LOG_ERR("Failed to create crypto op pool");
        exit(1);
    }
    
    mbuf_pool_ = rte_pktmbuf_pool_create("xts_mbuf_pool", 4096, 256, 128, 32768, xts_dev_->socket_id());
    if (mbuf_pool_ == nullptr) {
        DOCA_LOG_ERR("Failed to create mbuf pool");
        exit(1);
    }
    
    // Initialize per-thread state
    thread_states_.resize(num_workers_);
    work_queues_.resize(num_workers_);
    
    for (uint16_t i = 0; i < num_workers_; i++) {
        auto& state = thread_states_[i];
        state.queue_pair_id = i;
        state.inflight_units_count = 0;  // Initialize count

        // Create shared session (same session for all threads)
        const uint8_t temp_key[XTS_128_KEY_SIZE * 2] = {0}; // also include the tweak key
        state.session_ = xts_dev_->create_cipher_session(
            session_pool_,
            temp_key,
            XTS_128_KEY_SIZE * 2, // include the tweak key
            XTS_DATA_UNIT_SIZE,
            true);
        if (state.session_ == nullptr) {
            DOCA_LOG_ERR("Failed to create cipher session");
            exit(1);
        }
        
        // Create work queue (power of 2 capacity)
        work_queues_[i].reset(new SpscRing<WriteWork>(16));
        state.work_queue_ = work_queues_[i].get();

        // Create completion queue (power of 2 capacity)
        completion_queues_[i].reset(new SpscRing<uint64_t>(16));
        state.completion_queue_ = completion_queues_[i].get();
    }
    
    DOCA_LOG_INFO("XtsLogProcessor initialized with %u workers", num_workers_);
}

XtsLogProcessor::~XtsLogProcessor() {
    // Cleanup sessions (each thread has its own session)
    for (auto& state : thread_states_) {
        if (state.session_ != nullptr) {
            rte_cryptodev_sym_session_free(xts_dev_->get_dev_id(), state.session_);
        }
    }
    
    // Cleanup pools
    if (session_pool_ != nullptr) {
        rte_mempool_free(session_pool_);
        session_pool_ = nullptr;
    }
    
    if (crypto_op_pool_ != nullptr) {
        rte_mempool_free(crypto_op_pool_);
    }
    if (mbuf_pool_ != nullptr) {
        rte_mempool_free(mbuf_pool_);
    }
}

bool XtsLogProcessor::distribute_write_work(const void* src_header, void* dst_header,
                                            uint64_t submission_id) {
    WriteWork work = {
        .src_header = src_header,
        .dst_header = dst_header,
        .submission_id = submission_id
    };
    
    // Round-robin distribution (including thread 0)
    for (uint16_t i = 0; i < num_workers_; i++) {

        uint16_t worker_id = next_worker_;
        next_worker_ = (next_worker_ + 1) % num_workers_;
        
        // Try to push to selected worker's queue
        if (work_queues_[worker_id]->try_push(work)) {
            return true;  // Successfully distributed
        }
    }
    
    return false; // Failed to distribute
}

bool XtsLogProcessor::process_worker_queue(uint16_t thread_id) {
    auto& state = thread_states_[thread_id];
    WriteWork work;
    bool progress_made = false;
    // Drain work from this thread's queue and process it
    while (state.waiting_units_.size() < 64 && state.work_queue_->try_pop(work)) {
        process_write_work(thread_id, work);
        progress_made = true;
    }
    
    // Submit ops from waiting queue to hardware (does not poll)
    size_t enqueued_count = submit_from_waiting_queue(thread_id);
    if (enqueued_count > 0) {
        progress_made = true;
    }
    return progress_made;
}

void XtsLogProcessor::process_write_work(uint16_t thread_id, const WriteWork& work) {
    auto& state = thread_states_[thread_id];
    
    // Extract write log from header
    const lite_log_write* write_log = reinterpret_cast<const lite_log_write*>(
        reinterpret_cast<const uint8_t*>(work.src_header) + sizeof(lite_log_header));
    
    uint32_t inode = write_log->ino;
    uint64_t file_offset = write_log->offset;
    size_t data_length = write_log->length;
    
    // Get src/dst data pointers
    const uint8_t* src_data = reinterpret_cast<const uint8_t*>(work.src_header) + 
                               sizeof(lite_log_header) + sizeof(lite_log_write);
    uint8_t* dst_data = reinterpret_cast<uint8_t*>(work.dst_header) + 
                        sizeof(lite_log_header) + sizeof(lite_log_write);
    
    // Copy header and write_log struct (unencrypted)
    memcpy(work.dst_header, work.src_header, sizeof(lite_log_header) + sizeof(lite_log_write));
    
    // Initialize tweak
    uint8_t tweak[16];
    xts_init_tweak(tweak, inode);
    size_t first_unit_skew = file_offset % XTS_DATA_UNIT_SIZE;
    xts_tweak_advance(tweak, file_offset - first_unit_skew);
    
    size_t remaining = data_length;
    uint32_t op_count = 0;
    
    // Calculate total ops needed accounting for potential splits
    uint32_t total_ops_needed = 0;
    size_t temp_remaining = remaining;
    size_t temp_skew = first_unit_skew;
    while(temp_remaining > 0) {
        auto padded_remaining = temp_remaining + temp_skew;
        size_t op_len = (padded_remaining < XTS_PER_OP_SIZE) ? padded_remaining : XTS_PER_OP_SIZE;
        size_t copy_len = op_len - temp_skew;
        temp_remaining -= copy_len;
        temp_skew = 0;
        total_ops_needed++;
    }

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
         exit(1);
    }

    for (uint32_t i = 0; i < total_ops_needed; ++i) {
        // For each op, always start at aligned boundary, never cross unit boundaries
        auto padded_remaining = remaining + first_unit_skew;
        size_t op_len = (padded_remaining < XTS_PER_OP_SIZE) ? padded_remaining : XTS_PER_OP_SIZE;
        
        struct rte_crypto_op* op = ops_bulk[i];
        struct rte_mbuf* src_buf = mbufs_bulk[2 * i];
        struct rte_mbuf* dst_buf = mbufs_bulk[2 * i + 1];
        
        // Copy data to source mbuf
        rte_pktmbuf_reset(src_buf);
        rte_pktmbuf_append(src_buf, op_len);
        uint8_t* src_mbuf_ptr = rte_pktmbuf_mtod_offset(src_buf, uint8_t*, first_unit_skew);
        size_t copy_len = op_len - first_unit_skew;
        rte_memcpy(src_mbuf_ptr, src_data, copy_len);
        
        // Prep the destination mbuf
        rte_pktmbuf_reset(dst_buf);
        rte_pktmbuf_append(dst_buf, op_len);
        
        // Prep the crypto op
        op->sym->m_src = src_buf;
        op->sym->m_dst = dst_buf;
        op->sym->cipher.data.offset = 0;
        op->sym->cipher.data.length = op_len;
        
        // Setup tweak
        memcpy(rte_crypto_op_ctod_offset(op, uint8_t*, kIvOffset), tweak, XTS_128_KEY_SIZE);

        // Store in op's private data
        *(PendingUnit*)(rte_crypto_op_ctod_offset(op, uint8_t*, kPrivateDataOffset)) = {
            .submission_id = work.submission_id,
            .dest_base = dst_data,
            .dest_offset = first_unit_skew,
            .unit_len = copy_len,
        };

        // Attach session and push to waiting queue
        rte_crypto_op_attach_sym_session(op, state.session_);
        state.waiting_units_.push_back(op);
        op_count++;
        
        // Update state
        remaining -= copy_len;
        file_offset += copy_len;
        dst_data += copy_len;
        src_data += copy_len;
        first_unit_skew = 0;
        
        // Advance tweak
        if (remaining > 0) {
            xts_tweak_advance(tweak, op_len);
        }
    }

    // Track this WriteWork (for completion counting)
    state.active_works_[work.submission_id] = {.total_ops = op_count, .completed_ops = 0};
    
}

size_t XtsLogProcessor::submit_from_waiting_queue(uint16_t thread_id) {
    auto& state = thread_states_[thread_id];
    size_t inflight_limit = CRYPTO_DEFAULT_NB_DESCRIPTORS;
    size_t submittable_count = std::min(inflight_limit - state.inflight_units_count, state.waiting_units_.size());
    if (submittable_count == 0) {
        return 0;
    }
    
    struct rte_crypto_op *ops[CRYPTO_DEFAULT_NB_DESCRIPTORS];
    auto it = state.waiting_units_.cbegin();
    for (size_t i = 0; i < submittable_count; i++) {
        if (it == state.waiting_units_.cend()) break;
        ops[i] = *it++;
    }
    
    size_t enqueued_count = 0;
    size_t burst_size = 64;
    for (size_t offset = 0; offset < submittable_count; offset += burst_size) {
        size_t current_burst_size = std::min(burst_size, submittable_count - offset);
        size_t current_enqueued = rte_cryptodev_enqueue_burst(
            xts_dev_->get_dev_id(), state.queue_pair_id, ops + offset, current_burst_size);
        enqueued_count += current_enqueued;
        state.inflight_units_count += current_enqueued;
        if (current_enqueued < current_burst_size) {
            DOCA_LOG_WARN("Partial enqueue: requested %zu, got %zu", current_burst_size, current_enqueued);
            break; // Exit if unable to submit the full burst
        }
    }
    
    // Move successfully enqueued units to inflight
    for (size_t i = 0; i < enqueued_count; i++) {
        state.waiting_units_.pop_front();
    }
    
    return enqueued_count;
}

bool XtsLogProcessor::worker_poll(uint16_t thread_id) {
    auto& state = thread_states_[thread_id];
    struct rte_crypto_op* completed_op[64];
    bool progress_made = false;
    
    size_t dequeued = rte_cryptodev_dequeue_burst(
        xts_dev_->get_dev_id(), 
        state.queue_pair_id, 
        completed_op, 
        64);
    
    if (dequeued > 0) progress_made = true;

    struct rte_mbuf* mbufs_to_free[64 * 2]; // src and dst per op
    unsigned int mbufs_count = 0;

    for (size_t i = 0; i < dequeued; i++) {
        // Extract per-thread op ID from private data
        PendingUnit& pending_unit = *(PendingUnit*)(rte_crypto_op_ctod_offset(completed_op[i], uint8_t*, kPrivateDataOffset));
        uint64_t submission_id = pending_unit.submission_id;
        
        if (completed_op[i]->status != RTE_CRYPTO_OP_STATUS_SUCCESS) {
            DOCA_LOG_ERR("Crypto op %lu failed with status %d", submission_id, completed_op[i]->status);
        }
        
        // Copy result
        memcpy(pending_unit.dest_base, 
                rte_pktmbuf_mtod_offset(completed_op[i]->sym->m_dst, uint8_t*, pending_unit.dest_offset), 
                pending_unit.unit_len);
        
        mbufs_to_free[mbufs_count++] = completed_op[i]->sym->m_src;
        mbufs_to_free[mbufs_count++] = completed_op[i]->sym->m_dst;
        
        state.inflight_units_count--;
        
        // Update completion tracking for this WriteWork
        auto work_it = state.active_works_.find(submission_id);
        if (work_it != state.active_works_.end()) {
            auto& work_status = work_it->second;
            work_status.completed_ops++;
        } else {
            DOCA_LOG_ERR("Completion for unknown submission_id %lu", submission_id);
            exit(1);
        }
    }
    
    if (dequeued > 0) {
        // Bulk free mbufs
        rte_pktmbuf_free_bulk(mbufs_to_free, mbufs_count);
        // Bulk free ops
        rte_mempool_put_bulk(crypto_op_pool_, (void**)completed_op, dequeued);
    }

    // Check and send completed WriteWorks
    for (auto it = state.active_works_.begin(); it != state.active_works_.end(); ) {
        auto& work_status = it->second;
        auto it2 = it;
        it++;
        if (work_status.completed_ops >= work_status.total_ops) {
            // All ops done, send completion notification
            if(completion_queues_[thread_id]->try_push(it2->first)) {
                state.active_works_.erase(it2);
                progress_made = true;
            }
        }
    }

    return progress_made;
}

size_t XtsLogProcessor::submit_chunk(const void* src_chunk, size_t src_chunk_size) {
    assert(!buffer_queue_.empty());
    auto& current_buffer = buffer_queue_.back();
    if (current_buffer.reserved + src_chunk_size > current_buffer.capacity) {
        DOCA_LOG_ERR("Not enough space in buffer to process chunk (reserved: %zu, capacity: %zu, chunk_size: %zu)",
                     current_buffer.reserved, current_buffer.capacity, src_chunk_size);
        exit(1);
    }
    const uint8_t* src_ptr = static_cast<const uint8_t*>(src_chunk);
    size_t src_offset = 0;

    while (src_offset < src_chunk_size) {
        void* dest_ptr = static_cast<uint8_t*>(current_buffer.dest_buffer) + current_buffer.reserved;
        const auto* src_header = reinterpret_cast<const lite_log_header*>(src_ptr + src_offset);
        if (src_header->entry_size < sizeof(lite_log_header)) break;
        if (current_buffer.reserved + src_header->entry_size > current_buffer.capacity) {
            DOCA_LOG_WARN("Chunk exceeds buffer capacity, stopping at offset %zu", src_offset);
            break;
        }
        
        if (src_header->op_type == LITE_LOG_OP_WRITE) {
            
            // Allocate submission_id for this WriteWork
            uint64_t submission_id = next_async_id_++;
            
            // Get pointers to headers
            const void* src_hdr = src_header;
            
            if(!distribute_write_work(src_hdr, dest_ptr, submission_id)) {
                break;
            }
            
            // Advance offsets (we advance even if distribution failed, to avoid infinite loops)
            src_offset += src_header->entry_size;
            current_buffer.reserved += src_header->entry_size;
            dest_ptr = static_cast<uint8_t*>(dest_ptr) + src_header->entry_size;
            dest_buffer_reservations_.emplace_back(DestBufferReservation{submission_id, current_buffer.dest_buffer, current_buffer.reserved});
        } else {
            // Non-WRITE: memcpy directly (main thread)
            memcpy(dest_ptr, src_ptr + src_offset, src_header->entry_size);
            src_offset += src_header->entry_size;
            if (current_buffer.written == current_buffer.reserved) {
                current_buffer.written += src_header->entry_size;
            } else {
                // we are writing behind a reserved region
                // update the ending offset of the last reservation to include the current write since we are done already. 
                assert(!dest_buffer_reservations_.empty());
                assert(dest_buffer_reservations_.back().dest_buffer == current_buffer.dest_buffer);
                dest_buffer_reservations_.back().ending_offset += src_header->entry_size;
            }
            current_buffer.reserved += src_header->entry_size;
        }
    }
    return src_offset;
}

bool XtsLogProcessor::poll_ready() {
    bool progress_made = worker_poll(0);           // do secondary worker - worker 0
    
    // 2. Collect completion notifications from all workers
    uint64_t submission_id;
    for (uint16_t i = 0; i < num_workers_; i++) {
        while (completion_queues_[i]->try_pop(submission_id)) {
            completed_submissions_.insert(submission_id);
            progress_made = true;
        }
    }
    
    while (!completed_submissions_.empty()) {
        auto it = completed_submissions_.begin();
        if (*it != completed_count_) {
            break;
        }

        // Update the buffer info with the completed submission
        assert(!dest_buffer_reservations_.empty());
        DestBufferReservation& reservation = dest_buffer_reservations_.front();
        assert(reservation.submission_id == *it);
        bool found = false;
        for (auto& bufferInfo : buffer_queue_) {
            if (bufferInfo.dest_buffer == reservation.dest_buffer) {
                bufferInfo.written = reservation.ending_offset;
                found = true;
                break;
            }
        }
        if (!found) {
            DOCA_LOG_ERR("Completed submission_id %lu not found in buffer_queue_", *it);
            exit(1);
        }
        dest_buffer_reservations_.pop_front();

        completed_count_++;
        completed_submissions_.erase(it);
    }

    if (!progress_made) {
        progress_made = process_worker_queue(0);
    }
    
    // 3. Return number of new completions
    return progress_made;
}