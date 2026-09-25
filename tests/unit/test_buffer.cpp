#include "test_harness.h"

#include <cstring>
#include <string>

#include "net/buffer.h"

using gredis::Buffer;

TEST(empty_buffer_has_no_readable_bytes) {
    Buffer buf;
    CHECK(buf.empty());
    CHECK_EQ(buf.size(), static_cast<size_t>(0));
    CHECK(buf.readable().empty());
}

TEST(append_and_readable_roundtrip) {
    Buffer buf;
    buf.append("hello, ");
    buf.append("world");
    CHECK_EQ(buf.size(), static_cast<size_t>(12));
    CHECK_EQ(std::string(buf.readable()), std::string("hello, world"));
}

TEST(append_is_binary_safe) {
    Buffer buf;
    const char data[] = {'a', '\0', 'b', '\r', '\n', 'c'};
    buf.append(data, sizeof(data));
    CHECK_EQ(buf.size(), sizeof(data));
    const auto r = buf.readable();
    CHECK(std::memcmp(r.data(), data, sizeof(data)) == 0);
}

TEST(consume_partial_leaves_the_remainder_readable) {
    Buffer buf;
    buf.append("abcdef");
    buf.consume(2);
    CHECK_EQ(buf.size(), static_cast<size_t>(4));
    CHECK_EQ(std::string(buf.readable()), std::string("cdef"));
}

TEST(consume_to_completion_empties_for_free) {
    Buffer buf;
    buf.append("abc");
    buf.consume(3);
    CHECK(buf.empty());
    CHECK_EQ(buf.size(), static_cast<size_t>(0));

    buf.append("xyz");
    CHECK_EQ(std::string(buf.readable()), std::string("xyz"));
}

TEST(prepare_write_and_commit_written_full_amount) {
    Buffer buf;
    char* dst = buf.prepare_write(5);
    std::memcpy(dst, "abcde", 5);
    buf.commit_written(5);
    CHECK_EQ(buf.size(), static_cast<size_t>(5));
    CHECK_EQ(std::string(buf.readable()), std::string("abcde"));
}

TEST(prepare_write_and_commit_written_short_amount) {
    // simulates a short recv(): only commit what was actually written,
    // rest of the reserved space must not leak into readable()
    Buffer buf;
    char* dst = buf.prepare_write(10);
    std::memcpy(dst, "abc", 3);
    buf.commit_written(3);
    CHECK_EQ(buf.size(), static_cast<size_t>(3));
    CHECK_EQ(std::string(buf.readable()), std::string("abc"));
}

TEST(prepare_write_after_existing_data_appends_after_it) {
    Buffer buf;
    buf.append("pre-");
    char* dst = buf.prepare_write(4);
    std::memcpy(dst, "post", 4);
    buf.commit_written(4);
    CHECK_EQ(std::string(buf.readable()), std::string("pre-post"));
}

TEST(commit_written_zero_discards_the_reservation) {
    // simulates recv() returning EAGAIN after space was already reserved
    Buffer buf;
    buf.append("kept");
    buf.prepare_write(100);
    buf.commit_written(0);
    CHECK_EQ(buf.size(), static_cast<size_t>(4));
    CHECK_EQ(std::string(buf.readable()), std::string("kept"));
}

TEST(compaction_preserves_unread_bytes_across_a_reclaim) {
    Buffer buf;
    const std::string big(8192, 'a');
    buf.append(big);

    // read cursor now way past half of capacity, next prepare_write() must compact
    buf.consume(big.size() - 10);
    CHECK_EQ(buf.size(), static_cast<size_t>(10));
    CHECK_EQ(std::string(buf.readable()), big.substr(big.size() - 10));

    char* dst = buf.prepare_write(4);
    std::memcpy(dst, "bbbb", 4);
    buf.commit_written(4);

    CHECK_EQ(buf.size(), static_cast<size_t>(14));
    CHECK_EQ(std::string(buf.readable()), big.substr(big.size() - 10) + "bbbb");
}

TEST(steady_state_traffic_does_not_grow_capacity_unbounded) {
    // read cursor drifts forward every round without ever fully draining
    // (never hits consume()'s empty fast path) -- without compaction
    // capacity would grow with the number of rounds, not the live bytes
    Buffer buf;
    buf.append(std::string(10, 'x'));
    for (int i = 0; i < 200000; ++i) {
        buf.append("y");
        buf.consume(1);
        CHECK_EQ(buf.size(), static_cast<size_t>(10));
    }
    CHECK(buf.capacity() < static_cast<size_t>(4096));
}

TEST_MAIN()
