#include "net/server.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "commands/dispatcher.h"
#include "net/socket_util.h"
#include "persistence/snapshot.h"
#include "protocol/resp_parser.h"
#include "protocol/resp_writer.h"
#include "util/clock.h"
#include "util/log.h"

namespace gredis {

namespace {

// cap accepts per wakeup so a connection storm can't starve existing clients
constexpr int kMaxAcceptsPerWakeup = 1000;

// cap reads per client per wakeup (4 x 16 KiB) so one firehose client can't starve others
constexpr size_t kReadChunkSize = 16 * 1024;
constexpr int kMaxRecvsPerWakeup = 4;

// fallback poll timeout if the timer queue is ever empty (shouldn't happen, cron re-arms itself)
constexpr int kPollTimeoutMs = 1000;

constexpr int64_t kCronIntervalMs = 100; // 10Hz cron tick

// whichever limit hits first per cron tick, keys or wall time
// (old 200/1ms was too slow to drain a real backlog in time, bumped it up)
constexpr size_t kExpiryBudgetKeys = 2000;
constexpr double kExpiryBudgetWallMs = 10.0;

// rehash buckets migrated per cron tick, so an idle server still finishes a rehash
constexpr size_t kCronRehashBudget = 100;

Fd create_completion_eventfd() {
    // non-blocking to be safe even though level-triggered epoll only reads
    // this when there's actually a count; CLOEXEC matches every other fd here
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR("eventfd() failed: %s", std::strerror(errno));
        std::exit(1);
    }
    return Fd(fd);
}

// main.cpp already blocked SIGINT/SIGTERM via pthread_sigmask() before any
// thread existed, so they arrive here instead of via an async handler
Fd create_signal_fd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    const int fd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR("signalfd() failed: %s", std::strerror(errno));
        std::exit(1);
    }
    return Fd(fd);
}

} // namespace

Server::Server(const Config& cfg)
    : cfg_(cfg), lastsave_unix_ms_(unix_ms()), thread_pool_(static_cast<size_t>(cfg.threads)) {}

int Server::run() {
    if (!cfg_.snapshot_path.empty()) {
        load_snapshot_at_startup();
    }
    listener_ = create_listener(cfg_.bind, cfg_.port, /*backlog=*/128);
    loop_.add(listener_.get(), /*readable=*/true, /*writable=*/false);
    completion_eventfd_ = create_completion_eventfd();
    loop_.add(completion_eventfd_.get(), /*readable=*/true, /*writable=*/false);
    signal_fd_ = create_signal_fd();
    loop_.add(signal_fd_.get(), /*readable=*/true, /*writable=*/false);
    // held open unused, so on_listener_readable() can free it briefly on EMFILE
    reserve_fd_ = Fd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (!reserve_fd_) {
        LOG_ERROR("open(/dev/null) for the fd reserve failed: %s", std::strerror(errno));
        std::exit(1);
    }
    LOG_INFO("listening on %s:%u (epoll, RESP2 + command dispatch, %d background thread(s))",
              cfg_.bind.c_str(), static_cast<unsigned>(cfg_.port), cfg_.threads);

    // re-arms itself every tick forever, see cron_tick(); the only timer
    // registered here -- TTLs live in Database's own expiry heap, not per-key timers
    timers_.add_timer(monotonic_ms(), [this] { cron_tick(); });

    while (!shutting_down_) {
        const int64_t timeout_ms = timers_.next_timeout_ms(monotonic_ms());
        loop_.run_once(timeout_ms >= 0 ? static_cast<int>(timeout_ms) : kPollTimeoutMs,
                        [this](int fd, Readiness r) {
                            if (fd == listener_.get()) {
                                on_listener_readable();
                            } else if (fd == completion_eventfd_.get()) {
                                on_completion_readable();
                            } else if (fd == signal_fd_.get()) {
                                on_signal_readable();
                            } else {
                                on_client_event(fd, r);
                            }
                        });
        // deferred close: wait until the whole batch is handled, otherwise a fd
        // freed by one close() could get reused by accept() and collide with a
        // stale event for the old connection later in this same batch
        reap_closed();

        timers_.run_due(monotonic_ms());
    }

    // stop accepting, SAVE if configured, close connections; thread pool joins
    // implicitly via ~ThreadPool() once Server goes out of scope in main()
    LOG_INFO("shutting down");
    loop_.remove(listener_.get());
    if (!cfg_.snapshot_path.empty()) {
        std::string error;
        if (!save_snapshot(error)) {
            LOG_WARN("shutdown SAVE failed: %s", error.c_str());
        }
    }
    LOG_INFO("closing %zu connection(s)", conns_.size());
    conns_.clear(); // each Connection's destructor closes its socket
    idle_list_.clear(); // conns_ owned every pointer in here
    return 0;
}

