// drives dispatch() directly with a Connection wrapping an invalid Fd --
// safe since none of these handlers touch the socket itself
#include "test_harness.h"

#include <initializer_list>
#include <string>
#include <vector>

#include "commands/dispatcher.h"
#include "config.h"
#include "net/buffer.h"
#include "net/connection.h"
#include "net/server.h"
#include "storage/database.h"

using gredis::Buffer;
using gredis::CommandContext;
using gredis::Config;
using gredis::Connection;
using gredis::Database;
using gredis::Fd;
using gredis::Server;
using gredis::dispatch;

namespace {

std::string readable_str(const Buffer& b) {
    return std::string(b.readable());
}

std::vector<std::string> args(std::initializer_list<std::string> a) {
    return std::vector<std::string>(a);
}

// none of these handlers submit background work, so one shared Server is
// enough -- CommandContext just needs a live Server& to construct
Server& shared_test_server() {
    static Config cfg;
    static Server server(cfg);
    return server;
}

} // namespace

TEST(ping_no_args_replies_pong) {
    Buffer out;
    Connection conn(Fd(), 1, 0);
    Database db;
    CommandContext ctx{out, conn, db, shared_test_server()};
    auto argv = args({"PING"});
    dispatch(ctx, argv);
    CHECK_EQ(readable_str(out), std::string("+PONG\r\n"));
}

TEST(ping_with_message_replies_bulk) {
    Buffer out;
    Connection conn(Fd(), 1, 0);
    Database db;
    CommandContext ctx{out, conn, db, shared_test_server()};
    auto argv = args({"PING", "hello"});
    dispatch(ctx, argv);
    CHECK_EQ(readable_str(out), std::string("$5\r\nhello\r\n"));
}

TEST(ping_with_too_many_args_is_wrong_arity) {
    Buffer out;
    Connection conn(Fd(), 1, 0);
    Database db;
    CommandContext ctx{out, conn, db, shared_test_server()};
    auto argv = args({"PING", "a", "b"});
    dispatch(ctx, argv);
    CHECK_EQ(readable_str(out), std::string("-ERR wrong number of arguments for 'PING' command\r\n"));
}

TEST(echo_replies_bulk) {
    Buffer out;
    Connection conn(Fd(), 1, 0);
    Database db;
    CommandContext ctx{out, conn, db, shared_test_server()};
    auto argv = args({"ECHO", "hi there"});
    dispatch(ctx, argv);
    CHECK_EQ(readable_str(out), std::string("$8\r\nhi there\r\n"));
}

TEST(echo_with_zero_extra_args_is_wrong_arity) {
    Buffer out;
    Connection conn(Fd(), 1, 0);
    Database db;
    CommandContext ctx{out, conn, db, shared_test_server()};
    auto argv = args({"ECHO"});
    dispatch(ctx, argv);
    CHECK_EQ(readable_str(out), std::string("-ERR wrong number of arguments for 'ECHO' command\r\n"));
}

TEST(quit_replies_ok_and_marks_want_close) {
    Buffer out;
    Connection conn(Fd(), 1, 0);
    Database db;
    CommandContext ctx{out, conn, db, shared_test_server()};
    auto argv = args({"QUIT"});
    CHECK(!conn.want_close);
    dispatch(ctx, argv);
    CHECK_EQ(readable_str(out), std::string("+OK\r\n"));
    CHECK(conn.want_close);
}

TEST(command_stub_returns_empty_array) {
    Buffer out;
    Connection conn(Fd(), 1, 0);
    Database db;
    CommandContext ctx{out, conn, db, shared_test_server()};
    auto argv = args({"COMMAND"});
    dispatch(ctx, argv);
    CHECK_EQ(readable_str(out), std::string("*0\r\n"));
}

TEST(config_get_stub_returns_empty_array) {
    Buffer out;
    Connection conn(Fd(), 1, 0);
    Database db;
    CommandContext ctx{out, conn, db, shared_test_server()};
    auto argv = args({"CONFIG", "GET", "save"});
    dispatch(ctx, argv);
    CHECK_EQ(readable_str(out), std::string("*0\r\n"));
}

TEST(config_with_no_subcommand_is_wrong_arity) {
    Buffer out;
    Connection conn(Fd(), 1, 0);
    Database db;
    CommandContext ctx{out, conn, db, shared_test_server()};
    auto argv = args({"CONFIG"});
    dispatch(ctx, argv);
    CHECK_EQ(readable_str(out), std::string("-ERR wrong number of arguments for 'CONFIG' command\r\n"));
}

TEST(hello_is_an_error) {
    Buffer out;
    Connection conn(Fd(), 1, 0);
    Database db;
    CommandContext ctx{out, conn, db, shared_test_server()};
    auto argv = args({"HELLO", "3"});
    dispatch(ctx, argv);
    CHECK_EQ(readable_str(out), std::string("-ERR unknown command 'HELLO'\r\n"));
}

TEST(unknown_command_is_an_error_and_lists_args) {
    Buffer out;
    Connection conn(Fd(), 1, 0);
    Database db;
    CommandContext ctx{out, conn, db, shared_test_server()};
    auto argv = args({"FROBNICATE", "a", "b"});
    dispatch(ctx, argv);
    CHECK_EQ(readable_str(out),
             std::string("-ERR unknown command 'FROBNICATE', with args beginning with: 'a', 'b', \r\n"));
}

TEST(unknown_command_with_no_args_lists_nothing) {
    Buffer out;
    Connection conn(Fd(), 1, 0);
    Database db;
    CommandContext ctx{out, conn, db, shared_test_server()};
    auto argv = args({"FROBNICATE"});
    dispatch(ctx, argv);
    CHECK_EQ(readable_str(out),
             std::string("-ERR unknown command 'FROBNICATE', with args beginning with: \r\n"));
}

TEST(command_names_are_case_insensitive) {
    Buffer out;
    Connection conn(Fd(), 1, 0);
    Database db;
    CommandContext ctx{out, conn, db, shared_test_server()};
    auto argv = args({"PiNg"});
    dispatch(ctx, argv);
    CHECK_EQ(readable_str(out), std::string("+PONG\r\n"));
}

TEST(mixed_case_echo_still_dispatches) {
    Buffer out;
    Connection conn(Fd(), 1, 0);
    Database db;
    CommandContext ctx{out, conn, db, shared_test_server()};
    auto argv = args({"EcHo", "x"});
    dispatch(ctx, argv);
    CHECK_EQ(readable_str(out), std::string("$1\r\nx\r\n"));
}

TEST_MAIN()
