#ifndef LITEFS_DMA_HPP
#define LITEFS_DMA_HPP

#ifdef __cplusplus
extern "C" {
#endif

#include "/opt/mellanox/doca/include/doca_dma.h"
#include "/opt/mellanox/doca/include/doca_error.h"

#ifdef __cplusplus
}
#endif

#include <string>
#include <list>
#include <memory>
#include "local_region.h"

// Configure the size of the DOCA memcpy task pool per DMA context
#ifndef LITEFS_DMA_TASK_POOL_SIZE
#define LITEFS_DMA_TASK_POOL_SIZE 64
#endif

/**
 * @brief An RAII wrapper for the sender-side (host) of a DOCA DMA operation.
 *
 * This class initializes the necessary DOCA resources to export a memory buffer
 * and automatically cleans them up upon destruction.
 */
class DmaExporter {
public:
    /**
     * @brief Constructs the exporter, initializes DOCA, and exports the buffer.
     *
     * @param pci_addr The PCI address of the DOCA device.
     * @param src_buffer The buffer on the host to be made available for DMA.
     * @param buffer_len The length of the source buffer.
     * @throws std::runtime_error on failure.
     */
    DmaExporter(const std::string &pci_addr, const char *src_buffer, size_t buffer_len);
    ~DmaExporter();

    /**
     * @brief Gets the exported memory description.
     * @return A const reference to the string containing the export description.
     */
    const std::string& get_export_desc() const { return export_desc_; }

    DmaExporter(const DmaExporter&) = delete;
    DmaExporter& operator=(const DmaExporter&) = delete;
    DmaExporter(DmaExporter&&) = delete;
    DmaExporter& operator=(DmaExporter&&) = delete;

private:
    struct doca_dev *dev_;
    struct doca_mmap *mmap_;
    std::string export_desc_;
};

class DmaUser {
public:
    /**
     * @brief Constructs the DMA user, initializing long-lived DOCA resources and
     *        importing the remote export to create a remote mmap.
     *
     * @param pci_addr The PCI address of the DOCA device.
     * @param export_desc The remote export descriptor from DmaExporter.
     * @param local_region_len Size in bytes of the local region to allocate and map.
     * @throws std::runtime_error on failure.
     */
    DmaUser(const std::string &pci_addr, const std::string &export_desc, size_t local_region_len);
    // New ctor: accept shared local region allocator for multi-user sharing
    DmaUser(const std::string &pci_addr, const std::string &export_desc, std::shared_ptr<LocalRegionAllocator> shared_region);
    ~DmaUser();

    // Thread-safe local region access (allocator exposed via getter only)
    std::shared_ptr<LocalRegionAllocator> get_local_region_ptr() const { return allocator_; }
    const LocalRegionAllocator& get_local_region() const { return *allocator_.get(); }

    // Copy using local region (dst/src are offsets within local region)
    // Non-blocking: enqueues a DMA copy and returns immediately
    doca_error_t submit_copy_from_remote(size_t src_offset, size_t dst_local_offset, size_t length = 0);

    /**
     * @brief Performs a partial DMA copy from a remote, exported buffer.
     *
     * When length is 0, copies the maximum possible bytes bounded by both buffers
     * starting at the given offsets.
     *
     * @param export_desc The memory description from the DmaExporter.
     * @param src_offset  Offset into the remote buffer in bytes.
     * @param dst_offset  Offset into the local buffer in bytes.
     * @param length      Number of bytes to copy (0 for "as much as possible").
     * @return DOCA_SUCCESS on success, or a DOCA error code on failure.
     */
    // Non-blocking: enqueues a DMA copy and returns immediately
    doca_error_t submit_copy_to_remote(size_t src_local_offset, size_t dst_offset, size_t length = 0);

    // Poll for completed DMA tasks. Returns DOCA_SUCCESS if one completion is dequeued
    // and fills out local_offset (the local region offset used for the task) and task_status.
    // Returns DOCA_ERROR_AGAIN if no completions are available.
    doca_error_t dequeue_completed(size_t &local_offset);

    // Number of inflight (submitted but not yet completed and dequeued) tasks
    size_t get_inflight_count() const;

    DmaUser(const DmaUser&) = delete;
    DmaUser& operator=(const DmaUser&) = delete;
    DmaUser(DmaUser&&) = delete;
    DmaUser& operator=(DmaUser&&) = delete;

private:
    struct doca_dev *dev_;
    struct doca_mmap *mmap_;
    struct doca_mmap *remote_mmap_;
    struct doca_buf_inventory *buf_inv_;
    struct doca_dma *dma_;
    struct doca_ctx *ctx_;
    struct doca_pe *pe_;
    void *remote_base_;
    size_t remote_len_;
    // Managed local region
    std::shared_ptr<LocalRegionAllocator> allocator_;

    struct TaskNode;
    std::list<std::unique_ptr<TaskNode>> inflight_;

    // Shared helper to enqueue a memcpy task between two mmaps
    // Computes safe copy length if length==0, validates bounds, creates buffers,
    // allocates and submits the memcpy task, and tracks it in inflight_.
    doca_error_t enqueue_memcpy_task(struct doca_mmap *src_mmap,
                                     struct doca_mmap *dst_mmap,
                                     void *src_addr,
                                     void *dst_addr,
                                     size_t length,
                                     size_t local_offset_to_record);

    void dump_allocator_state(const char *tag, size_t max_blocks = 32) const;
};

#endif /* LITEFS_DMA_HPP */
