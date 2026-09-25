#include "commands/cmd_keys.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "net/server.h"
#include "protocol/resp_writer.h"
#include "storage/database.h"
#include "storage/value.h"
#include "util/clock.h"
#include "util/parse.h"

namespace gredis {

namespace {

constexpr std::string_view kNotInt = "ERR value is not an integer or out of range";

// values bigger than this get freed on a worker thread instead of inline --
// small ones aren't worth the cost of a thread-pool task
constexpr size_t kUnlinkBackgroundThreshold = 64;

bool add_would_overflow(int64_t a, int64_t b) {
    if (b > 0 && a > std::numeric_limits<int64_t>::max() - b) {
        return true;
    }
    if (b < 0 && a < std::numeric_limits<int64_t>::min() - b) {
        return true;
    }
    return false;
}

bool sub_would_overflow(int64_t a, int64_t b) {
    if (b > 0 && a < std::numeric_limits<int64_t>::min() + b) {
        return true;
    }
    if (b < 0 && a > std::numeric_limits<int64_t>::max() + b) {
        return true;
    }
    return false;
}

bool seconds_to_ms(int64_t seconds, int64_t& out_ms) {
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    if (seconds > kMax / 1000 || seconds < kMin / 1000) {
        return false;
    }
    out_ms = seconds * 1000;
    return true;
}

// EXPIRE/PEXPIRE: amount is relative to now on the monotonic clock,
// so just an overflow-checked add, no unit conversion needed.
void expire_relative(CommandContext& ctx, const std::string& cmd_name, const std::string& key,
                      int64_t amount, bool is_seconds) {
    int64_t delta_ms = amount;
    if (is_seconds && !seconds_to_ms(amount, delta_ms)) {
        resp::error(ctx.out, "ERR invalid expire time in '" + cmd_name + "' command");
        return;
    }
    const int64_t now = monotonic_ms();
    if (add_would_overflow(now, delta_ms)) {
        resp::error(ctx.out, "ERR invalid expire time in '" + cmd_name + "' command");
        return;
    }
    const bool existed = ctx.db.expire_at(key, now + delta_ms, now);
    resp::integer(ctx.out, existed ? 1 : 0);
}

// EXPIREAT/PEXPIREAT: amount is an absolute unix time, but deadlines are
// stored on the monotonic clock, so convert:
// deadline = now_monotonic + (unix_target - now_unix)
void expire_absolute(CommandContext& ctx, const std::string& cmd_name, const std::string& key,
                      int64_t amount, bool is_seconds) {
    int64_t unix_target_ms = amount;
    if (is_seconds && !seconds_to_ms(amount, unix_target_ms)) {
        resp::error(ctx.out, "ERR invalid expire time in '" + cmd_name + "' command");
        return;
    }
    const int64_t now_mono = monotonic_ms();
    const int64_t now_unix = unix_ms();
    if (sub_would_overflow(unix_target_ms, now_unix)) {
        resp::error(ctx.out, "ERR invalid expire time in '" + cmd_name + "' command");
        return;
    }
    const int64_t delta = unix_target_ms - now_unix;
    if (add_would_overflow(now_mono, delta)) {
        resp::error(ctx.out, "ERR invalid expire time in '" + cmd_name + "' command");
        return;
    }
    const bool existed = ctx.db.expire_at(key, now_mono + delta, now_mono);
    resp::integer(ctx.out, existed ? 1 : 0);
}

} // namespace

void cmd_del(CommandContext& ctx, std::vector<std::string>& argv) {
    const int64_t now = monotonic_ms();
    int64_t count = 0;
    for (size_t i = 1; i < argv.size(); ++i) {
        // lookup_write does the lazy-expiry check first, so an already-
        // expired key doesn't count as deleted here.
        if (ctx.db.lookup_write(argv[i], now) != nullptr) {
            ctx.db.del(argv[i]);
            ++count;
        }
    }
    resp::integer(ctx.out, count);
}

void cmd_unlink(CommandContext& ctx, std::vector<std::string>& argv) {
    const int64_t now = monotonic_ms();
    int64_t count = 0;
    for (size_t i = 1; i < argv.size(); ++i) {
        // take() does the same lazy-expiry check as del's lookup_write,
        // but moves the value out instead of destroying it in place.
        std::optional<Value> value = ctx.db.take(argv[i], now);
        if (!value) {
            continue;
        }
        ++count;
        if (value_element_count(*value) > kUnlinkBackgroundThreshold) {
            // Value is move-only, but std::function needs a copy-
            // constructible target (no move_only_function pre-C++23) --
            // shared_ptr just to satisfy that; only this one task touches it.
            auto detached = std::make_shared<Value>(std::move(*value));
            ctx.server.run_in_background([detached] { (void)detached; });
        }
        // else: value just destructs here inline, too small to bother with a task
    }
    resp::integer(ctx.out, count);
}

void cmd_exists(CommandContext& ctx, std::vector<std::string>& argv) {
    // counts per occurrence in argv, not per distinct key --
    // EXISTS k k on an existing k replies :2
    const int64_t now = monotonic_ms();
    int64_t count = 0;
    for (size_t i = 1; i < argv.size(); ++i) {
        if (ctx.db.lookup_read(argv[i], now) != nullptr) {
            ++count;
        }
    }
    resp::integer(ctx.out, count);
}

void cmd_type(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::simple(ctx.out, "none");
        return;
    }
    resp::simple(ctx.out, type_name(*v));
}

void cmd_expire(CommandContext& ctx, std::vector<std::string>& argv) {
    const auto amount = parse_int64(argv[2]);
    if (!amount) {
        resp::error(ctx.out, kNotInt);
        return;
    }
    expire_relative(ctx, argv[0], argv[1], *amount, /*is_seconds=*/true);
}

void cmd_pexpire(CommandContext& ctx, std::vector<std::string>& argv) {
    const auto amount = parse_int64(argv[2]);
    if (!amount) {
        resp::error(ctx.out, kNotInt);
        return;
    }
    expire_relative(ctx, argv[0], argv[1], *amount, /*is_seconds=*/false);
}

void cmd_expireat(CommandContext& ctx, std::vector<std::string>& argv) {
    const auto amount = parse_int64(argv[2]);
    if (!amount) {
        resp::error(ctx.out, kNotInt);
        return;
    }
    expire_absolute(ctx, argv[0], argv[1], *amount, /*is_seconds=*/true);
}

void cmd_pexpireat(CommandContext& ctx, std::vector<std::string>& argv) {
    const auto amount = parse_int64(argv[2]);
    if (!amount) {
        resp::error(ctx.out, kNotInt);
        return;
    }
    expire_absolute(ctx, argv[0], argv[1], *amount, /*is_seconds=*/false);
}

void cmd_ttl(CommandContext& ctx, std::vector<std::string>& argv) {
    const int64_t pttl = ctx.db.pttl(argv[1], monotonic_ms());
    if (pttl < 0) {
        resp::integer(ctx.out, pttl); // -2 missing, -1 no ttl
        return;
    }
    // round to nearest second, same as redis's (ttl+500)/1000
    resp::integer(ctx.out, (pttl + 500) / 1000);
}

void cmd_pttl(CommandContext& ctx, std::vector<std::string>& argv) {
    resp::integer(ctx.out, ctx.db.pttl(argv[1], monotonic_ms()));
}

void cmd_persist(CommandContext& ctx, std::vector<std::string>& argv) {
    const bool removed = ctx.db.persist(argv[1], monotonic_ms());
    resp::integer(ctx.out, removed ? 1 : 0);
}

} // namespace gredis
