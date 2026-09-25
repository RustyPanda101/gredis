#include "test_harness.h"

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>

#include "storage/hash_table.h"

using gredis::HashTable;

TEST(basic_insert_find_erase) {
    HashTable<std::string, int> t;
    CHECK(t.empty());

    auto [ptr1, inserted1] = t.insert_or_assign("a", 1);
    CHECK(inserted1);
    CHECK_EQ(*ptr1, 1);
    CHECK_EQ(t.size(), static_cast<size_t>(1));

    auto [ptr2, inserted2] = t.insert_or_assign("a", 2);
    CHECK(!inserted2);
    CHECK_EQ(*ptr2, 2);
    CHECK_EQ(t.size(), static_cast<size_t>(1));

    int* found = t.find("a");
    CHECK(found != nullptr);
    if (found) {
        CHECK_EQ(*found, 2);
    }
    CHECK(t.find("missing") == nullptr);

    CHECK(t.erase("a"));
    CHECK(!t.erase("a"));
    CHECK(t.empty());
}

TEST(heterogeneous_lookup_by_string_view) {
    HashTable<std::string, int> t;
    t.insert_or_assign("hello", 42);
    const std::string_view sv = "hello";
    int* found = t.find(sv);
    CHECK(found != nullptr);
    if (found) {
        CHECK_EQ(*found, 42);
    }
    CHECK(t.erase(sv));
}

TEST(take_moves_value_out) {
    HashTable<std::string, std::string> t;
    t.insert_or_assign("k", "value-data");
    auto taken = t.take("k");
    CHECK(taken.has_value());
    if (taken) {
        CHECK_EQ(*taken, std::string("value-data"));
    }
    CHECK(t.find("k") == nullptr);
    CHECK(!t.take("k").has_value());
}

TEST(clear_empties_and_resets_for_reuse) {
    HashTable<std::string, int> t;
    for (int i = 0; i < 100; ++i) {
        t.insert_or_assign("k" + std::to_string(i), i);
    }
    CHECK_EQ(t.size(), static_cast<size_t>(100));
    t.clear();
    CHECK(t.empty());
    CHECK(!t.is_rehashing());

    t.insert_or_assign("x", 1);
    CHECK_EQ(t.size(), static_cast<size_t>(1));
    CHECK(t.find("x") != nullptr);
}

TEST(for_each_visits_every_entry_exactly_once) {
    HashTable<std::string, int> t;
    std::map<std::string, int> expected;
    for (int i = 0; i < 50; ++i) {
        std::string k = "key" + std::to_string(i);
        t.insert_or_assign(k, i);
        expected[k] = i;
    }
    std::map<std::string, int> seen;
    t.for_each([&](const std::string& k, int& v) { seen[k] = v; });
    CHECK(seen == expected);
}

TEST(const_for_each_visits_every_entry) {
    HashTable<std::string, int> t;
    for (int i = 0; i < 20; ++i) {
        t.insert_or_assign("k" + std::to_string(i), i);
    }
    const HashTable<std::string, int>& ct = t;
    int count = 0;
    ct.for_each([&](const std::string&, const int&) { ++count; });
    CHECK_EQ(count, 20);
}

TEST(growth_triggers_rehashing_and_eventually_completes) {
    HashTable<std::string, int> t;
    for (int i = 0; i < 10; ++i) {
        t.insert_or_assign("k" + std::to_string(i), i);
    }
    CHECK(t.check_invariants());

    // enough ops to guarantee any in-flight rehash finishes
    for (int i = 0; i < 200; ++i) {
        t.find("nonexistent");
    }
    CHECK(!t.is_rehashing());
    CHECK_EQ(t.size(), static_cast<size_t>(10));
    for (int i = 0; i < 10; ++i) {
        CHECK(t.find("k" + std::to_string(i)) != nullptr);
    }
}

