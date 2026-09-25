#include "storage/database.h"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

#include "storage/value.h"
#include "test_harness.h"

using gredis::Database;
using gredis::Value;

namespace {
constexpr int64_t kT0 = 1'000'000; // an arbitrary fixed "now" for injected-clock tests
} // namespace

TEST(lookup_on_empty_database_returns_null) {
    Database db;
    CHECK(db.lookup_read("missing", kT0) == nullptr);
    CHECK_EQ(db.size(), size_t{0});
    CHECK(db.empty());
}

TEST(set_then_lookup_round_trips) {
    Database db;
    db.set("k", Value(std::string("v")));
    Value* v = db.lookup_read("k", kT0);
    CHECK(v != nullptr);
    CHECK(std::get<std::string>(*v) == "v");
    CHECK_EQ(db.size(), size_t{1});
}

TEST(set_overwrites_existing_key) {
    Database db;
    db.set("k", Value(std::string("first")));
    db.set("k", Value(std::string("second")));
    CHECK_EQ(db.size(), size_t{1});
    Value* v = db.lookup_read("k", kT0);
    CHECK(std::get<std::string>(*v) == "second");
}

TEST(del_removes_key_and_reports_presence) {
    Database db;
    CHECK(!db.del("missing"));
    db.set("k", Value(std::string("v")));
    CHECK(db.del("k"));
    CHECK(db.lookup_read("k", kT0) == nullptr);
    CHECK(!db.del("k")); // already gone
}

TEST(clear_empties_the_database) {
    Database db;
    db.set("a", Value(std::string("1")));
    db.set("b", Value(std::string("2")));
    CHECK_EQ(db.size(), size_t{2});
    db.clear();
    CHECK(db.empty());
    CHECK(db.lookup_read("a", kT0) == nullptr);
}

TEST(lookup_accepts_string_view_without_materializing_a_string) {
    Database db;
    db.set("key", Value(std::string("value")));
    // string_view lookup exercises HashTable's transparent find path
    std::string_view sv = "key";
    CHECK(db.lookup_read(sv, kT0) != nullptr);
}

TEST(type_name_reports_string_for_every_stored_value) {
    Database db;
    db.set("k", Value(std::string("v")));
    Value* v = db.lookup_read("k", kT0);
    CHECK(std::string(gredis::type_name(*v)) == "string");
}

TEST(type_name_reports_hash_for_a_hash_value) {
    Database db;
    db.set("h", Value(std::make_unique<gredis::HashValue>()));
    Value* v = db.lookup_read("h", kT0);
    CHECK(v != nullptr);
    CHECK(std::string(gredis::type_name(*v)) == "hash");
    CHECK(std::holds_alternative<std::unique_ptr<gredis::HashValue>>(*v));
}

TEST(type_name_reports_list_for_a_list_value) {
    Database db;
    db.set("l", Value(std::make_unique<gredis::ListValue>()));
    Value* v = db.lookup_read("l", kT0);
    CHECK(v != nullptr);
    CHECK(std::string(gredis::type_name(*v)) == "list");
    CHECK(std::holds_alternative<std::unique_ptr<gredis::ListValue>>(*v));
}

TEST(type_name_reports_set_for_a_set_value) {
    Database db;
    db.set("s", Value(std::make_unique<gredis::SetValue>()));
    Value* v = db.lookup_read("s", kT0);
    CHECK(v != nullptr);
    CHECK(std::string(gredis::type_name(*v)) == "set");
    CHECK(std::holds_alternative<std::unique_ptr<gredis::SetValue>>(*v));
}

TEST(many_keys_survive_a_rehash) {
    Database db;
    constexpr int kN = 10000;
    for (int i = 0; i < kN; ++i) {
        db.set("key-" + std::to_string(i), Value(std::to_string(i)));
    }
    CHECK_EQ(db.size(), static_cast<size_t>(kN));
    for (int i = 0; i < kN; ++i) {
        Value* v = db.lookup_read("key-" + std::to_string(i), kT0);
        CHECK(v != nullptr);
        CHECK(std::get<std::string>(*v) == std::to_string(i));
    }
}

