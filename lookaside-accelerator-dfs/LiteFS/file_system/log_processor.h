#ifndef LFS_LOG_PROCESSOR_H
#define LFS_LOG_PROCESSOR_H

#include <cstdint>
#include <cstddef>
#include <deque>
#include <cassert>

/**
 * @brief Base class for log processors that manages buffer queues and async operations.
 * Provides the infrastructure for async processing with buffer management.
 */
class LogProcessor {
public:
    virtual ~LogProcessor() = default;

    /**
     * @brief Estimate the maximum number of bytes produced after processing an input
     *        chunk of the given size. Default assumes size-preserving.
     */
    virtual size_t estimate_processed_size(size_t src_chunk_size) const = 0;

    /**
     * @brief Provide the destination buffer where processed output will be written.
     *        The processor may append to this buffer across multiple submissions.
     */
    void set_output_buffer(void* dest_buffer, size_t dest_capacity) {
        if (!buffer_queue_.empty() && buffer_queue_.back().dest_buffer == dest_buffer) {
            return;
        }
        // Set up new buffer
        buffer_queue_.emplace_back(BufferInfo{dest_buffer, dest_capacity, 0, 0});
    }

    /**
     * @brief Submit a chunk for processing. Returns the number of bytes accepted
     *        for processing (may be less than input if capacity is limited).
     *        Completion will be reported via poll_ready().
     *        if creates a new async operation, it will update the submitted count.
     */
    virtual size_t submit_chunk(const void* src_chunk, size_t src_chunk_size) = 0;

    /**
     * @brief Returns the count of completed items ready to be consumed.
     *        Results are assumed to come back in order, so this returns
     *        the count of consecutive completed items starting from the first.
     * @return True if works are done, false otherwise.
     */
    virtual bool poll_ready() = 0;

    /**
     * @brief Process the worker queue for the given thread, for multi-thread log processors. 
     * @param thread_id The ID of the thread to process. 0 for the main thread. 
     * @return True if works are done, false otherwise.
     */
    virtual bool process_worker_queue(uint16_t thread_id) = 0;

    /**
     * @brief Poll the worker for the given thread, for multi-thread log processors.
     * @param thread_id The ID of the thread to poll. 0 for the main thread.
     * @return True if works are done, false otherwise.
     */
    virtual bool worker_poll(uint16_t thread_id) = 0;

    /**
     * @brief Returns the total count of submitted items.
     */
    uint64_t get_submitted_count() const {
        return next_async_id_;
    }

    uint64_t get_completed_count() const {
        return completed_count_;
    }

    uint16_t get_num_workers() const {
        return num_workers_;
    }

    struct BufferInfo {
        void* dest_buffer;
        size_t capacity;
        size_t written;
        size_t reserved; // for multi-thread processing, the number of bytes reserved for async operations (including written)
    };

    size_t get_front_buffer_written() const {
        assert(!buffer_queue_.empty());
        return buffer_queue_.front().written;
    }

    bool has_buffers() const {
        return !buffer_queue_.empty();
    }

    void* get_current_output_buffer() const {
        assert(!buffer_queue_.empty());
        return buffer_queue_.back().dest_buffer;
    }

    /**
     * @brief Pops the oldest completed batch and returns the buffer info.
     *        Returns nullptr if no batches are ready to be popped.
     */
    BufferInfo pop_completed_buffer() {
        assert(!buffer_queue_.empty());
        assert(buffer_queue_.front().reserved == buffer_queue_.front().written); // all reserved bytes should be written
        auto info = buffer_queue_.front();
        buffer_queue_.pop_front();
        return info;
    }

protected:

    std::deque<BufferInfo> buffer_queue_;
    uint64_t next_async_id_ = 0;
    uint64_t completed_count_ = 0;

    uint16_t num_workers_ = 1;
};

/**
 * @brief A simple LogProcessor that performs memcpy (pass-through) processing.
 */
class MemcpyLogProcessor : public LogProcessor {
public:
    /**
     * @brief Implements simple memcpy processing.
     */
    size_t submit_chunk(const void* src_chunk, size_t src_chunk_size) override;
    bool poll_ready() override;
    size_t estimate_processed_size(size_t src_chunk_size) const override { return src_chunk_size; }

    bool process_worker_queue(uint16_t thread_id) override;
    bool worker_poll(uint16_t thread_id) override;
};

#endif // LFS_LOG_PROCESSOR_H
