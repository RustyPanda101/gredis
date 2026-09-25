# gredis

A Redis-compatible in-memory key-value server, written from scratch in C++20.

`gredis-server` speaks RESP2 over TCP, so `redis-cli` and `redis-benchmark`
work against it unmodified. It multiplexes every client connection on a
single event-loop thread with `epoll` and non-blocking sockets, stores
strings/hashes/lists/sets/sorted sets in memory with key expiration, and
persists to its own versioned binary snapshot format. A small thread pool
handles background work (snapshot I/O, freeing large values) that shouldn't
block the command path.

It's a systems-programming project, not a production database, and
everything in it is meant to be read, not hidden behind abstractions.

## Why

Most of what makes Redis interesting under the hood — the event loop, the
custom hash table, the protocol parser, the persistence format — is hidden
behind years of production hardening. This project rebuilds the core of it
from POSIX sockets up, as a way to actually understand:

- non-blocking I/O, partial reads/writes, and backpressure over a raw TCP
  socket
- an epoll readiness loop with a single thread doing all command processing
- an incremental RESP2 parser that survives partial input, pipelining, and
  hostile input
- a hash table with incremental rehashing (so a resize never causes an O(n)
  latency spike) and an AVL tree with subtree sizes for O(log n) rank queries
- explicit memory ownership with RAII for every OS resource, verified under
  ASan/UBSan/TSan
- a thread pool that hands completions back to the loop through an `eventfd`
- a versioned binary snapshot format with checksums and atomic replacement
- profiling a server against Redis/Valkey and only optimizing where the
  numbers say to

## Build & run

Requires a C++20 compiler, CMake ≥ 3.20, and Linux (You can use WSL too)
(epoll/eventfd/signalfd are used directly, no libevent/libuv/asio).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DGREDIS_SANITIZE=asan
cmake --build build
ctest --test-dir build                         # unit tests
python3 -m unittest discover tests/integration  # integration tests

./build/gredis-server --port 6380
redis-cli -p 6380 ping
```

For a real benchmark, build Release instead (sanitizers are for
development/testing, not throughput numbers):

```sh
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./build-release/gredis-server --port 6380
```

```
gredis-server [options]

  --bind <addr>              address to listen on (default 127.0.0.1)
  --port <1-65535>           TCP port to listen on (default 6380)
  --maxclients <n>           max simultaneous client connections (default 10000)
  --idle-timeout-sec <n>     disconnect clients idle longer than this; 0 disables (default 0)
  --threads <n>              background thread-pool worker count (default 2)
  --snapshot <path>          snapshot file to load at startup / save to (default: disabled)
  --log-level <level>        debug|info|warn|error (default info)
```

## Architecture

```
                  TCP clients (redis-cli, redis-benchmark, tests)
                                  |
                                  v
               listening socket + client sockets (O_NONBLOCK)
                                  |
                                  v
        +---------------- EventLoop (ONE thread) -----------------+
        |  epoll_wait(timeout = time until next timer)             |
        |     |                  |                     |           |
        |  listen fd          client fds           eventfd from    |
        |  (accept4 loop)     (read/write)         thread pool     |
        |                        |                     |           |
        |                   Connection            completion queue |
        |              (inbuf, outbuf, state)     (BGSAVE done,    |
        |                        |                 etc.)           |
        |                   RespParser  (bytes -> argv)            |
        |                        |                                 |
        |                   Dispatcher  (argv -> handler)          |
        |                        |                                 |
        |                   Database  (keyspace + expires)         |
        |                        |                                 |
        |                   RespWriter  (reply -> outbuf)          |
        |                                                          |
        |  TimerQueue: periodic cron tick (10 Hz)                  |
        |     -> active expiration cycle (bounded work)            |
        |     -> idle-connection sweep                             |
        +----------------------------------------------------------+
                                  |
                   submit(task)   |   (only self-contained work)
                                  v
                    ThreadPool (N workers)
                    - write snapshot bytes to disk, fsync, rename
                    - destroy detached large values (UNLINK, FLUSHALL ASYNC)
