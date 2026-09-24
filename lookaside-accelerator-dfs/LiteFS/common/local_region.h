#ifndef LITEFS_LOCAL_REGION_H
#define LITEFS_LOCAL_REGION_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>

class LocalRegionAllocator {
public:
    LocalRegionAllocator(void* base, size_t length);

    void* alloc(size_t size, size_t alignment);

    void free(void* ptr);

    size_t get_offset(const void* ptr) const;
    void* get_base() const { return base_; }
    size_t get_length() const { return length_; }
    void dump_allocator_state(const char* tag, size_t max_blocks = 32) const;

private:
    struct Block { size_t offset; size_t length; bool allocated; };
    uint8_t* base_;
    size_t length_;
    mutable std::mutex mu_;
    std::map<size_t, Block> blocks_by_offset_;
};

#endif // LITEFS_LOCAL_REGION_H


