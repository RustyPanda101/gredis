// sorted-set commands: ZADD, ZREM, ZSCORE, ZINCRBY, ZCARD, ZRANK,
// ZREVRANK, ZRANGE, ZREVRANGE, ZRANGEBYSCORE, ZCOUNT, ZPOPMIN
#pragma once

#include <string>
#include <vector>

#include "commands/dispatcher.h"

namespace gredis {

void cmd_zadd(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_zrem(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_zscore(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_zincrby(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_zcard(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_zrank(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_zrevrank(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_zrange(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_zrevrange(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_zrangebyscore(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_zcount(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_zpopmin(CommandContext& ctx, std::vector<std::string>& argv);

} // namespace gredis
