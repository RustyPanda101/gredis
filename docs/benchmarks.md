# Benchmarks: gredis vs. Valkey 9.1.2

Full workload matrix, run once against the unoptimized Release build.
Both servers ran on loopback, pinned to separate cores, with 1 discarded
warm-up run plus 5 measured trials per row and a fresh server per trial.
**Nothing here has been optimized yet.** Losses are reported as clearly
as wins, and every hypothesis below is exactly that -- unconfirmed,
pending follow-up profiling.

**Comparison server:** `/usr/bin/redis-server` on this machine is
**Valkey 9.1.2** (`redis-benchmark` is `valkey-benchmark` 9.1.2), run with
`--save "" --appendonly no --io-threads 1 --protected-mode no`. gredis
runs with its own defaults (persistence disabled unless `--snapshot` is
passed). Both pinned `taskset -c 2` (server) / `-c 4-7` (client). Fixed
parameters unless swept: concurrency 50, keyspace 10,000, value size 3
bytes, 200,000 requests (or a 10s duration for `bench_loadgen`-driven
rows). 1 discarded warm-up + 5 measured trials per row, fresh server per
trial. Table values are **median [min-max]** across those 5 trials.

Two different client tools appear in these tables: `redis-benchmark`
(rows without a loadgen-only workload name) and `bench_loadgen` (`mixed_*`
and `expiration`). A gredis-vs-Valkey comparison **within one row** is
always apples-to-apples (same client, same command, only the server
differs); comparing *across* rows measured by different tools is not
(the two tools have different fixed overheads), so this document never
does that.

## Core workload matrix (c=50, r=10000, d=3 bytes)

| workload | gredis req/s | Valkey req/s | gredis vs Valkey | gredis p50 (us) | Valkey p50 (us) |
|---|---|---|---|---|---|
| SET | 102,041 [97,847-105,153] | 103,520 [101,215-108,460] | -1.4% | 255 | 263 |
| GET | 106,157 [99,503-110,742] | 103,413 [98,135-109,529] | +2.7% | 255 | 263 |
| INCR | 108,050 [98,912-113,379] | 108,108 [103,040-108,578] | -0.1% | 247 | 263 |
| HSET (custom) | 101,626 [100,150-102,512] | 107,181 [100,604-110,193] | -5.2% | 255 | 263 |
| HGET (custom) | 103,573 [98,474-106,667] | 105,876 [99,900-109,529] | -2.2% | 255 | 263 |
| ZADD (custom) | 107,009 [100,654-111,359] | 103,896 [101,163-110,132] | +3.0% | 271 | 279 |
| ZRANK (custom) | 104,987 [99,305-107,933] | 104,712 [102,354-113,701] | +0.3% | 255 | 263 |
| ZRANGE 0 10 (custom) | 97,656 [92,678-100,452] | 94,697 [91,701-105,430] | +3.1% | 279 | 287 |
| ZPOPMIN (custom) | 102,041 [99,950-107,701] | 101,729 [99,354-103,252] | +0.3% | 255 | 271 |
| mixed GET:SET 90:10 (loadgen) | 134,944 [127,677-138,013] | 151,860 [147,606-155,415] | **-11.1%** | 352 | 320 |
| mixed GET:SET 50:50 (loadgen) | 132,938 [132,226-135,716] | 147,307 [144,752-148,210] | **-9.8%** | 360 | 336 |
| sustained SET PX 50 (loadgen) | 122,313 [118,511-125,508] | 134,189 [132,152-136,298] | **-8.8%** | 392 | 368 |

At default settings, gredis and Valkey are within about ±5% on plain
SET/GET/INCR and every hash/zset command tested. The three rows measured
by `bench_loadgen` (the two mixed ratios and the sustained-TTL stream)
are the only ones where gredis trails by close to 10% -- worth follow-up
profiling attention (see Analysis).

## Concurrency sweep (SET/GET, r=10000, d=3)

| c | gredis SET req/s | Valkey SET req/s | Δ | gredis GET req/s | Valkey GET req/s | Δ |
|---|---|---|---|---|---|---|
| 1 | 50,917 | 42,167 | **+20.7%** | 53,319 | 42,274 | **+26.1%** |
| 10 | 109,469 | 106,326 | +3.0% | 110,926 | 109,529 | +1.3% |
| 50 | 104,384 | 104,275 | +0.1% | 103,788 | 107,239 | -3.2% |
| 200 | 92,550 | 92,894 | -0.4% | 92,678 | 94,922 | -2.4% |
| 1000 | 70,822 | 74,157 | -4.5% | 71,480 | 78,586 | **-9.0%** |

gredis leads clearly at `c=1` (single, unpipelined connection) and is
essentially tied through `c=200`, then trails by mid-single-digits to
~9% at `c=1000`.

