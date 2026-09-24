#include "doca_dma.h" // local public API for LiteFS DMA

#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_pe.h>
#include <doca_dma.h>

#include <cstring>
#include <cstdlib>
#include <stdexcept>
// <chrono> no longer needed after removing blocking timing

DOCA_LOG_REGISTER(LITEFS_DMA);

// Define internal task tracking node before use
struct DmaUser::TaskNode {
    size_t local_offset;
    struct doca_buf *src_buf;
    struct doca_buf *dst_buf;
    struct doca_task *base_task;
    doca_error_t task_result;
    bool completed;
};

// Helper functions (remains C-style, internal to this file)
// (Removed blocking submit/wait/timing helper to enable non-blocking API)
static doca_error_t open_doca_device_with_pci(const char *pci_addr_str, struct doca_dev **dev) {
    struct doca_devinfo **dev_list = NULL;
    uint32_t nb_devs = 0;
    doca_error_t res = doca_devinfo_create_list(&dev_list, &nb_devs);
    if (res != DOCA_SUCCESS)
        return res;

    struct doca_dev *found_dev = NULL;
    if (pci_addr_str == NULL || pci_addr_str[0] == '\0') {
        // Auto-select first available device
        for (uint32_t i = 0; i < nb_devs; i++) {
            if (doca_dev_open(dev_list[i], &found_dev) == DOCA_SUCCESS && found_dev != NULL) {
                res = DOCA_SUCCESS;
                break;
            }
        }
    } else {
        for (uint32_t i = 0; i < nb_devs; i++) {
            uint8_t is_equal = 0;
            if (doca_devinfo_is_equal_pci_addr(dev_list[i], pci_addr_str, &is_equal) == DOCA_SUCCESS && is_equal) {
                res = doca_dev_open(dev_list[i], &found_dev);
                break;
            }
        }
    }

    doca_devinfo_destroy_list(dev_list);

    if (res == DOCA_SUCCESS && found_dev != NULL) {
        *dev = found_dev;
        return DOCA_SUCCESS;
    }

    return (found_dev == NULL) ? DOCA_ERROR_NOT_FOUND : res;
}

// DmaExporter implementation
DmaExporter::DmaExporter(const std::string &pci_addr, const char *src_buffer, size_t buffer_len)
    : dev_(NULL), mmap_(NULL) {
    doca_error_t res;

    if ((res = open_doca_device_with_pci(pci_addr.c_str(), &dev_)) != DOCA_SUCCESS) {
        throw std::runtime_error("Failed to open DOCA device");
    }
    if ((res = doca_mmap_create(&mmap_)) != DOCA_SUCCESS)
        throw std::runtime_error("mmap create failed");

    // Allow PCI read/write access by the remote for export over PCI
    if ((res = doca_mmap_set_permissions(mmap_, DOCA_ACCESS_FLAG_PCI_READ_WRITE)) != DOCA_SUCCESS)
        throw std::runtime_error("mmap set permissions failed");

    if ((res = doca_mmap_add_dev(mmap_, dev_)) != DOCA_SUCCESS)
        throw std::runtime_error("mmap add dev failed");
    if ((res = doca_mmap_set_memrange(mmap_, (void *)src_buffer, buffer_len)) != DOCA_SUCCESS)
        throw std::runtime_error("mmap set memrange failed");
    if ((res = doca_mmap_start(mmap_)) != DOCA_SUCCESS)
        throw std::runtime_error("mmap start failed");

    const void *export_desc_ptr = NULL;
    size_t export_len = 0;
    res = doca_mmap_export_pci(mmap_, dev_, &export_desc_ptr, &export_len);
    if (res != DOCA_SUCCESS)
        throw std::runtime_error("mmap export failed");

    export_desc_.assign(reinterpret_cast<const char *>(export_desc_ptr), export_len);
}

DmaExporter::~DmaExporter() {
    // Stop and destroy exporter resources
    if (mmap_) {
        doca_mmap_stop(mmap_);
        doca_mmap_destroy(mmap_);
        mmap_ = NULL;
    }
    if (dev_) {
        doca_dev_close(dev_);
        dev_ = NULL;
    }
}

