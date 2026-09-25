// epoll-driven server: accept, buffer I/O, feed RESP2 parser + dispatcher
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "config.h"
#include "net/connection.h"
#include "net/event_loop.h"
#include "net/timer_queue.h"
#include "storage/database.h"
#include "util/fd.h"
#include "util/thread_pool.h"

namespace gredis {

class Server {
public:
    explicit Server(const Config& cfg);

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // runs until SIGINT/SIGTERM, then stops accepting, SAVEs if configured,
    // closes connections, returns 0. main() should `return server.run();`
    // unchanged so destructors run normally (thread pool join, LSan check).
    int run();

    // task must be self-contained (move-captured or detached from the db) since
    // it runs on a worker thread that must never touch the live Database.
    // used by UNLINK/FLUSHALL ASYNC to free large values off the loop thread.
    void run_in_background(std::function<void()> task);

    // callable from any thread; queues fn and wakes the loop via the eventfd.
    // fn always runs on the loop thread, in on_completion_readable().
    void post_completion(std::function<void()> fn);

    // blocks the loop thread for the whole write. false + error left unset
    // snapshot untouched if no path configured or write fails.
    bool save_snapshot(std::string& error);

    // encodes here on the loop thread (stalls proportional to dataset size),
    // hands the buffer to the pool for write+fsync+rename. false immediately
    // if no snapshot path or a bgsave is already running.
    bool start_bgsave(std::string& error);

    bool bgsave_in_progress() const { return bgsave_in_progress_; }

    // Redis reports LASTSAVE in unix seconds, unlike the ms used elsewhere here
    int64_t last_save_unix_s() const { return lastsave_unix_ms_ / 1000; }

private:
    // called from run() before the listener exists -- the one place blocking
    // I/O on this thread is fine, since the loop hasn't started yet
    void load_snapshot_at_startup();

    void on_listener_readable();
    void on_client_event(int fd, Readiness r);

    // reads up to this wakeup's budget, hands it to process(). sets want_close
    // on EOF/error but doesn't destroy conn -- reap_closed() does that later.
    void handle_read(Connection& conn);

    void process(Connection& conn);

    // partial write leaves the rest for the next attempt; hard error drops
    // whatever's left and calls close_later()
    void attempt_write(Connection& conn);

    void update_backpressure(Connection& conn);

    // must not leave EPOLLOUT enabled once out drains -- a level-triggered
    // socket is almost always writable, so that would busy-spin
    void update_epoll_interest(Connection& conn);

    // never destroys immediately -- see reap_closed() for why
    void close_later(Connection& conn);

    // runs once per batch, after every fd in it is handled, never mid-batch --
    // that's what lets a fd freed here get reused safely by a later accept()
    void reap_closed();

    void cron_tick();

    // called on every read/write; kept as one function so last_active_ms and
    // list order can't drift apart (sweep depends on "front is oldest")
    void mark_active(Connection& conn);

    // idle_timeout_sec == 0 disables this, matching Redis's default. list is
    // oldest-first so this stops at the first non-expired entry: O(expired+1).
    void sweep_idle_connections(int64_t now_ms);

    void on_completion_readable();

    // main.cpp blocks SIGINT/SIGTERM before any thread exists, so they only
    // ever arrive here via signal_fd_, never as an async handler
    void on_signal_readable();

    const Config& cfg_;
    EventLoop loop_;
    Fd listener_;
    // held open unused; freed briefly to accept+drop one conn on EMFILE, then reopened
    Fd reserve_fd_;
    Fd signal_fd_;
    bool shutting_down_ = false;
    TimerQueue timers_;
    // owns every accepted client's Connection (and its socket)
    std::unordered_map<int, std::unique_ptr<Connection>> conns_;
    uint64_t next_connection_id_ = 1;
    // LRU by activity, front = least recent. non-owning Connection*, conns_
    // owns them; inserted right after construction, erased right before destruction.
    std::list<Connection*> idle_list_;

    // single keyspace shared by every connection, touched only from this thread
    Database db_;

    // both written only on the loop thread (save_snapshot directly, bgsave via
    // its completion callback which always runs there)
    int64_t lastsave_unix_ms_;
    bool bgsave_in_progress_ = false;

    // declared before thread_pool_ so they're destroyed after it -- ~ThreadPool()
    // joins every worker first, so nothing can call post_completion() once these are gone
    std::mutex completions_mutex_;
    std::deque<std::function<void()>> completions_;
    Fd completion_eventfd_;

    ThreadPool thread_pool_;
};

} // namespace gredis
