#include "local_region.h"
#include <doca_log.h>
DOCA_LOG_REGISTER(LITEFS_LOCAL_REGION);

LocalRegionAllocator::LocalRegionAllocator(void* base, size_t length)
    : base_(reinterpret_cast<uint8_t*>(base)), length_(length) {
    Block initial{0, length_, false};
    blocks_by_offset_.emplace(initial.offset, initial);
    DOCA_LOG_INFO("LocalRegionAllocator init: base=%p len=%zu", base_, length_);
}

void* LocalRegionAllocator::alloc(size_t size, size_t alignment) {
    if (size == 0) return nullptr;
    std::lock_guard<std::mutex> g(mu_);
    for (auto it = blocks_by_offset_.begin(); it != blocks_by_offset_.end(); ++it) {
        Block &blk = it->second;
        if (blk.allocated) continue;
        size_t offset = blk.offset;
        size_t length = blk.length;
        size_t aligned = (offset + alignment - 1) / alignment * alignment;
        if (aligned < offset) aligned = offset;
        size_t pad = aligned - offset;
        if (pad <= length && (length - pad) >= size) {
            size_t remaining = length - pad - size;
            if (pad > 0) {
                blk.length = pad;
                Block alloc_blk{aligned, size, true};
                blocks_by_offset_.emplace(alloc_blk.offset, alloc_blk);
                if (remaining > 0) {
                    Block tail_blk{aligned + size, remaining, false};
                    blocks_by_offset_.emplace(tail_blk.offset, tail_blk);
                }
            } else {
                blocks_by_offset_.erase(it);
                Block alloc_blk{aligned, size, true};
                blocks_by_offset_.emplace(alloc_blk.offset, alloc_blk);
                if (remaining > 0) {
                    Block tail_blk{aligned + size, remaining, false};
                    blocks_by_offset_.emplace(tail_blk.offset, tail_blk);
                }
            }
            DOCA_LOG_DBG("LocalRegionAllocator alloc: off=%zu len=%zu align=%zu", aligned, size, alignment);
            return base_ + aligned;
        }
    }
    DOCA_LOG_WARN("LocalRegionAllocator alloc failed: len=%zu align=%zu", size, alignment);
    dump_allocator_state("alloc_failed");
    return nullptr;
}

void LocalRegionAllocator::free(void* ptr) {
    if (ptr == nullptr) return;
    std::lock_guard<std::mutex> g(mu_);
    size_t off = reinterpret_cast<uint8_t*>(ptr) - base_;
    auto it = blocks_by_offset_.find(off);
    if (it == blocks_by_offset_.end()) {
        DOCA_LOG_WARN("LocalRegionAllocator free: unknown off=%zu", off);
        return;
    }
    Block &blk = it->second;
    if (!blk.allocated) {
        DOCA_LOG_WARN("LocalRegionAllocator free: already free off=%zu len=%zu", blk.offset, blk.length);
        return;
    }
    blk.allocated = false;
    auto next = std::next(it);
    if (next != blocks_by_offset_.end() && !next->second.allocated && blk.offset + blk.length == next->second.offset) {
        blk.length += next->second.length;
        blocks_by_offset_.erase(next);
    }
    if (it != blocks_by_offset_.begin()) {
        auto prev = std::prev(it);
        if (!prev->second.allocated && prev->second.offset + prev->second.length == blk.offset) {
            prev->second.length += blk.length;
            blocks_by_offset_.erase(it);
        }
    }
}

size_t LocalRegionAllocator::get_offset(const void* ptr) const {
    if (ptr == nullptr) return 0;
    return static_cast<size_t>(reinterpret_cast<const uint8_t*>(ptr) - base_);
}

void LocalRegionAllocator::dump_allocator_state(const char* tag, size_t max_blocks) const {
    DOCA_LOG_INFO("LocalRegionAllocator state[%s]:", tag);
    size_t count = 0;
    for (auto it = blocks_by_offset_.begin(); it != blocks_by_offset_.end() && count < max_blocks; ++it, ++count) {
        const Block &b = it->second;
        DOCA_LOG_INFO("  block off=%zu len=%zu %s", b.offset, b.length, b.allocated ? "ALLOC" : "FREE");
    }
}


