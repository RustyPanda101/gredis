// minimal RESP2 reply parser for loadgen. gredis_core's resp_parser.h only
// parses requests (bulk-string arrays), not the simple-string/error/int/
// array shapes servers reply with, so this handles those instead. Only
// reports completeness and byte length -- loadgen never needs the actual value.
#pragma once

#include <cstddef>
#include <span>

namespace gredis::loadgen {

enum class ReplyParseStatus {
    Ok,         // one full reply value was parsed
    Incomplete, // not enough bytes yet; consumed is 0, retry once more arrives
    Error,      // bytes that can never be a valid RESP2 reply
};

struct ReplyParseResult {
    ReplyParseStatus status = ReplyParseStatus::Incomplete;
    size_t consumed = 0;
    bool is_error = false; // meaningful only when status == Ok
};

// Incomplete consumes nothing; caller re-parses from the same start once
// more bytes arrive. Replies are small enough that re-scanning is cheap.
ReplyParseResult parse_reply(std::span<const char> in);

} // namespace gredis::loadgen