void Server::cron_tick() {
    const int64_t now = monotonic_ms();
    const size_t expired = db_.run_active_expiration_cycle(now, kExpiryBudgetKeys, kExpiryBudgetWallMs);
    if (expired > 0) {
        LOG_DEBUG("active expiration: %zu key(s) expired this cycle", expired);
    }
    db_.rehash_step(kCronRehashBudget);
    sweep_idle_connections(now);
    timers_.add_timer(now + kCronIntervalMs, [this] { cron_tick(); });
}

void Server::on_listener_readable() {
    for (int i = 0; i < kMaxAcceptsPerWakeup; ++i) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        const int raw_fd = ::accept4(listener_.get(), reinterpret_cast<sockaddr*>(&client_addr),
                                      &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (raw_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return; // drained
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EMFILE || errno == ENFILE) {
                // out of fds: accept4() would keep failing forever and the listener
                // stays readable under level-triggered epoll, spinning the loop at
                // 100% CPU. free the reserve fd, accept+drop the one pending conn
                // it lets through, then reopen the reserve.
                LOG_WARN("%s: out of file descriptors, dropping one pending connection",
                          std::strerror(errno));
                reserve_fd_.reset();
                const int dropped =
                    ::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (dropped >= 0) {
                    ::close(dropped);
                }
                reserve_fd_ = Fd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
                return;
            }
            // connection can die between being queued and accept()ing it (ECONNABORTED etc) -- not fatal
            LOG_WARN("accept4() failed: %s", std::strerror(errno));
            continue;
        }

        if (conns_.size() >= static_cast<size_t>(cfg_.maxclients)) {
            // best-effort send, not retried -- client's getting dropped either way
            static constexpr std::string_view kMaxClientsMsg = "-ERR max number of clients reached\r\n";
            ::send(raw_fd, kMaxClientsMsg.data(), kMaxClientsMsg.size(), MSG_NOSIGNAL);
            ::close(raw_fd);
            continue;
        }

        set_tcp_nodelay(raw_fd);

        auto conn = std::make_unique<Connection>(Fd(raw_fd), next_connection_id_++, monotonic_ms());
        loop_.add(conn->fd.get(), /*readable=*/true, /*writable=*/false);
        idle_list_.push_back(conn.get());
        conn->idle_iter = std::prev(idle_list_.end());
        LOG_DEBUG("client connected (fd=%d, total=%zu)", raw_fd, conns_.size() + 1);
        conns_.emplace(raw_fd, std::move(conn));
    }
    LOG_DEBUG("accept loop hit its per-wakeup cap (%d)", kMaxAcceptsPerWakeup);
}

void Server::on_client_event(int fd, Readiness r) {
    const auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return; // shouldn't happen: conns_ never mutates mid-batch, guarded anyway
    }
    Connection& conn = *it->second;

    // EPOLLERR/EPOLLHUP can arrive with data still unread, so read first
    // regardless, then let handle_read()'s recv() set want_close as needed
    if (r.readable || r.error || r.hangup) {
        handle_read(conn);
    }
    if (r.error) {
        close_later(conn); // something's wrong with the fd, stop using it
    }

    // try to flush right after processing instead of waiting for EPOLLOUT to be
    // requested and reported on a later wakeup -- avoids doubling epoll_ctl calls
    // for the common case where the send buffer has room immediately.
    // no-op when out is empty, stops cleanly on EAGAIN otherwise.
    attempt_write(conn);

    update_backpressure(conn);
    update_epoll_interest(conn);
}

void Server::handle_read(Connection& conn) {
    for (int i = 0; i < kMaxRecvsPerWakeup; ++i) {
        char* dst = conn.in.prepare_write(kReadChunkSize);
        const ssize_t n = ::recv(conn.fd.get(), dst, kReadChunkSize, 0);

        if (n > 0) {
            conn.in.commit_written(static_cast<size_t>(n));
            mark_active(conn);
            if (static_cast<size_t>(n) < kReadChunkSize) {
                break; // short read -- drained for now
            }
            continue; // got a full chunk; there might be more, try again
        }

        conn.in.commit_written(0); // release the reserved-but-unused space

        if (n == 0) {
            close_later(conn); // EOF
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break; // drained for now
        }
        if (errno == EINTR) {
            continue;
        }
        LOG_WARN("recv() failed on fd=%d: %s", conn.fd.get(), std::strerror(errno));
        close_later(conn);
        break;
    }

    // checked once per wakeup rather than per recv() -- the per-wakeup read cap is
    // negligible slop against a limit meant to be tens/hundreds of MiB
    if (!conn.want_close && conn.in.size() > cfg_.max_query_buf) {
        resp::error(conn.out, "protocol error: query buffer limit exceeded");
        close_later(conn);
        return;
    }

    process(conn);
}

