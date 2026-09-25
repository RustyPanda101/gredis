// list commands: LPUSH, RPUSH, LPOP, RPOP, LLEN, LRANGE, LINDEX
#pragma once

#include <string>
#include <vector>

#include "commands/dispatcher.h"

namespace gredis {

void cmd_lpush(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_rpush(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_lpop(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_rpop(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_llen(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_lrange(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_lindex(CommandContext& ctx, std::vector<std::string>& argv);

} // namespace gredis
