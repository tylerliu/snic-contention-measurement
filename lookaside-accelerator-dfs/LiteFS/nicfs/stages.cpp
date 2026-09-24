#include "stages.h"
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstddef>
#include <iostream>
#include "file_system/lite_fs.h"
#include "doca_log.h"
#include <string.h>

DOCA_LOG_REGISTER(LITEFS_STAGES);

using std::size_t;

ReceiverStage::ReceiverStage(DoorbellListenerChannel& chan,
                               DmaUser& log_dma,
                               SpscRing<ChunkMsg>& out12,
                               SpscRing<BatchMsg>& out12meta,
                               SpscRing<uint64_t>& out21freeing,
                               size_t logAreaSize,
                               size_t chunkSize)
    : chan_(chan), log_dma_(log_dma), out12_(out12), out12meta_(out12meta), out21freeing_(out21freeing),
      log_area_size_(logAreaSize), chunk_size_(chunkSize) {}

bool ReceiverStage::tick() {
    bool done_work = false;

    // receive doorbell message and send sequence initialization messages
    DoorbellMsg d = {};
    while (chan_.poll(d)) {
        done_work = true;
        size_t pos = static_cast<size_t>(d.start);
        size_t need = static_cast<size_t>(d.length);
        if (need == 0) continue;
        void* batch_local_ptr = log_dma_.get_local_region_ptr()->alloc(need, LITE_DMA_DEFAULT_ALIGN);
        assert(batch_local_ptr != nullptr);
        size_t batch_local_off = log_dma_.get_local_region().get_offset(batch_local_ptr);
        SeqState st{d.seq, batch_local_ptr, batch_local_off, need, need, pos, batch_local_off};
        seq_states_.push_back(st);
        assert(out12meta_.try_push(BatchMsg{d.seq, batch_local_ptr, need}));
        local_buffers_[d.seq] = batch_local_ptr;
    }

    // process completed DMAs
    size_t completed_off = 0;
    while (log_dma_.dequeue_completed(completed_off) == DOCA_SUCCESS) {
        done_work = true;
        auto it = dma_inflight_chunk_.find(completed_off);
        assert (it != dma_inflight_chunk_.end());
        it->second.completed = true;
    }

    // send chunk messages to next stage in order
    while (!dma_reorder_queue_.empty() && 
            dma_inflight_chunk_.find(dma_reorder_queue_.front()) != dma_inflight_chunk_.end() &&
            dma_inflight_chunk_.find(dma_reorder_queue_.front())->second.completed) {
        done_work = true;
        size_t off = dma_reorder_queue_.front();
        dma_reorder_queue_.pop_front();
        auto it = dma_inflight_chunk_.find(off);
        auto &chunk = it->second;
        assert(out12_.try_push(ChunkMsg{chunk.seq, chunk.chunk_ptr, chunk.chunk_len, chunk.lastInBatch}));
        dma_inflight_chunk_.erase(it);
    }

    // Schedule as many chunk DMAs as possible within inflight limits
    while (!seq_states_.empty() && log_dma_.get_inflight_count() < LITEFS_DMA_TASK_POOL_SIZE) {
        auto &st = seq_states_.front();
        done_work = true;
            size_t chunk = (st.remaining > 2 * chunk_size_) ? chunk_size_ : st.remaining;
            if (chunk == 0) { seq_states_.pop_front(); continue; }
        // since batch is stored contiguously in remote ring, submit a single DMA per chunk
        (void)log_dma_.submit_copy_from_remote(st.ringPos, st.localPos, chunk);
        dma_inflight_chunk_.emplace(st.localPos, 
            DmaInflightChunk{chunk, st.seq, false, (uint8_t*)log_dma_.get_local_region().get_base() + st.localPos, chunk == st.remaining});
        dma_reorder_queue_.push_back(st.localPos);
        st.ringPos = (st.ringPos + chunk) % log_area_size_;
        st.localPos += chunk;
        st.remaining -= chunk;
        if (st.remaining == 0) {
            seq_states_.pop_front();
        }
    }

    // receive freeing messages, then free the local buffer
    uint64_t freeing_seq;
    while (out21freeing_.try_pop(freeing_seq)) {
        done_work = true;
        auto it = local_buffers_.find(freeing_seq);
        assert(it != local_buffers_.end());
        void* local_ptr = it->second;
        local_buffers_.erase(it);
        log_dma_.get_local_region_ptr()->free(local_ptr);
    }

    return done_work;
}

