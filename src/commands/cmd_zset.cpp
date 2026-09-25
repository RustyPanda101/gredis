#include "commands/cmd_zset.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <memory>
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

using Node = ZSet::Node;
using Tree = ZSet::Tree;

constexpr std::string_view kWrongType = "WRONGTYPE Operation against a key holding the wrong kind of value";
constexpr std::string_view kNotInt = "ERR value is not an integer or out of range";
constexpr std::string_view kNotFloat = "ERR value is not a valid float";
constexpr std::string_view kNotPositive = "ERR value is out of range, must be positive";

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

// v is non-null, same pattern as cmd_hash.cpp / cmd_list.cpp / cmd_set.cpp
ZSet* as_zset_or_error(CommandContext& ctx, Value* v) {
    auto* z = std::get_if<std::unique_ptr<ZSet>>(v);
    if (z == nullptr) {
        resp::error(ctx.out, kWrongType);
        return nullptr;
    }
    return z->get();
}

// creates an empty zset if key doesn't exist yet, for ZADD/ZINCRBY.
// returns null (WRONGTYPE already sent) if key holds something else.
ZSet* get_or_create_zset(CommandContext& ctx, const std::string& key, int64_t now_ms) {
    Value* v = ctx.db.lookup_write(key, now_ms);
    if (v != nullptr) {
        return as_zset_or_error(ctx, v);
    }
    ctx.db.set(key, Value(std::make_unique<ZSet>()));
    return std::get<std::unique_ptr<ZSet>>(*ctx.db.lookup_write(key, now_ms)).get();
}

// same negative-index convention as LRANGE/LINDEX, just duplicated here
int64_t resolve_index(int64_t idx, int64_t len) {
    return idx < 0 ? idx + len : idx;
}

// AvlTree has no root() accessor, so walk up parent pointers from any
// node (select(0) gives the smallest leaf, not the root). Gives ZCOUNT's
// manual descent below a starting point.
Node* tree_root(Tree& tree) {
    if (tree.size() == 0) {
        return nullptr;
    }
    Node* n = tree.select(0);
    while (n->parent != nullptr) {
        n = n->parent;
    }
    return n;
}

// first node with score strictly greater than `score`, ignoring member
// tie-break. lower_bound() compares full (score, member) so it can't
// skip ties like this needs -- plain root-to-leaf descent instead,
// still O(log n).
Node* first_with_score_greater_than(Node* root, double score) {
    Node* result = nullptr;
    for (Node* cur = root; cur != nullptr;) {
        if (cur->key.score > score) {
            result = cur;
            cur = cur->left;
        } else {
            cur = cur->right;
        }
    }
    return result;
}

struct ZaddOptions {
    bool nx = false;
    bool xx = false;
    bool ch = false;
};

// parses NX|XX/CH tokens from argv[2], sets idx to the first score.
// false means an error was already written (NX+XX together, bad arg count).
bool parse_zadd_options(CommandContext& ctx, const std::vector<std::string>& argv, size_t& idx,
                         ZaddOptions& opts) {
    idx = 2;
    while (idx < argv.size()) {
        if (ieq(argv[idx], "NX")) {
            opts.nx = true;
            ++idx;
        } else if (ieq(argv[idx], "XX")) {
            opts.xx = true;
            ++idx;
        } else if (ieq(argv[idx], "CH")) {
            opts.ch = true;
            ++idx;
        } else {
            break;
        }
    }
    if (opts.nx && opts.xx) {
        resp::error(ctx.out, "ERR XX and NX options at the same time are not compatible");
        return false;
    }
    const size_t remaining = argv.size() - idx;
    if (remaining == 0 || (remaining % 2) != 0) {
        resp::error(ctx.out, "ERR wrong number of arguments for '" + argv[0] + "' command");
        return false;
    }
    return true;
}

struct ScoreBound {
    double value = 0.0;
    bool exclusive = false;
};

