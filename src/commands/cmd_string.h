// string commands: GET, SET, MGET, MSET, INCR, DECR, INCRBY, DECRBY, APPEND, STRLEN
#pragma once

#include <string>
#include <vector>

#include "commands/dispatcher.h"

namespace gredis {

void cmd_get(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_set(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_mget(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_mset(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_incr(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_decr(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_incrby(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_decrby(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_append(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_strlen(CommandContext& ctx, std::vector<std::string>& argv);

} // namespace gredis