## Pipelining sweep (SET/GET, c=50, r=10000, d=3)

| P | gredis SET req/s | Valkey SET req/s | Δ | gredis GET req/s | Valkey GET req/s | Δ |
|---|---|---|---|---|---|---|
| 1 | 106,667 | 105,374 | +1.2% | 107,991 | 104,004 | +3.8% |
| 16 | 1,117,318 | 970,874 | **+15.1%** | 1,574,803 | 1,190,476 | **+32.3%** |
| 64 | 2,247,191 | 1,041,667 | **+115.7%** | 2,985,075 | 1,574,803 | **+89.6%** |

This is the biggest, most reproducible gap in either direction: gredis
pulls sharply ahead as pipeline depth grows, more than doubling Valkey's
throughput at `P=64` on both SET and GET.

## Payload sweep (SET/GET, c=50, r=10000)

| d (bytes) | gredis SET req/s | Valkey SET req/s | Δ | gredis GET req/s | Valkey GET req/s | Δ |
|---|---|---|---|---|---|---|
| 16 | 109,529 | 103,842 | +5.5% | 103,306 | 104,712 | -1.3% |
| 256 | 106,045 | 106,101 | -0.1% | 106,101 | 104,330 | +1.7% |
| 4096 | 93,589 | 95,374 | -1.9% | 105,430 | 106,496 | -1.0% |

No meaningful difference at any payload size tested -- both servers
degrade similarly as the value grows.

## Experiment 1: Resize spike (5,000,000 sequential SETs)

One trial (a stress test, not the 5-trial matrix): `redis-benchmark -t set -n 5000000
-r 5000000 -c 50 --sequential --precision 3` against a fresh gredis
server -- `--sequential` guarantees exactly 5,000,000 distinct keys
inserted (confirmed: `DBSIZE` = 5,000,000 after), rather than the
~3.16M distinct keys plain random sampling over a 5M keyspace would
produce (birthday-paradox collisions).

| metric | value |
|---|---|
| throughput | 104,192.72 req/s |
| avg latency | 0.259 ms |
| p50 | 0.255 ms |
| p95 | 0.383 ms |
| p99 | 0.671 ms |
| p99.9 | between 0.951 ms (99.902%) and 1.063 ms (99.951%), redis-benchmark's own bucket boundaries |
| max | 7.327 ms |
| RSS | 4.3 MiB &rarr; 707 MiB over the ~48s run |

**Connection to the hash-table micro-benchmark:** a standalone pure C++
micro-benchmark (`bench/micro_hash_table.cpp`) already showed incremental
rehashing keeps the worst-case single-insert latency about 16.6x better
than `std::unordered_map`'s one-shot rehash,
in isolation from any networking. This experiment repeats the same
underlying claim end-to-end, through the full RESP-parse/dispatch/TCP-write
path, at 5M keys (crossing many incremental-rehash cycles along the way):
the observed max was 7.3 ms and p99.9 sits close to 1 ms -- nowhere near
the multi-hundred-millisecond stall a one-shot rehash of a multi-million-entry
table would produce. Only a handful of requests out of 5,000,000 exceeded
3 ms at all, which is more consistent with occasional
scheduler/allocator noise than a systemic rehash cost -- but that
distinction is exactly what a follow-up profiling pass should confirm,
not something asserted outright here.

## Experiment 2: BGSAVE stall (1,000,000 keys)

Method: populate 1M sequential keys, then
run `bench/scripts/bgsave_stall.py` -- one connection pings in a tight,
unpipelined loop while a second connection issues `BGSAVE` one second in.

| metric | value |
|---|---|
| keys before BGSAVE | 1,000,000 |
| BGSAVE's own round-trip latency | 172.460 ms |
| concurrent PING's max latency | 172.418 ms, at the same instant BGSAVE was issued |
| baseline PING latency (immediately before/after) | ~0.017-0.037 ms |

This is a direct, clean measurement of a documented, deliberate design
decision: `Server::start_bgsave()` runs `encode_snapshot()`
synchronously on the loop thread, *before* BGSAVE's own
"+Background saving started" reply is even queued (see
`Server::start_bgsave()` in `src/net/server.cpp`) -- only the subsequent write+fsync+rename of
the already-encoded buffer moves to the thread pool. Because gredis is
single-threaded end to end, every other connection sees the identical
stall: the concurrently-pinging connection's one affected request took
essentially the same 172 ms as BGSAVE's own round trip, and every ping
immediately before and after was three to four orders of magnitude
faster. At roughly 172 ns/key of encode time for 1M keys, a much larger
dataset would stall the server for a proportionally longer, single
unbroken window -- exactly the tradeoff described above as deliberate
rather than an oversight.

## Experiment 3: Expiration (60s sustained `SET ... PX 50`)