TEST(pttl_is_minus_two_for_a_missing_key) {
    Database db;
    CHECK_EQ(db.pttl("missing", kT0), int64_t{-2});
}

TEST(pttl_is_minus_one_for_a_key_with_no_ttl) {
    Database db;
    db.set("k", Value(std::string("v")));
    CHECK_EQ(db.pttl("k", kT0), int64_t{-1});
}

TEST(expire_at_sets_a_ttl_that_pttl_reports) {
    Database db;
    db.set("k", Value(std::string("v")));
    CHECK(db.expire_at("k", kT0 + 5000, kT0));
    CHECK_EQ(db.pttl("k", kT0), int64_t{5000});
    CHECK_EQ(db.pttl("k", kT0 + 3000), int64_t{2000});
}

TEST(expire_at_on_missing_key_returns_false) {
    Database db;
    CHECK(!db.expire_at("missing", kT0 + 1000, kT0));
}

TEST(key_is_gone_once_its_deadline_passes) {
    Database db;
    db.set("k", Value(std::string("v")));
    db.expire_at("k", kT0 + 100, kT0);
    CHECK(db.lookup_read("k", kT0 + 50) != nullptr);  // not yet
    CHECK(db.lookup_read("k", kT0 + 100) == nullptr); // exactly at deadline: expired
    CHECK(db.lookup_read("k", kT0 + 200) == nullptr); // well past
    CHECK_EQ(db.size(), size_t{0}); // lazily deleted, not just hidden
}

TEST(expire_at_with_a_past_deadline_deletes_immediately) {
    Database db;
    db.set("k", Value(std::string("v")));
    CHECK(db.expire_at("k", kT0 - 1, kT0)); // "existed" (before deletion)
    CHECK(db.lookup_read("k", kT0) == nullptr);
    CHECK_EQ(db.size(), size_t{0});
}

TEST(set_clears_a_previous_ttl) {
    Database db;
    db.set("k", Value(std::string("v1")));
    db.expire_at("k", kT0 + 1000, kT0);
    CHECK_EQ(db.pttl("k", kT0), int64_t{1000});
    db.set("k", Value(std::string("v2"))); // plain set(), no KEEPTTL
    CHECK_EQ(db.pttl("k", kT0), int64_t{-1});
}

TEST(persist_removes_ttl_and_reports_whether_one_existed) {
    Database db;
    db.set("k", Value(std::string("v")));
    CHECK(!db.persist("k", kT0)); // no TTL to remove
    db.expire_at("k", kT0 + 1000, kT0);
    CHECK(db.persist("k", kT0));
    CHECK_EQ(db.pttl("k", kT0), int64_t{-1});
}

TEST(persist_on_missing_key_returns_false) {
    Database db;
    CHECK(!db.persist("missing", kT0));
}

TEST(active_cycle_expires_only_keys_past_their_deadline) {
    Database db;
    db.set("a", Value(std::string("1")));
    db.expire_at("a", kT0 + 100, kT0);
    db.set("b", Value(std::string("2")));
    db.expire_at("b", kT0 + 1000, kT0); // not due yet at kT0 + 100

    const size_t expired = db.run_active_expiration_cycle(kT0 + 100, /*max_keys=*/1000, /*max_wall_ms=*/1000.0);
    CHECK_EQ(expired, size_t{1});
    CHECK_EQ(db.size(), size_t{1});
    CHECK(db.lookup_read("a", kT0 + 100) == nullptr);
    CHECK(db.lookup_read("b", kT0 + 100) != nullptr);
}

TEST(active_cycle_respects_the_max_keys_budget) {
    Database db;
    for (int i = 0; i < 10; ++i) {
        const std::string key = "k" + std::to_string(i);
        db.set(key, Value(std::string("v")));
        db.expire_at(key, kT0 + 10, kT0);
    }
    const size_t expired = db.run_active_expiration_cycle(kT0 + 10, /*max_keys=*/3, /*max_wall_ms=*/1000.0);
    CHECK_EQ(expired, size_t{3});
    CHECK_EQ(db.size(), size_t{7}); // the rest are still there, to be picked up next cycle
}

