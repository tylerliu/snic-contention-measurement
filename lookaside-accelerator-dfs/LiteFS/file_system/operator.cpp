#include "operator.h"

#include <string.h> // For memcpy
#include <cstdlib>
#include <time.h>   // For time()
#include <doca_log.h>
#include <algorithm>

DOCA_LOG_REGISTER(FileSystemOperator);

// --- Helper Functions ---

static uint8_t* get_data_bitmap_ptr(uint8_t* bitmap_block) {
    const int inode_bitmap_bytes = LITE_TOTAL_INODES / 8;
    return bitmap_block + inode_bitmap_bytes;
}

// --- FileSystemOperator Implementation ---

FileSystemOperator::FileSystemOperator(uint8_t* local_fs_base, size_t local_fs_size)
    : cached_bitmap_block(nullptr), cached_inode_block(nullptr), cached_superblock(nullptr), dir_block_scratch_(nullptr) {
    local_fs_base_ = local_fs_base;
    local_fs_size_ = local_fs_size;
    // Metadata caches point directly into the local FS memory
    cached_superblock = reinterpret_cast<lite_superblock*>(local_fs_base_ + (LITE_SUPERBLOCK_BLOCK * LITE_DEFAULT_BLOCK_SIZE));
    cached_bitmap_block = local_fs_base_ + (LITE_BITMAP_BLOCK * LITE_DEFAULT_BLOCK_SIZE);
    cached_inode_block  = local_fs_base_ + (LITE_INODETABLE_BLOCK * LITE_DEFAULT_BLOCK_SIZE);
    // Scratch buffer: allocate using malloc; used only for directory block edits
    dir_block_scratch_  = std::malloc(LITE_DEFAULT_BLOCK_SIZE);
    if (!dir_block_scratch_) {
        // Fallback: terminate if allocation fails
        fprintf(stderr, "FileSystemOperator: failed to allocate dir_block_scratch_\n");
        abort();
    }
}

size_t FileSystemOperator::process_logs(const void* data, size_t size) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t offset = 0;
    while (size - offset >= sizeof(lite_log_header)) {
        const lite_log_header* header = reinterpret_cast<const lite_log_header*>(ptr + offset);
        if (header->entry_size == 0) {
            DOCA_LOG_WARN("Empty entry: entry_size=%u size=%zu pointer=%p", header->entry_size, size, data);
            break;
        }
        if (header->entry_size > size - offset) {
            break; // truncated entry
        }
        const void* payload = ptr + offset + sizeof(lite_log_header);

        switch (header->op_type) {
            case LITE_LOG_OP_CREATE:
                apply_create(static_cast<const lite_log_create*>(payload));
                break;
            case LITE_LOG_OP_SETATTR:
                apply_setattr(static_cast<const lite_log_setattr*>(payload));
                break;
            case LITE_LOG_OP_UNLINK:
                apply_unlink(static_cast<const lite_log_unlink*>(payload));
                break;
            case LITE_LOG_OP_WRITE: {
                const auto* w = static_cast<const lite_log_write*>(payload);
                const void* wdata = ptr + offset + sizeof(lite_log_header) + sizeof(lite_log_write);
                apply_write(w, wdata);
                break;
            }
            default:
                DOCA_LOG_ERR("Unknown operation type: op_type=%u", header->op_type);
                DOCA_LOG_ERR("entry_size=%u size=%zu pointer=%p", header->entry_size, size, data);
                exit(1);
                break;
        }
        offset += header->entry_size;
    }
    return offset;
}

