#ifndef LITEFS_STAGES_H
#define LITEFS_STAGES_H

#include <deque>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "common/ring_buffer.h"
#include "common/doorbell_channel.h"
#include "common/doca_dma.h"
#include "file_system/log_processor.h"
#include "common/local_region.h"

struct ChunkMsg {
    uint64_t seq;
    void* localPtr;
    size_t len;
    bool lastInBatch;
};

struct BatchMsg {
    uint64_t seq;
    void* localPtr;
    size_t len;
};

class ReceiverStage {
public:
    ReceiverStage(DoorbellListenerChannel& chan,
                   DmaUser& log_dma,
                   SpscRing<ChunkMsg>& out12,
                   SpscRing<BatchMsg>& out12meta,
                   SpscRing<uint64_t>& out21freeing,
                   size_t logAreaSize,
                   size_t chunkSize);

    // return true if things are done. (so pause on false) 
    bool tick(); // non-blocking

private:
    struct SeqState {
        uint64_t seq;
        void*  localBasePtr;
        size_t localBaseOff;
        size_t totalLen;
        size_t remaining;     // bytes left to schedule
        size_t ringPos;       // current remote ring offset (assumed contiguous within batch)
        size_t localPos;      // current local offset to place next chunk
    };
    struct DmaInflightChunk {
        size_t chunk_len;
        uint64_t seq;
        bool completed;
        void* chunk_ptr;
        bool lastInBatch;
    };
    DoorbellListenerChannel& chan_;
    DmaUser& log_dma_;
    SpscRing<ChunkMsg>& out12_;
    SpscRing<BatchMsg>& out12meta_;
    SpscRing<uint64_t>& out21freeing_;
    const size_t log_area_size_;
    const size_t chunk_size_;
    std::deque<SeqState> seq_states_; // base info
    std::unordered_map<size_t, DmaInflightChunk> dma_inflight_chunk_; // local off -> dma inflight chunk
    std::deque<size_t> dma_reorder_queue_; // local off -> dma reorder queue
    std::unordered_map<uint64_t, void*> local_buffers_; // seq -> local buffer
};

class LogProcessorStage {
public:
    LogProcessorStage(LogProcessor& log_processor,
                    SpscRing<ChunkMsg>& in12,
                    SpscRing<BatchMsg>& in12meta,
                    SpscRing<uint64_t>& in21freeing,
                    SpscRing<ChunkMsg>& out23,
                    SpscRing<ChunkMsg>& out24,
                    SpscRing<BatchMsg>& procBufferFreeingRing,
                    std::shared_ptr<LocalRegionAllocator> shared_allocator);

    bool tick();
    void finish_batch();
    void push_available_chunks();
    void handle_new_batch_metadata();
private:
    LogProcessor& log_processor_;
    SpscRing<ChunkMsg>& in12_;
    SpscRing<BatchMsg>& in12meta_;
    SpscRing<uint64_t>& in21freeing_;
    SpscRing<ChunkMsg>& out23_;
    SpscRing<ChunkMsg>& out24_;
    SpscRing<BatchMsg>& procBufferFreeingRing_;
    std::shared_ptr<LocalRegionAllocator> shared_allocator_;
    struct ProcBuffer {
        uint64_t seq;
        void* inputBufferPtr;
        void* procBufferPtr;
        size_t procBufferLen;
        size_t inputLen;
        size_t receivedLen;
        size_t submittedLen;
        bool hasWait;
        uint64_t waitId;
        size_t writtenLen;
    };
    std::deque<ProcBuffer> proc_buffers_;
};



// Optional replication stage: forwards processed batches to the first replica
// using RDMA WRITE and doorbell SEND/RECV for ACK. If no replica channel is
// provided, this stage acts as a pass-through.
class ReplicationStage {
public:
    ReplicationStage(VerbsManager* send_verbs,
        DoorbellRingerChannel* send_chan,
        std::vector<DoorbellRingerChannel*>& ack_chans,
        SpscRing<ChunkMsg>& in24,
        SpscRing<BatchMsg>& out45,
        size_t remote_region_size_bytes,
        size_t chunk_size_bytes,
        std::shared_ptr<LocalRegionAllocator> shared_allocator);
        
