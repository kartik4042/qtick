#pragma once

#include <cstdint>
#include <time.h>

namespace qtick {

// Get nanosecond timestamp using CLOCK_MONOTONIC_RAW
inline uint64_t get_timestamp_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + 
           static_cast<uint64_t>(ts.tv_nsec);
}

// Alternative: TSC-based timing (requires calibration)
#ifdef __x86_64__
inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}
#endif

// Convert ns to microseconds
inline double ns_to_us(uint64_t ns) {
    return static_cast<double>(ns) / 1000.0;
}

// Convert ns to milliseconds
inline double ns_to_ms(uint64_t ns) {
    return static_cast<double>(ns) / 1'000'000.0;
}

} // namespace qtick