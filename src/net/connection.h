// per-client socket + buffers + bookkeeping for the event loop
#pragma once

#include <cstdint>
#include <list>
#include <utility>

#include "net/buffer.h"
#include "util/fd.h"

namespace gredis {

struct Connection {
    Connection(Fd socket, uint64_t connection_id, int64_t now_ms)
        : fd(std::move(socket)), id(connection_id), last_active_ms(now_ms) {}

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = default;
    Connection& operator=(Connection&&) = default;

    Fd fd;
    Buffer in;
    Buffer out;

    // set on EOF, a hard I/O error, or hitting the output hard limit. stays
    // running until `out` drains (peer still gets its last reply); actual
    // destruction happens later in Server::reap_closed().
    bool want_close = false;

    // true while over the soft output limit -- pauses reading until out drains
    bool reading_paused = false;

    // interest last actually registered via epoll_ctl(MOD); update_epoll_interest()
    // compares against these and skips the syscall when nothing changed
    bool epoll_want_read = true;
    bool epoll_want_write = false;

    uint64_t id = 0; // unique for process lifetime, unlike the fd number which gets reused

    int64_t last_active_ms = 0; // updated on every read/write via Server::mark_active()

    // position in Server::idle_list_, set on insert, erased on destruction --
    // storing it makes both "move to back" and "erase on close" O(1)
    std::list<Connection*>::iterator idle_iter;
};

} // namespace gredis
