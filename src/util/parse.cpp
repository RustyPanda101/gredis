#include "util/parse.h"

#include <cctype>
#include <charconv>
#include <limits>

namespace gredis {

std::optional<int64_t> parse_int64(std::string_view s) {
    if (s.empty()) {
        return std::nullopt;
    }
    // from_chars already rejects '+' and whitespace; just need to check
    // the whole string got consumed (it stops silently at first non-digit)
    int64_t value = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return value;
}

namespace {

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<double> parse_double(std::string_view s) {
    if (s.empty()) {
        return std::nullopt;
    }

    // from_chars accepts leading '-' but not '+' (unlike strtod), so strip
    // '+' manually to allow "+5.5" / "+inf"
    std::string_view body = s;
    if (body.front() == '+') {
        body = body.substr(1);
        if (body.empty()) {
            return std::nullopt;
        }
    }

    // checked before from_chars: reject literal "nan" outright even though
    // from_chars would otherwise accept it
    std::string_view magnitude = body;
    bool negative = false;
    if (!magnitude.empty() && magnitude.front() == '-') {
        negative = true;
        magnitude = magnitude.substr(1);
    }
    if (ieq(magnitude, "inf") || ieq(magnitude, "infinity")) {
        return negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    }
    if (ieq(magnitude, "nan")) {
        return std::nullopt;
    }

    double value = 0.0;
    const auto [ptr, ec] = std::from_chars(body.data(), body.data() + body.size(), value);
    if (ec != std::errc{} || ptr != body.data() + body.size()) {
        return std::nullopt;
    }
    return value;
}

} // namespace gredis