LogProcessorStage::LogProcessorStage(LogProcessor& log_processor,
                                 SpscRing<ChunkMsg>& in12,
                                 SpscRing<BatchMsg>& in12meta,
                                 SpscRing<uint64_t>& in21freeing,
                                 SpscRing<ChunkMsg>& out23,
                                 SpscRing<ChunkMsg>& out24,
                                 SpscRing<BatchMsg>& procBufferFreeingRing,
                                 std::shared_ptr<LocalRegionAllocator> shared_allocator)
    : log_processor_(log_processor)
    , in12_(in12)
    , in12meta_(in12meta)
    , in21freeing_(in21freeing)
    , out23_(out23)
    , out24_(out24)
    , procBufferFreeingRing_(procBufferFreeingRing)
    , shared_allocator_(std::move(shared_allocator))
    {}

void LogProcessorStage::finish_batch() {
    assert(!proc_buffers_.empty());
    auto &proc_buffer = proc_buffers_.front();
    // Finalize buffer lifecycle; all produced bytes should have been forwarded already
    (void)log_processor_.pop_completed_buffer();
    assert(in21freeing_.try_push(proc_buffer.seq));
    assert(procBufferFreeingRing_.try_push(BatchMsg{proc_buffer.seq, proc_buffer.procBufferPtr, proc_buffer.procBufferLen}));
    proc_buffers_.pop_front();
}

void LogProcessorStage::push_available_chunks() {
    if (!proc_buffers_.empty() && log_processor_.has_buffers()) {
        auto &front_buffer = proc_buffers_.front();
        size_t new_written = log_processor_.get_front_buffer_written();
        if (front_buffer.writtenLen < new_written) {
            size_t delta = new_written - front_buffer.writtenLen;
            bool is_last = (new_written == front_buffer.inputLen);
            assert(out23_.try_push(ChunkMsg{front_buffer.seq, (uint8_t*)front_buffer.procBufferPtr + front_buffer.writtenLen, delta, is_last}));
            assert(out24_.try_push(ChunkMsg{front_buffer.seq, (uint8_t*)front_buffer.procBufferPtr + front_buffer.writtenLen, delta, is_last}));
            front_buffer.writtenLen = new_written;
        }
    }
}

void LogProcessorStage::handle_new_batch_metadata() {    // Handle new batch metadata
    BatchMsg sm;
    while (in12meta_.try_pop(sm)) {
        // Estimate output size and allocate processing buffer
        size_t estimated_size = log_processor_.estimate_processed_size(sm.len);
        auto procBuffer = shared_allocator_->alloc(estimated_size, LITE_DMA_DEFAULT_ALIGN);
        assert(procBuffer != nullptr);
        proc_buffers_.push_back(ProcBuffer{sm.seq, sm.localPtr, procBuffer, estimated_size, sm.len, 0, 0, false, 0, 0});
        DOCA_LOG_DBG("LogProcessorStage: Received batch metadata seq=%lu len=%zu estimated_size=%zu", sm.seq, sm.len, estimated_size);
    }
}