```

Layering is enforced: `net/`'s primitives (buffer, connection, event loop,
socket helpers, timer queue) know nothing about RESP or commands,
`protocol/` knows nothing about sockets or the database, `storage/` knows
nothing about sockets or RESP, and `commands/` is the only layer that talks
to both the database and the wire format. `net/server.cpp` is the one file
that wires all of it together.

Data structures: the keyspace, hash values, and set values all sit on one
custom open-chaining `HashTable` with incremental rehashing (grows/shrinks a
bucket at a time instead of stalling on a full-table rehash). Sorted sets are
a `HashTable<member, Node*>` (a non-owning pointer into the tree) for O(1)
score lookup, paired with a custom AVL tree (subtree sizes in every node)
ordered by `(score, member)`, which is
what makes `ZRANK`/`ZRANGE` by index O(log n). Lists use `std::deque`; the
expiry index and timers are `std::priority_queue` min-heaps with lazy
invalidation.

## Supported commands

| Group | Commands |
|---|---|
| Server | `PING`, `ECHO`, `QUIT`, `COMMAND`, `CONFIG GET`, `DBSIZE`, `FLUSHALL [ASYNC]` |
| Keys | `DEL`, `UNLINK`, `EXISTS`, `TYPE`, `EXPIRE`, `PEXPIRE`, `EXPIREAT`, `PEXPIREAT`, `TTL`, `PTTL`, `PERSIST` |
| Strings | `GET`, `SET [EX\|PX] [NX\|XX]`, `MGET`, `MSET`, `INCR`, `DECR`, `INCRBY`, `DECRBY`, `APPEND`, `STRLEN` |
| Hashes | `HSET`, `HGET`, `HMGET`, `HDEL`, `HEXISTS`, `HLEN`, `HGETALL`, `HKEYS`, `HVALS`, `HINCRBY` |
| Lists | `LPUSH`, `RPUSH`, `LPOP [count]`, `RPOP [count]`, `LLEN`, `LRANGE`, `LINDEX` |
| Sets | `SADD`, `SREM`, `SISMEMBER`, `SMEMBERS`, `SCARD`, `SPOP [count]`, `SINTER` |
| Sorted sets | `ZADD [NX\|XX] [CH]`, `ZREM`, `ZSCORE`, `ZINCRBY`, `ZCARD`, `ZRANK`, `ZREVRANK`, `ZRANGE`, `ZREVRANGE`, `ZRANGEBYSCORE`, `ZCOUNT`, `ZPOPMIN [count]` |
| Persistence | `SAVE`, `BGSAVE`, `LASTSAVE` |

Anything else returns the standard unknown-command error.[^1]

See [docs/commands.md](docs/commands.md) for what each command does.

## Testing

> Note: Tests and Benchmarking code has been generated with the help of Coding agents.

- **Unit tests** (`tests/unit/`, run via `ctest`): mostly pure-logic tests
  for the parser, writer, hash table, AVL tree, sorted set, snapshot codec,
  thread pool, and timer queue, with no sockets involved (`test_event_loop`
  uses a `pipe(2)` instead of a real socket to stay fast and deterministic).
  A couple of tests do touch real OS resources in isolation:
  `test_socket_util` binds one real listening socket to check for fd leaks,
  and `test_fd` exercises actual file descriptors.
- **Integration tests** (`tests/integration/`, Python stdlib only): spin up
  a real `gredis-server` subprocess and talk raw RESP over a socket,
  including fragmented, pipelined, and deliberately malformed input.
- **Randomized differential tests**: the hash table is checked against
  `std::unordered_map`, the AVL tree against `std::set`, and the sorted set
  against `std::map` over 200,000 random operations, with the size
  and every lookup checked against the reference container after every op,
  and a full structural invariant check every 1,000 ops.
- **Concurrency**: 200 simulated clients each running independent random
  operation sequences, plus a shared-key `INCR` stress test that verifies
  single-threaded command execution keeps it atomic.
- Everything above runs clean under ASan+UBSan; the thread pool and
  persistence paths are also run under TSan.
- A 10-minute libFuzzer run against the parser and dispatcher found zero
  crashes.

## Benchmarks

Full results and methodology are in [`docs/benchmarks.md`](docs/benchmarks.md),
measured against Valkey 9.1.2 on the same machine, same client, same
workload, 5 trials per row. No fabricated or cherry-picked numbers.

Headlines from the unoptimized baseline:

- Within about ±5% of Valkey on plain `SET`/`GET`/`INCR` and every
  hash/sorted-set command tested.
- Pulls ahead sharply at high pipeline depth (**+116%** throughput at
  `-P 64`) and at very low concurrency (**+21% to +26%** at `-c 1`).
- Trails by roughly 9-11% on sustained mixed-workload and TTL-heavy traffic,
  and by up to 9% at `-c 1000` — see the analysis in `docs/benchmarks.md`.
- A 5M-key insert run and a dedicated hash-table micro-benchmark both back
  up the incremental-rehash design: no multi-hundred-millisecond stalls,
  worst-case single-insert latency roughly 16x better than a one-shot
  `std::unordered_map` rehash at 10M inserts.

## Layout

```
src/
  net/          epoll event loop, connections, buffering
  protocol/     RESP2 parser and writer
  storage/      hash table, AVL tree, database, sorted sets
  commands/     command dispatcher and per-type handlers
  persistence/  binary snapshot encode/decode
  util/         logging, clock, RAII fd wrapper, thread pool, CRC32
tests/
  unit/         C++ unit tests (ctest)
  integration/  Python integration tests, raw-socket based
  fuzz/         libFuzzer target
bench/
  loadgen/      small epoll-based load generator with a latency histogram
  scripts/      benchmark suite runner + environment capture
docs/
  benchmarks.md benchmark tables and analysis
  commands.md   what each supported command does
```

---

[^1]: Replication, Cluster, Sentinel, sharding, Lua scripting, modules,
Streams, Pub/Sub, transactions (`MULTI`/`EXEC`/`WATCH`), blocking commands,
ACLs, `AUTH`, TLS, RESP3, `SCAN`, `KEYS`, multiple databases, `maxmemory`
eviction, and Redis RDB/AOF file compatibility are all left out on purpose
to keep this a focused single-machine project rather than a partial
reimplementation of Redis. Snapshots use gredis's own binary format instead.
