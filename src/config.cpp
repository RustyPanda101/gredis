#include "config.h"

#include <charconv>

namespace gredis {

namespace {

// whole string must be consumed, leading '+' rejected -- flags are untrusted input too
bool strict_parse_int(const std::string& s, long long& out) {
    if (s.empty() || s.front() == '+') {
        return false;
    }
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

bool parse_log_level(const std::string& s, LogLevel& out) {
    if (s == "debug") { out = LogLevel::Debug; return true; }
    if (s == "info") { out = LogLevel::Info; return true; }
    if (s == "warn") { out = LogLevel::Warn; return true; }
    if (s == "error") { out = LogLevel::Error; return true; }
    return false;
}

ParseResult fail(std::string message) {
    ParseResult r;
    r.status = ParseStatus::Error;
    r.error = std::move(message);
    return r;
}

} // namespace

std::string usage_text() {
    return
        "gredis-server [options]\n"
        "\n"
        "  --bind <addr>              address to listen on (default 127.0.0.1)\n"
        "  --port <1-65535>           TCP port to listen on (default 6380)\n"
        "  --maxclients <n>           max simultaneous client connections (default 10000)\n"
        "  --idle-timeout-sec <n>     disconnect clients idle longer than this; 0 disables (default 0)\n"
        "  --threads <n>              background thread-pool worker count (default 2)\n"
        "  --snapshot <path>          snapshot file to load at startup / save to (default: disabled)\n"
        "  --log-level <level>        debug|info|warn|error (default info)\n"
        "  --client-output-soft-limit <bytes>  pause reading from a client above this "
        "much queued output (default 1048576)\n"
        "  --client-output-hard-limit <bytes>  disconnect a client above this much "
        "queued output (default 268435456)\n"
        "  --max-query-buf <bytes>    protocol-error and disconnect a client whose "
        "unprocessed input grows past this (default 134217728)\n"
        "  --help                     print this message and exit\n";
}

ParseResult parse_args(int argc, char** argv) {
    Config cfg;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            ParseResult r;
            r.status = ParseStatus::HelpRequested;
            return r;
        }

        auto next_value = [&]() -> const char* {
            if (i + 1 >= argc) {
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--bind") {
            const char* v = next_value();
            if (!v) return fail("missing value for '--bind'");
            cfg.bind = v;
        } else if (arg == "--port") {
            const char* v = next_value();
            if (!v) return fail("missing value for '--port'");
            long long n = 0;
            if (!strict_parse_int(v, n) || n < 1 || n > 65535) {
                return fail("invalid value for '--port': '" + std::string(v) + "' (must be 1-65535)");
            }
            cfg.port = static_cast<uint16_t>(n);
        } else if (arg == "--maxclients") {
            const char* v = next_value();
            if (!v) return fail("missing value for '--maxclients'");
            long long n = 0;
            if (!strict_parse_int(v, n) || n < 1) {
                return fail("invalid value for '--maxclients': '" + std::string(v) + "' (must be >= 1)");
            }
            cfg.maxclients = static_cast<int>(n);
        } else if (arg == "--idle-timeout-sec") {
            const char* v = next_value();
            if (!v) return fail("missing value for '--idle-timeout-sec'");
            long long n = 0;
            if (!strict_parse_int(v, n) || n < 0) {
                return fail("invalid value for '--idle-timeout-sec': '" + std::string(v) + "' (must be >= 0)");
            }
            cfg.idle_timeout_sec = static_cast<int>(n);
        } else if (arg == "--threads") {
            const char* v = next_value();
            if (!v) return fail("missing value for '--threads'");
            long long n = 0;
            if (!strict_parse_int(v, n) || n < 1 || n > 64) {
                return fail("invalid value for '--threads': '" + std::string(v) + "' (must be 1-64)");
            }
            cfg.threads = static_cast<int>(n);
        } else if (arg == "--snapshot") {
            const char* v = next_value();
            if (!v) return fail("missing value for '--snapshot'");
            cfg.snapshot_path = v;
        } else if (arg == "--log-level") {
            const char* v = next_value();
            if (!v) return fail("missing value for '--log-level'");
            if (!parse_log_level(v, cfg.log_level)) {
                return fail("invalid value for '--log-level': '" + std::string(v) +
                            "' (must be debug|info|warn|error)");
            }
        } else if (arg == "--client-output-soft-limit") {
            const char* v = next_value();
            if (!v) return fail("missing value for '--client-output-soft-limit'");
            long long n = 0;
            if (!strict_parse_int(v, n) || n < 1) {
                return fail("invalid value for '--client-output-soft-limit': '" + std::string(v) +
                            "' (must be >= 1)");
            }
            cfg.output_soft_limit_bytes = static_cast<size_t>(n);
        } else if (arg == "--client-output-hard-limit") {
            const char* v = next_value();
            if (!v) return fail("missing value for '--client-output-hard-limit'");
            long long n = 0;
            if (!strict_parse_int(v, n) || n < 1) {
                return fail("invalid value for '--client-output-hard-limit': '" + std::string(v) +
                            "' (must be >= 1)");
            }
            cfg.output_hard_limit_bytes = static_cast<size_t>(n);
        } else if (arg == "--max-query-buf") {
            const char* v = next_value();
            if (!v) return fail("missing value for '--max-query-buf'");
            long long n = 0;
            if (!strict_parse_int(v, n) || n < 1) {
                return fail("invalid value for '--max-query-buf': '" + std::string(v) + "' (must be >= 1)");
            }
            cfg.max_query_buf = static_cast<size_t>(n);
        } else {
            return fail("unknown option '" + arg + "'");
        }
    }

    if (cfg.output_soft_limit_bytes > cfg.output_hard_limit_bytes) {
        return fail("--client-output-soft-limit must not be greater than "
                     "--client-output-hard-limit");
    }

    ParseResult r;
    r.status = ParseStatus::Ok;
    r.config = cfg;
    return r;
}

} // namespace gredis