bool LogProcessorStage::tick() {
    bool done_work = false;

    // Handle completed batches
    uint64_t waitId = log_processor_.get_completed_count();
    while (log_processor_.poll_ready()) {
        done_work = true;
        waitId = log_processor_.get_completed_count();
    }
    while (!proc_buffers_.empty()) {
        auto &proc_buffer = proc_buffers_.front();
        if (proc_buffer.submittedLen < proc_buffer.inputLen) {
            break;
        }
        if (proc_buffer.hasWait && proc_buffer.waitId > waitId) {
            break;
        }
        DOCA_LOG_DBG("LogProcessorStage: finishing batch %lu, its waitId %lu, current waitId %lu", proc_buffer.seq, proc_buffer.waitId, waitId);
        push_available_chunks();
        finish_batch();
    }

    // Process incoming chunks
    ChunkMsg m;
    while (in12_.try_pop(m)) {
        done_work = true;
        bool found = false;
        if (proc_buffers_.empty() || proc_buffers_.back().seq < m.seq) {
            handle_new_batch_metadata();
        }
        for (auto it = proc_buffers_.rbegin(); it != proc_buffers_.rend(); it++) {
            if (it->seq == m.seq) {
                found = true;
                assert(m.localPtr == (uint8_t*)it->inputBufferPtr + it->receivedLen);
                it->receivedLen += m.len;
                break;
            }
        }
        assert(found);
    }

    // submit chunks to log processor if possible
    auto it = proc_buffers_.begin();
    while (it != proc_buffers_.end() && it-> submittedLen == it->inputLen) { // waiting log buffer to complete
        it++;
    } 
    while (it != proc_buffers_.end() && it->submittedLen < it->receivedLen) {
        done_work = true;
        if (it->submittedLen == 0) {
            log_processor_.set_output_buffer(it->procBufferPtr, it->procBufferLen);
        }
        assert(log_processor_.get_current_output_buffer() == it->procBufferPtr);
        auto startWaitId = log_processor_.get_submitted_count();
        auto processed = log_processor_.submit_chunk((uint8_t*)it->inputBufferPtr + it->submittedLen, it->receivedLen - it->submittedLen);
        auto endWaitId = log_processor_.get_submitted_count();

        DOCA_LOG_DBG("LogProcessorStage: submitted count %lu -> %lu for seq %lu, processed %zu", startWaitId, endWaitId, it->seq, processed);

        it->submittedLen += processed;
        if (startWaitId != endWaitId) {
            it->hasWait = true;
            it->waitId = endWaitId;
        }
        if (it->submittedLen == it->inputLen) {
            // next buffer can start
            it++;
        } else {
            break;
        }
    }

    push_available_chunks();

    return done_work;
}




ReplicationStage::ReplicationStage(VerbsManager* send_verbs,
        DoorbellRingerChannel* send_chan,
        std::vector<DoorbellRingerChannel*>& ack_chans,
        SpscRing<ChunkMsg>& in24,
        SpscRing<BatchMsg>& out45,
        size_t remote_region_size_bytes,
        size_t chunk_size_bytes,
        std::shared_ptr<LocalRegionAllocator> shared_allocator)
    : send_verbs_(send_verbs)
    , send_chan_(send_chan)
    , ack_chans_(ack_chans)
    , in24_(in24)
    , out45_(out45)
    , remote_region_size_(remote_region_size_bytes)
    , remote_write_head_(0)
    , chunk_size_(chunk_size_bytes) {
        assert(remote_region_size_bytes > 0);
        
        // Register the entire shared region once for efficient memory management
        if (send_verbs_ != nullptr) {
            shared_mr_ = send_verbs_->RegisterMemory(shared_allocator->get_base(), shared_allocator->get_length());
            assert(shared_mr_ != nullptr);
        }
    }

ReplicationStage::~ReplicationStage() {
    if (send_verbs_ != nullptr) {
        send_verbs_->DeregisterMemory(shared_mr_);
    }
}

