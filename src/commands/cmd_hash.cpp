#include "commands/cmd_hash.h"

#include <cstdint>
#include <limits>
#include <memory>
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

bool add_would_overflow(int64_t a, int64_t b) {
    if (b > 0 && a > std::numeric_limits<int64_t>::max() - b) {
        return true;
    }
    if (b < 0 && a < std::numeric_limits<int64_t>::min() - b) {
        return true;
    }
    return false;
}

// v is non-null; missing-key handling differs per caller (nil vs 0 vs
// empty array) so that's left to them. Writes WRONGTYPE and returns
// null if v isn't a hash.
HashValue* as_hash_or_error(CommandContext& ctx, Value* v) {
    auto* h = std::get_if<std::unique_ptr<HashValue>>(v);
    if (h == nullptr) {
        resp::error(ctx.out, kWrongType);
        return nullptr;
    }
    return h->get();
}

// creates an empty hash if key doesn't exist yet, for HSET/HINCRBY.
// returns null (WRONGTYPE already sent) if key holds something else.
HashValue* get_or_create_hash(CommandContext& ctx, const std::string& key, int64_t now_ms) {
    Value* v = ctx.db.lookup_write(key, now_ms);
    if (v != nullptr) {
        return as_hash_or_error(ctx, v);
    }
    ctx.db.set(key, Value(std::make_unique<HashValue>()));
    // re-lookup instead of threading a pointer through set() -- cheap, key was just inserted
    return std::get<std::unique_ptr<HashValue>>(*ctx.db.lookup_write(key, now_ms)).get();
}

} // namespace

void cmd_hset(CommandContext& ctx, std::vector<std::string>& argv) {
    // argv must be even (name+key+pairs); dispatcher arity only guarantees at least one pair
    if ((argv.size() % 2) != 0) {
        resp::error(ctx.out, "ERR wrong number of arguments for '" + argv[0] + "' command");
        return;
    }
    HashValue* h = get_or_create_hash(ctx, argv[1], monotonic_ms());
    if (h == nullptr) {
        return;
    }
    int64_t added = 0;
    for (size_t i = 2; i + 1 < argv.size(); i += 2) {
        // dup field in one HSET: counted once, last value wins
        const auto [ptr, inserted] = h->insert_or_assign(argv[i], argv[i + 1]);
        if (inserted) {
            ++added;
        }
    }
    resp::integer(ctx.out, added);
}

void cmd_hget(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::null_bulk(ctx.out);
        return;
    }
    HashValue* h = as_hash_or_error(ctx, v);
    if (h == nullptr) {
        return;
    }
    const std::string* val = h->find(argv[2]);
    if (val == nullptr) {
        resp::null_bulk(ctx.out);
    } else {
        resp::bulk(ctx.out, *val);
    }
}

void cmd_hmget(CommandContext& ctx, std::vector<std::string>& argv) {
    const int64_t now = monotonic_ms();
    Value* v = ctx.db.lookup_read(argv[1], now);
    HashValue* h = nullptr;
    if (v != nullptr) {
        h = as_hash_or_error(ctx, v);
        if (h == nullptr) {
            return; // WRONGTYPE already written
        }
    }
    // missing key acts like empty hash -- every field comes back nil
    resp::array_header(ctx.out, argv.size() - 2);
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string* val = (h != nullptr) ? h->find(argv[i]) : nullptr;
        if (val != nullptr) {
            resp::bulk(ctx.out, *val);
        } else {
            resp::null_bulk(ctx.out);
        }
    }
}

void cmd_hdel(CommandContext& ctx, std::vector<std::string>& argv) {
    const int64_t now = monotonic_ms();
    Value* v = ctx.db.lookup_write(argv[1], now);
    if (v == nullptr) {
        resp::integer(ctx.out, 0);
        return;
    }
    HashValue* h = as_hash_or_error(ctx, v);
    if (h == nullptr) {
        return;
    }
    int64_t removed = 0;
    for (size_t i = 2; i < argv.size(); ++i) {
        if (h->erase(argv[i])) {
            ++removed;
        }
    }
    if (h->empty()) {
        // last field gone means key gone too, same rule for every aggregate type
        ctx.db.del(argv[1]);
    }
    resp::integer(ctx.out, removed);
}

void cmd_hexists(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::integer(ctx.out, 0);
        return;
    }
    HashValue* h = as_hash_or_error(ctx, v);
    if (h == nullptr) {
        return;
    }
    resp::integer(ctx.out, h->find(argv[2]) != nullptr ? 1 : 0);
}

void cmd_hlen(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::integer(ctx.out, 0);
        return;
    }
    HashValue* h = as_hash_or_error(ctx, v);
    if (h == nullptr) {
        return;
    }
    resp::integer(ctx.out, static_cast<int64_t>(h->size()));
}

void cmd_hgetall(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::array_header(ctx.out, 0);
        return;
    }
    HashValue* h = as_hash_or_error(ctx, v);
    if (h == nullptr) {
        return;
    }
    // flattened field/value pairs, table order (unspecified), same as redis
    resp::array_header(ctx.out, 2 * h->size());
    h->for_each([&ctx](const std::string& field, const std::string& value) {
        resp::bulk(ctx.out, field);
        resp::bulk(ctx.out, value);
    });
}

void cmd_hkeys(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::array_header(ctx.out, 0);
        return;
    }
    HashValue* h = as_hash_or_error(ctx, v);
    if (h == nullptr) {
        return;
    }
    resp::array_header(ctx.out, h->size());
    h->for_each([&ctx](const std::string& field, const std::string& /*value*/) { resp::bulk(ctx.out, field); });
}

void cmd_hvals(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::array_header(ctx.out, 0);
        return;
    }
    HashValue* h = as_hash_or_error(ctx, v);
    if (h == nullptr) {
        return;
    }
    resp::array_header(ctx.out, h->size());
    h->for_each([&ctx](const std::string& /*field*/, const std::string& value) { resp::bulk(ctx.out, value); });
}

void cmd_hincrby(CommandContext& ctx, std::vector<std::string>& argv) {
    const auto delta = parse_int64(argv[3]);
    if (!delta) {
        resp::error(ctx.out, kNotInt);
        return;
    }
    HashValue* h = get_or_create_hash(ctx, argv[1], monotonic_ms());
    if (h == nullptr) {
        return;
    }
    int64_t current = 0;
    std::string* field = h->find(argv[2]);
    if (field != nullptr) {
        const auto parsed = parse_int64(*field);
        if (!parsed) {
            resp::error(ctx.out, kNotInt);
            return;
        }
        current = *parsed;
    }
    if (add_would_overflow(current, *delta)) {
        resp::error(ctx.out, kOverflow);
        return;
    }
    const int64_t result = current + *delta;
    h->insert_or_assign(argv[2], std::to_string(result));
    resp::integer(ctx.out, result);
}

} // namespace gredis
