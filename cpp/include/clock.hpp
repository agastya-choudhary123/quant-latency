// Real high-resolution timing. On Apple Silicon there is no user-space rdtsc;
// mach_absolute_time() is the cheapest monotonic tick source (a few ns/call,
// no syscall) and is what CLOCK_UPTIME_RAW is built on. We read the timebase
// once and convert ticks->ns. This is the timestamp source for every latency
// measurement on the hot path.
#pragma once
#include <cstdint>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

namespace fa {

class Clock {
public:
    static Clock& instance() { static Clock c; return c; }

    // Raw monotonic ticks — cheapest possible, use for interval measurement.
    static inline uint64_t ticks() {
#if defined(__APPLE__)
        return mach_absolute_time();
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
#endif
    }

    // Monotonic nanoseconds since an arbitrary epoch.
    static inline uint64_t now_ns() {
#if defined(__APPLE__)
        return ticks_to_ns(mach_absolute_time());
#else
        return ticks();
#endif
    }

    static inline uint64_t ticks_to_ns(uint64_t t) {
#if defined(__APPLE__)
        const auto& c = instance();
        // t * numer / denom, done in 128-bit to avoid overflow.
        return static_cast<uint64_t>(
            (static_cast<__uint128_t>(t) * c.numer_) / c.denom_);
#else
        return t;
#endif
    }

private:
    Clock() {
#if defined(__APPLE__)
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        numer_ = tb.numer;
        denom_ = tb.denom;
#endif
    }
#if defined(__APPLE__)
    uint64_t numer_ = 1, denom_ = 1;
#endif
};

// RAII stopwatch: measures the real ns elapsed over its scope into a sink.
class ScopedTimer {
public:
    explicit ScopedTimer(uint64_t& out_ns) : out_(out_ns), start_(Clock::ticks()) {}
    ~ScopedTimer() { out_ = Clock::ticks_to_ns(Clock::ticks() - start_); }
private:
    uint64_t& out_;
    uint64_t  start_;
};

} // namespace fa