bool ReplicationStage::tick() {
    bool done_work = false;
    // Drain chunk inputs
    ChunkMsg chunk;
    while (in24_.try_pop(chunk)) {
        done_work = true;
        if (send_verbs_ != nullptr && send_chan_ != nullptr) {
            // Special allocator: contiguous per-batch; prefer start=0 if write head >= half of region
            uint64_t remote_offset = 0;
            if (!pending_.empty() && pending_.back().seq == chunk.seq) {
                remote_offset = remote_write_head_;
            } else {
                assert(pending_.empty() || pending_.back().seq < chunk.seq);
                if (remote_write_head_ >= (remote_region_size_ >> 1) && (pending_.empty() || pending_.front().remote_start_offset > 0)) {
                    remote_write_head_ = 0;
                }
                remote_offset = remote_write_head_;
                pending_.push_back(PendingBatch{chunk.seq, remote_offset, 0, false, 0});
            }
            auto& back = pending_.back();

            // Segment the incoming chunk into kChunkSize pieces (last can be up to 2x kChunkSize)
            size_t remaining = chunk.len;
            size_t local_off = 0;
            while (remaining > 0) {
                size_t seg = 0;
                if (remaining > 2 * chunk_size_) {
                    seg = chunk_size_;
                } else {
                    seg = remaining; // allow last up to 2x chunk_size_
                }

                // Bounds check against remote region or wrap boundary of pending_ front
                assert(remote_offset == back.remote_start_offset + back.sentLen);
                if (back.remote_start_offset < pending_.front().remote_start_offset) {
                    assert(remote_offset + seg <= pending_.front().remote_start_offset);
                } else {
                    assert(remote_offset + seg <= remote_region_size_);
                }

                void* seg_ptr = (uint8_t*)chunk.localPtr + local_off;
                send_verbs_->PostWrite(shared_mr_, seg_ptr, seg, send_verbs_->GetRemoteConnData(), remote_offset);
                bool is_last_segment = (seg == remaining);
                send_chan_->send_doorbell(chunk.seq, remote_offset, seg, is_last_segment && chunk.lastInBatch);

                // advance
                back.sentLen += seg;
                remote_write_head_ = remote_offset + seg;
                remote_offset += seg;
                local_off += seg;
                remaining -= seg;
            }

            if (chunk.lastInBatch) {
                back.completed = true;
            }
        } else {
            // Track the batch
            if (pending_.empty() || pending_.back().seq != chunk.seq) {
                assert(pending_.empty() || pending_.back().seq < chunk.seq);
                pending_.push_back(PendingBatch{chunk.seq, 0, 0, false, 0});
            }
            auto &back = pending_.back();
            back.sentLen += chunk.len;
            if (chunk.lastInBatch) {
                back.completed = true;
            }
        }
    }

    // Do not poll CQ here to avoid racing with doorbell channel polls on the same CQ.
    // Write completion is implied by downstream ACK reception; we clean up MRs on ACK.

    // Send doorbell immediately after each chunk is sent (not waiting for batch completion)
    // This is handled in the chunk processing loop above

    // Poll for ACKs and forward completed batches to next stage
    AckMsg ack;
    for (auto ch : ack_chans_) {
        while (ch->poll_ack(ack)) {
            done_work = true;
            bool found = false;
            for (auto it = pending_.begin(); it != pending_.end(); it++) {
                if (it->seq == ack.seq) {
                    if (ack.length != it->sentLen) {
                        DOCA_LOG_ERR("Received ACK for seq=%lu with unexpected length %lu expected %lu", 
                                    ack.seq, ack.length, it->sentLen);
                        exit(1);
                    }
                    assert(it->completed);
                    it->ackedCount += 1;
                    found = true;
                    DOCA_LOG_DBG("Received replication ACK for seq=%lu ackedCount=%zu", ack.seq, it->ackedCount);
                    break;
                }
            }
            if (!found) {
                DOCA_LOG_ERR("Received ACK for unknown sequence: seq=%lu", ack.seq);
                assert(found);
            }
        }
    }
    
    // Forward completed batches (when all ACKs received and we have the last chunk)
    while (!pending_.empty()) {
        auto &p = pending_.front();
        if (p.ackedCount == ack_chans_.size()) {
            assert(out45_.try_push(BatchMsg{p.seq, nullptr, p.sentLen}));
            pending_.pop_front();
        } else {
            break;
        }
    }

    return done_work;
}

void ReplicationStage::drain_completions() {
    if (send_verbs_ == nullptr) return;
    struct ibv_wc wc = {};
    while (send_verbs_->PollCompletion(&wc) > 0) {
        if (wc.status != IBV_WC_SUCCESS) {
            std::cerr << "[ReplicationStage] CQE error status=" << wc.status
                      << " opcode=" << wc.opcode
                      << " wr_id=" << (void*)wc.wr_id
                      << " vendor_err=" << wc.vendor_err << std::endl;
        }
    }
}

// --- PersistenceStage ---
PersistenceStage::PersistenceStage(DmaUser& disk_dma,
        SpscRing<ChunkMsg>& in23,
        SpscRing<BatchMsg>& out35,
        size_t ring_region_size_bytes,
        size_t chunk_size_bytes,
        DoorbellRingerChannel& persist_ringer)
    : disk_dma_(disk_dma)
    , in23_(in23)
    , out35_(out35)
    , persist_ringer_(persist_ringer)
    , ring_region_size_(ring_region_size_bytes)
    , remote_write_head_(0)
    , chunk_size_(chunk_size_bytes) {
    auto local_region = disk_dma_.get_local_region_ptr();
}

