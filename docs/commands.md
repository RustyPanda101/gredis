# Command reference

What each supported command actually does, based on the handler code, not aspirational Redis semantics. Command names are case-insensitive; a key holding the wrong type replies `-WRONGTYPE Operation against a key holding the wrong kind of value`, and a wrong number of arguments replies `-ERR wrong number of arguments for '<cmd>' command` — both omitted below unless something about them is unusual.

## Server

- **`PING [message]`** — Health check. Without an argument replies `+PONG`; with one, echoes it back instead. Reply: simple string, or bulk string if given a message.
- **`ECHO message`** — Returns the argument unchanged. Reply: bulk string.
- **`QUIT`** — Replies `+OK`, then closes the connection once that reply is flushed. Reply: simple string `+OK`.
- **`COMMAND`** — Stub. Ignores any arguments and always replies an empty array — just enough to satisfy `redis-cli`'s startup probe. Reply: empty array.
- **`CONFIG GET parameter`** — Stub. Ignores the parameter (and any other arguments) and always replies an empty array; no config values are actually tracked. Reply: empty array.
- **`HELLO [args]`** — Not supported. Always replies an error, regardless of arguments, which makes RESP3-aware clients fall back to RESP2. Reply: error `-ERR unknown command 'HELLO'` — a fixed message, distinct from the generic unknown-command error other unrecognized commands get.
- **`DBSIZE`** — Number of keys physically in the keyspace. This counts keys that have logically expired but haven't been lazily or actively cleaned up yet, so it can briefly overcount. Reply: integer.
- **`FLUSHALL [ASYNC]`** — Deletes every key. Plain `FLUSHALL` clears the keyspace synchronously on the event loop. `FLUSHALL ASYNC` swaps in a fresh empty keyspace immediately and hands the old one to a background thread to free, so the command returns right away even with a huge dataset. Reply: simple string `+OK`.

## Keys

- **`DEL key [key ...]`** — Deletes each given key that exists (a key that's already lazily expired doesn't count). Reply: integer, number of keys actually removed.
- **`UNLINK key [key ...]`** — Same counting rule as `DEL`, but the value is moved out first; values with more than 64 elements are freed on a background thread instead of inline, so unlinking a huge hash/list/set/zset doesn't stall the event loop. Reply: integer.
- **`EXISTS key [key ...]`** — Counts how many of the given keys exist, once per occurrence in the argument list — `EXISTS k k` on an existing `k` replies `2`. Reply: integer.
- **`TYPE key`** — Reply: simple string, one of `string`, `hash`, `list`, `set`, `zset`, or `none` if the key doesn't exist.
- **`EXPIRE key seconds`** / **`PEXPIRE key ms`** — Sets a TTL relative to now. A zero or negative amount deletes the key immediately instead of storing a TTL. Reply: integer `1` if the key existed (and got a TTL, or got deleted), `0` if it didn't exist at all.
- **`EXPIREAT key unix-seconds`** / **`PEXPIREAT key unix-ms`** — Same as `EXPIRE`/`PEXPIRE` but the time is an absolute Unix timestamp; a timestamp at or before now deletes the key. Reply: same as `EXPIRE`.
- All four expire commands reject an amount that would overflow the internal millisecond deadline with `-ERR invalid expire time in '<cmd>' command`.
- **`TTL key`** / **`PTTL key`** — Remaining time to live, in seconds (rounded to the nearest second) or milliseconds. Reply: integer; `-2` if the key doesn't exist, `-1` if it exists but has no TTL, otherwise the remaining time.
- **`PERSIST key`** — Removes a key's TTL. Reply: integer `1` if a TTL was actually removed, `0` if the key doesn't exist or had no TTL.

## Strings

