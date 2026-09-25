// thin wrappers over raw socket syscalls, no RESP/connection knowledge here
#pragma once

#include <cstdint>
#include <string>

#include "util/fd.h"

namespace gredis {

// SO_REUSEADDR so a restart doesn't fail while old connections sit in TIME_WAIT.
// fatal (logs + exit(1)) on any failed step -- nothing sensible to do if we can't bind.
Fd create_listener(const std::string& ip, uint16_t port, int backlog);

// disables Nagle so small replies aren't held back; just a latency optimization,
// not fatal on failure
void set_tcp_nodelay(int fd);

} // namespace gredis
