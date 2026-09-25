#include "commands/dispatcher.h"

#include <array>
#include <cctype>
#include <string_view>
#include <unordered_map>

#include "commands/cmd_hash.h"
#include "commands/cmd_keys.h"
#include "commands/cmd_list.h"
#include "commands/cmd_server.h"
#include "commands/cmd_set.h"
#include "commands/cmd_string.h"
#include "commands/cmd_persist.h"
#include "commands/cmd_zset.h"
#include "protocol/resp_writer.h"

namespace gredis {

namespace {

constexpr CommandSpec kCommands[] = {
    {"ping", cmd_ping, -1},
    {"echo", cmd_echo, 2},
    {"quit", cmd_quit, 1},
    {"command", cmd_command, -1},
    {"config", cmd_config, -2}, // CONFIG GET <param>: name + at least a subcommand
    {"hello", cmd_hello, -1},
    {"dbsize", cmd_dbsize, 1},
    {"flushall", cmd_flushall, -1}, // optional ASYNC arg checked in the handler, like ping

    // strings + keyspace basics
    {"get", cmd_get, 2},
    {"set", cmd_set, -3}, // key + value + up to 4 option tokens (EX/PX n, NX|XX)
    {"mget", cmd_mget, -2},
    {"mset", cmd_mset, -3}, // at least one pair; oddness checked in the handler
    {"incr", cmd_incr, 2},
    {"decr", cmd_decr, 2},
    {"incrby", cmd_incrby, 3},
    {"decrby", cmd_decrby, 3},
    {"append", cmd_append, 3},
    {"strlen", cmd_strlen, 2},
    {"del", cmd_del, -2},
    {"unlink", cmd_unlink, -2}, // same arity as DEL, frees big values in the background
    {"exists", cmd_exists, -2},
    {"type", cmd_type, 2},

    // TTL commands
    {"expire", cmd_expire, 3},
    {"pexpire", cmd_pexpire, 3},
    {"expireat", cmd_expireat, 3},
    {"pexpireat", cmd_pexpireat, 3},
    {"ttl", cmd_ttl, 2},
    {"pttl", cmd_pttl, 2},
    {"persist", cmd_persist, 2},

    // hashes
    {"hset", cmd_hset, -4},   // key + at least one field/value pair; evenness checked in the handler
    {"hget", cmd_hget, 3},
    {"hmget", cmd_hmget, -3},
    {"hdel", cmd_hdel, -3},
    {"hexists", cmd_hexists, 3},
    {"hlen", cmd_hlen, 2},
    {"hgetall", cmd_hgetall, 2},
    {"hkeys", cmd_hkeys, 2},
    {"hvals", cmd_hvals, 2},
    {"hincrby", cmd_hincrby, 4},

    // lists
    {"lpush", cmd_lpush, -3},
    {"rpush", cmd_rpush, -3},
    {"lpop", cmd_lpop, -2},  // optional count arg; upper bound checked in the handler
    {"rpop", cmd_rpop, -2},
    {"llen", cmd_llen, 2},
    {"lrange", cmd_lrange, 4},
    {"lindex", cmd_lindex, 3},

    // sets
    {"sadd", cmd_sadd, -3},
    {"srem", cmd_srem, -3},
    {"sismember", cmd_sismember, 3},
    {"smembers", cmd_smembers, 2},
    {"scard", cmd_scard, 2},
    {"spop", cmd_spop, -2}, // optional count arg; upper bound checked in the handler
    {"sinter", cmd_sinter, -2},

    // sorted sets
    {"zadd", cmd_zadd, -4}, // key + at least one score/member pair; options and evenness checked in the handler
    {"zrem", cmd_zrem, -3},
    {"zscore", cmd_zscore, 3},
    {"zincrby", cmd_zincrby, 4},
    {"zcard", cmd_zcard, 2},
    {"zrank", cmd_zrank, 3},
    {"zrevrank", cmd_zrevrank, 3},
    {"zrange", cmd_zrange, -4},        // optional WITHSCORES arg
    {"zrevrange", cmd_zrevrange, -4},  // optional WITHSCORES arg
    {"zrangebyscore", cmd_zrangebyscore, -4}, // optional WITHSCORES / LIMIT off count
    {"zcount", cmd_zcount, 4},
    {"zpopmin", cmd_zpopmin, -2}, // optional count arg

    // persistence
    {"save", cmd_save, 1},
    {"bgsave", cmd_bgsave, 1},
    {"lastsave", cmd_lastsave, 1},
};

// lets the table be looked up by string_view, no temporary std::string needed
struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

using CommandTable =
    std::unordered_map<std::string, const CommandSpec*, TransparentStringHash, std::equal_to<>>;

const CommandTable& command_table() {
    static const CommandTable table = [] {
        CommandTable t;
        for (const auto& spec : kCommands) {
            t.emplace(spec.name, &spec);
        }
        return t;
    }();
    return table;
}

// longest real command name is "zrangebyscore" (13 chars); anything longer
// can't match anyway, so skip lowercasing instead of sizing this exactly
constexpr size_t kMaxCommandNameLen = 15;

bool arity_ok(int arity, size_t argc) {
    if (arity >= 0) {
        return argc == static_cast<size_t>(arity);
    }
    return argc >= static_cast<size_t>(-arity);
}

} // namespace

void dispatch(CommandContext& ctx, std::vector<std::string>& argv) {
    // argv is never empty here (server drops the "*0\r\n" case earlier).
    // lowercase into a stack buffer instead of allocating a string per
    // dispatch -- profiling showed tolower()'s heap alloc costing real time.
    const std::string_view raw_name = argv[0];
    const CommandSpec* spec = nullptr;
    if (raw_name.size() <= kMaxCommandNameLen) {
        std::array<char, kMaxCommandNameLen> lowered{};
        for (size_t i = 0; i < raw_name.size(); ++i) {
            lowered[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(raw_name[i])));
        }
        const auto& table = command_table();
        const auto it = table.find(std::string_view(lowered.data(), raw_name.size()));
        if (it != table.end()) {
            spec = it->second;
        }
    }

    if (spec == nullptr) {
        std::string msg = "ERR unknown command '" + argv[0] + "', with args beginning with: ";
        for (size_t i = 1; i < argv.size(); ++i) {
            msg += "'" + argv[i] + "', ";
        }
        resp::error(ctx.out, msg);
        return;
    }

    if (!arity_ok(spec->arity, argv.size())) {
        resp::error(ctx.out, "ERR wrong number of arguments for '" + argv[0] + "' command");
        return;
    }

    spec->handler(ctx, argv);
}

} // namespace gredis
