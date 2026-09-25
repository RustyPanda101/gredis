#include "commands/cmd_set.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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

// v is non-null, same pattern as cmd_hash.cpp / cmd_list.cpp
SetValue* as_set_or_error(CommandContext& ctx, Value* v) {
    auto* s = std::get_if<std::unique_ptr<SetValue>>(v);
    if (s == nullptr) {
        resp::error(ctx.out, kWrongType);
        return nullptr;
    }
    return s->get();
}

// creates an empty set if key doesn't exist yet, for SADD.
// returns null (WRONGTYPE already sent) if key holds something else.
SetValue* get_or_create_set(CommandContext& ctx, const std::string& key, int64_t now_ms) {
    Value* v = ctx.db.lookup_write(key, now_ms);
    if (v != nullptr) {
        return as_set_or_error(ctx, v);
    }
    ctx.db.set(key, Value(std::make_unique<SetValue>()));
    return std::get<std::unique_ptr<SetValue>>(*ctx.db.lookup_write(key, now_ms)).get();
}

// one PRNG for the process, seeded once. SPOP's random_entry isn't
// perfectly uniform (bucket then chain) but fine, not crypto
std::mt19937& spop_rng() {
    static std::mt19937 gen(std::random_device{}());
    return gen;
}

} // namespace

void cmd_sadd(CommandContext& ctx, std::vector<std::string>& argv) {
    SetValue* set = get_or_create_set(ctx, argv[1], monotonic_ms());
    if (set == nullptr) {
        return;
    }
    int64_t added = 0;
    for (size_t i = 2; i < argv.size(); ++i) {
        // dup member (this call or earlier) only counts once as newly inserted
        const auto [ptr, inserted] = set->insert_or_assign(argv[i], Empty{});
        if (inserted) {
            ++added;
        }
    }
    resp::integer(ctx.out, added);
}

void cmd_srem(CommandContext& ctx, std::vector<std::string>& argv) {
    const int64_t now = monotonic_ms();
    Value* v = ctx.db.lookup_write(argv[1], now);
    if (v == nullptr) {
        resp::integer(ctx.out, 0);
        return;
    }
    SetValue* set = as_set_or_error(ctx, v);
    if (set == nullptr) {
        return;
    }
    int64_t removed = 0;
    for (size_t i = 2; i < argv.size(); ++i) {
        if (set->erase(argv[i])) {
            ++removed;
        }
    }
    if (set->empty()) {
        ctx.db.del(argv[1]); // removing the last member deletes the key
    }
    resp::integer(ctx.out, removed);
}

void cmd_sismember(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::integer(ctx.out, 0);
        return;
    }
    SetValue* set = as_set_or_error(ctx, v);
    if (set == nullptr) {
        return;
    }
    resp::integer(ctx.out, set->find(argv[2]) != nullptr ? 1 : 0);
}

void cmd_smembers(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::array_header(ctx.out, 0);
        return;
    }
    SetValue* set = as_set_or_error(ctx, v);
    if (set == nullptr) {
        return;
    }
    // table order, unspecified, same as HGETALL
    resp::array_header(ctx.out, set->size());
    set->for_each([&ctx](const std::string& member, const Empty& /*unused*/) { resp::bulk(ctx.out, member); });
}

void cmd_scard(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::integer(ctx.out, 0);
        return;
    }
    SetValue* set = as_set_or_error(ctx, v);
    if (set == nullptr) {
        return;
    }
    resp::integer(ctx.out, static_cast<int64_t>(set->size()));
}

void cmd_spop(CommandContext& ctx, std::vector<std::string>& argv) {
    // argv is {name, key} or {name, key, count}, same pattern as LPOP/RPOP
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
        // unlike LPOP/RPOP, SPOP with count on missing key replies empty array, not null
        if (has_count) {
            resp::array_header(ctx.out, 0);
        } else {
            resp::null_bulk(ctx.out);
        }
        return;
    }
    SetValue* set = as_set_or_error(ctx, v);
    if (set == nullptr) {
        return;
    }

    if (!has_count) {
        const auto [member_ptr, empty_ptr] = set->random_entry(spop_rng());
        const std::string member = *member_ptr; // copy before erase() invalidates it
        set->erase(member);
        if (set->empty()) {
            ctx.db.del(argv[1]);
        }
        resp::bulk(ctx.out, member);
        return;
    }

    const int64_t n = std::min<int64_t>(count, static_cast<int64_t>(set->size()));
    resp::array_header(ctx.out, n);
    for (int64_t i = 0; i < n; ++i) {
        const auto [member_ptr, empty_ptr] = set->random_entry(spop_rng());
        const std::string member = *member_ptr;
        set->erase(member); // erasing as we go guarantees no member repeats
        resp::bulk(ctx.out, member);
    }
    if (set->empty()) {
        ctx.db.del(argv[1]);
    }
}

void cmd_sinter(CommandContext& ctx, std::vector<std::string>& argv) {
    const int64_t now = monotonic_ms();
    std::vector<SetValue*> sets;
    sets.reserve(argv.size() - 1);
    for (size_t i = 1; i < argv.size(); ++i) {
        Value* v = ctx.db.lookup_read(argv[i], now);
        if (v == nullptr) {
            // missing key = empty set, so whole intersection is empty
            resp::array_header(ctx.out, 0);
            return;
        }
        SetValue* s = as_set_or_error(ctx, v);
        if (s == nullptr) {
            return; // WRONGTYPE already written
        }
        sets.push_back(s);
    }

    // iterate the smallest set, probe the rest -- cheaper when sizes differ a lot
    size_t smallest = 0;
    for (size_t i = 1; i < sets.size(); ++i) {
        if (sets[i]->size() < sets[smallest]->size()) {
            smallest = i;
        }
    }

    std::vector<std::string> result;
    sets[smallest]->for_each([&](const std::string& member, const Empty& /*unused*/) {
        for (size_t i = 0; i < sets.size(); ++i) {
            if (i != smallest && sets[i]->find(member) == nullptr) {
                return; // not in every set -- skip
            }
        }
        result.push_back(member);
    });

    resp::array_header(ctx.out, result.size());
    for (const auto& member : result) {
        resp::bulk(ctx.out, member);
    }
}

} // namespace gredis
