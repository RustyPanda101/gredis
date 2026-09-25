// log-bucketed latency histogram: fixed-size, allocation-free, cheap to
// update per request. simplified relative of a real HDR histogram, not
// byte-compatible with one -- see bucketing comment in histogram.cpp
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gredis::loadgen {

class LatencyHistogram {
public:
    // latency in microseconds. clamped into [1, kMaxTrackedUs] before
    // bucketing, but min/max/mean stay exact and unclamped.
    void record(int64_t latency_us);

    size_t count() const { return count_; }
    int64_t min_us() const { return count_ ? min_us_ : 0; }
    int64_t max_us() const { return count_ ? max_us_ : 0; }
    double mean_us() const {
        return count_ ? static_cast<double>(sum_us_) / static_cast<double>(count_) : 0.0;
    }

    // approximate value v where at least `p` of samples are <= v.
    // p<=0/p>=1 return exact min/max instead of a bucket approximation.
    int64_t percentile(double p) const;

    // power of two, so bucket-index math below is exact integer shifts.
    // 32 sub-buckets per octave -> ~3% width, coarser than a real HDR
    // histogram but plenty for benchmarking.
    static constexpr int kSubBucketBits = 5;
    static constexpr int kSubBucketCount = 1 << kSubBucketBits;
    // tracks up to 2^40-1 us (~12.7 days); anything bigger clamps into
    // the top bucket instead of growing the table
    static constexpr int kMaxExponent = 40;
    static constexpr int kBucketCount = kMaxExponent * kSubBucketCount;

private:
    static int bucket_index(int64_t clamped_v);
    static int64_t bucket_lower_bound(int bucket);

    std::array<uint64_t, kBucketCount> buckets_{};
    size_t count_ = 0;
    int64_t min_us_ = 0;
    int64_t max_us_ = 0;
    int64_t sum_us_ = 0;
};

} // namespace gredis::loadgen
