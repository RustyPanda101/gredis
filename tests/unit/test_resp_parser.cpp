#include "test_harness.h"

#include <span>
#include <string>
#include <vector>

#include "protocol/resp_parser.h"

using gredis::RespParseStatus;
using gredis::parse_command;
using gredis::ParserLimits;

namespace {

std::span<const char> as_span(const std::string& s) {
    return std::span<const char>(s.data(), s.size());
}

// feeds `full` byte by byte, checking Incomplete at every prefix boundary,
// then checks the complete message parses to Ok with the expected argv
void check_prefix_then_complete(const std::string& full, const std::vector<std::string>& expected_argv) {
    for (size_t p = 0; p < full.size(); ++p) {
        std::vector<std::string> argv;
        size_t consumed = 999;
        std::string err;
        const std::string prefix = full.substr(0, p);
        const auto status = parse_command(as_span(prefix), argv, consumed, err);
        CHECK(status == RespParseStatus::Incomplete);
        CHECK_EQ(consumed, static_cast<size_t>(0));
    }

    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const auto status = parse_command(as_span(full), argv, consumed, err);
    CHECK(status == RespParseStatus::Ok);
    CHECK_EQ(consumed, full.size());
    CHECK(argv == expected_argv);
}

} // namespace

TEST(single_command) {
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const std::string msg = "*1\r\n$4\r\nPING\r\n";
    const auto status = parse_command(as_span(msg), argv, consumed, err);
    CHECK(status == RespParseStatus::Ok);
    CHECK_EQ(consumed, msg.size());
    CHECK_EQ(argv.size(), static_cast<size_t>(1));
    CHECK_EQ(argv[0], std::string("PING"));
}

TEST(pipelined_commands_parsed_one_at_a_time) {
    const std::string msg = "*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n";
    std::span<const char> remaining(msg.data(), msg.size());
    int count = 0;
    while (!remaining.empty()) {
        std::vector<std::string> argv;
        size_t consumed = 0;
        std::string err;
        const auto status = parse_command(remaining, argv, consumed, err);
        CHECK(status == RespParseStatus::Ok);
        CHECK_EQ(argv.size(), static_cast<size_t>(1));
        CHECK_EQ(argv[0], std::string("PING"));
        remaining = remaining.subspan(consumed);
        ++count;
    }
    CHECK_EQ(count, 3);
}

TEST(multi_arg_command) {
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const std::string msg = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
    const auto status = parse_command(as_span(msg), argv, consumed, err);
    CHECK(status == RespParseStatus::Ok);
    CHECK_EQ(consumed, msg.size());
    const std::vector<std::string> expected = {"SET", "foo", "bar"};
    CHECK(argv == expected);
}

TEST(zero_length_bulk_is_a_valid_empty_string) {
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const std::string msg = "*2\r\n$4\r\nECHO\r\n$0\r\n\r\n";
    const auto status = parse_command(as_span(msg), argv, consumed, err);
    CHECK(status == RespParseStatus::Ok);
    CHECK_EQ(consumed, msg.size());
    CHECK_EQ(argv.size(), static_cast<size_t>(2));
    CHECK_EQ(argv[0], std::string("ECHO"));
    CHECK(argv[1].empty());
}

TEST(empty_array_is_ok_with_no_command) {
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const std::string msg = "*0\r\n";
    const auto status = parse_command(as_span(msg), argv, consumed, err);
    CHECK(status == RespParseStatus::Ok);
    CHECK_EQ(consumed, msg.size());
    CHECK(argv.empty());
}

TEST(empty_array_followed_by_a_real_command) {
    // caller is expected to skip the empty-argv Ok result and parse again
    const std::string msg = "*0\r\n*1\r\n$4\r\nPING\r\n";
    std::span<const char> remaining(msg.data(), msg.size());

    std::vector<std::string> argv1;
    size_t consumed1 = 0;
    std::string err1;
    CHECK(parse_command(remaining, argv1, consumed1, err1) == RespParseStatus::Ok);
    CHECK(argv1.empty());
    remaining = remaining.subspan(consumed1);

    std::vector<std::string> argv2;
    size_t consumed2 = 0;
    std::string err2;
    CHECK(parse_command(remaining, argv2, consumed2, err2) == RespParseStatus::Ok);
    CHECK_EQ(argv2.size(), static_cast<size_t>(1));
    CHECK_EQ(argv2[0], std::string("PING"));
    CHECK_EQ(consumed2, remaining.size());
}

TEST(binary_safe_payload_with_crlf_and_nul) {
    const char raw[] = {'a', '\r', '\n', '\0', 'b'};
    const std::string payload(raw, sizeof(raw));
    const std::string msg = "*2\r\n$4\r\nECHO\r\n$" + std::to_string(payload.size()) + "\r\n" + payload + "\r\n";

    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const auto status = parse_command(as_span(msg), argv, consumed, err);
    CHECK(status == RespParseStatus::Ok);
    CHECK_EQ(consumed, msg.size());
    CHECK_EQ(argv.size(), static_cast<size_t>(2));
    CHECK_EQ(argv[1].size(), payload.size());
    CHECK(argv[1] == payload);
}