- **`GET key`** — Reply: bulk string, or nil if the key doesn't exist.
- **`SET key value [EX seconds | PX ms] [NX | XX]`** — Sets the key, clearing any existing TTL unless `EX`/`PX` is given in the same call. `NX` only sets if the key doesn't already exist; `XX` only sets if it does; the two can't be combined. `EX`/`PX` must be a positive amount — `EX 0`, a negative value, or one that would overflow the internal deadline all reply `-ERR invalid expire time in 'set' command` (unlike standalone `EXPIRE`, where a non-positive time deletes the key instead of erroring). Reply: simple string `+OK`, or nil if `NX`/`XX` blocked the set.
- **`MGET key [key ...]`** — Reply: array of bulk strings; a missing key or a key of the wrong type both come back as nil in that position.
- **`MSET key value [key value ...]`** — Sets multiple pairs in one call, each clearing that key's existing TTL. Reply: simple string `+OK`.
- **`INCR key`** / **`DECR key`** / **`INCRBY key increment`** / **`DECRBY key decrement`** — Parses the key's current value as a 64-bit integer (a missing key counts as `0`), applies the delta, and stores the result back as a decimal string. Any existing TTL is preserved (unlike `SET`). A result that would overflow replies `-ERR increment or decrement would overflow`; a non-integer existing value replies `-ERR value is not an integer or out of range`. Reply: integer, the value after the operation.
- **`APPEND key value`** — Appends to the string at `key`, creating it if needed, preserving any existing TTL. Reply: integer, length of the string after appending.
- **`STRLEN key`** — Reply: integer, `0` if the key doesn't exist.

## Hashes

| Command | Description | Reply |
|---|---|---|
| `HSET key field value [field value ...]` | Sets one or more field/value pairs, creating the hash if needed. A duplicate field within the same call counts once, and the last value given wins. | Integer: number of *new* fields added |
| `HGET key field` | Gets one field's value. | Bulk string, or nil |
| `HMGET key field [field ...]` | Gets several fields at once; a missing key or missing field each become nil in that position. | Array of bulk strings/nils |
| `HDEL key field [field ...]` | Removes fields. Removing the last field deletes the key. | Integer: number removed |
| `HEXISTS key field` | Checks whether a field exists. | Integer `1` or `0` |
| `HLEN key` | Number of fields. | Integer, `0` if missing |
| `HGETALL key` | All fields and values, flattened into one array (field, value, field, value, ...), in table order (unspecified). | Array of bulk strings |
| `HKEYS key` | All field names. | Array of bulk strings |
| `HVALS key` | All values. | Array of bulk strings |
| `HINCRBY key field increment` | Adds a signed integer to a field's value, creating the hash and/or field at `0` first if needed. | Integer: field's new value |

## Lists

| Command | Description | Reply |
|---|---|---|
| `LPUSH key value [value ...]` | Pushes values onto the head, creating the list if needed. Each value is pushed in argument order, so the last argument ends up closest to the head. | Integer: length after the push |
| `RPUSH key value [value ...]` | Same as `LPUSH` but onto the tail. | Integer: length after the push |
| `LPOP key [count]` | Pops from the head: one element without `count`, up to `count` elements with it (fewer if the list is shorter). Popping the last element deletes the key. | Bulk string (no count), or array (with count). Missing key: nil bulk without `count`, **null array** with `count`. |
| `RPOP key [count]` | Same as `LPOP` but from the tail. | Same shape as `LPOP` |
| `LLEN key` | Number of elements. | Integer, `0` if missing |
| `LRANGE key start stop` | Elements from `start` to `stop` inclusive, using Redis's negative-index convention (`-1` is the last element); out-of-range bounds are clamped rather than rejected. | Array of bulk strings |
| `LINDEX key index` | Element at `index` (negative counts from the end). | Bulk string, or nil if out of range or missing |

## Sets

