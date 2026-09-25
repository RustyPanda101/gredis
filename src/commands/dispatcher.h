// command dispatch: name -> handler + arity, with an arity check
#pragma once

#include <string>
#include <vector>

#include "net/buffer.h"

namespace gredis {

struct Connection; // only handlers that need it (QUIT) include the full header
class Database;    // only handlers touching data include the full header
class Server;       // only handlers using background work or server state include the full header

struct CommandContext {
    Buffer& out;
    Connection& conn;
    Database& db;
    Server& server;
};

using CommandHandler = void (*)(CommandContext&, std::vector<std::string>& argv);

struct CommandSpec {
    const char* name; // lowercase
    CommandHandler handler;
    // positive N = exactly N argv entries, negative -N = at least N.
    // a bounded range (e.g. ping's 0-or-1 extra arg) uses the loose bound
    // here and checks the tighter one in the handler itself.
    int arity;
};

// looks up argv[0] case-insensitively, arity-checks, calls the handler.
// unknown command / wrong arity -> error reply, connection stays open
// (a protocol parse error closes the connection, but that happens before dispatch())
void dispatch(CommandContext& ctx, std::vector<std::string>& argv);

} // namespace gredis
