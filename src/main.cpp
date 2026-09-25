#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "net/server.h"
#include "util/log.h"

int main(int argc, char** argv) {
    // block before any thread exists (incl. thread pool workers spawned in
    // Server's ctor) -- threads inherit the signal mask at creation, so this
    // makes both signals arrive only via Server::run()'s signalfd
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        std::fprintf(stderr, "gredis-server: pthread_sigmask failed: %s\n", std::strerror(errno));
        return 1;
    }
    // MSG_NOSIGNAL on our send() calls already covers this, but ignore it
    // globally too as defense in depth
    std::signal(SIGPIPE, SIG_IGN);

    const gredis::ParseResult result = gredis::parse_args(argc, argv);

    if (result.status == gredis::ParseStatus::HelpRequested) {
        std::fputs(gredis::usage_text().c_str(), stdout);
        return 0;
    }
    if (result.status == gredis::ParseStatus::Error) {
        std::fprintf(stderr, "gredis-server: %s\n\n", result.error.c_str());
        std::fputs(gredis::usage_text().c_str(), stderr);
        return 2;
    }

    const gredis::Config& cfg = result.config;
    gredis::g_log_level = cfg.log_level;

    LOG_INFO("gredis-server (RESP2 + PING/ECHO/QUIT/COMMAND/CONFIG/HELLO)");
    LOG_INFO("config: bind=%s port=%u maxclients=%d idle_timeout_sec=%d threads=%d snapshot=%s",
              cfg.bind.c_str(), static_cast<unsigned>(cfg.port), cfg.maxclients,
              cfg.idle_timeout_sec, cfg.threads,
              cfg.snapshot_path.empty() ? "(disabled)" : cfg.snapshot_path.c_str());

    gredis::Server server(cfg);
    return server.run();
}
