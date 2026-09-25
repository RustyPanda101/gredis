// tests loadgen's reply parser, mirrors test_resp_parser.cpp's prefix-split discipline
#include "reply_parser.h"

#include <span>
#include <string>

#include "test_harness.h"

using gredis::loadgen::parse_reply;
using gredis::loadgen::ReplyParseStatus;

namespace {

std::span<const char> as_span(const std::string& s) {
    return std::span<const char>(s.data(), s.size());
}

void check_prefix_then_complete(const std::string& full, bool expect_error = false) {
    for (size_t p = 0; p < full.size(); ++p) {
        const auto result = parse_reply(as_span(full.substr(0, p)));
        CHECK(result.status == ReplyParseStatus::Incomplete);
        CHECK_EQ(result.consumed, static_cast<size_t>(0));
    }
    const auto result = parse_reply(as_span(full));
    CHECK(result.status == ReplyParseStatus::Ok);
    CHECK_EQ(result.consumed, full.size());
    CHECK_EQ(result.is_error, expect_error);
}

} // namespace

TEST(simple_string_reply) {
    check_prefix_then_complete("+OK\r\n");
}

TEST(error_reply_sets_is_error) {
    check_prefix_then_complete("-ERR wrong number of arguments\r\n", /*expect_error=*/true);
}

TEST(integer_reply) {
    check_prefix_then_complete(":12345\r\n");
    check_prefix_then_complete(":-1\r\n");
}

TEST(bulk_string_reply) {
    check_prefix_then_complete("$5\r\nhello\r\n");
}

TEST(empty_bulk_string_reply) {
    check_prefix_then_complete("$0\r\n\r\n");
}

TEST(null_bulk_reply) {
    check_prefix_then_complete("$-1\r\n");
}

TEST(binary_safe_bulk_string_with_embedded_crlf_and_nul) {
    const std::string payload("a\r\n\0b", 5);
    const std::string msg = "$5\r\n" + payload + "\r\n";
    check_prefix_then_complete(msg);
}

TEST(null_array_reply) {
    check_prefix_then_complete("*-1\r\n");
}

TEST(empty_array_reply) {
    check_prefix_then_complete("*0\r\n");
}

TEST(array_of_bulk_strings_like_hgetall_or_zrange) {
    check_prefix_then_complete("*4\r\n$5\r\nfield\r\n$5\r\nvalue\r\n$1\r\na\r\n$1\r\nb\r\n");
}

TEST(nested_array_of_mixed_types) {
    check_prefix_then_complete("*2\r\n:1\r\n*2\r\n+OK\r\n$-1\r\n");
}

TEST(two_pipelined_replies_only_the_first_is_consumed) {
    const std::string msg = "+OK\r\n:42\r\n";
    const auto result = parse_reply(as_span(msg));
    CHECK(result.status == ReplyParseStatus::Ok);
    CHECK_EQ(result.consumed, static_cast<size_t>(5)); // just "+OK\r\n"
    CHECK_EQ(result.is_error, false);

    const auto second = parse_reply(as_span(msg.substr(result.consumed)));
    CHECK(second.status == ReplyParseStatus::Ok);
    CHECK_EQ(second.consumed, msg.size() - result.consumed);
}

TEST(unknown_type_byte_is_an_error) {
    const auto result = parse_reply(as_span(std::string("@nope\r\n")));
    CHECK(result.status == ReplyParseStatus::Error);
}

TEST(bulk_length_not_a_number_is_an_error) {
    const auto result = parse_reply(as_span(std::string("$abc\r\nxxx\r\n")));
    CHECK(result.status == ReplyParseStatus::Error);
}

TEST(bulk_missing_trailing_crlf_is_an_error) {
    const auto result = parse_reply(as_span(std::string("$3\r\nabcXX")));
    CHECK(result.status == ReplyParseStatus::Error);
}

TEST(bulk_length_below_negative_one_is_an_error) {
    const auto result = parse_reply(as_span(std::string("$-2\r\n")));
    CHECK(result.status == ReplyParseStatus::Error);
}

TEST(empty_input_is_incomplete) {
    const auto result = parse_reply(std::span<const char>());
    CHECK(result.status == ReplyParseStatus::Incomplete);
    CHECK_EQ(result.consumed, static_cast<size_t>(0));
}

TEST_MAIN()