// ZRANGEBYSCORE/ZCOUNT bound: leading '(' marks it exclusive, inf/-inf
// accepted, error text matches redis's "min or max is not a float"
// (different wording than ZADD/ZINCRBY's generic one).
bool parse_score_bound(CommandContext& ctx, const std::string& s, ScoreBound& out) {
    std::string_view sv = s;
    bool exclusive = false;
    if (!sv.empty() && sv.front() == '(') {
        exclusive = true;
        sv = sv.substr(1);
    }
    const auto score = parse_double(sv);
    if (!score) {
        resp::error(ctx.out, "ERR min or max is not a float");
        return false;
    }
    out = ScoreBound{*score, exclusive};
    return true;
}

void zrange_generic(CommandContext& ctx, std::vector<std::string>& argv, bool reverse) {
    const auto start_opt = parse_int64(argv[2]);
    const auto stop_opt = parse_int64(argv[3]);
    if (!start_opt || !stop_opt) {
        resp::error(ctx.out, kNotInt);
        return;
    }
    bool with_scores = false;
    if (argv.size() == 5) {
        if (!ieq(argv[4], "WITHSCORES")) {
            resp::error(ctx.out, "ERR syntax error");
            return;
        }
        with_scores = true;
    } else if (argv.size() > 5) {
        resp::error(ctx.out, "ERR syntax error");
        return;
    }

    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::array_header(ctx.out, 0);
        return;
    }
    ZSet* zset = as_zset_or_error(ctx, v);
    if (zset == nullptr) {
        return;
    }

    const int64_t len = static_cast<int64_t>(zset->size());
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

    const int64_t count = stop - start + 1;
    resp::array_header(ctx.out, with_scores ? count * 2 : count);

    Tree& tree = zset->tree();
    for (int64_t i = 0; i < count; ++i) {
        const int64_t idx = reverse ? (len - 1 - (start + i)) : (start + i);
        Node* node = tree.select(static_cast<size_t>(idx));
        resp::bulk(ctx.out, node->key.member);
        if (with_scores) {
            resp::double_as_bulk(ctx.out, node->key.score);
        }
    }
}

} // namespace

void cmd_zadd(CommandContext& ctx, std::vector<std::string>& argv) {
    ZaddOptions opts;
    size_t idx = 0;
    if (!parse_zadd_options(ctx, argv, idx, opts)) {
        return;
    }

    // validate every score first, so a bad one later can't leave earlier pairs applied
    std::vector<std::pair<double, const std::string*>> pairs;
    for (size_t i = idx; i + 1 < argv.size(); i += 2) {
        const auto score = parse_double(argv[i]);
        if (!score) {
            resp::error(ctx.out, kNotFloat);
            return;
        }
        pairs.emplace_back(*score, &argv[i + 1]);
    }

    const int64_t now = monotonic_ms();
    Value* existing_value = ctx.db.lookup_write(argv[1], now);
    if (existing_value == nullptr && opts.xx) {
        // XX on a missing key means nothing to add, and ZADD shouldn't
        // create a key for that -- bail before get_or_create_zset would make one
        resp::integer(ctx.out, 0);
        return;
    }

    ZSet* zset = get_or_create_zset(ctx, argv[1], now);
    if (zset == nullptr) {
        return;
    }

    int64_t added = 0;
    int64_t changed = 0;
    for (const auto& [score, member_ptr] : pairs) {
        const std::string& member = *member_ptr;
        const auto old_score = zset->score_of(member);
        const bool exists = old_score.has_value();
        if ((opts.nx && exists) || (opts.xx && !exists)) {
            continue;
        }
        const auto [applied_score, is_new] = zset->upsert(member, score);
        (void)applied_score;
        if (is_new) {
            ++added;
        } else if (*old_score != score) {
            ++changed;
        }
    }
    resp::integer(ctx.out, opts.ch ? added + changed : added);
}

