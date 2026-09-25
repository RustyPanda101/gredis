// gredis::HashTable vs std::unordered_map: insert/find throughput and,
// the actual point, worst-case single-op latency across 10M inserts --
// evidence for incremental rehashing avoiding a big rehash-all stall.
// Manual benchmark, not part of ctest. Build Release, run directly.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage/hash_table.h"

namespace {

using Clock = std::chrono::steady_clock;

std::vector<std::string> make_keys(size_t n) {
    std::vector<std::string> keys;
    keys.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        keys.push_back("key-" + std::to_string(i));
    }
    // shuffle so insertion order doesn't correlate with bucket order
    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);
    return keys;
}

struct LatencyStats {
    double total_ms = 0.0;
    double worst_single_op_us = 0.0;
};

template <typename Fn>
LatencyStats time_ops(const std::vector<std::string>& keys, Fn&& op) {
    LatencyStats stats;
    const auto t0 = Clock::now();
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto op_start = Clock::now();
        op(keys[i], i);
        const auto op_end = Clock::now();
        const double us = std::chrono::duration<double, std::micro>(op_end - op_start).count();
        stats.worst_single_op_us = std::max(stats.worst_single_op_us, us);
    }
    const auto t1 = Clock::now();
    stats.total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return stats;
}

void print_stats(const char* label, const LatencyStats& s, size_t n) {
    std::printf("%-32s total=%9.2f ms   throughput=%10.0f ops/s   worst_single_op=%8.3f us\n",
                label, s.total_ms, static_cast<double>(n) / (s.total_ms / 1000.0), s.worst_single_op_us);
}

} // namespace

int main() {
    constexpr size_t kN = 10'000'000;
    std::printf("gredis hash table micro-benchmark: %zu operations\n", kN);
    std::printf("(raw numbers only -- see docs/benchmarks.md for context)\n\n");

    const std::vector<std::string> keys = make_keys(kN);

    {
        gredis::HashTable<std::string, int> table;
        const LatencyStats insert_stats = time_ops(keys, [&](const std::string& k, size_t i) {
            table.insert_or_assign(k, static_cast<int>(i));
        });
        print_stats("gredis::HashTable insert", insert_stats, kN);

        const LatencyStats find_stats = time_ops(keys, [&](const std::string& k, size_t) {
            volatile bool found = (table.find(k) != nullptr);
            (void)found;
        });
        print_stats("gredis::HashTable find", find_stats, kN);
    }

    {
        std::unordered_map<std::string, int> table;
        const LatencyStats insert_stats = time_ops(keys, [&](const std::string& k, size_t i) {
            table[k] = static_cast<int>(i);
        });
        print_stats("std::unordered_map insert", insert_stats, kN);

        const LatencyStats find_stats = time_ops(keys, [&](const std::string& k, size_t) {
            volatile bool found = (table.find(k) != table.end());
            (void)found;
        });
        print_stats("std::unordered_map find", find_stats, kN);
    }

    return 0;
}
