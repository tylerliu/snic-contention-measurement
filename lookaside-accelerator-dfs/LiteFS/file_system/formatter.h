#ifndef LFS_FORMATTER_H
#define LFS_FORMATTER_H

#include <cstdint>
#include <cstddef>

#include "lite_fs.h"

/**
 * @brief Provides the logic for formatting a raw memory region into a valid
 * LiteFS file system. This includes writing the superblock, initializing bitmaps,
 * and creating a root directory.
 */
class FileSystemFormatter {
public:
    /**
     * @brief Formats a given memory region as a LiteFS filesystem.
     *
     * @param fs_mem A pointer to the start of the memory region.
     * @param fs_size The total size of the memory region in bytes.
     * @return true on success, false on failure (e.g., size is too small).
     */
    bool format(void* fs_mem, size_t fs_size);
};

#endif // LFS_FORMATTER_H
