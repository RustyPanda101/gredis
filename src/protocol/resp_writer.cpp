#include "protocol/resp_writer.h"

#include <charconv>
#include <cmath>
#include <string>

namespace gredis::resp {

namespace {

std::string sanitize_error_text(std::string_view s) {
    std::string sanitized(s);
    for (char& c : sanitized) {
        if (c == '\r' || c == '\n') {
            c = ' ';
        }
    }
    return sanitized;
}

} // namespace

void simple(Buffer& out, std::string_view s) {
    out.append("+");
    out.append(s);
    out.append("\r\n");
}

void error(Buffer& out, std::string_view s) {
    out.append("-");
    out.append(sanitize_error_text(s));
    out.append("\r\n");
}

void integer(Buffer& out, int64_t n) {
    out.append(":");
    out.append(std::to_string(n));
    out.append("\r\n");
}

void bulk(Buffer& out, std::string_view s) {
    out.append("$");
    out.append(std::to_string(s.size()));
    out.append("\r\n");
    out.append(s);
    out.append("\r\n");
}

void null_bulk(Buffer& out) {
    out.append("$-1\r\n");
}

void array_header(Buffer& out, size_t n) {
    out.append("*");
    out.append(std::to_string(n));
    out.append("\r\n");
}

void null_array(Buffer& out) {
    out.append("*-1\r\n");
}

void double_as_bulk(Buffer& out, double d) {
    if (std::isnan(d)) {
        bulk(out, "nan");
        return;
    }
    if (std::isinf(d)) {
        bulk(out, d > 0 ? std::string_view("inf") : std::string_view("-inf"));
        return;
    }
    char buf[64];
    const auto result = std::to_chars(buf, buf + sizeof(buf), d);
    bulk(out, std::string_view(buf, static_cast<size_t>(result.ptr - buf)));
}

} // namespace gredis::resp
