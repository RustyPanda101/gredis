#include "protocol/resp_parser.h"

#include <algorithm>
#include <charconv>
#include <string_view>

namespace gredis {

namespace {

using std::string_view;

constexpr string_view kCRLF = "\r\n";

// searches only data[start, start+limit); npos if no match in that window.
// caller tells "not enough bytes yet" from "line too long" by comparing
// data.size() against start+limit.
size_t find_crlf_bounded(string_view data, size_t start, size_t limit) {
    if (start >= data.size()) {
        return string_view::npos;
    }
    const size_t window_end = std::min(data.size(), start + limit);
    const string_view window = data.substr(start, window_end - start);
    const size_t rel = window.find(kCRLF);
    if (rel == string_view::npos) {
        return string_view::npos;
    }
    return start + rel;
}

// whole range must be consumed by the number; from_chars already rejects
// leading '+' and reports overflow as failure rather than UB
bool parse_signed_line(string_view data, size_t start, size_t end, long long& out) {
    if (start >= end) {
        return false; // empty "*<CRLF>" or "$<CRLF>" with no digits
    }
    const char* begin = data.data() + start;
    const char* stop = data.data() + end;
    const auto [ptr, ec] = std::from_chars(begin, stop, out);
    return ec == std::errc{} && ptr == stop;
}

// a header line like "*3\r\n" or "$512\r\n"; marker_pos is the marker char's
// index (caller already validated it), digits start right after
struct HeaderLine {
    RespParseStatus status;
    long long value = 0; // meaningful only if status == Ok
    size_t next_pos = 0; // position right after the CRLF; meaningful only if status == Ok
};

HeaderLine parse_header_line(string_view data, size_t marker_pos, size_t max_line_len,
                              std::string& err, const char* what) {
    const size_t digits_start = marker_pos + 1;
    const size_t crlf_pos = find_crlf_bounded(data, digits_start, max_line_len);
    if (crlf_pos == string_view::npos) {
        if (data.size() >= digits_start + max_line_len) {
            err = std::string("protocol error: ") + what + " line too long";
            return {RespParseStatus::Error};
        }
        return {RespParseStatus::Incomplete};
    }
    long long value = 0;
    if (!parse_signed_line(data, digits_start, crlf_pos, value)) {
        err = std::string("protocol error: invalid ") + what + " length";
        return {RespParseStatus::Error};
    }
    return {RespParseStatus::Ok, value, crlf_pos + kCRLF.size()};
}

} // namespace

RespParseStatus parse_command(std::span<const char> in, std::vector<std::string>& argv,
                               size_t& consumed, std::string& err, const ParserLimits& limits) {
    argv.clear();
    consumed = 0;
    err.clear();

    const string_view data(in.data(), in.size());

    if (data.empty()) {
        return RespParseStatus::Incomplete;
    }
    if (data[0] != '*') {
        err = "protocol error: expected '*', got '" + std::string(1, data[0]) + "'";
        return RespParseStatus::Error;
    }

    const HeaderLine array_header =
        parse_header_line(data, 0, limits.max_header_line_len, err, "multibulk");
    if (array_header.status != RespParseStatus::Ok) {
        return array_header.status;
    }
    if (array_header.value < 0) {
        err = "protocol error: invalid multibulk length";
        return RespParseStatus::Error;
    }
    const size_t count = static_cast<size_t>(array_header.value);
    if (count > limits.max_array_len) {
        err = "protocol error: invalid multibulk length: too large";
        return RespParseStatus::Error;
    }

    size_t pos = array_header.next_pos;

    if (count == 0) {
        // bare "*0\r\n": valid no-op, consume it, report no command
        consumed = pos;
        return RespParseStatus::Ok;
    }

    std::vector<std::string> parsed;
    parsed.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        if (pos >= data.size()) {
            return RespParseStatus::Incomplete;
        }
        if (data[pos] != '$') {
            err = "protocol error: expected '$', got '" + std::string(1, data[pos]) + "'";
            return RespParseStatus::Error;
        }

        const HeaderLine bulk_header =
            parse_header_line(data, pos, limits.max_header_line_len, err, "bulk");
        if (bulk_header.status != RespParseStatus::Ok) {
            return bulk_header.status;
        }
        if (bulk_header.value < 0) {
            err = "protocol error: invalid bulk length";
            return RespParseStatus::Error;
        }
        const size_t len = static_cast<size_t>(bulk_header.value);
        if (len > limits.max_bulk_len) {
            err = "protocol error: invalid bulk length: too large";
            return RespParseStatus::Error;
        }

        const size_t payload_start = bulk_header.next_pos;
        // check declared length against available bytes before touching any payload
        // byte -- keeps a huge bulk arriving in fragments from being rescanned each attempt
        if (data.size() < payload_start + len + kCRLF.size()) {
            return RespParseStatus::Incomplete;
        }

        if (data[payload_start + len] != '\r' || data[payload_start + len + 1] != '\n') {
            err = "protocol error: missing CRLF after bulk string";
            return RespParseStatus::Error;
        }

        parsed.emplace_back(data.substr(payload_start, len));
        pos = payload_start + len + kCRLF.size();
    }

    argv = std::move(parsed);
    consumed = pos;
    return RespParseStatus::Ok;
}

} // namespace gredis