void Server::process(Connection& conn) {
    // no separate cap needed: `in` only ever holds this wakeup's bytes plus at
    // most one trailing incomplete command, and reads are already capped
    for (;;) {
        const auto readable = conn.in.readable();
        std::vector<std::string> argv;
        size_t consumed = 0;
        std::string err;
        const auto status = parse_command(std::span<const char>(readable.data(), readable.size()),
                                           argv, consumed, err);

        if (status == RespParseStatus::Incomplete) {
            break; // wait for more bytes
        }
        if (status == RespParseStatus::Error) {
            resp::error(conn.out, err);
            close_later(conn);
            break;
        }

        // Ok
        conn.in.consume(consumed);
        if (argv.empty()) {
            continue; // a bare "*0\r\n" -- no command, keep parsing
        }

        CommandContext ctx{conn.out, conn, db_, *this};
        dispatch(ctx, argv);

        if (conn.want_close) {
            break; // QUIT etc -- don't parse further pipelined commands
        }
    }
}

void Server::attempt_write(Connection& conn) {
    while (!conn.out.empty()) {
        const auto span = conn.out.readable();
        const ssize_t sent = ::send(conn.fd.get(), span.data(), span.size(), MSG_NOSIGNAL);

        if (sent > 0) {
            conn.out.consume(static_cast<size_t>(sent));
            mark_active(conn);
            continue; // short write leaves bytes for next loop iteration
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break; // send buffer full -- wait for EPOLLOUT
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }

        // hard error or sent==0: nothing more can go to this peer, drop the rest
        if (sent < 0) {
            LOG_WARN("send() failed on fd=%d: %s", conn.fd.get(), std::strerror(errno));
        }
        conn.out.clear();
        close_later(conn);
        break;
    }
}

void Server::update_backpressure(Connection& conn) {
    if (conn.out.size() > cfg_.output_hard_limit_bytes) {
        LOG_WARN("fd=%d: output buffer exceeded hard limit (%zu > %zu bytes), closing",
                  conn.fd.get(), conn.out.size(), cfg_.output_hard_limit_bytes);
        conn.out.clear();
        conn.reading_paused = false;
        close_later(conn);
        return;
    }
    if (conn.out.size() > cfg_.output_soft_limit_bytes) {
        conn.reading_paused = true;
    } else if (conn.out.empty()) {
        conn.reading_paused = false;
    }
}

void Server::update_epoll_interest(Connection& conn) {
    const bool want_read = !conn.want_close && !conn.reading_paused;
    const bool want_write = !conn.out.empty();
    // skip epoll_ctl(MOD) when interest already matches what's registered --
    // the common "just reading, empty out buffer" case costs zero calls now
    if (want_read == conn.epoll_want_read && want_write == conn.epoll_want_write) {
        return;
    }
    loop_.modify(conn.fd.get(), want_read, want_write);
    conn.epoll_want_read = want_read;
    conn.epoll_want_write = want_write;
}

void Server::close_later(Connection& conn) {
    conn.want_close = true;
}

void Server::reap_closed() {
    for (auto it = conns_.begin(); it != conns_.end();) {
        Connection& conn = *it->second;
        if (conn.want_close && conn.out.empty()) {
            loop_.remove(conn.fd.get());
            idle_list_.erase(conn.idle_iter);
            LOG_DEBUG("client disconnected (fd=%d, total=%zu)", conn.fd.get(), conns_.size() - 1);
            it = conns_.erase(it); // erasing the unique_ptr closes the socket
        } else {
            ++it;
        }
    }
}

void Server::mark_active(Connection& conn) {
    conn.last_active_ms = monotonic_ms();
    idle_list_.splice(idle_list_.end(), idle_list_, conn.idle_iter);
}

void Server::sweep_idle_connections(int64_t now_ms) {
    if (cfg_.idle_timeout_sec <= 0) {
        return; // disabled, matches Redis default
    }
    const int64_t timeout_ms = static_cast<int64_t>(cfg_.idle_timeout_sec) * 1000;
    for (Connection* conn : idle_list_) {
        if (now_ms - conn->last_active_ms < timeout_ms) {
            break; // list is oldest-first, nothing after this is expired either
        }
        // close_later() just sets a bool, safe to call again if already want_close
        if (!conn->want_close) {
            LOG_DEBUG("fd=%d idle for >= %ds, closing", conn->fd.get(), cfg_.idle_timeout_sec);
        }
        close_later(*conn);
    }
}

