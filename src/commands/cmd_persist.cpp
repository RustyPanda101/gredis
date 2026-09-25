#include "commands/cmd_persist.h"

#include "net/server.h"
#include "protocol/resp_writer.h"

namespace gredis {

void cmd_save(CommandContext& ctx, std::vector<std::string>& /*argv*/) {
    std::string error;
    if (!ctx.server.save_snapshot(error)) {
        resp::error(ctx.out, "ERR " + error);
        return;
    }
    resp::simple(ctx.out, "OK");
}

void cmd_bgsave(CommandContext& ctx, std::vector<std::string>& /*argv*/) {
    std::string error;
    if (!ctx.server.start_bgsave(error)) {
        resp::error(ctx.out, "ERR " + error);
        return;
    }
    resp::simple(ctx.out, "Background saving started");
}

void cmd_lastsave(CommandContext& ctx, std::vector<std::string>& /*argv*/) {
    resp::integer(ctx.out, ctx.server.last_save_unix_s());
}

} // namespace gredis