bool PersistenceStage::tick() {
    bool done_work = false;

    // Drain chunk inputs and write into ring region at offset DISK_RING_HEADER_SIZE .. + ring size
    ChunkMsg chunk;
    while (in23_.try_pop(chunk)) {
        done_work = true;
        if (pending_.empty() || pending_.back().seq != chunk.seq) {
            pending_.push_back(PendingBatch{chunk.seq, chunk.localPtr, 0, 0, 0, 0, false});
        }
        auto &back = pending_.back();
        assert(!back.completed);
        back.receivedLen += chunk.len;
        if (chunk.lastInBatch) {
            back.completed = true;
        }
    }

    // Poll completions for inflight data copies and update completedLen
    size_t completed_off = 0;
    while (disk_dma_.dequeue_completed(completed_off) == DOCA_SUCCESS) {
        done_work = true;
        completed_offsets_.insert(completed_off);
    }
    while (!inflight_copies_.empty()) {
        auto &copy = inflight_copies_.front();
        if (completed_offsets_.count(copy.local_off)) {
            bool found = false;
            for (auto &p : pending_) {
                if (p.seq == copy.seq) {
                    p.completedLen += copy.len;
                    found = true;
                    bool last_segment = (p.completedLen == p.receivedLen) && p.completed;
                    persist_ringer_.send_doorbell(copy.seq, copy.remote_off, copy.len, last_segment);
                    break;
                }
            }
            assert(found);
            completed_offsets_.erase(copy.local_off);
            inflight_copies_.pop_front();
        } else {
            break;
        }
    }

    // Emit done when batch completed
    while (!pending_.empty()) {
        auto &p = pending_.front();
        if (p.completed && p.completedLen == p.receivedLen) {
            assert(out35_.try_push(BatchMsg{p.seq, nullptr, p.sentLen}));
            doorbell_batches_.push_back(DoorbellBatch{p.seq, p.remote_start_offset, p.sentLen});
            pending_.pop_front();
        } else {
            break;
        }
    }

    AckMsg ack;
    while (persist_ringer_.poll_ack(ack)) {
        done_work = true;
        assert(ack.magic == LITEFS_ACK_MAGIC);
        assert(ack.seq == doorbell_batches_.front().seq);
        doorbell_batches_.pop_front();
    }

    // Send DMA ops
    while (disk_dma_.get_inflight_count() < LITEFS_DMA_TASK_POOL_SIZE) {
        auto pending_it = pending_.begin();
        while (pending_it != pending_.end() && pending_it->sentLen == pending_it->receivedLen && pending_it->completed) {
            pending_it++; // nothing to send
        }
        if (pending_it == pending_.end() || pending_it->sentLen == pending_it->receivedLen) {
            break;
        }
        done_work = true;
        auto &p = *pending_it;
        assert(p.sentLen < p.receivedLen);
        if (p.sentLen == 0) {
            if (remote_write_head_ > (ring_region_size_ >> 1)) {
                if (!doorbell_batches_.empty() && doorbell_batches_.front().remote_start_offset == 0) {
                    DOCA_LOG_WARN("Try to wrap but start is 0 - Host may not update start correctly");
                    break;
                }
                remote_write_head_ = 0;
            }
            p.remote_start_offset = remote_write_head_;
            DOCA_LOG_DBG("PersistenceStage: Setting remote_start_offset for seq=%lu to %zu", p.seq, p.remote_start_offset);
        }
        auto send_size = std::min(p.receivedLen - p.sentLen, chunk_size_);
        auto src_offset = disk_dma_.get_local_region().get_offset((uint8_t*)p.local_ptr + p.sentLen);
        auto remote_offset = p.remote_start_offset + p.sentLen;
        if (p.remote_start_offset + p.sentLen + send_size > ring_region_size_) {
            DOCA_LOG_ERR("DMA trying to write beyond ring: seq=%lu remote_start_offset=%zu sentLen=%zu receivedLen=%zu", p.seq, p.remote_start_offset, p.sentLen, p.receivedLen);
            DOCA_LOG_ERR("remote_range=%zu-%zu, ", p.remote_start_offset + p.sentLen, p.remote_start_offset + p.sentLen + send_size);
            break;
            exit(1);
        } else if (!doorbell_batches_.empty()) {
            auto start = doorbell_batches_.front().remote_start_offset;
            if (p.remote_start_offset + p.sentLen < start && p.remote_start_offset + p.sentLen + send_size > start) {
                DOCA_LOG_DBG("DMA stop submitting as ring wrapped");
                break;
            }
        }

        (void)disk_dma_.submit_copy_to_remote(src_offset, remote_offset, send_size);
        inflight_copies_.push_back(InflightCopy{src_offset, remote_offset, send_size, p.seq});
        DOCA_LOG_DBG("PersistenceStage: Submitted copy to remote: seq=%lu src_offset=%zu send_size=%zu", p.seq, src_offset, send_size);
        p.sentLen += send_size;
        remote_write_head_ += send_size;
    }

    return done_work;
}