void FileSystemOperator::apply_write(const lite_log_write* w, const void* data) {
    if (w == nullptr) return;
    if (w->ino >= LITE_TOTAL_INODES) return;

    lite_inode* inode_table = reinterpret_cast<lite_inode*>(cached_inode_block);
    lite_inode& inode = inode_table[w->ino];

    // Ensure sufficient blocks are allocated
    uint64_t current_blocks = (inode.size + LITE_DEFAULT_BLOCK_SIZE - 1) / LITE_DEFAULT_BLOCK_SIZE;
    uint64_t required_blocks = (w->offset + w->length + LITE_DEFAULT_BLOCK_SIZE - 1) / LITE_DEFAULT_BLOCK_SIZE;
    while (current_blocks < required_blocks) {
        int new_block = allocate_block_for_inode(inode);
        if (new_block < 0) break;
        current_blocks++;
    }

    // Update inode size and mtime
    if (w->offset + w->length > inode.size) {
        inode.size = w->offset + w->length;
    }
    inode.mtime = time(NULL);

    // Process write in block-aligned chunks, allocating only what's needed for each DMA
    size_t remaining = w->length;
    size_t src_offset = 0;
    uint64_t current_block = w->offset / LITE_DEFAULT_BLOCK_SIZE;
    size_t block_offset = w->offset % LITE_DEFAULT_BLOCK_SIZE;
    
    const uint8_t* src_ptr = static_cast<const uint8_t*>(data);
    
    while (remaining > 0) {
        // Find contiguous block range
        uint64_t start_block = current_block;
        uint32_t start_physical_block = inode.direct_blocks[start_block];
        size_t chunk_size = std::min(remaining, static_cast<size_t>(LITE_DEFAULT_BLOCK_SIZE - block_offset));
        
        memcpy(local_fs_base_ + (LITE_DATABLOCKS_START_BLOCK + start_physical_block) * LITE_DEFAULT_BLOCK_SIZE + block_offset, 
        src_ptr + src_offset, chunk_size);
        
        // Move to next chunk
        remaining -= chunk_size;
        src_offset += chunk_size;
        current_block++;
        block_offset = 0; // Next chunk starts at block boundary
    }
}

// --- Log-applying Helpers ---

void FileSystemOperator::apply_create(const lite_log_create* log) {
    int free_ino = find_free_inode();
    if (free_ino < 0) {
        return; // No free inodes
    }

    set_inode_bitmap(free_ino, true);

    lite_inode* inode_table = reinterpret_cast<lite_inode*>(cached_inode_block);
    lite_inode& new_inode = inode_table[free_ino];

    new_inode.type = log->type;
    new_inode.nlink = 1;
    new_inode.size = 0;
    new_inode.mtime = time(NULL);
    for (int i = 0; i < LITE_MAX_DIRECT_BLOCKS; ++i) {
        new_inode.direct_blocks[i] = 0;
    }
    if (log->parent_ino < LITE_TOTAL_INODES) {
        lite_inode& parent_inode = inode_table[log->parent_ino];
        if (parent_inode.type == LITE_TYPE_DIR) {
            const uint64_t entries_per_block = LITE_DEFAULT_BLOCK_SIZE / sizeof(lite_dir_entry);
            const uint64_t total_entries = parent_inode.size / sizeof(lite_dir_entry);

            bool inserted = false;
            uint64_t processed_entries = 0;
            for (int b = 0; b < LITE_MAX_DIRECT_BLOCKS && processed_entries < total_entries; ++b) {
                uint32_t dir_block_num = parent_inode.direct_blocks[b];
                if (dir_block_num == 0) {
                    break;
                }

                if (!fetch_data_block_to_scratch(dir_block_num)) break;
                auto* block_entries = reinterpret_cast<lite_dir_entry*>(dir_block_scratch_);

                uint64_t entries_to_scan = entries_per_block;
                if (entries_to_scan > (total_entries - processed_entries)) {
                    entries_to_scan = total_entries - processed_entries;
                }

                for (uint64_t j = 0; j < entries_to_scan; ++j) {
                    if (block_entries[j].inode_num == 0) {
                        block_entries[j].inode_num = static_cast<uint32_t>(free_ino);
                        memcpy(block_entries[j].name, log->name, LITE_MAX_FILENAME_LEN);
                        block_entries[j].name[LITE_MAX_FILENAME_LEN - 1] = '\0';
                        parent_inode.mtime = time(NULL);
                        flush_scratch_to_data_block(dir_block_num);
                        inserted = true;
                        break;
                    }
                }

                if (inserted) break;
                processed_entries += entries_to_scan;
            }

            if (!inserted) {
                uint64_t entry_index = total_entries;
                uint64_t block_idx = entry_index / entries_per_block;
                uint64_t offset_in_block = entry_index % entries_per_block;

                if (block_idx >= LITE_MAX_DIRECT_BLOCKS) {
                    DOCA_LOG_ERR("Out of directory capacity");
                    exit(1);
                }

                if (parent_inode.direct_blocks[block_idx] == 0) {
                    int new_block = allocate_block_for_inode(parent_inode);
                    if (new_block < 0) return;
                    memset(dir_block_scratch_, 0, LITE_DEFAULT_BLOCK_SIZE);
                    flush_scratch_to_data_block(new_block);
                }

                uint32_t dir_block_num = parent_inode.direct_blocks[block_idx];
                if (!fetch_data_block_to_scratch(dir_block_num)) return;
                auto* block_entries = reinterpret_cast<lite_dir_entry*>(dir_block_scratch_);
                lite_dir_entry& new_entry = block_entries[offset_in_block];
                new_entry.inode_num = static_cast<uint32_t>(free_ino);
                memcpy(new_entry.name, log->name, LITE_MAX_FILENAME_LEN);
                new_entry.name[LITE_MAX_FILENAME_LEN - 1] = '\0';

                parent_inode.size += sizeof(lite_dir_entry);
                parent_inode.mtime = time(NULL);
                flush_scratch_to_data_block(dir_block_num);
            }
        }
    }
}

