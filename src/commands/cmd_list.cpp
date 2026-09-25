#include "commands/cmd_list.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
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
constexpr std::string_view kNotPositive = "ERR value is out of range, must be positive";

// v is non-null; missing-key handling differs per caller (nil for
// LINDEX/LPOP, 0 for LLEN, empty array for LRANGE). Writes WRONGTYPE
// and returns null if v isn't a list.
ListValue* as_list_or_error(CommandContext& ctx, Value* v) {
    auto* l = std::get_if<std::unique_ptr<ListValue>>(v);
    if (l == nullptr) {
        resp::error(ctx.out, kWrongType);
        return nullptr;
    }
    return l->get();
}

// creates an empty list if key doesn't exist yet, for LPUSH/RPUSH.
// returns null (WRONGTYPE already sent) if key holds something else.
ListValue* get_or_create_list(CommandContext& ctx, const std::string& key, int64_t now_ms) {
    Value* v = ctx.db.lookup_write(key, now_ms);
    if (v != nullptr) {
        return as_list_or_error(ctx, v);
    }
    ctx.db.set(key, Value(std::make_unique<ListValue>()));
    // re-lookup instead of threading a pointer through set(), same as cmd_hash.cpp
    return std::get<std::unique_ptr<ListValue>>(*ctx.db.lookup_write(key, now_ms)).get();
}

// -1 = last element, etc; result can still be out of range, caller
// bounds-checks it (LRANGE clamps, LINDEX treats oob as no element)
int64_t resolve_index(int64_t idx, int64_t len) {
    return idx < 0 ? idx + len : idx;
}

void push_generic(CommandContext& ctx, std::vector<std::string>& argv, bool front) {
    ListValue* list = get_or_create_list(ctx, argv[1], monotonic_ms());
    if (list == nullptr) {
        return;
    }
    for (size_t i = 2; i < argv.size(); ++i) {
        if (front) {
            list->push_front(argv[i]);
        } else {
            list->push_back(argv[i]);
        }
    }
    resp::integer(ctx.out, static_cast<int64_t>(list->size()));
}

void pop_generic(CommandContext& ctx, std::vector<std::string>& argv, bool from_front) {
    // argv is {name, key} or {name, key, count}; dispatcher only
    // guarantees "at least key" so check the upper bound here
    if (argv.size() > 3) {
        resp::error(ctx.out, "ERR wrong number of arguments for '" + argv[0] + "' command");
        return;
    }
    const bool has_count = argv.size() == 3;
    int64_t count = 1;
    if (has_count) {
        const auto n = parse_int64(argv[2]);
        if (!n) {
            resp::error(ctx.out, kNotInt);
            return;
        }
        if (*n < 0) {
            resp::error(ctx.out, kNotPositive);
            return;
        }
        count = *n;
    }

    const int64_t now = monotonic_ms();
    Value* v = ctx.db.lookup_write(argv[1], now);
    if (v == nullptr) {
        // missing key: null bulk without count, null array with count -- different shapes
        if (has_count) {
            resp::null_array(ctx.out);
        } else {
            resp::null_bulk(ctx.out);
        }
        return;
    }
    ListValue* list = as_list_or_error(ctx, v);
    if (list == nullptr) {
        return;
    }

    if (!has_count) {
        std::string val = from_front ? std::move(list->front()) : std::move(list->back());
        if (from_front) {
            list->pop_front();
        } else {
            list->pop_back();
        }
        if (list->empty()) {
            ctx.db.del(argv[1]); // removing the last element deletes the key
        }
        resp::bulk(ctx.out, val);
        return;
    }

    const int64_t n = std::min<int64_t>(count, static_cast<int64_t>(list->size()));
    resp::array_header(ctx.out, n);
    for (int64_t i = 0; i < n; ++i) {
        std::string val = from_front ? std::move(list->front()) : std::move(list->back());
        if (from_front) {
            list->pop_front();
        } else {
            list->pop_back();
        }
        resp::bulk(ctx.out, val);
    }
    if (list->empty()) {
        ctx.db.del(argv[1]);
    }
}

} // namespace

void cmd_lpush(CommandContext& ctx, std::vector<std::string>& argv) {
    push_generic(ctx, argv, /*front=*/true);
}

void cmd_rpush(CommandContext& ctx, std::vector<std::string>& argv) {
    push_generic(ctx, argv, /*front=*/false);
}

void cmd_lpop(CommandContext& ctx, std::vector<std::string>& argv) {
    pop_generic(ctx, argv, /*from_front=*/true);
}

void cmd_rpop(CommandContext& ctx, std::vector<std::string>& argv) {
    pop_generic(ctx, argv, /*from_front=*/false);
}

void cmd_llen(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::integer(ctx.out, 0);
        return;
    }
    ListValue* list = as_list_or_error(ctx, v);
    if (list == nullptr) {
        return;
    }
    resp::integer(ctx.out, static_cast<int64_t>(list->size()));
}

void cmd_lrange(CommandContext& ctx, std::vector<std::string>& argv) {
    const auto start_opt = parse_int64(argv[2]);
    const auto stop_opt = parse_int64(argv[3]);
    if (!start_opt || !stop_opt) {
        resp::error(ctx.out, kNotInt);
        return;
    }
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::array_header(ctx.out, 0);
        return;
    }
    ListValue* list = as_list_or_error(ctx, v);
    if (list == nullptr) {
        return;
    }

    const int64_t len = static_cast<int64_t>(list->size());
    int64_t start = resolve_index(*start_opt, len);
    int64_t stop = resolve_index(*stop_opt, len);
    if (start < 0) {
        start = 0;
    }
    if (stop >= len) {
        stop = len - 1;
    }
    if (len == 0 || start > stop) {
        resp::array_header(ctx.out, 0);
        return;
    }

    resp::array_header(ctx.out, stop - start + 1);
    auto it = list->begin() + start;
    for (int64_t i = start; i <= stop; ++i, ++it) {
        resp::bulk(ctx.out, *it);
    }
}

void cmd_lindex(CommandContext& ctx, std::vector<std::string>& argv) {
    const auto idx_opt = parse_int64(argv[2]);
    if (!idx_opt) {
        resp::error(ctx.out, kNotInt);
        return;
    }
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::null_bulk(ctx.out);
        return;
    }
    ListValue* list = as_list_or_error(ctx, v);
    if (list == nullptr) {
        return;
    }

    const int64_t len = static_cast<int64_t>(list->size());
    const int64_t idx = resolve_index(*idx_opt, len);
    if (idx < 0 || idx >= len) {
        resp::null_bulk(ctx.out);
        return;
    }
    resp::bulk(ctx.out, (*list)[static_cast<size_t>(idx)]);
}

} // namespace gredis