TEST(active_cycle_skips_a_stale_entry_after_persist) {
    Database db;
    db.set("k", Value(std::string("v")));
    db.expire_at("k", kT0 + 100, kT0); // pushes a heap entry for deadline kT0+100
    db.persist("k", kT0);              // clears the TTL -- the heap entry above is now stale

    const size_t expired = db.run_active_expiration_cycle(kT0 + 100, /*max_keys=*/1000, /*max_wall_ms=*/1000.0);
    CHECK_EQ(expired, size_t{0});             // the stale entry must not delete the (now TTL-less) key
    CHECK(db.lookup_read("k", kT0 + 100) != nullptr);
    CHECK_EQ(db.size(), size_t{1});
}

TEST(active_cycle_skips_a_stale_entry_after_re_expire) {
    Database db;
    db.set("k", Value(std::string("v")));
    db.expire_at("k", kT0 + 100, kT0);  // stale heap entry once superseded below
    db.expire_at("k", kT0 + 5000, kT0); // re-expire to a much later deadline

    const size_t expired = db.run_active_expiration_cycle(kT0 + 100, /*max_keys=*/1000, /*max_wall_ms=*/1000.0);
    CHECK_EQ(expired, size_t{0}); // the kT0+100 entry is stale; the kT0+5000 one isn't due yet
    CHECK(db.lookup_read("k", kT0 + 100) != nullptr);
}

TEST(active_cycle_skips_a_stale_entry_after_del) {
    Database db;
    db.set("k", Value(std::string("v")));
    db.expire_at("k", kT0 + 100, kT0);
    db.del("k");
    db.set("k", Value(std::string("v2"))); // reuse the key, no TTL this time

    const size_t expired = db.run_active_expiration_cycle(kT0 + 100, /*max_keys=*/1000, /*max_wall_ms=*/1000.0);
    CHECK_EQ(expired, size_t{0});
    CHECK(db.lookup_read("k", kT0 + 100) != nullptr); // the *new* k survives
}

TEST(active_cycle_over_many_ticks_eventually_expires_every_due_key) {
    Database db;
    constexpr int kN = 1000;
    for (int i = 0; i < kN; ++i) {
        const std::string key = "k" + std::to_string(i);
        db.set(key, Value(std::string("v")));
        db.expire_at(key, kT0 + 10, kT0);
    }
    size_t total_expired = 0;
    for (int tick = 0; tick < 20 && db.size() > 0; ++tick) {
        total_expired += db.run_active_expiration_cycle(kT0 + 10, /*max_keys=*/200, /*max_wall_ms=*/1000.0);
    }
    CHECK_EQ(total_expired, static_cast<size_t>(kN));
    CHECK(db.empty());
}

TEST(compaction_shrinks_a_heap_bloated_with_stale_entries) {
    Database db;
    db.set("k", Value(std::string("v")));
    // re-expiring the same key never removes the old heap entry, so this is
    // the fastest way to bloat the heap without needing distinct keys
    for (int i = 0; i < 3000; ++i) {
        db.expire_at("k", kT0 + 1'000'000 + i, kT0); // far future, never due
    }
    CHECK_EQ(db.expiry_heap_size_for_testing(), size_t{3000});

    // compaction runs once per cycle even when nothing expires
    const size_t expired = db.run_active_expiration_cycle(kT0, /*max_keys=*/200, /*max_wall_ms=*/1000.0);
    CHECK_EQ(expired, size_t{0});
    CHECK_EQ(db.expiry_heap_size_for_testing(), size_t{1}); // rebuilt down to the 1 live TTL
    CHECK_EQ(db.pttl("k", kT0), int64_t{1'000'000 + 2999}); // the latest deadline survived the rebuild
}

TEST_MAIN()