void FileSystemOperator::apply_setattr(const lite_log_setattr* log) {
    if (log->ino >= LITE_TOTAL_INODES) return;

    lite_inode* inode_table = reinterpret_cast<lite_inode*>(cached_inode_block);
    lite_inode& inode = inode_table[log->ino];

    if (log->valid & LITE_SETATTR_SIZE) {
        inode.size = log->size;
    }
    if (log->valid & LITE_SETATTR_MTIME) {
        inode.mtime = log->mtime;
    }
}

void FileSystemOperator::apply_unlink(const lite_log_unlink* log) {
    if (log == nullptr) return;
    if (log->parent_ino >= LITE_TOTAL_INODES) return;

    lite_inode* inode_table = reinterpret_cast<lite_inode*>(cached_inode_block);
    lite_inode& parent_inode = inode_table[log->parent_ino];
    if (parent_inode.type != LITE_TYPE_DIR) return;

    const uint64_t total_entries = parent_inode.size / sizeof(lite_dir_entry);
    uint64_t processed_entries = 0;

    int child_ino = -1;
    for (int i = 0; i < LITE_MAX_DIRECT_BLOCKS && processed_entries < total_entries; ++i) {
        uint32_t dir_block_num = parent_inode.direct_blocks[i];
        if (dir_block_num == 0) continue;

        if (!fetch_data_block_to_scratch(dir_block_num)) continue;
        auto* block_entries = reinterpret_cast<lite_dir_entry*>(dir_block_scratch_);

        const uint64_t entries_per_block = LITE_DEFAULT_BLOCK_SIZE / sizeof(lite_dir_entry);
        uint64_t entries_to_scan = entries_per_block;
        if (entries_to_scan > (total_entries - processed_entries)) {
            entries_to_scan = total_entries - processed_entries;
        }

        for (uint64_t j = 0; j < entries_to_scan; ++j) {
            if (block_entries[j].inode_num != 0 &&
                strncmp(block_entries[j].name, log->name, LITE_MAX_FILENAME_LEN) == 0) {
                child_ino = static_cast<int>(block_entries[j].inode_num);
                block_entries[j].inode_num = 0;
                block_entries[j].name[0] = '\0';
                parent_inode.mtime = time(NULL);
                flush_scratch_to_data_block(dir_block_num);
                break;
            }
        }

        if (child_ino >= 0) {
            break;
        }

        processed_entries += entries_to_scan;
    }

    if (child_ino < 0 || child_ino >= static_cast<int>(LITE_TOTAL_INODES)) return;

    lite_inode& child_inode = inode_table[child_ino];
    if (child_inode.nlink > 0) {
        child_inode.nlink--;
    }

    if (child_inode.nlink == 0) {
        for (int i = 0; i < LITE_MAX_DIRECT_BLOCKS; ++i) {
            if (child_inode.direct_blocks[i] != 0) {
                set_data_bitmap(child_inode.direct_blocks[i], false);
                child_inode.direct_blocks[i] = 0;
            }
        }
        set_inode_bitmap(child_ino, false);
        child_inode.type = 0;
        child_inode.size = 0;
        child_inode.mtime = time(NULL);
    }
}

// --- Allocation and Bitmap Helpers ---

