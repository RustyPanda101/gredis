#include "net/event_loop.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <sys/epoll.h>

#include "util/log.h"

namespace gredis {

namespace {

constexpr int kMaxEventsPerWait = 128;

[[noreturn]] void die(const char* what) {
    LOG_ERROR("%s: %s", what, std::strerror(errno));
    std::exit(1);
}

uint32_t to_epoll_events(bool readable, bool writable) {
    uint32_t events = 0;
    if (readable) {
        events |= static_cast<uint32_t>(EPOLLIN);
    }
    if (writable) {
        events |= static_cast<uint32_t>(EPOLLOUT);
    }
    return events;
}

} // namespace

EventLoop::EventLoop() : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)) {
    if (!epoll_fd_) {
        die("epoll_create1()");
    }
}

void EventLoop::add(int fd, bool readable, bool writable) {
    epoll_event ev{};
    ev.events = to_epoll_events(readable, writable);
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
        die("epoll_ctl(EPOLL_CTL_ADD)");
    }
}

void EventLoop::modify(int fd, bool readable, bool writable) {
    epoll_event ev{};
    ev.events = to_epoll_events(readable, writable);
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, fd, &ev) < 0) {
        die("epoll_ctl(EPOLL_CTL_MOD)");
    }
}

void EventLoop::remove(int fd) {
    // older kernels require a non-null event pointer for DEL even though they ignore it
    epoll_event ev{};
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, fd, &ev) < 0) {
        // not fatal, fd may already be gone (close() implicitly removes it)
        if (errno != ENOENT && errno != EBADF) {
            LOG_WARN("epoll_ctl(EPOLL_CTL_DEL) on fd=%d failed: %s", fd, std::strerror(errno));
        }
    }
}

int EventLoop::run_once(int timeout_ms, const EventHandler& on_event) {
    std::array<epoll_event, kMaxEventsPerWait> events{};

    int n = 0;
    for (;;) {
        n = ::epoll_wait(epoll_fd_.get(), events.data(), static_cast<int>(events.size()), timeout_ms);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("epoll_wait()");
        }
        break;
    }

    for (int i = 0; i < n; ++i) {
        Readiness r;
        r.readable = (events[static_cast<size_t>(i)].events & EPOLLIN) != 0;
        r.writable = (events[static_cast<size_t>(i)].events & EPOLLOUT) != 0;
        r.error = (events[static_cast<size_t>(i)].events & EPOLLERR) != 0;
        r.hangup = (events[static_cast<size_t>(i)].events & EPOLLHUP) != 0;
        on_event(events[static_cast<size_t>(i)].data.fd, r);
    }

    return n;
}

} // namespace gredis
