// set commands: SADD, SREM, SISMEMBER, SMEMBERS, SCARD, SPOP, SINTER
#pragma once

#include <string>
#include <vector>

#include "commands/dispatcher.h"

namespace gredis {

void cmd_sadd(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_srem(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_sismember(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_smembers(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_scard(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_spop(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_sinter(CommandContext& ctx, std::vector<std::string>& argv);

} // namespace gredis