int FileSystemOperator::allocate_block_for_inode(lite_inode& inode) {
    int last_block_idx = -1;
    for (int i = 0; i < LITE_MAX_DIRECT_BLOCKS; ++i) {
        if (inode.direct_blocks[i] != 0) {
            last_block_idx = i;
        }
    }

    // Try to allocate contiguously
    if (last_block_idx != -1) {
        int next_block_num = inode.direct_blocks[last_block_idx] + 1;
        if (!is_data_block_used(next_block_num)) {
            set_data_bitmap(next_block_num, true);
            inode.direct_blocks[last_block_idx + 1] = next_block_num;
            return next_block_num;
        }
    }

    int free_block = find_free_data_block();
    if (free_block != -1) {
        set_data_bitmap(free_block, true);
        if (last_block_idx + 1 < LITE_MAX_DIRECT_BLOCKS) {
            inode.direct_blocks[last_block_idx + 1] = free_block;
        }
    }
    return free_block;
}

bool FileSystemOperator::is_data_block_used(int block_num) {
    const int inode_bitmap_bytes = LITE_TOTAL_INODES / 8;
    const int total_data_blocks = (LITE_DEFAULT_BLOCK_SIZE - inode_bitmap_bytes) * 8;
    if (block_num < 0 || block_num >= total_data_blocks) return true;

    uint8_t* data_bitmap = get_data_bitmap_ptr(cached_bitmap_block);
    int byte_idx = block_num / 8;
    int bit_idx = block_num % 8;
    return (data_bitmap[byte_idx] & (1 << bit_idx));
}

int FileSystemOperator::find_free_inode() {
    for (int i = 0; i < LITE_TOTAL_INODES; ++i) {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        if (!(cached_bitmap_block[byte_idx] & (1 << bit_idx))) {
            return i;
        }
    }
    return -1;
}

void FileSystemOperator::set_inode_bitmap(int ino, bool used) {
    if (ino >= LITE_TOTAL_INODES) return;
    int byte_idx = ino / 8;
    int bit_idx = ino % 8;
    if (used) {
        cached_bitmap_block[byte_idx] |= (1 << bit_idx);
    } else {
        cached_bitmap_block[byte_idx] &= ~(1 << bit_idx);
    }
}

int FileSystemOperator::find_free_data_block() {
    const int inode_bitmap_bytes = LITE_TOTAL_INODES / 8;
    const int total_data_blocks = (LITE_DEFAULT_BLOCK_SIZE - inode_bitmap_bytes) * 8;

    for (int i = 0; i < total_data_blocks; ++i) {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        if (!(get_data_bitmap_ptr(cached_bitmap_block)[byte_idx] & (1 << bit_idx))) {
            return i;
        }
    }
    return -1;
}

void FileSystemOperator::set_data_bitmap(int block_num, bool used) {
    const int inode_bitmap_bytes = LITE_TOTAL_INODES / 8;
    const int total_data_blocks = (LITE_DEFAULT_BLOCK_SIZE - inode_bitmap_bytes) * 8;
    if (block_num >= total_data_blocks) return;

    uint8_t* data_bitmap = get_data_bitmap_ptr(cached_bitmap_block);
    int byte_idx = block_num / 8;
    int bit_idx = block_num % 8;

    if (used) {
        data_bitmap[byte_idx] |= (1 << bit_idx);
    } else {
        data_bitmap[byte_idx] &= ~(1 << bit_idx);
    }
}

bool FileSystemOperator::fetch_data_block_to_scratch(uint32_t block_num) {
    if (dir_block_scratch_ == nullptr) return false;
    size_t off = (LITE_DATABLOCKS_START_BLOCK + block_num) * LITE_DEFAULT_BLOCK_SIZE;
    memcpy(dir_block_scratch_, local_fs_base_ + off, LITE_DEFAULT_BLOCK_SIZE);
    return true;
}

void FileSystemOperator::flush_scratch_to_data_block(uint32_t block_num) {
    if (dir_block_scratch_ == nullptr) return;
    size_t off2 = (LITE_DATABLOCKS_START_BLOCK + block_num) * LITE_DEFAULT_BLOCK_SIZE;
    memcpy(local_fs_base_ + off2, dir_block_scratch_, LITE_DEFAULT_BLOCK_SIZE);
}
