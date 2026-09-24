#ifndef LITE_LOG_H
#define LITE_LOG_H

#include <cstdint>
#include "lite_fs.h" // For LITE_MAX_FILENAME_LEN

/**
 * @brief Defines the types of operations that can be recorded in the log.
 */
enum lite_log_op_type {
    LITE_LOG_OP_INVALID = 0,
    LITE_LOG_OP_CREATE,      // Create a new file or directory.
    LITE_LOG_OP_WRITE,       // Write data to a file.
    LITE_LOG_OP_SETATTR,     // Set attributes on an inode (e.g., size, mtime).
    LITE_LOG_OP_UNLINK,      // Unlink a file or directory.
};

/**
 * @brief The standard header that begins every log entry in the shared Log Area.
 * This allows for easy parsing of the log stream.
 */
struct lite_log_header {
    uint32_t entry_size; // Total size of the entry (header + payload + data)
    uint16_t op_type;    // Operation type from lite_log_op_type
    uint16_t reserved;   // Padding for alignment
    uint64_t txn_id;     // Transaction/sequence ID for ordering
};

/**
 * @brief Payload for a CREATE operation.
 */
struct lite_log_create {
    uint32_t parent_ino; // Inode number of the parent directory
    uint16_t type;       // Type of the new entry (LITE_TYPE_FILE or LITE_TYPE_DIR)
    char name[LITE_MAX_FILENAME_LEN];
};

/**
 * @brief Payload for a WRITE operation.
 * The actual file data immediately follows this struct in the log.
 */
struct lite_log_write {
    uint32_t ino;
    uint64_t offset;
    uint32_t length; // Length of the data that follows this struct
};

// Flags to indicate which fields in lite_log_setattr are valid.
#define LITE_SETATTR_SIZE  (1 << 0) // The 'size' field is valid (for truncation)
#define LITE_SETATTR_MTIME (1 << 1) // The 'mtime' field is valid

/**
 * @brief Payload for a generic SETATTR operation to modify inode metadata.
 * A bitmask in the 'valid' field indicates which attributes to update.
 */
struct lite_log_setattr {
    uint32_t ino;          // Inode number to modify
    uint32_t valid;        // Bitmask of fields to update (e.g., LITE_SETATTR_SIZE)

    // New values for the inode attributes.
    uint64_t size;
    uint64_t mtime;
};

/**
 * @brief Payload for an UNLINK operation.
 */
struct lite_log_unlink {
    uint32_t parent_ino; // Inode number of the parent directory
    char name[LITE_MAX_FILENAME_LEN];
};

#endif // LITE_LOG_H