TEST(prefix_split_single_arg) {
    check_prefix_then_complete("*1\r\n$4\r\nPING\r\n", {"PING"});
}

TEST(prefix_split_multi_arg) {
    check_prefix_then_complete("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", {"SET", "foo", "bar"});
}

TEST(prefix_split_empty_bulk) {
    check_prefix_then_complete("*2\r\n$4\r\nECHO\r\n$0\r\n\r\n", {"ECHO", ""});
}

TEST(prefix_split_varying_lengths) {
    check_prefix_then_complete("*4\r\n$4\r\nMSET\r\n$1\r\na\r\n$1\r\n1\r\n$5\r\nhello\r\n",
                                {"MSET", "a", "1", "hello"});
}

TEST(prefix_split_single_element_array) {
    check_prefix_then_complete("*1\r\n$6\r\nfoobar\r\n", {"foobar"});
}

TEST(bad_type_byte_is_error) {
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const std::string msg = "#3\r\n";
    const auto status = parse_command(as_span(msg), argv, consumed, err);
    CHECK(status == RespParseStatus::Error);
    CHECK_EQ(consumed, static_cast<size_t>(0));
    CHECK(!err.empty());
}

TEST(negative_multibulk_length_is_error) {
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const std::string msg = "*-5\r\n";
    const auto status = parse_command(as_span(msg), argv, consumed, err);
    CHECK(status == RespParseStatus::Error);
}

TEST(negative_one_multibulk_length_is_error) {
    // unlike a reply, where *-1 means a null array, a request has no
    // meaning for a negative array count
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const std::string msg = "*-1\r\n";
    const auto status = parse_command(as_span(msg), argv, consumed, err);
    CHECK(status == RespParseStatus::Error);
}

TEST(negative_bulk_length_inside_request_is_error) {
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const std::string msg = "*1\r\n$-1\r\n";
    const auto status = parse_command(as_span(msg), argv, consumed, err);
    CHECK(status == RespParseStatus::Error);
}

TEST(non_numeric_multibulk_length_is_error) {
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const std::string msg = "*abc\r\n";
    const auto status = parse_command(as_span(msg), argv, consumed, err);
    CHECK(status == RespParseStatus::Error);
}

TEST(non_numeric_bulk_length_is_error) {
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const std::string msg = "*1\r\n$xy\r\n";
    const auto status = parse_command(as_span(msg), argv, consumed, err);
    CHECK(status == RespParseStatus::Error);
}

TEST(bulk_length_overflow_is_error) {
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const std::string msg = "*1\r\n$99999999999999999999\r\nx\r\n";
    const auto status = parse_command(as_span(msg), argv, consumed, err);
    CHECK(status == RespParseStatus::Error);
}

TEST(unexpected_type_byte_for_array_element_is_error) {
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const std::string msg = "*1\r\n#4\r\nPING\r\n";
    const auto status = parse_command(as_span(msg), argv, consumed, err);
    CHECK(status == RespParseStatus::Error);
}

TEST(missing_crlf_after_bulk_payload_is_error) {
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const std::string msg = "*1\r\n$4\r\nPINGXX"; // declares 4 bytes, but no CRLF follows them
    const auto status = parse_command(as_span(msg), argv, consumed, err);
    CHECK(status == RespParseStatus::Error);
}

TEST(oversized_bulk_length_is_error) {
    ParserLimits limits;
    limits.max_bulk_len = 10;
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const std::string msg = "*1\r\n$11\r\nhello world\r\n";
    const auto status = parse_command(as_span(msg), argv, consumed, err, limits);
    CHECK(status == RespParseStatus::Error);
}

TEST(oversized_array_length_is_error) {
    ParserLimits limits;
    limits.max_array_len = 2;
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const std::string msg = "*3\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n";
    const auto status = parse_command(as_span(msg), argv, consumed, err, limits);
    CHECK(status == RespParseStatus::Error);
}

TEST(header_line_longer_than_limit_without_crlf_is_error) {
    ParserLimits limits;
    limits.max_header_line_len = 8;
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    // "*" followed by 20 digits and no CRLF within the 8-byte window.
    const std::string msg = "*12345678901234567890";
    const auto status = parse_command(as_span(msg), argv, consumed, err, limits);
    CHECK(status == RespParseStatus::Error);
}

TEST(header_line_within_limit_but_not_yet_arrived_is_incomplete) {
    ParserLimits limits;
    limits.max_header_line_len = 64;
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    // well under the header-line limit, no CRLF yet -- must be Incomplete
    const std::string msg = "*123";
    const auto status = parse_command(as_span(msg), argv, consumed, err, limits);
    CHECK(status == RespParseStatus::Incomplete);
}

TEST(leading_plus_sign_is_error) {
    std::vector<std::string> argv;
    size_t consumed = 0;
    std::string err;
    const std::string msg = "*1\r\n$+4\r\nPING\r\n";
    const auto status = parse_command(as_span(msg), argv, consumed, err);
    CHECK(status == RespParseStatus::Error);
}

TEST_MAIN()
