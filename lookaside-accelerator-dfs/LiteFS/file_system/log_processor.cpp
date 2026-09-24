#include "log_processor.h"
#include <string.h>
#include <rte_mbuf.h>
#include "doca_log.h"

DOCA_LOG_REGISTER(LITEFS_LOG_PROCESSOR);

// --- MemcpyLogProcessor Implementation ---

size_t MemcpyLogProcessor::submit_chunk(const void* src_chunk, size_t src_chunk_size) {
    assert(!buffer_queue_.empty());
    auto& current_buffer = buffer_queue_.back();
    assert(current_buffer.written + src_chunk_size <= current_buffer.capacity);

    void* dest_ptr = static_cast<uint8_t*>(current_buffer.dest_buffer) + current_buffer.written;
    memcpy(dest_ptr, src_chunk, src_chunk_size);
    
    current_buffer.written += src_chunk_size;
    current_buffer.reserved += src_chunk_size;
    return src_chunk_size; // accepted full input
}

bool MemcpyLogProcessor::poll_ready() {
    return false;
}

bool MemcpyLogProcessor::process_worker_queue(uint16_t thread_id __attribute__((unused))) {
    DOCA_LOG_ERR("MemcpyLogProcessor does not support multi-thread processing");
    return false;
}

bool MemcpyLogProcessor::worker_poll(uint16_t thread_id __attribute__((unused))) {
    DOCA_LOG_ERR("MemcpyLogProcessor does not support multi-thread processing");
    return false;
}

// XTS implementation moved to xts_log_processor.cpp
