#include "commands/cmd_string.h"

#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

#include "protocol/resp_writer.h"
#include "storage/database.h"
#include "storage/value.h"
#include "util/clock.h"
#include "util/parse.h"

namespace gredis {

namespace {

constexpr std::string_view kWrongType = "WRONGTYPE Operation against a key holding the wrong kind of value";
constexpr std::string_view kNotInt = "ERR value is not an integer or out of range";
constexpr std::string_view kOverflow = "ERR increment or decrement would overflow";

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

// missing key isn't a type error, caller handles that -- this only catches wrong type
std::string* as_string_or_error(CommandContext& ctx, Value* v) {
    if (v == nullptr) {
        return nullptr;
    }
    std::string* s = std::get_if<std::string>(v);
    if (s == nullptr) {
        resp::error(ctx.out, kWrongType);
    }
    return s;
}

bool add_would_overflow(int64_t a, int64_t b) {
    if (b > 0 && a > std::numeric_limits<int64_t>::max() - b) {
        return true;
    }
    if (b < 0 && a < std::numeric_limits<int64_t>::min() - b) {
        return true;
    }
    return false;
}

// shared by INCR/DECR/INCRBY/DECRBY: read the int value (0 if missing),
// add delta, store back as a decimal string, reply with the new value.
//
// unlike SET, INCR must keep any existing TTL -- so an existing key is
// overwritten through the lookup_write() pointer directly, never via
// Database::set() (which always clears TTL). set() is only used for the
// brand-new-key case, where there's no TTL to keep anyway.
void incr_by(CommandContext& ctx, const std::string& key, int64_t delta) {
    const int64_t now = monotonic_ms();
    Value* v = ctx.db.lookup_write(key, now);
    int64_t current = 0;
    if (v != nullptr) {
        std::string* s = std::get_if<std::string>(v);
        if (s == nullptr) {
            resp::error(ctx.out, kWrongType);
            return;
        }
        const auto parsed = parse_int64(*s);
        if (!parsed) {
            resp::error(ctx.out, kNotInt);
            return;
        }
        current = *parsed;
    }
    if (add_would_overflow(current, delta)) {
        resp::error(ctx.out, kOverflow);
        return;
    }
    const int64_t result = current + delta;
    if (v != nullptr) {
        *v = Value(std::to_string(result));
    } else {
        ctx.db.set(key, Value(std::to_string(result)));
    }
    resp::integer(ctx.out, result);
}

// SET key val [EX seconds | PX milliseconds] [NX | XX]. options start
// at argv[3]; dispatcher arity only guarantees the minimum.
struct SetOptions {
    bool has_expire = false;
    int64_t expire_ms = 0; // relative to "now" at SET time
    bool nx = false;
    bool xx = false;
};

bool seconds_to_ms(int64_t seconds, int64_t& out_ms) {
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    if (seconds > kMax / 1000 || seconds < kMin / 1000) {
        return false;
    }
    out_ms = seconds * 1000;
    return true;
}

bool parse_set_options(CommandContext& ctx, const std::vector<std::string>& argv, SetOptions& opts) {
    for (size_t i = 3; i < argv.size(); ++i) {
        if (ieq(argv[i], "EX") || ieq(argv[i], "PX")) {
            if (opts.has_expire || i + 1 >= argv.size()) {
                resp::error(ctx.out, "ERR syntax error");
                return false;
            }
            const bool is_seconds = ieq(argv[i], "EX");
            ++i;
            const auto n = parse_int64(argv[i]);
            if (!n) {
                resp::error(ctx.out, kNotInt);
                return false;
            }
            // EX 0 / EX -1 are errors here, unlike standalone EXPIRE where non-positive deletes the key
            if (*n <= 0) {
                resp::error(ctx.out, "ERR invalid expire time in 'set' command");
                return false;
            }
            int64_t ms = *n;
            if (is_seconds && !seconds_to_ms(*n, ms)) {
                resp::error(ctx.out, "ERR invalid expire time in 'set' command");
                return false;
            }
            opts.has_expire = true;
            opts.expire_ms = ms;
        } else if (ieq(argv[i], "NX")) {
            if (opts.xx) {
                resp::error(ctx.out, "ERR syntax error");
                return false;
            }
            opts.nx = true;
        } else if (ieq(argv[i], "XX")) {
            if (opts.nx) {
                resp::error(ctx.out, "ERR syntax error");
                return false;
            }
            opts.xx = true;
        } else {
            resp::error(ctx.out, "ERR syntax error");
            return false;
        }
    }
    return true;
}

} // namespace

void cmd_get(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::null_bulk(ctx.out);
        return;
    }
    const std::string* s = as_string_or_error(ctx, v);
    if (s == nullptr) {
        return; // as_string_or_error already wrote the WRONGTYPE reply
    }
    resp::bulk(ctx.out, *s);
}

