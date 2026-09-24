#ifndef LFS_LOG_GEN_H
#define LFS_LOG_GEN_H

#include <cstdint>
#include <cstddef>

#include "lite_log.h"

const int NUM_FILES_TO_GENERATE = 16;

/**
 * @brief Generates log sequences into a provided buffer, acting as a producer
 * for a ring buffer.
 */
class LogGenerator {
public:
    LogGenerator();
    ~LogGenerator();

    /**
     * @brief Fills the given buffer with logs for creating 16 files and writing 1MB to each.
     *
     * @param buffer A pointer to the start of the available contiguous memory.
     * @param capacity The available capacity in the buffer.
     * @return The total size in bytes of the generated log chunk.
     */
    size_t generate_create_phase(void* buffer, size_t capacity);

    /**
     * @brief Fills the given buffer with logs for rewriting 256KB at random offsets to each of the 16 files.
     *
     * @param buffer A pointer to the start of the available contiguous memory.
     * @param capacity The available capacity in the buffer.
     * @return The total size in bytes of the generated log chunk.
     */
    size_t generate_rewrite_phase(void* buffer, size_t capacity);

private:
    // Workload state
    uint32_t file_inodes[NUM_FILES_TO_GENERATE];
    uint32_t next_inode;
    int create_phase_file_idx; // Tracks progress through the create phase
    int rewrite_phase_file_idx; // Tracks progress through the rewrite phase

    // Pre-allocated buffer of random data for writes
    uint8_t* random_data_source;

    // Serialization helpers
    bool serialize_create(void* buffer, size_t& offset, size_t capacity, const char* name, uint32_t parent_ino);
    bool serialize_write(void* buffer, size_t& offset, size_t capacity, uint32_t ino, uint64_t write_offset, uint32_t length);
};

#endif // LFS_LOG_GEN_H
