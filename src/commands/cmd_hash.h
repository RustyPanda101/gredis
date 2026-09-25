// hash commands: HSET, HGET, HMGET, HDEL, HEXISTS, HLEN, HGETALL, HKEYS, HVALS, HINCRBY
#pragma once

#include <string>
#include <vector>

#include "commands/dispatcher.h"

namespace gredis {

void cmd_hset(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_hget(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_hmget(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_hdel(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_hexists(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_hlen(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_hgetall(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_hkeys(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_hvals(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_hincrby(CommandContext& ctx, std::vector<std::string>& argv);

} // namespace gredis