void cmd_set(CommandContext& ctx, std::vector<std::string>& argv) {
    SetOptions opts;
    if (!parse_set_options(ctx, argv, opts)) {
        return;
    }

    const int64_t now = monotonic_ms();
    Value* existing = ctx.db.lookup_write(argv[1], now);
    if ((opts.nx && existing != nullptr) || (opts.xx && existing == nullptr)) {
        resp::null_bulk(ctx.out);
        return;
    }

    if (opts.has_expire && add_would_overflow(now, opts.expire_ms)) {
        resp::error(ctx.out, "ERR invalid expire time in 'set' command");
        return;
    }

    // key copied not moved -- still needed below for expire_at() if EX/PX given
    ctx.db.set(argv[1], Value(std::move(argv[2]))); // always clears any prior TTL
    if (opts.has_expire) {
        ctx.db.expire_at(argv[1], now + opts.expire_ms, now);
    }
    resp::simple(ctx.out, "OK");
}

void cmd_mget(CommandContext& ctx, std::vector<std::string>& argv) {
    const int64_t now = monotonic_ms();
    resp::array_header(ctx.out, argv.size() - 1);
    for (size_t i = 1; i < argv.size(); ++i) {
        Value* v = ctx.db.lookup_read(argv[i], now);
        const std::string* s = (v != nullptr) ? std::get_if<std::string>(v) : nullptr;
        // missing key and wrong type both become null bulk, same as redis
        if (s != nullptr) {
            resp::bulk(ctx.out, *s);
        } else {
            resp::null_bulk(ctx.out);
        }
    }
}

void cmd_mset(CommandContext& ctx, std::vector<std::string>& argv) {
    if ((argv.size() % 2) == 0) {
        // dispatcher arity guarantees an odd lower bound, not that argc itself is odd
        resp::error(ctx.out, "ERR wrong number of arguments for 'mset' command");
        return;
    }
    for (size_t i = 1; i + 1 < argv.size(); i += 2) {
        ctx.db.set(argv[i], Value(argv[i + 1])); // clears any prior TTL, like SET
    }
    resp::simple(ctx.out, "OK");
}

void cmd_incr(CommandContext& ctx, std::vector<std::string>& argv) {
    incr_by(ctx, argv[1], 1);
}

void cmd_decr(CommandContext& ctx, std::vector<std::string>& argv) {
    incr_by(ctx, argv[1], -1);
}

void cmd_incrby(CommandContext& ctx, std::vector<std::string>& argv) {
    const auto delta = parse_int64(argv[2]);
    if (!delta) {
        resp::error(ctx.out, kNotInt);
        return;
    }
    incr_by(ctx, argv[1], *delta);
}

void cmd_decrby(CommandContext& ctx, std::vector<std::string>& argv) {
    const auto delta = parse_int64(argv[2]);
    if (!delta) {
        resp::error(ctx.out, kNotInt);
        return;
    }
    if (*delta == std::numeric_limits<int64_t>::min()) {
        // negating INT64_MIN overflows by itself, before incr_by's own check even runs
        resp::error(ctx.out, kOverflow);
        return;
    }
    incr_by(ctx, argv[1], -*delta);
}

void cmd_append(CommandContext& ctx, std::vector<std::string>& argv) {
    const int64_t now = monotonic_ms();
    Value* v = ctx.db.lookup_write(argv[1], now);
    if (v == nullptr) {
        ctx.db.set(argv[1], Value(argv[2]));
        resp::integer(ctx.out, static_cast<int64_t>(argv[2].size()));
        return;
    }
    std::string* s = std::get_if<std::string>(v);
    if (s == nullptr) {
        resp::error(ctx.out, kWrongType);
        return;
    }
    // mutate in place through the lookup_write() pointer -- keeps TTL
    // (set() would clear it), and safe since nothing else touches the
    // db between this lookup and the reply.
    s->append(argv[2]);
    resp::integer(ctx.out, static_cast<int64_t>(s->size()));
}

void cmd_strlen(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::integer(ctx.out, 0);
        return;
    }
    const std::string* s = std::get_if<std::string>(v);
    if (s == nullptr) {
        resp::error(ctx.out, kWrongType);
        return;
    }
    resp::integer(ctx.out, static_cast<int64_t>(s->size()));
}

} // namespace gredis