TEST(erase_immediately_after_rehash_starts) {
    // 4 initial buckets, so the 4th insert triggers growth right here --
    // most deterministic point to test erase-while-mid-rehash
    HashTable<std::string, int> t;
    t.insert_or_assign("a", 1);
    t.insert_or_assign("b", 2);
    t.insert_or_assign("c", 3);
    CHECK(!t.is_rehashing());
    t.insert_or_assign("d", 4);
    CHECK(t.is_rehashing());

    CHECK(t.erase("a"));
    CHECK(t.find("a") == nullptr);
    CHECK(t.find("b") != nullptr);
    CHECK(t.find("c") != nullptr);
    CHECK(t.find("d") != nullptr);
    CHECK(t.check_invariants());
    CHECK_EQ(t.size(), static_cast<size_t>(3));
}

TEST(shrink_after_many_erases) {
    HashTable<std::string, int> t;
    constexpr int n = 200;
    for (int i = 0; i < n; ++i) {
        t.insert_or_assign("k" + std::to_string(i), i);
    }
    for (int i = 0; i < 500; ++i) {
        t.find("nonexistent"); // drain any in-flight growth rehash
    }
    CHECK(!t.is_rehashing());
    const size_t grown_buckets = t.bucket_count();

    for (int i = 0; i < n; ++i) {
        CHECK(t.erase("k" + std::to_string(i)));
        for (int j = 0; j < 20; ++j) {
            t.find("nonexistent"); // let any shrink rehash progress
        }
    }
    CHECK(t.empty());
    CHECK(t.check_invariants());
    CHECK(t.bucket_count() < grown_buckets);
    CHECK(t.bucket_count() >= static_cast<size_t>(4));
}

namespace {
struct ConstantHash {
    uint64_t operator()(std::string_view) const noexcept { return 42; }
};
} // namespace

TEST(forced_collisions_keep_correctness) {
    HashTable<std::string, int, ConstantHash> t;
    for (int i = 0; i < 200; ++i) {
        t.insert_or_assign("k" + std::to_string(i), i);
    }
    CHECK_EQ(t.size(), static_cast<size_t>(200));
    CHECK(t.check_invariants());

    for (int i = 0; i < 200; ++i) {
        int* v = t.find("k" + std::to_string(i));
        CHECK(v != nullptr);
        if (v) {
            CHECK_EQ(*v, i);
        }
    }

    for (int i = 0; i < 100; ++i) {
        CHECK(t.erase("k" + std::to_string(i)));
    }
    CHECK_EQ(t.size(), static_cast<size_t>(100));
    CHECK(t.check_invariants());
    for (int i = 100; i < 200; ++i) {
        CHECK(t.find("k" + std::to_string(i)) != nullptr);
    }
}

namespace {
struct MoveOnly {
    explicit MoveOnly(int v) : value(v) {}
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
    int value;
};
} // namespace

TEST(move_only_values_work) {
    HashTable<std::string, MoveOnly> t;
    t.insert_or_assign("a", MoveOnly(1));
    t.insert_or_assign("b", MoveOnly(2));
    CHECK_EQ(t.size(), static_cast<size_t>(2));

    MoveOnly* found = t.find("a");
    CHECK(found != nullptr);
    if (found) {
        CHECK_EQ(found->value, 1);
    }

    auto taken = t.take("b");
    CHECK(taken.has_value());
    if (taken) {
        CHECK_EQ(taken->value, 2);
    }
}

TEST(hash_table_move_constructor_transfers_ownership) {
    HashTable<std::string, int> t1;
    t1.insert_or_assign("x", 1);
    HashTable<std::string, int> t2(std::move(t1));
    CHECK_EQ(t2.size(), static_cast<size_t>(1));
    CHECK(t2.find("x") != nullptr);

    CHECK(t1.empty());
    t1.insert_or_assign("y", 2);
    CHECK_EQ(t1.size(), static_cast<size_t>(1));
    CHECK(t1.find("y") != nullptr);
}

