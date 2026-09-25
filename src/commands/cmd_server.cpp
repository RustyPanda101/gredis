#include "commands/cmd_server.h"

#include <cctype>
#include <cstdint>
#include <memory>
#include <utility>

#include "net/connection.h"
#include "net/server.h"
#include "protocol/resp_writer.h"
#include "storage/database.h"

namespace gredis {

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

void cmd_ping(CommandContext& ctx, std::vector<std::string>& argv) {
    // dispatcher arity only guarantees "at least 1"; check the 0-or-1-extra-arg bound here
    if (argv.size() > 2) {
        resp::error(ctx.out, "ERR wrong number of arguments for '" + argv[0] + "' command");
        return;
    }
    if (argv.size() == 1) {
        resp::simple(ctx.out, "PONG");
    } else {
        resp::bulk(ctx.out, argv[1]);
    }
}

void cmd_echo(CommandContext& ctx, std::vector<std::string>& argv) {
    resp::bulk(ctx.out, argv[1]);
}

void cmd_quit(CommandContext& ctx, std::vector<std::string>& /*argv*/) {
    resp::simple(ctx.out, "OK");
    // closed once the reply actually flushes, not immediately here (deferred close)
    ctx.conn.want_close = true;
}

void cmd_command(CommandContext& ctx, std::vector<std::string>& /*argv*/) {
    // stub: empty array keeps redis-cli's startup probe happy
    resp::array_header(ctx.out, 0);
}

void cmd_config(CommandContext& ctx, std::vector<std::string>& /*argv*/) {
    // stub: redis-benchmark asks for a few config values at startup, empty reply is fine
    resp::array_header(ctx.out, 0);
}

void cmd_hello(CommandContext& ctx, std::vector<std::string>& /*argv*/) {
    // no RESP3 support; refusing HELLO makes clients fall back to RESP2
    resp::error(ctx.out, "ERR unknown command 'HELLO'");
}

void cmd_dbsize(CommandContext& ctx, std::vector<std::string>& /*argv*/) {
    resp::integer(ctx.out, static_cast<int64_t>(ctx.db.size()));
}

void cmd_flushall(CommandContext& ctx, std::vector<std::string>& argv) {
    // dispatcher can't express the 0-or-1-extra-arg bound, so check ASYNC here (same as ping)
    if (argv.size() > 2 || (argv.size() == 2 && !ieq(argv[1], "async"))) {
        resp::error(ctx.out, "ERR syntax error");
        return;
    }
    if (argv.size() == 2) {
        // ASYNC: take_all() swaps in a fresh empty keyspace and hands the
        // old one to a worker to free. shared_ptr here just satisfies
        // std::function's copy-constructible requirement (same as cmd_unlink).
        auto detached = std::make_shared<DetachedKeyspace>(ctx.db.take_all());
        ctx.server.run_in_background([detached] { (void)detached; });
    } else {
        ctx.db.clear();
    }
    resp::simple(ctx.out, "OK");
}

} // namespace gredis
