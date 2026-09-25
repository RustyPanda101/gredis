#include "histogram.h"

#include <algorithm>
#include <cmath>

namespace gredis::loadgen {

namespace {
// largest e such that 2^e <= v < 2^(e+1); v must be >= 1 (clzll(0) is UB)
int highest_bit(uint64_t v) {
    return 63 - __builtin_clzll(v);
}

constexpr int64_t kMaxTrackedUs = (int64_t{1} << LatencyHistogram::kMaxExponent) - 1;
} // namespace

void LatencyHistogram::record(int64_t latency_us) {
    if (count_ == 0 || latency_us < min_us_) {
        min_us_ = latency_us;
    }
    if (count_ == 0 || latency_us > max_us_) {
        max_us_ = latency_us;
    }
    sum_us_ += latency_us;
    ++count_;

    const int64_t clamped = std::clamp<int64_t>(latency_us, 1, kMaxTrackedUs);
    ++buckets_[static_cast<size_t>(bucket_index(clamped))];
}

int LatencyHistogram::bucket_index(int64_t clamped_v) {
    const auto v = static_cast<uint64_t>(clamped_v);
    const int e = highest_bit(v);
    // v's position within [2^e, 2^(e+1)) scaled to [0, kSubBucketCount),
    // via shift since 2^e is a power of two
    const int sub = static_cast<int>(((v - (uint64_t{1} << e)) << kSubBucketBits) >> e);
    return e * kSubBucketCount + sub;
}

int64_t LatencyHistogram::bucket_lower_bound(int bucket) {
    const int e = bucket / kSubBucketCount;
    const int sub = bucket % kSubBucketCount;
    const int64_t octave_base = int64_t{1} << e;
    return octave_base + ((octave_base * sub) >> kSubBucketBits);
}

int64_t LatencyHistogram::percentile(double p) const {
    if (count_ == 0) {
        return 0;
    }
    if (p <= 0.0) {
        return min_us_;
    }
    if (p >= 1.0) {
        return max_us_;
    }

    // ceil(p * count_), clamped to at least 1
    const auto target = static_cast<uint64_t>(
        std::max(1.0, std::ceil(p * static_cast<double>(count_))));

    uint64_t cumulative = 0;
    for (int b = 0; b < kBucketCount; ++b) {
        cumulative += buckets_[static_cast<size_t>(b)];
        if (cumulative >= target) {
            return bucket_lower_bound(b);
        }
    }
    return max_us_; // unreachable in practice
}

} // namespace gredis::loadgen