    ~ReplicationStage();

    bool tick();

private:
    struct PendingBatch { 
        uint64_t seq; 
        uint64_t remote_start_offset;
        size_t sentLen; 
        bool completed; 
        size_t ackedCount;
    };

    void drain_completions();

    VerbsManager* send_verbs_;
    DoorbellRingerChannel* send_chan_;
    std::vector<DoorbellRingerChannel*>& ack_chans_;
    SpscRing<ChunkMsg>& in24_;
    SpscRing<BatchMsg>& out45_;

    std::deque<PendingBatch> pending_;

    // Remote memory management (special allocator: contiguous per batch, two halves)
    size_t remote_region_size_ = 0;
    uint64_t remote_write_head_ = 0; // next preferred start for a new batch
    size_t chunk_size_ = 0; // segmentation unit
    
    // Shared memory region registration
    struct ibv_mr* shared_mr_ = nullptr; // single MR for the entire shared region
};

// PersistenceStage: DMA processed log chunks into host DiskArea ring buffer
// and update DiskRingHeader's end/cut fields. Emits a done message per batch
// for the AckCleanupStage to free buffers and ACK host.
class PersistenceStage {
public:
    PersistenceStage(DmaUser& disk_dma,
        SpscRing<ChunkMsg>& in23,
        SpscRing<BatchMsg>& out35,
        size_t ring_region_size_bytes,
        size_t chunk_size_bytes,
        DoorbellRingerChannel& persist_ringer);

    bool tick();

private:
    struct PendingBatch {
        uint64_t seq;
        void* local_ptr;
        uint64_t remote_start_offset; // within ring region
        size_t receivedLen;
        size_t sentLen;      // bytes submitted to DMA
        size_t completedLen; // bytes completed by DMA
        bool completed; // all chunks in batch have been received
    };

    struct InflightCopy {
        size_t local_off;
        size_t remote_off;
        size_t len;
        uint64_t seq;
    };

    struct DoorbellBatch {
        uint64_t seq;
        uint64_t remote_start_offset;
        size_t len;
    };

    // Helpers for DiskRingHeader
    void synchronize_start_from_remote();
    void write_endcut_to_remote(uint64_t end_val, uint64_t cut_val);

    DmaUser& disk_dma_;
    SpscRing<ChunkMsg>& in23_;
    SpscRing<BatchMsg>& out35_;
    DoorbellRingerChannel& persist_ringer_;

    std::deque<PendingBatch> pending_;
    std::deque<InflightCopy> inflight_copies_;
    std::deque<DoorbellBatch> doorbell_batches_;
    std::unordered_set<uint64_t> completed_offsets_;

    size_t ring_region_size_ = 0;
    uint64_t remote_write_head_ = 0; // next write position within ring region

    // Local staging area for header writes
    size_t chunk_size_ = 0;
};

class AckCleanupStage {
public:
    AckCleanupStage(DoorbellListenerChannel& chan,
                SpscRing<BatchMsg>& procBufferFreeingRing,
                SpscRing<BatchMsg>& inPersistDone,
                SpscRing<BatchMsg>& inRepDone,
                std::shared_ptr<LocalRegionAllocator> shared_allocator);

    bool tick();

private:
    DoorbellListenerChannel& chan_;
    SpscRing<BatchMsg>& procBufferFreeingRing_;
    SpscRing<BatchMsg>& inPersistDone_;
    SpscRing<BatchMsg>& inRepDone_;
    std::shared_ptr<LocalRegionAllocator> shared_allocator_;
    uint64_t waitingSeq_ = 1;
    void* procPtr_ = nullptr;
    size_t len_ = 0;
    bool persistDone_ = false;
    bool repDone_ = false;
};
#endif


