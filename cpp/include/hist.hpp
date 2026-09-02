// Fixed-bucket latency histogram for high-volume measurement. Unlike the rolling
// LatencyTracker (which keeps a 256-sample window and sorts on every query), this
// records millions of samples in O(1) with no allocation and answers percentiles
// from the bucket counts — the right tool for the isolated hot-path benchmark.
//
// Bucketing is log-linear (HdrHistogram-style): coarse enough to hold a wide
// dynamic range in a small fixed array, fine enough that percentile error stays
// under a couple of percent. Sample unit is nanoseconds.
#pragma once
#include <cstdint>
#include <array>
#include <algorithm>

namespace fa {

class Histogram {
public:
    // 64 power-of-two magnitude buckets x 16 linear sub-buckets = 1024 buckets,
    // covering 0 .. ~2^63 ns with <=1/16 relative resolution per magnitude.
    static constexpr int kMagnitudes = 64;
    static constexpr int kSub = 16;
    static constexpr int kBuckets = kMagnitudes * kSub;

    void record(uint64_t ns) {
        int b = bucket_of(ns);
        ++counts_[b];
        ++total_;
        if (ns < min_) min_ = ns;
        if (ns > max_) max_ = ns;
        sum_ += ns;
    }

    uint64_t count() const { return total_; }
    uint64_t min() const { return total_ ? min_ : 0; }
    uint64_t max() const { return max_; }
    double   mean() const { return total_ ? double(sum_) / double(total_) : 0.0; }

    // Value at quantile q in [0,1]. Returns the representative value of the
    // bucket containing the q-th sample.
    uint64_t percentile(double q) const {
        if (total_ == 0) return 0;
        if (q < 0) q = 0; if (q > 1) q = 1;
        uint64_t target = uint64_t(q * (total_ - 1));
        uint64_t cum = 0;
        for (int b = 0; b < kBuckets; ++b) {
            cum += counts_[b];
            if (cum > target) return value_of(b);
        }
        return max_;
    }

    uint64_t p50() const { return percentile(0.50); }
    uint64_t p90() const { return percentile(0.90); }
    uint64_t p99() const { return percentile(0.99); }
    uint64_t p999() const { return percentile(0.999); }

    void merge(const Histogram& o) {
        for (int b = 0; b < kBuckets; ++b) counts_[b] += o.counts_[b];
        total_ += o.total_;
        sum_ += o.sum_;
        if (o.total_) {
            min_ = std::min(min_, o.min_);
            max_ = std::max(max_, o.max_);
        }
    }

private:
    // Magnitude = index of the highest set bit; sub-bucket = next 4 bits.
    static int bucket_of(uint64_t ns) {
        if (ns == 0) return 0;
        int mag = 63 - __builtin_clzll(ns);              // 0..63
        int shift = mag >= 4 ? mag - 4 : 0;
        int sub = int((ns >> shift) & (kSub - 1));
        int b = mag * kSub + sub;
        return b < kBuckets ? b : kBuckets - 1;
    }

    // Lower edge of a bucket, used as its representative value. Inverse of
    // bucket_of: for mag<4 the value equals the bucket's sub index; for mag>=4
    // it is 2^mag plus the sub-bucket offset scaled by 2^(mag-4).
    static uint64_t value_of(int b) {
        int mag = b / kSub;
        int sub = b % kSub;
        if (mag < 4) return uint64_t(sub);
        int shift = mag - 4;
        return (uint64_t(1) << mag) + (uint64_t(sub) << shift);
    }

    std::array<uint64_t, kBuckets> counts_{};
    uint64_t total_ = 0;
    uint64_t min_ = UINT64_MAX;
    uint64_t max_ = 0;
    unsigned __int128 sum_ = 0;
};

} // namespace fa
