# LiteFS Data Replication Pipeline

This document outlines the log replication process in LiteFS and the design of its underlying simplified file system.

## Part 1: Log Replication Process

The replication process involves three main components: the `hostfs` application, a primary `nicfs` instance, and a secondary `nicfs` instance. The goal is to offload log processing from the host and ensure data is redundantly stored.

The processing is "fake" in that it does not perform complex logical operations like log coalescing; it is a direct, pass-through application of accelerator functions to the log chunks.

### 1.1. Bootstrap

*   `hostfs` allocates two host memory regions:
    1.  A **Log Area**, which it exports for DMA access. This is where `hostfs` writes its logs.
    2.  A **Persistence Simulation Area**, also exported for DMA, where replicas will write back the final, processed data.
*   `hostfs` establishes RDMA connections with all `nicfs` replicas for doorbells and acknowledgments.
*   `hostfs` shares the DMA export descriptors with the replicas via a one-time TCP handshake.

### 1.2. Primary NIC Data Pipeline

1.  **Log Write & Doorbell**: `hostfs` writes a new log chunk into its **Log Area** and performs an **RDMA Send** to the primary `nicfs` to signal that the chunk is ready.
2.  **DMA Pull**: The primary `nicfs` receives the doorbell and uses its `DmaReceiver` to pull the log chunk from the host's **Log Area** into its own local DRAM.
3.  **(Optional) At-Rest Encryption**: The raw log data is encrypted using the `AesXtsDevice` (AES-XTS).
4.  **(Optional) Compression**: The at-rest-encrypted data is compressed using the `CompressDevice` (LZ4/Deflate).
5.  **(Optional) In-Transit Encryption**: The compressed data is encrypted using the `AesGcmDevice` (AES-GCM).
6.  **Forwarding**: The final payload is forwarded to the secondary `nicfs` instance using an RDMA Write.

### 1.3. Secondary NIC Data Pipeline

1.  **Data Reception**: The secondary `nicfs` receives the payload from the primary via RDMA.
2.  **(Optional) In-Transit Decryption**: The payload is decrypted and its integrity verified using the `AesGcmDevice`.
3.  **(Optional) Decompression**: The data is decompressed using the `CompressDevice`.
4.  **Forwarding**: The data is forwarded to the next NIC down the chain if needed.

### 1.4. Simulated Persistence

`nicfs` of primary and replicas uses its `DmaUser` to perform a **DMA write** of the at-rest-encrypted data back to the host's **Persistence Simulation Area**. This happens concurrently with any forwarding operations.

### 1.5. Acknowledgment

*   After its processing and simulated persistence are complete, **each replica sends an RDMA Send back to the Primary NIC** as an acknowledgment (ACK).
*   The Primary NIC sends a final ACK to the host after its own persistence is done and all replica ACKs are collected. This signals that the log buffer area can be rotated/reused.

---

## Part 2: Simplified File System Design

This section details the on-disk structures for LiteFS, defined in `file_system/lite_fs.h`.

### 2.1. On-Disk Layout (Fixed)

To maximize simplicity, the filesystem has a deterministic, fixed layout. All metadata structures occupy a single 64KiB block each.

*   **Block 0**: Boot Block (Reserved)
*   **Block 1**: Superblock
*   **Block 2**: Combined Bitmap Block (contains both the inode and data bitmaps)
*   **Block 3**: Inode Table Block
*   **Block 4 onwards**: Data Blocks

### 2.2. Core Structures & Constraints

*   **`lite_superblock`**: Contains global filesystem metadata. It stores pointers to the fixed layout locations for convenience.

*   **`lite_inode`**: Stores metadata for a file or directory.
    *   **Size**: Each inode is exactly **128 bytes**.
    *   **Total Inodes**: The entire inode table fits in one 64KiB block, allowing for a fixed total of **512 inodes** (64KiB / 128B).
    *   **Max File Size**: With 24 direct block pointers, the maximum file size is **1.5 MiB** (24 * 64KiB).

    ```c
    struct lite_inode {
        uint16_t type;
        uint16_t nlink;
        uint64_t size;
        uint64_t mtime;
        uint8_t  reserved[12]; // Padding to 128B
        uint32_t direct_blocks[24];
    };
    ```

*   **`lite_dir_entry`**: A single entry within a directory file.

### 2.3. Allocation Strategy

*   **Inode Allocation**: The inode bitmap (64 bytes, for 512 inodes) is located at the start of the combined bitmap block. The allocator finds a free bit to get a new inode number.

*   **Data Allocation**: The data bitmap occupies the remainder of the combined bitmap block. It has one bit for each 64KiB data block.

---

## Part 3: Log Structure Design

This section details the structure of the log entries that `hostfs` writes into the Log Area. These are defined in `common/lite_log.h`.

### 3.1. Log Operation Types

An enum defines the supported filesystem operations.

```c
enum lite_log_op_type {
    LITE_LOG_OP_INVALID = 0,
    LITE_LOG_OP_CREATE,      // Create a new file or directory.
    LITE_LOG_OP_WRITE,       // Write data to a file.
    LITE_LOG_OP_SETATTR,     // Set attributes on an inode (e.g., size, mtime).
    LITE_LOG_OP_UNLINK,      // Unlink a file or directory.
};
```

### 3.2. Core Log Structures

#### Log Entry Header (`lite_log_header`)
Every log entry begins with this header, allowing the NICs to parse the log stream.

```c
struct lite_log_header {
    uint32_t entry_size; // Total size of the entry (header + payload + data)
    uint16_t op_type;    // Operation type from lite_log_op_type
    uint16_t reserved;   // Padding for alignment
    uint64_t txn_id;     // Transaction/sequence ID for ordering
};
```

#### Operation Payloads

*   **Create (`lite_log_create`)**: Contains `parent_ino`, `type`, and `name`.

    ```c
    struct lite_log_create {
        uint32_t parent_ino;
        uint16_t type;
        char name[LITE_MAX_FILENAME_LEN];
    };
    ```

*   **Write (`lite_log_write`)**: Contains `ino`, `offset`, and `length`. The raw write data immediately follows this struct in the log.

    ```c
    struct lite_log_write {
        uint32_t ino;
        uint64_t offset;
        uint32_t length;
    };
    ```

*   **Set Attribute (`lite_log_setattr`)**: A generic operation to modify inode metadata. A bitmask determines which attributes to change.

    ```c
    // Bitmask flags for the 'valid' field
    #define LITE_SETATTR_SIZE  (1 << 0)
    #define LITE_SETATTR_MTIME (1 << 1)

    struct lite_log_setattr {
        uint32_t ino;
        uint32_t valid; // Bitmask of fields to update
        uint64_t size;  // New value for size (if valid)
        uint64_t mtime; // New value for mtime (if valid)
    };
    ```

*   **Unlink (`lite_log_unlink`)**: Contains `parent_ino` and `name`.

    ```c
    struct lite_log_unlink {
        uint32_t parent_ino;
        char name[LITE_MAX_FILENAME_LEN];
    };
    ```
