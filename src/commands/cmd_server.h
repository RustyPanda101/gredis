// server/connection commands: PING, ECHO, QUIT, COMMAND, CONFIG GET, HELLO
#pragma once

#include <string>
#include <vector>

#include "commands/dispatcher.h"

namespace gredis {

void cmd_ping(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_echo(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_quit(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_command(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_config(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_hello(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_dbsize(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_flushall(CommandContext& ctx, std::vector<std::string>& argv);

} // namespace gredis
