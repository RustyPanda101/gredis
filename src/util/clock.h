#pragma once

#include <cstdint>

namespace gredis {

// never goes backwards -- use for TTL deadlines, timers, idle tracking.
// mixing this up with unix_ms() is a correctness bug: an NTP jump or a
// user changing the clock must never un-expire or mass-expire keys.
int64_t monotonic_ms();

// wall time, only for snapshot TTLs and LASTSAVE -- never for scheduling
int64_t unix_ms();

} // namespace gredis