void cmd_zrem(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_write(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::integer(ctx.out, 0);
        return;
    }
    ZSet* zset = as_zset_or_error(ctx, v);
    if (zset == nullptr) {
        return;
    }
    int64_t removed = 0;
    for (size_t i = 2; i < argv.size(); ++i) {
        if (zset->erase(argv[i])) {
            ++removed;
        }
    }
    if (zset->empty()) {
        ctx.db.del(argv[1]); // removing the last member deletes the key
    }
    resp::integer(ctx.out, removed);
}

void cmd_zscore(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::null_bulk(ctx.out);
        return;
    }
    ZSet* zset = as_zset_or_error(ctx, v);
    if (zset == nullptr) {
        return;
    }
    const auto score = zset->score_of(argv[2]);
    if (!score) {
        resp::null_bulk(ctx.out);
        return;
    }
    resp::double_as_bulk(ctx.out, *score);
}

void cmd_zincrby(CommandContext& ctx, std::vector<std::string>& argv) {
    const auto delta = parse_double(argv[2]);
    if (!delta) {
        resp::error(ctx.out, kNotFloat);
        return;
    }
    ZSet* zset = get_or_create_zset(ctx, argv[1], monotonic_ms());
    if (zset == nullptr) {
        return;
    }
    const auto old_score = zset->score_of(argv[3]);
    const double result = old_score.value_or(0.0) + *delta;
    if (std::isnan(result)) {
        // only reachable if the member already has an infinite score --
        // so no spurious empty zset gets left behind by this error path
        resp::error(ctx.out, "ERR resulting score is not a number (NaN)");
        return;
    }
    zset->upsert(argv[3], result);
    resp::double_as_bulk(ctx.out, result);
}

void cmd_zcard(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::integer(ctx.out, 0);
        return;
    }
    ZSet* zset = as_zset_or_error(ctx, v);
    if (zset == nullptr) {
        return;
    }
    resp::integer(ctx.out, static_cast<int64_t>(zset->size()));
}

void cmd_zrank(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::null_bulk(ctx.out);
        return;
    }
    ZSet* zset = as_zset_or_error(ctx, v);
    if (zset == nullptr) {
        return;
    }
    const auto r = zset->rank(argv[2]);
    if (!r) {
        resp::null_bulk(ctx.out);
        return;
    }
    resp::integer(ctx.out, static_cast<int64_t>(*r));
}

void cmd_zrevrank(CommandContext& ctx, std::vector<std::string>& argv) {
    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::null_bulk(ctx.out);
        return;
    }
    ZSet* zset = as_zset_or_error(ctx, v);
    if (zset == nullptr) {
        return;
    }
    const auto r = zset->rank(argv[2]);
    if (!r) {
        resp::null_bulk(ctx.out);
        return;
    }
    resp::integer(ctx.out, static_cast<int64_t>(zset->size() - 1 - *r));
}

void cmd_zrange(CommandContext& ctx, std::vector<std::string>& argv) {
    zrange_generic(ctx, argv, /*reverse=*/false);
}

void cmd_zrevrange(CommandContext& ctx, std::vector<std::string>& argv) {
    zrange_generic(ctx, argv, /*reverse=*/true);
}

