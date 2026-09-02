// Real lock-free single-producer/single-consumer ring buffer. This is the
// hand-off between each venue's network receive thread (producer) and the
// strategy/router thread (consumer): no locks, no allocation on the hot path,
// cache-line padded to avoid false sharing between the head and tail indices.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace fa {

#if defined(__cpp_lib_hardware_interference_size)
constexpr std::size_t kCacheLine = std::hardware_destructive_interference_size;
#else
constexpr std::size_t kCacheLine = 64;  // Apple Silicon L1 line is 128; 64 is safe padding granularity
#endif

// Capacity must be a power of two. T must be trivially copyable (POD messages).
template <typename T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing needs POD messages");
public:
    explicit SpscRing(std::size_t capacity_pow2)
        : cap_(capacity_pow2), mask_(capacity_pow2 - 1), buf_(capacity_pow2) {
        // power-of-two check
        if ((capacity_pow2 & mask_) != 0 || capacity_pow2 == 0)
            throw std::invalid_argument("SpscRing capacity must be power of two");
    }

    // Producer side. Returns false if full (one slot kept empty to disambiguate
    // full vs empty).
    bool push(const T& v) {
        std::size_t head = head_.load(std::memory_order_relaxed);
        std::size_t next = (head + 1) & mask_;
        if (next == tail_.load(std::memory_order_acquire))
            return false;  // full
        buf_[head] = v;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false if empty.
    bool pop(T& out) {
        std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;  // empty
        out = buf_[tail];
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    std::size_t capacity() const { return cap_; }

    std::size_t size_approx() const {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & mask_;
    }

private:
    const std::size_t cap_;
    const std::size_t mask_;
    std::vector<T>    buf_;
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // written by producer
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // written by consumer
    char pad_[kCacheLine];  // trailing pad
};

} // namespace fa
