// key commands: DEL, EXISTS, TYPE, EXPIRE family, TTL, PERSIST
#pragma once

#include <string>
#include <vector>

#include "commands/dispatcher.h"

namespace gredis {

void cmd_del(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_unlink(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_exists(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_type(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_expire(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_pexpire(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_expireat(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_pexpireat(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_ttl(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_pttl(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_persist(CommandContext& ctx, std::vector<std::string>& argv);

} // namespace gredis
