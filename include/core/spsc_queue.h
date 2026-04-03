#pragma once

#include <atomic>
#include <array>

// Single-Producer Single-Consumer lock-free ring buffer.
// Thread safety: exactly one thread may call push(), exactly one thread may call pop().
template<typename T, size_t Size>
class SPSCQueue {
    static_assert(Size > 0, "SPSCQueue size must be greater than zero");

public:
    bool push(const T& item) {
        const auto current_tail = tail_.load(std::memory_order_relaxed);
        const auto next_tail = increment(current_tail);
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        buffer_[current_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const auto current_head = head_.load(std::memory_order_relaxed);
        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        item = buffer_[current_head];
        head_.store(increment(current_head), std::memory_order_release);
        return true;
    }

private:
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
    std::array<T, Size> buffer_;

    size_t increment(size_t idx) const {
        return (idx + 1) % Size;
    }
};
