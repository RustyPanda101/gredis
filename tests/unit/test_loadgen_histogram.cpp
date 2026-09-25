#include "histogram.h"

#include <algorithm>
#include <random>
#include <vector>

#include "test_harness.h"

using gredis::loadgen::LatencyHistogram;

TEST(empty_histogram_reports_zeroes) {
    LatencyHistogram h;
    CHECK_EQ(h.count(), size_t{0});
    CHECK_EQ(h.min_us(), int64_t{0});
    CHECK_EQ(h.max_us(), int64_t{0});
    CHECK_EQ(h.percentile(0.50), int64_t{0});
}

TEST(single_sample_is_exact_at_every_percentile) {
    LatencyHistogram h;
    h.record(1234);
    CHECK_EQ(h.count(), size_t{1});
    CHECK_EQ(h.min_us(), int64_t{1234});
    CHECK_EQ(h.max_us(), int64_t{1234});
    CHECK_EQ(h.percentile(0.0), int64_t{1234});
    CHECK_EQ(h.percentile(1.0), int64_t{1234});
    // bucket-approximated, so p50 may round down slightly but not by much
    const int64_t p50 = h.percentile(0.50);
    CHECK(p50 <= 1234);
    CHECK(p50 > 1234 - 1234 / 16); // well within one bucket's width
}

TEST(min_and_max_are_always_exact) {
    LatencyHistogram h;
    for (int64_t v : {500, 1, 999999, 42, 7}) {
        h.record(v);
    }
    CHECK_EQ(h.min_us(), int64_t{1});
    CHECK_EQ(h.max_us(), int64_t{999999});
}

TEST(mean_matches_exact_average) {
    LatencyHistogram h;
    h.record(10);
    h.record(20);
    h.record(30);
    CHECK(h.mean_us() > 19.9 && h.mean_us() < 20.1);
}

TEST(percentile_is_monotonically_nondecreasing) {
    LatencyHistogram h;
    std::mt19937 rng(7);
    std::uniform_int_distribution<int64_t> dist(1, 2'000'000);
    for (int i = 0; i < 20000; ++i) {
        h.record(dist(rng));
    }
    int64_t prev = 0;
    for (double p : {0.0, 0.10, 0.25, 0.50, 0.75, 0.90, 0.95, 0.99, 0.999, 1.0}) {
        const int64_t v = h.percentile(p);
        CHECK(v >= prev);
        prev = v;
    }
}

TEST(percentile_of_uniform_distribution_is_within_bucket_tolerance) {
    // true p50=50000, p99=99000 -- allow ~5% band for bucket rounding
    LatencyHistogram h;
    for (int64_t v = 1; v <= 100000; ++v) {
        h.record(v);
    }
    const int64_t p50 = h.percentile(0.50);
    const int64_t p99 = h.percentile(0.99);
    CHECK(p50 > 47500 && p50 <= 50000);
    CHECK(p99 > 94050 && p99 <= 99000);
}

TEST(values_beyond_the_tracked_range_clamp_instead_of_crashing) {
    LatencyHistogram h;
    h.record(1);
    h.record(int64_t{1} << 50); // far beyond kMaxExponent -- must clamp, not UB
    CHECK_EQ(h.count(), size_t{2});
    CHECK_EQ(h.min_us(), int64_t{1});
    CHECK_EQ(h.max_us(), int64_t{1} << 50); // exact min/max are unaffected by clamping
    CHECK_EQ(h.percentile(1.0), int64_t{1} << 50);
}

TEST_MAIN()
