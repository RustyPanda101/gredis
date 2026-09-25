// RESP2 request parser: raw bytes -> one command (argv) at a time.
// pure, no socket/Connection/Buffer dependency, so it's unit-testable and fuzzable.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace gredis {

enum class RespParseStatus {
    Ok,         // a full command (possibly empty, see below) was parsed
    Incomplete, // not enough bytes yet; consumed is 0, try again once more arrives
    Error,      // bytes that can never be valid RESP; the connection must close
};

struct ParserLimits {
    size_t max_bulk_len = 64ULL * 1024 * 1024; // 64 MiB
    size_t max_array_len = 1'048'576;
    size_t max_header_line_len = 64; // bytes, e.g. "*3" or "$1048576" before CRLF
};

// Ok: argv holds the args. argv.empty() means input was a bare "*0\r\n" (valid
// no-op), caller just skips dispatch and parses again. consumed = bytes to consume().
// Incomplete: consumed is always 0, next call re-parses from the same start.
// Error: err is a plain string (not RESP-formatted yet), caller replies + closes,
// no way to resync a corrupted stream.
RespParseStatus parse_command(std::span<const char> in, std::vector<std::string>& argv,
                               size_t& consumed, std::string& err,
                               const ParserLimits& limits = ParserLimits{});

} // namespace gredis
