#include "reply_parser.h"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace gredis::loadgen {

namespace {

// cap on how far ahead to search for a header line's CRLF, so a non-RESP
// peer gets an error instead of being scanned forever
constexpr size_t kMaxHeaderLineLen = 512;

std::optional<size_t> find_crlf(std::span<const char> in, size_t from) {
    const size_t limit = std::min(in.size(), from + kMaxHeaderLineLen + 2);
    for (size_t i = from; i + 1 < limit; ++i) {
        if (in[i] == '\r' && in[i + 1] == '\n') {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<int64_t> parse_i64(std::string_view s) {
    if (s.empty()) {
        return std::nullopt;
    }
    int64_t v = 0;
    const auto res = std::from_chars(s.data(), s.data() + s.size(), v);
    if (res.ec != std::errc{} || res.ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

// Parses one reply value starting at `pos`, writing how many bytes of
// `in` it consumed to `consumed_out` on Ok. Recurses for array elements.
ReplyParseStatus parse_value(std::span<const char> in, size_t pos, size_t& consumed_out) {
    if (pos >= in.size()) {
        return ReplyParseStatus::Incomplete;
    }

    const char type = in[pos];
    if (type != '+' && type != '-' && type != ':' && type != '$' && type != '*') {
        return ReplyParseStatus::Error;
    }

    const auto line_end_opt = find_crlf(in, pos + 1);
    if (!line_end_opt) {
        // genuinely incomplete, or header is past kMaxHeaderLineLen with
        // no CRLF -- either way Incomplete is fine, loadgen isn't hardened
        // against a hostile server
        return ReplyParseStatus::Incomplete;
    }
    const size_t line_end = *line_end_opt;
    const std::string_view line(in.data() + pos + 1, line_end - (pos + 1));

    if (type == '+' || type == '-' || type == ':') {
        consumed_out = (line_end + 2) - pos;
        return ReplyParseStatus::Ok;
    }

    // '$' (bulk string) or '*' (array): the line is a length/count.
    const auto n_opt = parse_i64(line);
    if (!n_opt || *n_opt < -1) {
        return ReplyParseStatus::Error;
    }
    const int64_t n = *n_opt;
    const size_t header_len = (line_end + 2) - pos;

    if (n == -1) {
        // Null bulk ($-1) or null array (*-1): the header is the whole value.
        consumed_out = header_len;
        return ReplyParseStatus::Ok;
    }

    if (type == '$') {
        const size_t body_start = line_end + 2;
        const size_t body_needed = static_cast<size_t>(n) + 2; // payload + trailing CRLF
        if (in.size() < body_start + body_needed) {
            return ReplyParseStatus::Incomplete;
        }
        if (in[body_start + static_cast<size_t>(n)] != '\r' ||
            in[body_start + static_cast<size_t>(n) + 1] != '\n') {
            return ReplyParseStatus::Error;
        }
        consumed_out = (body_start + body_needed) - pos;
        return ReplyParseStatus::Ok;
    }

    // '*': recurse for each of the n elements.
    size_t cursor = line_end + 2;
    for (int64_t i = 0; i < n; ++i) {
        size_t element_consumed = 0;
        const auto status = parse_value(in, cursor, element_consumed);
        if (status != ReplyParseStatus::Ok) {
            return status; // propagate Incomplete/Error as-is
        }
        cursor += element_consumed;
    }
    consumed_out = cursor - pos;
    return ReplyParseStatus::Ok;
}

} // namespace

ReplyParseResult parse_reply(std::span<const char> in) {
    ReplyParseResult result;
    size_t consumed = 0;
    result.status = parse_value(in, 0, consumed);
    if (result.status == ReplyParseStatus::Ok) {
        result.consumed = consumed;
        result.is_error = !in.empty() && in[0] == '-';
    }
    return result;
}

} // namespace gredis::loadgen
