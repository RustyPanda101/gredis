#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "util/log.h"

namespace gredis {

struct Config {
    std::string bind = "127.0.0.1";
    uint16_t port = 6380;
    int maxclients = 10000;
    int idle_timeout_sec = 0; // 0 = disabled (matches Redis's default)
    int threads = 2;          // thread-pool worker count
    std::string snapshot_path; // empty = persistence disabled
    LogLevel log_level = LogLevel::Info;

    // above soft limit, pause reading until drained; above hard, disconnect.
    // configurable so tests can lower them instead of pushing 256 MiB through a socket
    size_t output_soft_limit_bytes = 1024ULL * 1024;       // 1 MiB
    size_t output_hard_limit_bytes = 256ULL * 1024 * 1024; // 256 MiB

    // inbound buffer cap; over this, protocol error + close. configurable
    // so tests can lower it instead of pushing 128 MiB through a socket
    size_t max_query_buf = 128ULL * 1024 * 1024; // 128 MiB
};

enum class ParseStatus { Ok, HelpRequested, Error };

struct ParseResult {
    ParseStatus status = ParseStatus::Error;
    Config config;
    std::string error; // populated only when status == Error
};

// never calls exit() or prints itself -- main.cpp decides that, and it keeps
// this unit-testable without spawning a process
ParseResult parse_args(int argc, char** argv);

std::string usage_text();

} // namespace gredis
