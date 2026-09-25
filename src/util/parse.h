// strict numeric parsing for command args (INCR/INCRBY/EXPIRE/ZADD/...)
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace gredis {

// entire string must be consumed: no whitespace, no leading '+', no empty
// string, overflow rejected rather than clamped/wrapped
std::optional<int64_t> parse_int64(std::string_view s);

// ZSET score parsing: accepts finite decimals/exponents and inf/infinity
// (optional sign, case-insensitive) -- more permissive than parse_int64 about
// leading '+'. never accepts literal "nan" (ZINCRBY handles inf + -inf itself).
std::optional<double> parse_double(std::string_view s);

} // namespace gredis
