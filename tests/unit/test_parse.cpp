#include "util/parse.h"

#include <cmath>
#include <cstdint>
#include <limits>

#include "test_harness.h"

using gredis::parse_double;
using gredis::parse_int64;

TEST(parses_plain_positive_and_negative) {
    // named locals, not CHECK_EQ(*parse_int64(...), ...) directly -- binding
    // auto&& to a temporary optional's dereference doesn't extend its
    // lifetime, ASan caught this as a stack-use-after-scope
    const auto a = parse_int64("0");
    const auto b = parse_int64("42");
    const auto c = parse_int64("-42");
    const auto d = parse_int64("-0");
    CHECK_EQ(*a, 0);
    CHECK_EQ(*b, 42);
    CHECK_EQ(*c, -42);
    CHECK_EQ(*d, 0);
}

TEST(parses_int64_limits) {
    const auto max_val = parse_int64("9223372036854775807");
    const auto min_val = parse_int64("-9223372036854775808");
    CHECK_EQ(*max_val, std::numeric_limits<int64_t>::max());
    CHECK_EQ(*min_val, std::numeric_limits<int64_t>::min());
}

TEST(rejects_overflow) {
    CHECK(!parse_int64("9223372036854775808").has_value());
    CHECK(!parse_int64("-9223372036854775809").has_value());
    CHECK(!parse_int64("99999999999999999999999").has_value());
}

TEST(rejects_empty_and_whitespace) {
    CHECK(!parse_int64("").has_value());
    CHECK(!parse_int64(" ").has_value());
    CHECK(!parse_int64(" 1").has_value());
    CHECK(!parse_int64("1 ").has_value());
    CHECK(!parse_int64("\t5").has_value());
}

TEST(rejects_leading_plus) {
    CHECK(!parse_int64("+1").has_value());
    CHECK(!parse_int64("+0").has_value());
}

TEST(rejects_junk_and_partial_numbers) {
    CHECK(!parse_int64("abc").has_value());
    CHECK(!parse_int64("1abc").has_value());
    CHECK(!parse_int64("abc1").has_value());
    CHECK(!parse_int64("1.5").has_value());
    CHECK(!parse_int64("--1").has_value());
    CHECK(!parse_int64("1e10").has_value());
}

TEST(parse_double_plain_and_exponent) {
    const auto a = parse_double("0");
    const auto b = parse_double("1.5");
    const auto c = parse_double("-1.5");
    const auto d = parse_double("1e300");
    const auto e = parse_double("-2.5e-10");
    CHECK_EQ(*a, 0.0);
    CHECK_EQ(*b, 1.5);
    CHECK_EQ(*c, -1.5);
    CHECK_EQ(*d, 1e300);
    CHECK_EQ(*e, -2.5e-10);
}

TEST(parse_double_accepts_leading_plus) {
    // unlike parse_int64, scores accept a leading '+'
    const auto a = parse_double("+5.5");
    CHECK_EQ(*a, 5.5);
}

TEST(parse_double_accepts_inf_case_insensitive) {
    const auto a = parse_double("inf");
    const auto b = parse_double("+inf");
    const auto c = parse_double("-inf");
    const auto d = parse_double("INF");
    const auto e = parse_double("Infinity");
    const auto f = parse_double("-INFINITY");
    CHECK(std::isinf(*a) && *a > 0);
    CHECK(std::isinf(*b) && *b > 0);
    CHECK(std::isinf(*c) && *c < 0);
    CHECK(std::isinf(*d) && *d > 0);
    CHECK(std::isinf(*e) && *e > 0);
    CHECK(std::isinf(*f) && *f < 0);
}

TEST(parse_double_rejects_nan_empty_and_junk) {
    CHECK(!parse_double("").has_value());
    CHECK(!parse_double("nan").has_value());
    CHECK(!parse_double("NaN").has_value());
    CHECK(!parse_double("-nan").has_value());
    CHECK(!parse_double("abc").has_value());
    CHECK(!parse_double("1.2.3").has_value());
    CHECK(!parse_double("1.5abc").has_value());
    CHECK(!parse_double(" 1.5").has_value());
    CHECK(!parse_double("1.5 ").has_value());
    CHECK(!parse_double("+").has_value());
    CHECK(!parse_double("-").has_value());
}

TEST_MAIN()
