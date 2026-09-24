#ifndef LFS_OPERATOR_H
#define LFS_OPERATOR_H

#include <cstdint>
#include <cstddef>

#include "lite_fs.h"
#include "lite_log.h"

/**
 * @brief The FileSystemOperator applies all log entries (metadata and data) to the
 * file system state located in host memory. 
 */
class FileSystemOperator {
public:
    // Construct for local filesystem base address and size
    FileSystemOperator(uint8_t* local_fs_base, size_t local_fs_size);

    // Streaming processing of logs; returns bytes consumed
    size_t process_logs(const void* data, size_t size);

private:
    uint8_t* local_fs_base_ = nullptr;
    size_t local_fs_size_ = 0;

    // Local caches for the critical metadata blocks (contiguous region)
    uint8_t* cached_bitmap_block;
    uint8_t* cached_inode_block;
    lite_superblock* cached_superblock;

    // Scratch buffer in DMA local region for directory block read-modify-write
    void* dir_block_scratch_ = nullptr;

    // --- Log-applying helpers ---
    void apply_create(const lite_log_create* log);
    void apply_setattr(const lite_log_setattr* log);
    void apply_unlink(const lite_log_unlink* log);
    void apply_write(const lite_log_write* w, const void* data);

    // --- Allocation and bitmap helpers ---
    int allocate_block_for_inode(lite_inode& inode);
    int find_free_inode();
    int find_free_data_block();
    bool is_data_block_used(int block_num);
    void set_inode_bitmap(int ino, bool used);
    void set_data_bitmap(int block_num, bool used);

    // --- Data block helpers ---
    bool fetch_data_block_to_scratch(uint32_t block_num);
    void flush_scratch_to_data_block(uint32_t block_num);

};

#endif // LFS_OPERATOR_H