| Command | Description | Reply |
|---|---|---|
| `SADD key member [member ...]` | Adds members, creating the set if needed. A duplicate member (this call or already present) counts once. | Integer: number actually added |
| `SREM key member [member ...]` | Removes members. Removing the last member deletes the key. | Integer: number removed |
| `SISMEMBER key member` | Checks membership. | Integer `1` or `0` |
| `SMEMBERS key` | All members, in table order (unspecified). | Array of bulk strings |
| `SCARD key` | Number of members. | Integer, `0` if missing |
| `SPOP key [count]` | Removes and returns random member(s). Without `count`, a missing key replies nil bulk; **with** `count`, a missing key replies an **empty array**, not a null array — the opposite of `LPOP`/`RPOP`'s missing-key behavior with a count. | Bulk string (no count), or array (with count) |
| `SINTER key [key ...]` | Intersection of all given sets. A key that doesn't exist is treated as an empty set, which makes the whole intersection empty. | Array of bulk strings |

## Sorted sets

- **`ZADD key [NX|XX] [CH] score member [score member ...]`** — Adds or updates members, creating the sorted set if needed. Every score is parsed and validated *before* anything is applied, so one bad score fails the whole command with no partial effect. `NX` skips members that already exist; `XX` skips members that don't (and never creates the key if it didn't already exist); combining `NX` and `XX` replies `-ERR XX and NX options at the same time are not compatible`. Without `CH`, the reply counts only newly added members; with `CH`, it also counts members whose score changed. An invalid score replies `-ERR value is not a valid float`. Reply: integer.
- **`ZREM key member [member ...]`** — Removes members. Removing the last member deletes the key. Reply: integer, number removed.
- **`ZSCORE key member`** — Reply: bulk string (score formatted as text), or nil if the key or member doesn't exist.
- **`ZINCRBY key increment member`** — Adds `increment` to a member's score (a missing member starts at `0`), creating the key if needed. A NaN result (e.g. incrementing `+inf` by `-inf`) is rejected without modifying anything: `-ERR resulting score is not a number (NaN)`. Reply: bulk string, the new score.
- **`ZCARD key`** — Reply: integer, `0` if missing.
- **`ZRANK key member`** / **`ZREVRANK key member`** — 0-based rank by score ascending (`ZRANK`) or descending (`ZREVRANK`); ties broken by member bytes. Reply: integer, or nil if the key or member doesn't exist.
- **`ZRANGE key start stop [WITHSCORES]`** / **`ZREVRANGE key start stop [WITHSCORES]`** — Members by rank, using the same negative-index and clamping rules as `LRANGE`, ascending or descending. Reply: array of bulk strings, or flattened member/score pairs with `WITHSCORES`.
- **`ZRANGEBYSCORE key min max [WITHSCORES] [LIMIT offset count]`** — Members with score in `[min, max]`, ascending. Either bound can be prefixed with `(` for exclusive, and both accept `inf`/`-inf` (case-insensitive); an invalid bound replies `-ERR min or max is not a float` (different wording from `ZADD`'s float error). `LIMIT` paginates the matched range; a negative `count` means no limit. Reply: array of bulk strings, or flattened member/score pairs with `WITHSCORES`.
- **`ZCOUNT key min max`** — Number of members with score in `[min, max]`, same bound syntax as `ZRANGEBYSCORE`. Reply: integer.
- **`ZPOPMIN key [count]`** — Removes and returns the lowest-scoring member(s). Unlike `LPOP`/`SPOP`, a missing key always replies an empty array, with or without `count` — there's no nil or null-array case here. Removing the last member deletes the key. Reply: array of flattened member/score pairs.

## Persistence

- **`SAVE`** — Synchronously encodes the whole database and writes it to the configured `--snapshot` path (temp file, fsync, atomic rename), blocking the event loop for the duration. Reply: simple string `+OK`, or an error if no `--snapshot` path was configured or the write failed.
- **`BGSAVE`** — Encodes the database on the event-loop thread (this part still blocks, proportional to dataset size) but moves the file write/fsync/rename to a background thread, so the reply comes back before the write finishes. A second `BGSAVE` while one is already running replies `-ERR Background save already in progress`. Reply: simple string `+Background saving started`.
- **`LASTSAVE`** — Reply: integer, Unix time in seconds of the last successful `SAVE`/`BGSAVE`. Before any save has happened, this is the server's startup time, not `0`.
