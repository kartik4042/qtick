#pragma once

#include <atomic>
#include <array>
#include <cstddef>

namespace qtick {

// Single-Producer Single-Consumer lock-free ring buffer
// SIZE must be power of 2
template<typename T, size_t SIZE>
class SPSCRing {
    static_assert((SIZE & (SIZE - 1)) == 0, "SIZE must be power of 2");
    
public:
    SPSCRing() : head_(0), tail_(0) {}
    
    // Producer: try to push item (returns false if full)
    bool try_push(const T& item) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) & MASK;
        
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false; // full
        }
        
        buffer_[current_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }
    
    // Consumer: try to pop item (returns false if empty)
    bool try_pop(T& item) {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        
        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false; // empty
        }
        
        item = buffer_[current_head];
        head_.store((current_head + 1) & MASK, std::memory_order_release);
        return true;
    }
    
    // Check if empty (approximate - only use for monitoring)
    bool is_empty() const {
        return head_.load(std::memory_order_relaxed) == 
               tail_.load(std::memory_order_relaxed);
    }
    
    // Get approximate size (only use for monitoring)
    size_t approx_size() const {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_relaxed);
        return (t - h) & MASK;
    }
    
    static constexpr size_t capacity() { return SIZE; }
    
private:
    static constexpr size_t MASK = SIZE - 1;
    
    alignas(64) std::atomic<size_t> head_; // consumer index
    alignas(64) std::atomic<size_t> tail_; // producer index
    std::array<T, SIZE> buffer_;
};

} // namespace qtick