void Server::run_in_background(std::function<void()> task) {
    thread_pool_.submit(std::move(task));
}

void Server::post_completion(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        completions_.push_back(std::move(fn));
    }
    // wakes on_completion_readable() via epoll by bumping the eventfd counter
    const uint64_t one = 1;
    if (::write(completion_eventfd_.get(), &one, sizeof(one)) < 0) {
        LOG_ERROR("write() to completion eventfd failed: %s", std::strerror(errno));
    }
}

void Server::on_completion_readable() {
    // must drain the counter or level-triggered epoll keeps reporting it readable
    uint64_t count = 0;
    const ssize_t n = ::read(completion_eventfd_.get(), &count, sizeof(count));
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_WARN("read() on completion eventfd failed: %s", std::strerror(errno));
    }

    // swap out under the lock then run unlocked, so a completion that itself
    // calls post_completion() doesn't deadlock on completions_mutex_
    std::deque<std::function<void()>> due;
    {
        std::lock_guard<std::mutex> lock(completions_mutex_);
        due.swap(completions_);
    }
    for (auto& fn : due) {
        fn();
    }
}

void Server::on_signal_readable() {
    struct signalfd_siginfo info {};
    const ssize_t n = ::read(signal_fd_.get(), &info, sizeof(info));
    if (n != static_cast<ssize_t>(sizeof(info))) {
        return; // shouldn't happen, each read() returns one whole siginfo or EAGAINs
    }
    LOG_INFO("received signal %d, shutting down gracefully", info.ssi_signo);
    shutting_down_ = true;
}

void Server::load_snapshot_at_startup() {
    struct stat st {};
    if (::stat(cfg_.snapshot_path.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            LOG_INFO("no snapshot found at '%s', starting with an empty database", cfg_.snapshot_path.c_str());
            return;
        }
        LOG_ERROR("cannot stat snapshot '%s': %s", cfg_.snapshot_path.c_str(), std::strerror(errno));
        std::exit(1);
    }

    std::vector<uint8_t> bytes;
    std::string error;
    if (!read_snapshot_file(cfg_.snapshot_path, bytes, error)) {
        LOG_ERROR("failed to read snapshot '%s': %s", cfg_.snapshot_path.c_str(), error.c_str());
        std::exit(1);
    }

    const SnapshotLoadResult result = decode_snapshot(bytes, db_, monotonic_ms(), unix_ms());
    if (result.status != SnapshotLoadStatus::Ok) {
        LOG_ERROR("corrupt snapshot '%s': %s", cfg_.snapshot_path.c_str(), result.error.c_str());
        std::exit(1);
    }
    LOG_INFO("loaded %zu key(s) from snapshot '%s'", db_.size(), cfg_.snapshot_path.c_str());
}

bool Server::save_snapshot(std::string& error) {
    if (cfg_.snapshot_path.empty()) {
        error = "no snapshot path configured (start gredis-server with --snapshot <path>)";
        return false;
    }
    const int64_t now_unix = unix_ms();
    const std::vector<uint8_t> bytes = encode_snapshot(db_, monotonic_ms(), now_unix);
    if (!write_snapshot_file(cfg_.snapshot_path, bytes, error)) {
        LOG_WARN("SAVE failed: %s", error.c_str());
        return false;
    }
    lastsave_unix_ms_ = now_unix;
    return true;
}

bool Server::start_bgsave(std::string& error) {
    if (cfg_.snapshot_path.empty()) {
        error = "no snapshot path configured (start gredis-server with --snapshot <path>)";
        return false;
    }
    if (bgsave_in_progress_) {
        error = "Background save already in progress";
        return false;
    }

    const int64_t now_unix = unix_ms();
    // encoding happens here on the loop thread, stalls proportional to dataset size;
    // only write+fsync+rename moves to a worker. shared_ptr instead of move-capture
    // because std::function needs a copyable target and we don't want to copy the buffer.
    auto bytes = std::make_shared<std::vector<uint8_t>>(encode_snapshot(db_, monotonic_ms(), now_unix));
    const std::string path = cfg_.snapshot_path;

    bgsave_in_progress_ = true;
    run_in_background([this, bytes, path, now_unix] {
        std::string write_error;
        const bool ok = write_snapshot_file(path, *bytes, write_error);
        post_completion([this, ok, now_unix, write_error] {
            bgsave_in_progress_ = false;
            if (ok) {
                lastsave_unix_ms_ = now_unix;
            } else {
                LOG_WARN("BGSAVE failed: %s", write_error.c_str());
            }
        });
    });
    return true;
}

} // namespace gredis
