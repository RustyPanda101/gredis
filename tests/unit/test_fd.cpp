#include "test_harness.h"

#include <fcntl.h>
#include <unistd.h>

#include <utility>

#include "util/fd.h"

using gredis::Fd;

namespace {

// /dev/null needs no setup and nothing to clean up beyond closing it
int open_dummy_fd() {
    return ::open("/dev/null", O_RDONLY);
}

bool fd_is_open(int fd) {
    return ::fcntl(fd, F_GETFD) != -1;
}

} // namespace

TEST(default_constructed_fd_is_invalid) {
    Fd f;
    CHECK(!f.valid());
    CHECK_EQ(f.get(), -1);
    CHECK(!static_cast<bool>(f));
}

TEST(explicit_constructor_takes_ownership) {
    const int raw = open_dummy_fd();
    CHECK(raw >= 0);
    Fd f(raw);
    CHECK(f.valid());
    CHECK_EQ(f.get(), raw);
    CHECK(fd_is_open(raw));
}

TEST(destructor_closes_the_fd) {
    int raw = -1;
    {
        raw = open_dummy_fd();
        CHECK(raw >= 0);
        Fd f(raw);
        CHECK(fd_is_open(raw));
    }
    CHECK(!fd_is_open(raw));
}

TEST(move_constructor_transfers_ownership_and_invalidates_source) {
    const int raw = open_dummy_fd();
    CHECK(raw >= 0);
    Fd a(raw);
    Fd b(std::move(a));

    CHECK(!a.valid());
    CHECK_EQ(a.get(), -1);
    CHECK(b.valid());
    CHECK_EQ(b.get(), raw);
    CHECK(fd_is_open(raw)); // still open -- ownership moved, not closed
}

TEST(move_assignment_closes_the_previously_owned_fd) {
    const int raw_a = open_dummy_fd();
    const int raw_b = open_dummy_fd();
    CHECK(raw_a >= 0);
    CHECK(raw_b >= 0);
    CHECK(raw_a != raw_b);

    Fd a(raw_a);
    Fd b(raw_b);
    b = std::move(a);

    CHECK(!a.valid());
    CHECK_EQ(b.get(), raw_a);
    CHECK(fd_is_open(raw_a));
    CHECK(!fd_is_open(raw_b)); // b's original fd was closed by the assignment
}

TEST(move_self_assignment_is_a_no_op) {
    const int raw = open_dummy_fd();
    CHECK(raw >= 0);
    Fd a(raw);
    Fd* self = &a;
    *self = std::move(a);
    CHECK(a.valid());
    CHECK_EQ(a.get(), raw);
    CHECK(fd_is_open(raw));
}

TEST(reset_closes_current_and_takes_new_fd) {
    const int raw_a = open_dummy_fd();
    const int raw_b = open_dummy_fd();
    CHECK(raw_a >= 0);
    CHECK(raw_b >= 0);

    Fd a(raw_a);
    a.reset(raw_b);
    CHECK_EQ(a.get(), raw_b);
    CHECK(!fd_is_open(raw_a));
    CHECK(fd_is_open(raw_b));
}

TEST(reset_with_no_argument_closes_and_invalidates) {
    const int raw = open_dummy_fd();
    CHECK(raw >= 0);
    Fd a(raw);
    a.reset();
    CHECK(!a.valid());
    CHECK(!fd_is_open(raw));
}

TEST(release_gives_up_ownership_without_closing) {
    const int raw = open_dummy_fd();
    CHECK(raw >= 0);
    Fd a(raw);
    const int got = a.release();
    CHECK_EQ(got, raw);
    CHECK(!a.valid());
    CHECK(fd_is_open(raw)); // release() must not close it
    ::close(raw);           // test cleans up what Fd no longer owns
}

TEST_MAIN()