AckCleanupStage::AckCleanupStage(DoorbellListenerChannel& chan,
                         SpscRing<BatchMsg>& procBufferFreeingRing,
                         SpscRing<BatchMsg>& inPersistDone,
                         SpscRing<BatchMsg>& inRepDone,
                         std::shared_ptr<LocalRegionAllocator> shared_allocator)
    : chan_(chan)
    , procBufferFreeingRing_(procBufferFreeingRing)
    , inPersistDone_(inPersistDone)
    , inRepDone_(inRepDone)
    , shared_allocator_(std::move(shared_allocator)) {}

bool AckCleanupStage::tick() {
    bool done_work = false;
    // Track Persist-Done, Rep-Done and free/ACK strictly in order
    if (procPtr_ == nullptr) {
        BatchMsg bufferMsg;
        if (procBufferFreeingRing_.try_pop(bufferMsg)) {
            done_work = true;
            assert(bufferMsg.seq == waitingSeq_);
            procPtr_ = bufferMsg.localPtr;
            len_ = bufferMsg.len;
            DOCA_LOG_DBG("received procBufferFreeingRing batch seq=%lu len=%zu", bufferMsg.seq, bufferMsg.len);
        }
    }
    if (procPtr_ != nullptr) {
        BatchMsg msg;
        if (!persistDone_ && inPersistDone_.try_pop(msg)) {
            done_work = true;
            if (msg.seq != waitingSeq_) {
                DOCA_LOG_ERR("received Persist-Done batch with unexpected sequence seq=%lu expected=%lu", msg.seq, waitingSeq_);
                assert(msg.seq == waitingSeq_);
            }
            if (msg.len != len_) {
                DOCA_LOG_ERR("received Persist-Done batch with unexpected length seq=%lu len=%zu expected=%zu", msg.seq, msg.len, len_);
                assert(msg.len == len_);
            }
            persistDone_ = true;
            DOCA_LOG_DBG("received Persist-Done batch seq=%lu len=%zu", msg.seq, msg.len);
        }

        if (!repDone_ && inRepDone_.try_pop(msg)) {
            done_work = true;
            if (msg.seq != waitingSeq_) {
                DOCA_LOG_ERR("received batch with unexpected sequence seq=%lu expected=%lu", msg.seq, waitingSeq_);
                assert(msg.seq == waitingSeq_);
            }
            if (msg.len != len_) {
                DOCA_LOG_ERR("received batch with unexpected length seq=%lu len=%zu expected=%zu", msg.seq, msg.len, len_);
                assert(msg.len == len_);
            }
            repDone_ = true;
            DOCA_LOG_DBG("received Rep-Done batch seq=%lu len=%zu", msg.seq, msg.len);
        }
    }
    if (procPtr_ != nullptr && persistDone_ && repDone_) {
        shared_allocator_->free(procPtr_);
        DOCA_LOG_DBG("freed procBuffer seq=%lu len=%zu, ACKed seq=%lu", waitingSeq_ - 1, len_, waitingSeq_ - 1);
        procPtr_ = nullptr;
        len_ = 0;
        persistDone_ = false;
        repDone_ = false;
        chan_.ack(waitingSeq_);
        waitingSeq_++;
    }

    return done_work;
}