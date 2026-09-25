// RESP2 reply writer: append one reply value to an output Buffer. pure formatting.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "net/buffer.h"

namespace gredis::resp {

// "+<s>\r\n". s must not contain CR/LF -- only ever called with static text (OK, PONG)
void simple(Buffer& out, std::string_view s);

// "-<s>\r\n". CR/LF replaced with spaces first, since error text can echo back
// client input and must never inject an extra line into the reply stream
void error(Buffer& out, std::string_view s);

// ":<n>\r\n"
void integer(Buffer& out, int64_t n);

// "$<len>\r\n<s>\r\n" -- binary-safe
void bulk(Buffer& out, std::string_view s);

void null_bulk(Buffer& out); // "$-1\r\n"

// "*<n>\r\n" -- header only, caller writes n reply values right after
void array_header(Buffer& out, size_t n);

void null_array(Buffer& out); // "*-1\r\n"

// RESP2 has no double type, so scores go out as bulk strings. shortest
// round-tripping representation (integral scores print as "1" not "1.000...").
// inf/-inf print as such; NaN prints "nan" (shouldn't reach here, scores are
// validated on input) instead of relying on unspecified to_chars behavior.
void double_as_bulk(Buffer& out, double d);

} // namespace gredis::resp
