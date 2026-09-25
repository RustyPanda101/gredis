#include "persistence/snapshot.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "storage/database.h"
#include "storage/value.h"
#include "test_harness.h"

using gredis::Database;
using gredis::decode_snapshot;
using gredis::encode_snapshot;
using gredis::Empty;
using gredis::HashValue;
using gredis::ListValue;
using gredis::read_snapshot_file;
using gredis::SetValue;
using gredis::SnapshotLoadStatus;
using gredis::Value;
using gredis::write_snapshot_file;
using gredis::ZSet;

namespace {

constexpr int64_t kMono = 1'000'000;
constexpr int64_t kUnix = 1'700'000'000'000;

std::vector<uint8_t> load_fixture(const std::string& name) {
    std::ifstream f(std::string(GREDIS_SOURCE_DIR) + "/tests/data/" + name, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

} // namespace

TEST(round_trip_string_including_binary_data) {
    Database db;
    db.set("greeting", Value(std::string("hello world")));
    db.set("binary", Value(std::string("a\0b\r\n\xff", 6)));

    const auto bytes = encode_snapshot(db, kMono, kUnix);
    Database loaded;
    const auto result = decode_snapshot(bytes, loaded, kMono, kUnix);
    CHECK(result.status == SnapshotLoadStatus::Ok);
    CHECK_EQ(loaded.size(), size_t{2});

    Value* greeting = loaded.lookup_read("greeting", kMono);
    CHECK(greeting != nullptr);
    CHECK(std::get<std::string>(*greeting) == "hello world");

    Value* binary = loaded.lookup_read("binary", kMono);
    CHECK(binary != nullptr);
    CHECK(std::get<std::string>(*binary) == std::string("a\0b\r\n\xff", 6));
}

TEST(round_trip_hash_and_empty_hash) {
    Database db;
    auto h = std::make_unique<HashValue>();
    h->insert_or_assign("f1", "v1");
    h->insert_or_assign("f2", "v2");
    db.set("h", Value(std::move(h)));
    db.set("empty_h", Value(std::make_unique<HashValue>()));

    const auto bytes = encode_snapshot(db, kMono, kUnix);
    Database loaded;
    CHECK(decode_snapshot(bytes, loaded, kMono, kUnix).status == SnapshotLoadStatus::Ok);

    Value* h_loaded = loaded.lookup_read("h", kMono);
    CHECK(h_loaded != nullptr);
    HashValue& hv = *std::get<std::unique_ptr<HashValue>>(*h_loaded);
    CHECK_EQ(hv.size(), size_t{2});
    CHECK(*hv.find("f1") == "v1");
    CHECK(*hv.find("f2") == "v2");

    Value* empty_loaded = loaded.lookup_read("empty_h", kMono);
    CHECK(empty_loaded != nullptr);
    CHECK_EQ(std::get<std::unique_ptr<HashValue>>(*empty_loaded)->size(), size_t{0});
}

TEST(round_trip_list_preserves_order_and_empty_list) {
    Database db;
    auto l = std::make_unique<ListValue>();
    l->push_back("a");
    l->push_back("b");
    l->push_back("c");
    db.set("l", Value(std::move(l)));
    db.set("empty_l", Value(std::make_unique<ListValue>()));

    const auto bytes = encode_snapshot(db, kMono, kUnix);
    Database loaded;
    CHECK(decode_snapshot(bytes, loaded, kMono, kUnix).status == SnapshotLoadStatus::Ok);

    Value* l_loaded = loaded.lookup_read("l", kMono);
    CHECK(l_loaded != nullptr);
    ListValue& lv = *std::get<std::unique_ptr<ListValue>>(*l_loaded);
    CHECK_EQ(lv.size(), size_t{3});
    CHECK_EQ(lv[0], std::string("a"));
    CHECK_EQ(lv[1], std::string("b"));
    CHECK_EQ(lv[2], std::string("c"));

    Value* empty_loaded = loaded.lookup_read("empty_l", kMono);
    CHECK_EQ(std::get<std::unique_ptr<ListValue>>(*empty_loaded)->size(), size_t{0});
}

TEST(round_trip_set_and_empty_set) {
    Database db;
    auto s = std::make_unique<SetValue>();
    s->insert_or_assign("m1", Empty{});
    s->insert_or_assign("m2", Empty{});
    db.set("s", Value(std::move(s)));
    db.set("empty_s", Value(std::make_unique<SetValue>()));

    const auto bytes = encode_snapshot(db, kMono, kUnix);
    Database loaded;
    CHECK(decode_snapshot(bytes, loaded, kMono, kUnix).status == SnapshotLoadStatus::Ok);

    Value* s_loaded = loaded.lookup_read("s", kMono);
    CHECK(s_loaded != nullptr);
    SetValue& sv = *std::get<std::unique_ptr<SetValue>>(*s_loaded);
    CHECK_EQ(sv.size(), size_t{2});
    CHECK(sv.find("m1") != nullptr);
    CHECK(sv.find("m2") != nullptr);

    Value* empty_loaded = loaded.lookup_read("empty_s", kMono);
    CHECK_EQ(std::get<std::unique_ptr<SetValue>>(*empty_loaded)->size(), size_t{0});
}

TEST(round_trip_zset_preserves_scores_and_empty_zset) {
    Database db;
    auto z = std::make_unique<ZSet>();
    z->upsert("alice", 1.5);
    z->upsert("bob", -2.25);
    z->upsert("carol", 0.0);
    db.set("z", Value(std::move(z)));
    db.set("empty_z", Value(std::make_unique<ZSet>()));

    const auto bytes = encode_snapshot(db, kMono, kUnix);
    Database loaded;
    CHECK(decode_snapshot(bytes, loaded, kMono, kUnix).status == SnapshotLoadStatus::Ok);

    Value* z_loaded = loaded.lookup_read("z", kMono);
    CHECK(z_loaded != nullptr);
    ZSet& zv = *std::get<std::unique_ptr<ZSet>>(*z_loaded);
    CHECK_EQ(zv.size(), size_t{3});
    const auto alice_score = zv.score_of("alice");
    const auto bob_score = zv.score_of("bob");
    const auto carol_score = zv.score_of("carol");
    CHECK_EQ(*alice_score, 1.5);
    CHECK_EQ(*bob_score, -2.25);
    CHECK_EQ(*carol_score, 0.0);

    Value* empty_loaded = loaded.lookup_read("empty_z", kMono);
    CHECK_EQ(std::get<std::unique_ptr<ZSet>>(*empty_loaded)->size(), size_t{0});
}

TEST(ttl_survives_as_unix_ms_across_a_simulated_restart) {
    Database db;
    db.set("with_ttl", Value(std::string("v")));
    db.expire_at("with_ttl", kMono + 10'000, kMono); // 10s TTL
    db.set("no_ttl", Value(std::string("v2")));

    const auto bytes = encode_snapshot(db, kMono, kUnix);

    // simulated restart: monotonic clock resets to an unrelated origin,
    // only unix time is meaningfully "3s later"
    constexpr int64_t kRestartMono = 500;
    const int64_t restart_unix = kUnix + 3'000;

    Database loaded;
    CHECK(decode_snapshot(bytes, loaded, kRestartMono, restart_unix).status == SnapshotLoadStatus::Ok);

    // 10s TTL minus 3s elapsed = ~7s remaining
    const int64_t remaining = loaded.pttl("with_ttl", kRestartMono);
    CHECK(remaining > 6'000 && remaining <= 7'000);
    CHECK_EQ(loaded.pttl("no_ttl", kRestartMono), int64_t{-1}); // no TTL
}

TEST(encode_skips_keys_already_expired_at_save_time) {
    Database db;
    db.set("stale", Value(std::string("v")));
    db.expire_at("stale", kMono - 1, kMono - 100); // already past deadline but not lazily swept yet
    db.set("live", Value(std::string("v2")));

    const auto bytes = encode_snapshot(db, kMono, kUnix);
    Database loaded;
    CHECK(decode_snapshot(bytes, loaded, kMono, kUnix).status == SnapshotLoadStatus::Ok);
    CHECK_EQ(loaded.size(), size_t{1});
    CHECK(loaded.lookup_read("stale", kMono) == nullptr);
    CHECK(loaded.lookup_read("live", kMono) != nullptr);
}

TEST(decode_drops_keys_expired_while_the_file_sat_on_disk) {
    Database db;
    db.set("short_lived", Value(std::string("v")));
    db.expire_at("short_lived", kMono + 1'000, kMono); // 1s TTL at save time

    const auto bytes = encode_snapshot(db, kMono, kUnix);

    // decode as if 10 real seconds passed before the file was loaded
    Database loaded;
    const auto result = decode_snapshot(bytes, loaded, kMono, kUnix + 10'000);
    CHECK(result.status == SnapshotLoadStatus::Ok);
    CHECK(loaded.lookup_read("short_lived", kMono) == nullptr);
    CHECK_EQ(loaded.size(), size_t{0});
}

TEST(write_then_read_snapshot_file_round_trips) {
    Database db;
    db.set("k", Value(std::string("v")));
    const auto bytes = encode_snapshot(db, kMono, kUnix);

    const std::string path = "/tmp/gredis_test_snapshot_" + std::to_string(kMono) + ".grds";
    std::string error;
    CHECK(write_snapshot_file(path, bytes, error));

    std::vector<uint8_t> read_back;
    CHECK(read_snapshot_file(path, read_back, error));
    CHECK(read_back == bytes);

    Database loaded;
    CHECK(decode_snapshot(read_back, loaded, kMono, kUnix).status == SnapshotLoadStatus::Ok);
    CHECK(loaded.lookup_read("k", kMono) != nullptr);

    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
}

TEST(write_snapshot_file_to_an_unwritable_directory_fails_cleanly) {
    std::vector<uint8_t> bytes = {1, 2, 3};
    std::string error;
    const bool ok = write_snapshot_file("/no/such/directory/here/file.grds", bytes, error);
    CHECK(!ok);
    CHECK(!error.empty());
}

// one per tests/data/*.grds corruption fixture; running under ASan/UBSan
// is what actually proves these don't crash
void expect_fixture_rejected(const char* name) {
    const auto bytes = load_fixture(name);
    Database db;
    const auto result = decode_snapshot(bytes, db, kMono, kUnix);
    CHECK(result.status == SnapshotLoadStatus::Error);
    CHECK(!result.error.empty());
    CHECK(db.empty()); // never partially loaded
}

TEST(corrupt_zero_byte_file_is_rejected) {
    expect_fixture_rejected("zero_byte.grds");
}

TEST(corrupt_truncated_header_is_rejected) {
    expect_fixture_rejected("truncated_header.grds");
}

TEST(corrupt_bad_magic_is_rejected) {
    expect_fixture_rejected("bad_magic.grds");
}

TEST(corrupt_unsupported_version_is_rejected) {
    expect_fixture_rejected("unsupported_version.grds");
}

TEST(corrupt_flipped_byte_crc_fail_is_rejected) {
    expect_fixture_rejected("flipped_byte_crc_fail.grds");
}

TEST(corrupt_length_past_eof_is_rejected) {
    expect_fixture_rejected("length_past_eof.grds");
}

TEST(corrupt_unknown_type_tag_is_rejected) {
    expect_fixture_rejected("unknown_type_tag.grds");
}

TEST(corrupt_record_count_mismatch_is_rejected) {
    expect_fixture_rejected("record_count_mismatch.grds");
}

TEST_MAIN()
