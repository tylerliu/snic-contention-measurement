#include "formatter.h"
#include <string.h> // For memset
#include <time.h>   // For time()

// Helper to get a pointer to a specific block in the memory region
static void* get_block_ptr(void* fs_mem, uint32_t block_num) {
    return static_cast<uint8_t*>(fs_mem) + (block_num * LITE_DEFAULT_BLOCK_SIZE);
}

bool FileSystemFormatter::format(void* fs_mem, size_t fs_size) {
    // 1. Validate size
    const size_t min_size = LITE_DATABLOCKS_START_BLOCK * LITE_DEFAULT_BLOCK_SIZE;
    if (fs_size < min_size) {
        return false; // Filesystem is too small
    }

    // 2. Zero out the entire memory region
    memset(fs_mem, 0, fs_size);

    // 3. Setup Superblock (located at Block 1)
    lite_superblock* sb = static_cast<lite_superblock*>(get_block_ptr(fs_mem, LITE_SUPERBLOCK_BLOCK));

    sb->magic = LITE_FS_MAGIC;
    sb->total_inodes = LITE_TOTAL_INODES;
    sb->block_size = LITE_DEFAULT_BLOCK_SIZE;

    // Calculate total data blocks based on available space
    sb->total_blocks = (fs_size / LITE_DEFAULT_BLOCK_SIZE) - LITE_DATABLOCKS_START_BLOCK;

    // Set fixed layout pointers
    sb->inode_bitmap_start = LITE_BITMAP_BLOCK;
    sb->data_bitmap_start = LITE_BITMAP_BLOCK;
    sb->inode_table_start = LITE_INODETABLE_BLOCK;
    sb->data_blocks_start = LITE_DATABLOCKS_START_BLOCK;

    // 4. Setup Bitmaps (located at Block 2)
    uint8_t* bitmap_block = static_cast<uint8_t*>(get_block_ptr(fs_mem, LITE_BITMAP_BLOCK));

    // Inode bitmap is at the start of the block.
    // Mark inode 0 (root directory) as used.
    // The first bit of the first byte.
    bitmap_block[0] |= (1 << 0);

    // Data bitmap starts immediately after the inode bitmap.
    // No data blocks are used yet.

    // 5. Setup Inode Table (located at Block 3)
    lite_inode* inode_table = static_cast<lite_inode*>(get_block_ptr(fs_mem, LITE_INODETABLE_BLOCK));

    // Initialize inode 0 for the root directory
    lite_inode& root_inode = inode_table[0];
    root_inode.type = LITE_TYPE_DIR;
    root_inode.nlink = 2; // . and ..
    root_inode.size = 0;  // Empty directory
    root_inode.mtime = time(NULL);

    return true;
}
