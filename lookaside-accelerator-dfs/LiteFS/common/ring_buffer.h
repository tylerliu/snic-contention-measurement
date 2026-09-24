#ifndef LITEFS_RING_BUFFER_H
#define LITEFS_RING_BUFFER_H

#include <atomic>
#include <cstdint>
#include <type_traits>
#include <cassert>
#include <memory>

// Single-producer/single-consumer lock-free ring buffer with power-of-two capacity
// - T must be trivially copyable for simple by-value push/pop
// - No blocking: try_push/try_pop return false if full/empty
// - Memory ordering: producer uses release, consumer uses acquire
template <typename T>
class SpscRing {
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
public:
    SpscRing(uint32_t capacity) : head_(0), tail_(0), capacity_(capacity), buffer_(new T[capacity]) {
        assert((capacity & (capacity - 1)) == 0 && "Capacity must be power of two");
        assert(capacity > 0 && "Capacity must be positive");
        assert(capacity <= UINT32_MAX && "Capacity must be less than or equal to UINT32_MAX");
    }

    bool try_push(const T &item) {
        uint32_t head = head_.load(std::memory_order_relaxed);
        uint32_t next = head + 1;
        if ((next - tail_.load(std::memory_order_acquire)) > capacity_) {
            return false; // full
        }
        buffer_[head & mask()] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool try_pop(T &out) {
        uint32_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // empty
        }
        out = buffer_[tail & mask()];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    uint32_t approx_size() const {
        uint32_t h = head_.load(std::memory_order_acquire);
        uint32_t t = tail_.load(std::memory_order_acquire);
        return h - t;
    }

    constexpr uint32_t capacity() const { return capacity_; }

private:
    constexpr uint32_t mask() const { return capacity_ - 1; }

    alignas(64) std::atomic<uint32_t> head_;
    alignas(64) std::atomic<uint32_t> tail_;
    alignas(64) uint32_t capacity_;
    std::unique_ptr<T[]> buffer_;
};

#endif // LITEFS_RING_BUFFER_H


