#pragma once

#include <cstdint>
#include <cstring>
#include <array>

namespace qtick {

// Symbol enum - extend as needed
enum class Symbol : uint32_t {
    AAPL = 0,
    GOOGL = 1,
    MSFT = 2,
    AMZN = 3,
    TSLA = 4,
    COUNT = 5
};

// Convert symbol to string
inline const char* symbol_to_str(Symbol sym) {
    static constexpr std::array<const char*, 5> names = {
        "AAPL", "GOOGL", "MSFT", "AMZN", "TSLA"
    };
    return names[static_cast<uint32_t>(sym)];
}

// Fixed-size binary tick - 32 bytes, cache-line friendly
struct alignas(32) Tick {
    uint64_t ts_ns;        // nanosecond timestamp
    uint32_t sym_idx;      // symbol index
    uint32_t size;         // volume
    double price;          // price
    uint64_t seq;          // sequence number
    
    Tick() : ts_ns(0), sym_idx(0), size(0), price(0.0), seq(0) {}
    
    Tick(uint64_t ts, Symbol sym, double p, uint32_t sz, uint64_t s = 0)
        : ts_ns(ts), sym_idx(static_cast<uint32_t>(sym)), 
          size(sz), price(p), seq(s) {}
};

static_assert(sizeof(Tick) == 32, "Tick must be 32 bytes");

// Batch container for vectorized q inserts
template<size_t N>
struct TickBatch {
    static constexpr size_t CAPACITY = N;
    std::array<Tick, N> ticks;
    size_t count = 0;
    
    bool is_full() const { return count >= N; }
    bool is_empty() const { return count == 0; }
    
    void push(const Tick& t) {
        if (count < N) {
            ticks[count++] = t;
        }
    }
    
    void clear() { count = 0; }
    
    const Tick* data() const { return ticks.data(); }
    size_t size() const { return count; }
};

// Latency measurement points
struct LatencyMarkers {
    uint64_t t0_gen;      // tick generation
    uint64_t t1_recv;     // bridge received
    uint64_t t2_ring;     // pushed to ring
    uint64_t t3_batch;    // batch send start
    uint64_t t4_qack;     // q upd returned
    
    uint64_t end_to_end() const { return t4_qack - t0_gen; }
};

} // namespace qtick