#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace bitvm
{

/// one producer and one consumer; storage is allocated only during construction.
template <class T>
class SpscQueue {
    static_assert (std::is_trivially_copyable_v<T>);

public:
    explicit SpscQueue (std::size_t capacity) : slots_ (capacity + 1) {}
    bool
    try_push (const T& value) noexcept {
        const auto write = write_.load (std::memory_order_relaxed);
        const auto next  = advance (write);
        if (next == read_.load (std::memory_order_acquire)) {
            return false;
        }
        slots_[write] = value;
        write_.store (next, std::memory_order_release);
        return true;
    }
    bool
    try_pop (T& value) noexcept {
        const auto read = read_.load (std::memory_order_relaxed);
        if (read == write_.load (std::memory_order_acquire)) {
            return false;
        }
        value = slots_[read];
        read_.store (advance (read), std::memory_order_release);
        return true;
    }

private:
    std::size_t
    advance (std::size_t index) const noexcept {
        return index + 1 == slots_.size() ? 0 : index + 1;
    }
    std::vector<T> slots_;
    alignas (64) std::atomic<std::size_t> read_{ 0 };
    alignas (64) std::atomic<std::size_t> write_{ 0 };
    static_assert (std::atomic<std::size_t>::is_always_lock_free);
};

}  // namespace bitvm
