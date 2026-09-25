// libFuzzer entry point over the RESP2 parser + dispatcher, no sockets.
// Only built under -DGREDIS_SANITIZE=fuzzer (clang, needs libFuzzer).
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "commands/dispatcher.h"
#include "config.h"
#include "net/buffer.h"
#include "net/connection.h"
#include "net/server.h"
#include "protocol/resp_parser.h"
#include "storage/database.h"
#include "util/fd.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // static: Server spins up a real thread pool, too expensive to redo per input.
    // default Config has no --snapshot path, so SAVE/BGSAVE just errors, no disk I/O.
    static const gredis::Config cfg;
    static gredis::Server server(cfg);

    gredis::Database db;
    gredis::Buffer out;
    gredis::Connection conn(gredis::Fd(), 1, 0);

    const std::span<const char> input(reinterpret_cast<const char*>(data), size);
    size_t offset = 0;
    while (offset < size) {
        std::vector<std::string> argv;
        size_t consumed = 0;
        std::string err;
        const auto status = gredis::parse_command(input.subspan(offset), argv, consumed, err);
        if (status != gredis::RespParseStatus::Ok) {
            break;
        }
        offset += consumed;
        if (argv.empty()) {
            continue; // bare "*0\r\n", no command
        }
        gredis::CommandContext ctx{out, conn, db, server};
        gredis::dispatch(ctx, argv);
    }
    return 0;
}
