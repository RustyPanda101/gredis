#include "test_harness.h"

#include <limits>
#include <string>

#include "net/buffer.h"
#include "protocol/resp_writer.h"

using gredis::Buffer;
namespace resp = gredis::resp;

namespace {
std::string readable_str(const Buffer& b) {
    return std::string(b.readable());
}
} // namespace

TEST(simple_string_exact_bytes) {
    Buffer out;
    resp::simple(out, "OK");
    CHECK_EQ(readable_str(out), std::string("+OK\r\n"));
}

TEST(error_exact_bytes) {
    Buffer out;
    resp::error(out, "ERR unknown command");
    CHECK_EQ(readable_str(out), std::string("-ERR unknown command\r\n"));
}

TEST(error_sanitizes_embedded_crlf) {
    Buffer out;
    resp::error(out, "ERR bad\r\nINJECTED\r\n");
    CHECK_EQ(readable_str(out), std::string("-ERR bad  INJECTED  \r\n"));
}

TEST(integer_positive) {
    Buffer out;
    resp::integer(out, 42);
    CHECK_EQ(readable_str(out), std::string(":42\r\n"));
}

TEST(integer_negative) {
    Buffer out;
    resp::integer(out, -17);
    CHECK_EQ(readable_str(out), std::string(":-17\r\n"));
}

TEST(integer_zero) {
    Buffer out;
    resp::integer(out, 0);
    CHECK_EQ(readable_str(out), std::string(":0\r\n"));
}

TEST(integer_int64_extremes) {
    Buffer out;
    resp::integer(out, std::numeric_limits<int64_t>::max());
    resp::integer(out, std::numeric_limits<int64_t>::min());
    const std::string expected = ":9223372036854775807\r\n:-9223372036854775808\r\n";
    CHECK_EQ(readable_str(out), expected);
}

TEST(bulk_string_exact_bytes) {
    Buffer out;
    resp::bulk(out, "hello");
    CHECK_EQ(readable_str(out), std::string("$5\r\nhello\r\n"));
}

TEST(bulk_string_empty) {
    Buffer out;
    resp::bulk(out, "");
    CHECK_EQ(readable_str(out), std::string("$0\r\n\r\n"));
}

TEST(bulk_string_is_binary_safe) {
    Buffer out;
    const char raw[] = {'a', '\0', 'b', '\r', '\n', 'c'};
    resp::bulk(out, std::string_view(raw, sizeof(raw)));
    const std::string expected = "$6\r\n" + std::string(raw, sizeof(raw)) + "\r\n";
    CHECK_EQ(readable_str(out), expected);
}

TEST(null_bulk_exact_bytes) {
    Buffer out;
    resp::null_bulk(out);
    CHECK_EQ(readable_str(out), std::string("$-1\r\n"));
}

TEST(array_header_exact_bytes) {
    Buffer out;
    resp::array_header(out, 3);
    CHECK_EQ(readable_str(out), std::string("*3\r\n"));
}

TEST(array_header_zero) {
    Buffer out;
    resp::array_header(out, 0);
    CHECK_EQ(readable_str(out), std::string("*0\r\n"));
}

TEST(null_array_exact_bytes) {
    Buffer out;
    resp::null_array(out);
    CHECK_EQ(readable_str(out), std::string("*-1\r\n"));
}

TEST(nested_array_with_mixed_elements) {
    Buffer out;
    resp::array_header(out, 3);
    resp::bulk(out, "foo");
    resp::integer(out, 7);
    resp::null_bulk(out);
    const std::string expected = "*3\r\n$3\r\nfoo\r\n:7\r\n$-1\r\n";
    CHECK_EQ(readable_str(out), expected);
}

TEST(nested_array_of_arrays) {
    Buffer out;
    resp::array_header(out, 2);
    resp::array_header(out, 2);
    resp::bulk(out, "a");
    resp::bulk(out, "b");
    resp::null_array(out);
    const std::string expected = "*2\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n*-1\r\n";
    CHECK_EQ(readable_str(out), expected);
}

// std::to_chars output verified empirically before writing these expectations

TEST(double_integral_value_has_no_trailing_zeroes) {
    Buffer out;
    resp::double_as_bulk(out, 1.0);
    CHECK_EQ(readable_str(out), std::string("$1\r\n1\r\n"));
}

TEST(double_fractional_value) {
    Buffer out;
    resp::double_as_bulk(out, 1.5);
    CHECK_EQ(readable_str(out), std::string("$3\r\n1.5\r\n"));
}

TEST(double_negative_zero) {
    Buffer out;
    resp::double_as_bulk(out, -0.0);
    CHECK_EQ(readable_str(out), std::string("$2\r\n-0\r\n"));
}

TEST(double_positive_infinity) {
    Buffer out;
    resp::double_as_bulk(out, std::numeric_limits<double>::infinity());
    CHECK_EQ(readable_str(out), std::string("$3\r\ninf\r\n"));
}

TEST(double_negative_infinity) {
    Buffer out;
    resp::double_as_bulk(out, -std::numeric_limits<double>::infinity());
    CHECK_EQ(readable_str(out), std::string("$4\r\n-inf\r\n"));
}

TEST(double_large_value) {
    Buffer out;
    resp::double_as_bulk(out, 1e300);
    CHECK_EQ(readable_str(out), std::string("$6\r\n1e+300\r\n"));
}

TEST(double_nan_does_not_crash_and_produces_well_formed_bulk) {
    Buffer out;
    resp::double_as_bulk(out, std::numeric_limits<double>::quiet_NaN());
    CHECK_EQ(readable_str(out), std::string("$3\r\nnan\r\n"));
}

TEST_MAIN()