TEST(hash_table_move_assignment_transfers_ownership) {
    HashTable<std::string, int> t1;
    t1.insert_or_assign("x", 1);
    HashTable<std::string, int> t2;
    t2.insert_or_assign("old", 99);

    t2 = std::move(t1);
    CHECK_EQ(t2.size(), static_cast<size_t>(1));
    CHECK(t2.find("x") != nullptr);
    CHECK(t2.find("old") == nullptr);
    CHECK(t1.empty());

    t1.insert_or_assign("z", 3);
    CHECK(t1.find("z") != nullptr);
}

TEST(random_entry_returns_a_real_live_entry) {
    HashTable<std::string, int> t;
    for (int i = 0; i < 20; ++i) {
        t.insert_or_assign("k" + std::to_string(i), i);
    }
    std::mt19937 rng(12345);
    for (int trial = 0; trial < 50; ++trial) {
        auto [key, value] = t.random_entry(rng);
        CHECK(key != nullptr);
        CHECK(value != nullptr);
        if (key && value) {
            int* found = t.find(*key);
            CHECK(found == value);
        }
    }
}

TEST(random_entry_on_empty_table_returns_null) {
    HashTable<std::string, int> t;
    std::mt19937 rng(1);
    auto [key, value] = t.random_entry(rng);
    CHECK(key == nullptr);
    CHECK(value == nullptr);
}

TEST(differential_against_std_unordered_map_200k_ops) {
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_int_distribution<int> key_pick(0, 999); // small keyspace forces churn/collisions
    std::uniform_int_distribution<int> op_pick(0, 2);    // 0=insert, 1=erase, 2=find
    std::uniform_int_distribution<int> value_pick(0, 1'000'000);

    HashTable<std::string, int> t;
    std::unordered_map<std::string, int> ref;

    constexpr int kOps = 200'000;
    for (int i = 0; i < kOps; ++i) {
        const std::string key = "k" + std::to_string(key_pick(rng));
        const int op = op_pick(rng);

        if (op == 0) {
            const int value = value_pick(rng);
            auto [ptr, inserted] = t.insert_or_assign(key, value);
            const bool ref_inserted = (ref.count(key) == 0);
            ref[key] = value;
            CHECK_EQ(inserted, ref_inserted);
            CHECK_EQ(*ptr, value);
        } else if (op == 1) {
            const bool erased = t.erase(key);
            const bool ref_erased = ref.erase(key) > 0;
            CHECK_EQ(erased, ref_erased);
        } else {
            int* found = t.find(key);
            const auto it = ref.find(key);
            if (it == ref.end()) {
                CHECK(found == nullptr);
            } else {
                CHECK(found != nullptr);
                if (found) {
                    CHECK_EQ(*found, it->second);
                }
            }
        }

        CHECK_EQ(t.size(), ref.size());

        if (i % 1000 == 0) {
            CHECK(t.check_invariants());
        }
    }

    CHECK(t.check_invariants());
    CHECK_EQ(t.size(), ref.size());
    for (const auto& [k, v] : ref) {
        int* found = t.find(k);
        CHECK(found != nullptr);
        if (found) {
            CHECK_EQ(*found, v);
        }
    }
}

// meaningful only because this binary exits normally via TEST_MAIN() --
// a signal-killed process can't be leak-checked this way
TEST(no_leak_after_clear_with_100k_entries) {
    HashTable<std::string, std::string> t;
    for (int i = 0; i < 100'000; ++i) {
        t.insert_or_assign("key-" + std::to_string(i), "value-" + std::to_string(i));
    }
    CHECK_EQ(t.size(), static_cast<size_t>(100'000));
    t.clear();
    CHECK(t.empty());
}

TEST(no_leak_at_destruction_with_100k_entries) {
    {
        HashTable<std::string, std::string> t;
        for (int i = 0; i < 100'000; ++i) {
            t.insert_or_assign("key-" + std::to_string(i), "value-" + std::to_string(i));
        }
        CHECK_EQ(t.size(), static_cast<size_t>(100'000));
    }
}

TEST_MAIN()
