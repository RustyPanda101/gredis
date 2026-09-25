// persistence commands: SAVE, BGSAVE, LASTSAVE
#pragma once

#include <string>
#include <vector>

#include "commands/dispatcher.h"

namespace gredis {

void cmd_save(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_bgsave(CommandContext& ctx, std::vector<std::string>& argv);
void cmd_lastsave(CommandContext& ctx, std::vector<std::string>& argv);

} // namespace gredis