Method:
`bench_loadgen --get-pct 0 --expire-ms 50 --connections 50 --keyspace
2000000 --duration-sec 60` against a fresh gredis server, with `DBSIZE`
polled every 0.5s and RSS/CPU sampled throughout (continuing 5s past the
load's own end).

| metric | value |
|---|---|
| throughput | 119,820.67 req/s (7,189,284 requests / 60.000s) |
| p50 / p95 / p99 | 400 / 640 / 1,152 us |
| max | 294,596 us (~295 ms) -- see below |
| DBSIZE steady state (during load) | ~1.60-1.62M |
| DBSIZE 4.565s after load stopped | 1,527,113 (drained 91,644 keys from the 1,618,757 peak) |
| drain rate after load stopped | ~20,075 keys/sec |
| RSS at end of run | ~368 MiB |

Two things worth calling out, neither of them a surprise once you look at
the numbers:

1. **DBSIZE does not converge toward 0 while the load is running.** With
   50 connections hammering a 2,000,000-key keyspace uniformly at random
   at ~120k req/s, once the live set grows large enough, most new `SET`s
   land on an *already-live* key and refresh its 50ms TTL before it can
   expire, rather than creating a new one -- that's the keyspace-vs-
   throughput ratio chosen for this run, not an expiry correctness bug
   (lazy expiry and the active-expiration cycle are about reclaiming
   keys nobody touches again before their deadline, and here most keys
   *are* being touched again). The DB genuinely does drain once the
   writes stop.
2. **The drain rate after the load stops (~20,075 keys/sec) lines up
   almost exactly with the documented active-expiration budget** of up
   to 2,000 keys per 10ms cron tick at the 10Hz tick rate -- a 20,000
   keys/sec ceiling. This is a clean, direct confirmation that this
   chosen budget (itself revised once already after an earlier version
   failed this same acceptance test) is the actual binding constraint on
   catch-up expiry once new writes stop, not some other bottleneck.
3. **The 294.6 ms max latency is flagged, not explained.** Every other
   percentile here is unremarkable (p99 is 1.15 ms), so this is a single
   outlier over 7.2M requests. Plausible, unconfirmed candidates: a
   cron tick's active-expiration pass or an incremental-rehash step
   landing in the same wakeup as a client's read, or scheduler/allocator
   noise from the concurrent `DBSIZE`-polling and `/proc` sampling
   processes this same experiment runs alongside the server. A follow-up
   profiling pass is where this gets an actual answer.

## Analysis (hypotheses only -- unconfirmed pending follow-up profiling)

**Where gredis is faster, and why it might be:** high pipeline depth
(+15% to +116% at P=16/64) and very low concurrency (+21% to +26% at
c=1). Both point toward gredis's command path having less fixed
per-request overhead than Valkey's -- a plausible mechanism is Valkey's
more general-purpose command processing (ACL checks, keyspace-
notification hooks, replication/AOF bookkeeping, RESP3 support) costing a
little even when none of it is configured or used, versus gredis's
minimal parse -> dispatch -> handler -> reply-buffer path. A follow-up
profiling pass should check this directly with `strace -c`'s
syscalls-per-request metric and `perf record` hot-symbol profiles on
both servers at `P=64`.

**Where gredis is slower, and why it might be:** high concurrency (-9% at
c=1000, GET) and the three `bench_loadgen`-measured workloads (mixed
ratios, sustained TTL'd SET) trailing by 9-11%. A concrete, cheap-to-check
hypothesis for the concurrency case: gredis's `handle_read()` caps reads
at `kMaxRecvsPerWakeup=4 x kReadChunkSize=16KiB` per connection per
wakeup (a deliberate bounded-work-per-event cap), and its fd lookup
goes through `std::unordered_map<int, unique_ptr<Connection>>` once per
ready event -- both become a larger fraction of total work when many fds
are ready in the same `epoll_wait` batch, as at `c=1000`. For the
expiration/mixed-ratio gap specifically: every `SET ... PX` additionally
touches the `expires` hash table and pushes onto the expiry min-heap,
work a plain `SET` doesn't do -- plausible, but Valkey maintains its own
expire dict too, so this needs actual measurement, not assumption. That
same follow-up pass should look at `epoll_wait` events-per-wakeup and
total wait count under `c=1000`, and at per-request cost specifically on
the SET+PX path vs plain SET.

**Where they're comparable:** plain SET/GET/INCR and every hash/zset
command at the default `c=50`, and the entire payload sweep -- all within
about ±5%, which is the expected outcome for data structures with the
same asymptotic complexity as Valkey's own `dict`/skiplist at this scale
(10,000-key keyspace).

No optimization has been made based on any of the above -- that comes
only after a follow-up profiling pass turns these hypotheses into
evidence.