void cmd_zrangebyscore(CommandContext& ctx, std::vector<std::string>& argv) {
    ScoreBound min_bound;
    ScoreBound max_bound;
    if (!parse_score_bound(ctx, argv[2], min_bound)) {
        return;
    }
    if (!parse_score_bound(ctx, argv[3], max_bound)) {
        return;
    }

    bool with_scores = false;
    bool has_limit = false;
    int64_t limit_offset = 0;
    int64_t limit_count = -1; // negative means "all"

    size_t i = 4;
    while (i < argv.size()) {
        if (ieq(argv[i], "WITHSCORES")) {
            with_scores = true;
            ++i;
        } else if (ieq(argv[i], "LIMIT") && i + 2 < argv.size()) {
            const auto off = parse_int64(argv[i + 1]);
            const auto cnt = parse_int64(argv[i + 2]);
            if (!off || !cnt) {
                resp::error(ctx.out, kNotInt);
                return;
            }
            limit_offset = *off;
            limit_count = *cnt;
            has_limit = true;
            i += 3;
        } else {
            resp::error(ctx.out, "ERR syntax error");
            return;
        }
    }
    (void)has_limit;

    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::array_header(ctx.out, 0);
        return;
    }
    ZSet* zset = as_zset_or_error(ctx, v);
    if (zset == nullptr) {
        return;
    }

    Tree& tree = zset->tree();
    std::vector<Node*> matched;
    for (Node* n = tree.lower_bound(ZSet::Entry{min_bound.value, std::string()}); n != nullptr;
         n = Tree::next(n)) {
        const double score = n->key.score;
        if (score > max_bound.value || (score == max_bound.value && max_bound.exclusive)) {
            break;
        }
        if (!(min_bound.exclusive && score == min_bound.value)) {
            matched.push_back(n);
        }
    }

    size_t offset = limit_offset < 0 ? 0 : std::min(static_cast<size_t>(limit_offset), matched.size());
    size_t count = matched.size() - offset;
    if (limit_count >= 0) {
        count = std::min(static_cast<size_t>(limit_count), count);
    }

    resp::array_header(ctx.out, with_scores ? count * 2 : count);
    for (size_t j = offset; j < offset + count; ++j) {
        resp::bulk(ctx.out, matched[j]->key.member);
        if (with_scores) {
            resp::double_as_bulk(ctx.out, matched[j]->key.score);
        }
    }
}

void cmd_zcount(CommandContext& ctx, std::vector<std::string>& argv) {
    ScoreBound min_bound;
    ScoreBound max_bound;
    if (!parse_score_bound(ctx, argv[2], min_bound)) {
        return;
    }
    if (!parse_score_bound(ctx, argv[3], max_bound)) {
        return;
    }

    Value* v = ctx.db.lookup_read(argv[1], monotonic_ms());
    if (v == nullptr) {
        resp::integer(ctx.out, 0);
        return;
    }
    ZSet* zset = as_zset_or_error(ctx, v);
    if (zset == nullptr) {
        return;
    }

    Tree& tree = zset->tree();
    Node* root = tree_root(tree);

    // lo = first node counted, hi = first node not counted.
    // count = rank(hi) - rank(lo), O(log n).
    Node* lo = min_bound.exclusive ? first_with_score_greater_than(root, min_bound.value)
                                    : tree.lower_bound(ZSet::Entry{min_bound.value, std::string()});
    Node* hi = max_bound.exclusive ? tree.lower_bound(ZSet::Entry{max_bound.value, std::string()})
                                    : first_with_score_greater_than(root, max_bound.value);

    const size_t rank_lo = (lo != nullptr) ? tree.rank(lo) : zset->size();
    const size_t rank_hi = (hi != nullptr) ? tree.rank(hi) : zset->size();
    const int64_t count = (rank_hi > rank_lo) ? static_cast<int64_t>(rank_hi - rank_lo) : 0;
    resp::integer(ctx.out, count);
}

void cmd_zpopmin(CommandContext& ctx, std::vector<std::string>& argv) {
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

    Value* v = ctx.db.lookup_write(argv[1], monotonic_ms());
    if (v == nullptr) {
        // both forms reply empty array on missing key -- unlike LPOP/RPOP
        // there's no null-array case here
        resp::array_header(ctx.out, 0);
        return;
    }
    ZSet* zset = as_zset_or_error(ctx, v);
    if (zset == nullptr) {
        return;
    }

    Tree& tree = zset->tree();
    const int64_t n = std::min<int64_t>(count, static_cast<int64_t>(zset->size()));
    resp::array_header(ctx.out, n * 2);
    for (int64_t i = 0; i < n; ++i) {
        Node* node = tree.select(0); // smallest remaining
        const std::string member = node->key.member; // copy before erase() invalidates it
        const double score = node->key.score;
        zset->erase(member);
        resp::bulk(ctx.out, member);
        resp::double_as_bulk(ctx.out, score);
    }
    if (zset->empty()) {
        ctx.db.del(argv[1]);
    }
}

} // namespace gredis
