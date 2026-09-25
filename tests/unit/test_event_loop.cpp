// uses a pipe(2) instead of a real socket -- readiness works the same way,
// keeps this fast and deterministic. real socket behavior is covered in integration tests.
#include "test_harness.h"

#include <fcntl.h>
#include <unistd.h>

#include "net/event_loop.h"

using gredis::EventLoop;
using gredis::Readiness;

namespace {

struct Pipe {
    int read_fd = -1;
    int write_fd = -1;

    Pipe() {
        int fds[2];
        if (::pipe(fds) == 0) {
            read_fd = fds[0];
            write_fd = fds[1];
            // non-blocking on both ends, same as every fd gredis puts into epoll
            ::fcntl(read_fd, F_SETFL, O_NONBLOCK);
            ::fcntl(write_fd, F_SETFL, O_NONBLOCK);
        }
    }

    ~Pipe() {
        if (read_fd >= 0) {
            ::close(read_fd);
        }
        if (write_fd >= 0) {
            ::close(write_fd);
        }
    }
};

} // namespace

TEST(reports_readable_after_data_is_written) {
    Pipe p;
    CHECK(p.read_fd >= 0);
    EventLoop loop;
    loop.add(p.read_fd, /*readable=*/true, /*writable=*/false);

    const char byte = 'x';
    CHECK(::write(p.write_fd, &byte, 1) == 1);

    bool saw_readable = false;
    const int n = loop.run_once(1000, [&](int fd, Readiness r) {
        if (fd == p.read_fd) {
            saw_readable = r.readable;
        }
    });
    CHECK_EQ(n, 1);
    CHECK(saw_readable);

    loop.remove(p.read_fd);
}

TEST(times_out_when_nothing_is_ready) {
    Pipe p;
    EventLoop loop;
    loop.add(p.read_fd, /*readable=*/true, /*writable=*/false);

    bool called = false;
    const int n = loop.run_once(50, [&](int, Readiness) { called = true; });
    CHECK_EQ(n, 0);
    CHECK(!called);

    loop.remove(p.read_fd);
}

TEST(remove_stops_reporting_events) {
    Pipe p;
    EventLoop loop;
    loop.add(p.read_fd, /*readable=*/true, /*writable=*/false);
    loop.remove(p.read_fd);

    const char byte = 'x';
    CHECK(::write(p.write_fd, &byte, 1) == 1);

    bool called = false;
    const int n = loop.run_once(50, [&](int, Readiness) { called = true; });
    CHECK_EQ(n, 0);
    CHECK(!called);
}

TEST(modify_can_disable_interest) {
    Pipe p;
    EventLoop loop;
    loop.add(p.read_fd, /*readable=*/true, /*writable=*/false);
    loop.modify(p.read_fd, /*readable=*/false, /*writable=*/false);

    const char byte = 'x';
    CHECK(::write(p.write_fd, &byte, 1) == 1);

    bool called = false;
    const int n = loop.run_once(50, [&](int, Readiness) { called = true; });
    CHECK_EQ(n, 0);
    CHECK(!called);

    loop.remove(p.read_fd);
}

TEST(pipe_write_end_is_immediately_writable) {
    // a write end is almost always writable -- exactly why EPOLLOUT must only
    // be requested while something's actually queued, or a level-triggered
    // loop would spin on it forever
    Pipe p;
    EventLoop loop;
    loop.add(p.write_fd, /*readable=*/false, /*writable=*/true);

    bool saw_writable = false;
    const int n = loop.run_once(1000, [&](int fd, Readiness r) {
        if (fd == p.write_fd) {
            saw_writable = r.writable;
        }
    });
    CHECK_EQ(n, 1);
    CHECK(saw_writable);

    loop.remove(p.write_fd);
}

TEST(multiple_fds_are_each_reported_once) {
    Pipe p1;
    Pipe p2;
    EventLoop loop;
    loop.add(p1.read_fd, /*readable=*/true, /*writable=*/false);
    loop.add(p2.read_fd, /*readable=*/true, /*writable=*/false);

    const char byte = 'x';
    CHECK(::write(p1.write_fd, &byte, 1) == 1);
    CHECK(::write(p2.write_fd, &byte, 1) == 1);

    int p1_hits = 0;
    int p2_hits = 0;
    const int n = loop.run_once(1000, [&](int fd, Readiness) {
        if (fd == p1.read_fd) ++p1_hits;
        if (fd == p2.read_fd) ++p2_hits;
    });
    CHECK_EQ(n, 2);
    CHECK_EQ(p1_hits, 1);
    CHECK_EQ(p2_hits, 1);

    loop.remove(p1.read_fd);
    loop.remove(p2.read_fd);
}

TEST_MAIN()
