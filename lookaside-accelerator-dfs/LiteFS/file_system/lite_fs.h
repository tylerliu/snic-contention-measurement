#ifndef LITE_FS_H
#define LITE_FS_H

#include <cstdint>

// --- Filesystem Constants ---
#define LITE_FS_MAGIC 0xDEADBEEF
#define LITE_DEFAULT_BLOCK_SIZE (64 * 1024) // 64 KiB
#define LITE_MAX_FILENAME_LEN 28

// --- Fixed Layout Constants ---
#define LITE_INODE_SIZE 128
#define LITE_TOTAL_INODES 512 // (64KiB block / 128B per inode)
#define LITE_MAX_DIRECT_BLOCKS 24

#define LITE_SUPERBLOCK_BLOCK 1
#define LITE_BITMAP_BLOCK 2
#define LITE_INODETABLE_BLOCK 3
#define LITE_DATABLOCKS_START_BLOCK 4

// --- Derived Convenience Constants ---
// Root inode number
#define LITE_ROOT_INO 0
// Inode bitmap is stored at the start of the bitmap block
#define LITE_INODE_BITMAP_BYTES (LITE_TOTAL_INODES / 8)
// Remaining bytes in the bitmap block are for data block allocation bitmap
#define LITE_DATA_BITMAP_BYTES (LITE_DEFAULT_BLOCK_SIZE - LITE_INODE_BITMAP_BYTES)
#define LITE_TOTAL_DATA_BLOCKS (LITE_DATA_BITMAP_BYTES * 8)
// Max file size given only direct blocks
#define LITE_MAX_FILE_SIZE (1ULL * LITE_MAX_DIRECT_BLOCKS * LITE_DEFAULT_BLOCK_SIZE)
// Default DMA alignment used by staging allocations
#define LITE_DMA_DEFAULT_ALIGN 64

// --- Inode Type Constants ---
#define LITE_TYPE_FILE 1
#define LITE_TYPE_DIR  2

/**
 * @brief The superblock contains global metadata about the entire filesystem.
 * It is located at a fixed position (LITE_SUPERBLOCK_BLOCK).
 */
struct lite_superblock {
    uint32_t magic;              // Magic number to identify our filesystem.
    uint32_t total_blocks;       // Total number of data blocks.
    uint32_t total_inodes;       // Total number of available inodes (fixed at LITE_TOTAL_INODES).
    uint32_t block_size;         // Data block size in bytes (fixed at LITE_DEFAULT_BLOCK_SIZE).

    // The following are fixed by the layout but stored for convenience.
    uint32_t inode_bitmap_start; // Should always be LITE_BITMAP_BLOCK.
    uint32_t data_bitmap_start;  // Should also be LITE_BITMAP_BLOCK (bitmaps are combined).
    uint32_t inode_table_start;  // Should always be LITE_INODETABLE_BLOCK.
    uint32_t data_blocks_start;  // Should always be LITE_DATABLOCKS_START_BLOCK.
};

/**
 * @brief An inode stores all metadata for a file or directory.
 * The entire inode table (LITE_TOTAL_INODES * LITE_INODE_SIZE) fits in one block.
 * Each inode is exactly 128 bytes.
 */
struct lite_inode {
    uint16_t type;      // Type of the file (LITE_TYPE_FILE or LITE_TYPE_DIR).
    uint16_t nlink;     // Number of hard links to this inode.
    uint64_t size;      // Size of the file in bytes.
    uint64_t mtime;     // Last modification time (seconds since epoch).

    uint8_t reserved[12]; // Padding to make the struct exactly 128 bytes.

    /**
     * @brief Array of pointers to data blocks.
     * With LITE_MAX_DIRECT_BLOCKS = 24 and a block size of 64KB,
     * the max file size is 1.5MB (24 * 64KB).
     */
    uint32_t direct_blocks[LITE_MAX_DIRECT_BLOCKS];
};

/**
 * @brief A directory entry maps a name to an inode number.
 * A directory file is simply a sequence of these structures.
 */
struct lite_dir_entry {
    uint32_t inode_num;                      // Inode number for this entry.
    char name[LITE_MAX_FILENAME_LEN];        // Null-terminated filename.
};

// Directory layout helpers (need struct definition for sizeof)
#define LITE_DIR_ENTRY_SIZE (sizeof(lite_dir_entry))
#define LITE_DIR_ENTRIES_PER_BLOCK (LITE_DEFAULT_BLOCK_SIZE / LITE_DIR_ENTRY_SIZE)

#endif // LITE_FS_H
