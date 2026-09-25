// thin wrapper around one epoll instance: add/modify/remove + run_once()
#pragma once

#include <functional>

#include "util/fd.h"

namespace gredis {

// error/hangup are always reported by the kernel regardless of what was requested
struct Readiness {
    bool readable = false; // EPOLLIN
    bool writable = false; // EPOLLOUT
    bool error = false;    // EPOLLERR
    bool hangup = false;   // EPOLLHUP (peer closed / connection reset)
};

class EventLoop {
public:
    EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // fatal on failure -- an ADD failure here means the fd was misused (e.g. added twice)
    void add(int fd, bool readable, bool writable);

    // toggles EPOLLOUT as the output buffer fills/drains -- level-triggered
    // epoll would busy-spin on a socket that's almost always writable otherwise
    void modify(int fd, bool readable, bool writable);

    // doesn't close fd, that's Fd's job. safe on an already-gone fd (closing
    // implicitly removes it from epoll).
    void remove(int fd);

    // calls on_event(fd, readiness) once per ready fd, retries on EINTR,
    // returns count ready (0 on timeout)
    using EventHandler = std::function<void(int fd, Readiness)>;
    int run_once(int timeout_ms, const EventHandler& on_event);

private:
    Fd epoll_fd_;
};

} // namespace gredis