// One-shot receiver function
// DmaUser implementation
DmaUser::DmaUser(const std::string &pci_addr, const std::string &export_desc, size_t local_region_len)
    : DmaUser(pci_addr, export_desc, 
              std::make_shared<LocalRegionAllocator>(aligned_alloc(64, ((local_region_len + 63) / 64) * 64), local_region_len)) {
                assert(local_region_len > 0);
                if (allocator_->get_base() == nullptr) throw std::bad_alloc();
            }

DmaUser::DmaUser(const std::string &pci_addr, const std::string &export_desc, std::shared_ptr<LocalRegionAllocator> local_region)
    : dev_(NULL), mmap_(NULL), remote_mmap_(NULL), buf_inv_(NULL), dma_(NULL), ctx_(NULL), pe_(NULL), remote_base_(NULL), remote_len_(0), allocator_(std::move(local_region)) {
    doca_error_t res;
    if ((res = open_doca_device_with_pci(pci_addr.c_str(), &dev_)) != DOCA_SUCCESS) throw std::runtime_error("Failed to open DOCA device");
    
    // Allocate and map a persistent local region for copying
    if ((res = doca_mmap_create(&mmap_)) != DOCA_SUCCESS) throw std::runtime_error("mmap create failed");
    // Allow local read/write access by DMA to the DPU-local region
    if ((res = doca_mmap_set_permissions(mmap_, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE)) != DOCA_SUCCESS) throw std::runtime_error("mmap set permissions failed");
    if ((res = doca_mmap_add_dev(mmap_, dev_)) != DOCA_SUCCESS) throw std::runtime_error("mmap add dev failed");
    if ((res = doca_mmap_set_memrange(mmap_, allocator_->get_base(), allocator_->get_length())) != DOCA_SUCCESS) throw std::runtime_error("mmap set memrange failed");
    if ((res = doca_mmap_start(mmap_)) != DOCA_SUCCESS) throw std::runtime_error("mmap start failed");
    DOCA_LOG_DBG("Local region mapped: base=%p len=%zu", allocator_->get_base(), allocator_->get_length());

    if ((res = doca_buf_inventory_create(1024, &buf_inv_)) != DOCA_SUCCESS) throw std::runtime_error("inv create failed");
    if ((res = doca_buf_inventory_start(buf_inv_)) != DOCA_SUCCESS) throw std::runtime_error("inv start failed");

    if ((res = doca_dma_create(dev_, &dma_)) != DOCA_SUCCESS) throw std::runtime_error("dma create failed");
    ctx_ = doca_dma_as_ctx(dma_);

    if ((res = doca_pe_create(&pe_)) != DOCA_SUCCESS) throw std::runtime_error("pe create failed");
    if ((res = doca_pe_connect_ctx(pe_, ctx_)) != DOCA_SUCCESS) throw std::runtime_error("pe connect ctx failed");

    // Configure DMA task pool for memcpy tasks; callbacks mark completion but do not free resources
    auto completion_cb = [](struct doca_dma_task_memcpy *task, union doca_data task_user_data, union doca_data) {
        struct doca_task *base_task = doca_dma_task_memcpy_as_task(task);
        // user_data carries our TaskNode*
        DmaUser::TaskNode *node = (DmaUser::TaskNode *)task_user_data.ptr;
        if (node != NULL) {
            node->task_result = doca_task_get_status(base_task);
            node->completed = true;
            if (node->base_task != NULL) {
                doca_task_free(node->base_task);
                node->base_task = NULL;
            }
            if (node->src_buf != NULL) {
                doca_buf_dec_refcount(node->src_buf, NULL);
                node->src_buf = NULL;
            }
            if (node->dst_buf != NULL) {
                doca_buf_dec_refcount(node->dst_buf, NULL);
                node->dst_buf = NULL;
            }
        }
        // Do not free base_task here; we free on dequeue
    };
    // Configure task pool size to allow multiple concurrent tasks across users
    if ((res = doca_dma_task_memcpy_set_conf(dma_, completion_cb, completion_cb, LITEFS_DMA_TASK_POOL_SIZE)) != DOCA_SUCCESS)
        throw std::runtime_error("dma task conf failed");

    if ((res = doca_ctx_start(ctx_)) != DOCA_SUCCESS) throw std::runtime_error("ctx start failed");

    // Import remote export now
    if (export_desc.empty()) throw std::runtime_error("empty export descriptor");
    if ((res = doca_mmap_create_from_export(NULL,
                                            (const void *)export_desc.data(),
                                            export_desc.size(),
                                            dev_,
                                            &remote_mmap_)) != DOCA_SUCCESS) {
        throw std::runtime_error("doca_mmap_create_from_export failed");
    }
    void *remote_addr; size_t remote_len;
    if ((res = doca_mmap_get_memrange(remote_mmap_, &remote_addr, &remote_len)) != DOCA_SUCCESS)
        throw std::runtime_error("doca_mmap_get_memrange failed");
    remote_base_ = remote_addr;
    remote_len_ = remote_len;
    DOCA_LOG_INFO("Remote region imported: base=%p len=%zu", remote_base_, remote_len_);

    // Sanity: exported remote address should be at least cache-line aligned
    if (((uintptr_t)remote_base_ & 63) != 0) {
        DOCA_LOG_WARN("Remote base not 64B aligned: %p. DMA may fail.", remote_base_);
    }
}

