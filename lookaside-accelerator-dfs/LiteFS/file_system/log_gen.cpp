#include "log_gen.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cassert>

LogGenerator::LogGenerator() : next_inode(1), create_phase_file_idx(0), rewrite_phase_file_idx(0) {
    // Allocate a large buffer of random data to serve as a source for writes
    const size_t random_data_size = 1024 * 1024; // 1MB
    random_data_source = new uint8_t[random_data_size];
    for (size_t i = 0; i < random_data_size; ++i) {
        random_data_source[i] = rand() % 256;
    }
}

LogGenerator::~LogGenerator() {
    delete[] random_data_source;
}

size_t LogGenerator::generate_create_phase(void* buffer, size_t capacity) {
    size_t current_offset = 0;
    const uint32_t root_ino = 0;
    const size_t total_write_size = 1024 * 1024; // 1MB
    const size_t chunk_write_size = 256 * 1024; // 256KB

    while (create_phase_file_idx < NUM_FILES_TO_GENERATE) {
        char filename[32];
        snprintf(filename, sizeof(filename), "file_%d", create_phase_file_idx);

        if (!serialize_create(buffer, current_offset, capacity, filename, root_ino)) return current_offset;
        
        file_inodes[create_phase_file_idx] = next_inode++;

        for (size_t write_offset = 0; write_offset < total_write_size; write_offset += chunk_write_size) {
            if (!serialize_write(buffer, current_offset, capacity, file_inodes[create_phase_file_idx], write_offset, chunk_write_size)) {
                return current_offset; // Stop if buffer is full
            }
        }
        create_phase_file_idx++;
    }
    return current_offset;
}

size_t LogGenerator::generate_rewrite_phase(void* buffer, size_t capacity) {
    size_t current_offset = 0;
    const size_t write_size = 255 * 1024; // 255KB
    const size_t max_file_size = 1024 * 1024; // 1MB (same as create phase)

    while (true) { // Loop indefinitely over the files
        int file_idx = rewrite_phase_file_idx % NUM_FILES_TO_GENERATE;
        uint32_t ino = file_inodes[file_idx];

        // Generate random offset within file bounds
        size_t max_offset = (max_file_size > write_size) ? (max_file_size - write_size) : 0;
        size_t random_offset = (max_offset > 0) ? (rand() % max_offset) : 0;
        random_offset = random_offset & (~15ull); // align to 16-byte boundary for XTS

        if (!serialize_write(buffer, current_offset, capacity, ino, random_offset, write_size)) {
            return current_offset; // Stop if buffer is full
        }
        
        rewrite_phase_file_idx++;
    }
    return current_offset;
}

bool LogGenerator::serialize_create(void* buffer, size_t& offset, size_t capacity, const char* name, uint32_t parent_ino) {
    const size_t entry_size = sizeof(lite_log_header) + sizeof(lite_log_create);
    if (offset + entry_size > capacity) return false;

    uint8_t* ptr = static_cast<uint8_t*>(buffer) + offset;
    lite_log_header* header = reinterpret_cast<lite_log_header*>(ptr);
    lite_log_create* log = reinterpret_cast<lite_log_create*>(ptr + sizeof(lite_log_header));

    header->op_type = LITE_LOG_OP_CREATE;
    header->entry_size = entry_size;

    log->parent_ino = parent_ino;
    log->type = LITE_TYPE_FILE;
    strncpy(log->name, name, LITE_MAX_FILENAME_LEN);

    offset += entry_size;
    return true;
}

bool LogGenerator::serialize_write(void* buffer, size_t& offset, size_t capacity, uint32_t ino, uint64_t write_offset, uint32_t length) {
    const size_t entry_size = sizeof(lite_log_header) + sizeof(lite_log_write) + length;
    if (offset + entry_size > capacity) return false;

    uint8_t* ptr = static_cast<uint8_t*>(buffer) + offset;
    lite_log_header* header = reinterpret_cast<lite_log_header*>(ptr);
    lite_log_write* log = reinterpret_cast<lite_log_write*>(ptr + sizeof(lite_log_header));
    void* data_ptr = ptr + sizeof(lite_log_header) + sizeof(lite_log_write);

    header->op_type = LITE_LOG_OP_WRITE;
    header->entry_size = entry_size;

    log->ino = ino;
    log->offset = write_offset;
    log->length = length;

    // Copy random data into the log (bounds double-check for ASan diagnostics)
    memcpy(data_ptr, random_data_source, length);

    offset += entry_size;
    return true;
}
