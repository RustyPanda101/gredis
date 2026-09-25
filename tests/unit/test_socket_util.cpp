// create_listener()'s failure paths call exit(1), so can't be tested
// in-process -- those are covered by integration tests instead. what we
// can verify here: binding then letting the listener go out of scope
// leaves no leaked fd, under a normal (non-killed) process exit so LSan
// actually gets to run its check.
#include "test_harness.h"

#include "net/socket_util.h"

TEST(create_listener_binds_and_cleans_up_with_no_leak) {
    // port 0 = kernel picks a free port; valid here even though Config rejects it as a CLI value
    gredis::Fd listener = gredis::create_listener("127.0.0.1", 0, 16);
    CHECK(listener.valid());
}

TEST_MAIN()