DmaUser::~DmaUser() {
    if (ctx_) doca_ctx_stop(ctx_);
    if (buf_inv_) doca_buf_inventory_stop(buf_inv_);
    if (remote_mmap_) {
        doca_mmap_destroy(remote_mmap_);
        remote_mmap_ = NULL;
    }
    if (mmap_) {
        doca_mmap_stop(mmap_);
        doca_mmap_destroy(mmap_);
        mmap_ = NULL;
    }
    if (dma_) {
        doca_dma_destroy(dma_);
        dma_ = NULL;
    }
    if (pe_) {
        doca_pe_destroy(pe_);
        pe_ = NULL;
    }
    if (buf_inv_) {
        doca_buf_inventory_destroy(buf_inv_);
        buf_inv_ = NULL;
    }
    if (dev_) {
        doca_dev_close(dev_);
        dev_ = NULL;
    }
}

// init_remote_from_export removed; constructor handles import

doca_error_t DmaUser::enqueue_memcpy_task(struct doca_mmap *src_mmap,
                                     struct doca_mmap *dst_mmap,
                                     void *src_addr,
                                     void *dst_addr,
                                     size_t length,
                                     size_t local_offset_to_record) {
    doca_error_t final_res = DOCA_SUCCESS;

    struct doca_buf *src_doca_buf = NULL;
    struct doca_buf *dst_doca_buf = NULL;
    struct doca_dma_task_memcpy *dma_task = NULL;
    struct doca_task *base_task = NULL;

    auto throw_if_error = [](doca_error_t e, const char *msg) {
        if (e != DOCA_SUCCESS) {
            DOCA_LOG_ERR("%s: %s", msg, doca_error_get_descr(e));
            throw e;
        }
    };

    try {
        if (src_mmap == NULL || dst_mmap == NULL || src_addr == NULL || dst_addr == NULL) {
            DOCA_LOG_ERR("enqueue_memcpy_task: invalid args src_mmap=%p dst_mmap=%p src_addr=%p dst_addr=%p",
                         (void*)src_mmap, (void*)dst_mmap, src_addr, dst_addr);
            throw DOCA_ERROR_INVALID_VALUE;
        }

        void *src_base = NULL; size_t src_total_len = 0;
        void *dst_base = NULL; size_t dst_total_len = 0;
        throw_if_error(doca_mmap_get_memrange(src_mmap, &src_base, &src_total_len), "get src memrange");
        throw_if_error(doca_mmap_get_memrange(dst_mmap, &dst_base, &dst_total_len), "get dst memrange");

        // Validate that addrs are inside respective ranges
        if (!(src_addr >= src_base && (uint8_t*)src_addr <= (uint8_t*)src_base + src_total_len) ||
            !(dst_addr >= dst_base && (uint8_t*)dst_addr <= (uint8_t*)dst_base + dst_total_len)) {
            DOCA_LOG_ERR("enqueue_memcpy_task: address out of range (src_base=%p len=%zu src_addr=%p | dst_base=%p len=%zu dst_addr=%p)",
                         src_base, src_total_len, src_addr, dst_base, dst_total_len, dst_addr);
            throw DOCA_ERROR_INVALID_VALUE;
        }

        size_t src_off = (size_t)((uint8_t*)src_addr - (uint8_t*)src_base);
        size_t dst_off = (size_t)((uint8_t*)dst_addr - (uint8_t*)dst_base);
        size_t max_src = src_total_len - src_off;
        size_t max_dst = dst_total_len - dst_off;
        size_t copy_len = (length == 0) ? (max_src < max_dst ? max_src : max_dst) : length;
        if (copy_len == 0 || copy_len > max_src || copy_len > max_dst) {
            DOCA_LOG_ERR("enqueue_memcpy_task: invalid copy_len=%zu (max_src=%zu max_dst=%zu)", copy_len, max_src, max_dst);
            throw DOCA_ERROR_INVALID_VALUE;
        }

        DOCA_LOG_TRC("DMA %s: src=[%p..%p) dst=[%p..%p) len=%zu", src_mmap == remote_mmap_ ? "copy_from_remote" : "copy_to_remote",
                      src_addr, (uint8_t*)src_addr + copy_len, dst_addr, (uint8_t*)dst_addr + copy_len, copy_len);

        // Create DOCA buffers for the subranges
        throw_if_error(doca_buf_inventory_buf_get_by_addr(buf_inv_, src_mmap, src_addr, copy_len, &src_doca_buf),
                       "alloc src buf by subrange");
        throw_if_error(doca_buf_inventory_buf_get_by_addr(buf_inv_, dst_mmap, dst_addr, copy_len, &dst_doca_buf),
                       "alloc dst buf by subrange");

        // Set data length on source buffer
        throw_if_error(doca_buf_set_data_len(src_doca_buf, copy_len), "set src data len");

        // Track task
        std::unique_ptr<TaskNode> node(new TaskNode());
        node->local_offset = local_offset_to_record;
        node->src_buf = src_doca_buf;
        node->dst_buf = dst_doca_buf;
        node->task_result = DOCA_ERROR_AGAIN;
        node->completed = false;

        union doca_data user_data = {0};
        user_data.ptr = node.get();
        throw_if_error(doca_dma_task_memcpy_alloc_init(dma_, src_doca_buf, dst_doca_buf, user_data, &dma_task),
                       "alloc memcpy task");
        base_task = doca_dma_task_memcpy_as_task(dma_task);
        node->base_task = base_task;

        doca_error_t submit_res = doca_task_submit(base_task);
        if (submit_res != DOCA_SUCCESS) {
            final_res = submit_res;
            // Cleanup on submit failure
            doca_task_free(base_task);
            doca_buf_dec_refcount(src_doca_buf, NULL);
            doca_buf_dec_refcount(dst_doca_buf, NULL);
        } else {
            inflight_.push_back(std::move(node));
            final_res = DOCA_SUCCESS;
        }
    } catch (doca_error_t e) {
        final_res = e;
        DOCA_LOG_ERR("enqueue_memcpy_task exception: %s", doca_error_get_descr(e));
    } catch (...) {
        final_res = DOCA_ERROR_UNEXPECTED;
        DOCA_LOG_ERR("enqueue_memcpy_task exception: unexpected");
    }

    return final_res;
}

doca_error_t DmaUser::submit_copy_from_remote(size_t src_offset, size_t dst_local_offset,
                                       size_t length) {
    void *src_addr = (uint8_t*)remote_base_ + src_offset;
    void *dst_addr = (uint8_t*)allocator_->get_base() + dst_local_offset;
    return enqueue_memcpy_task(remote_mmap_, mmap_, src_addr, dst_addr, length, dst_local_offset);
}

doca_error_t DmaUser::submit_copy_to_remote(size_t src_local_offset, size_t dst_offset,
                                     size_t length) {
    void *src_addr = (uint8_t*)allocator_->get_base() + src_local_offset;
    void *dst_addr = (uint8_t*)remote_base_ + dst_offset;
    return enqueue_memcpy_task(mmap_, remote_mmap_, src_addr, dst_addr, length, src_local_offset);
}

doca_error_t DmaUser::dequeue_completed(size_t &local_offset) {
    if (inflight_.empty()) {
        return DOCA_ERROR_AGAIN;
    }
    // Progress the DOCA processing engine once
    (void)doca_pe_progress(pe_);

    // Find first completed task
    for (auto it = inflight_.begin(); it != inflight_.end(); ++it) {
        auto node = it->get();
        if (node->task_result != DOCA_ERROR_AGAIN) {
            local_offset = node->local_offset;
            auto task_status = node->task_result;
            // Remove from inflight
            inflight_.erase(it);
            return task_status;
        }
    }
    return DOCA_ERROR_AGAIN;
}

size_t DmaUser::get_inflight_count() const {
    return inflight_.size();
}
