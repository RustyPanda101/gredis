// the keyspace: owns every key/value pair plus their TTLs
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "storage/expiry.h"
#include "storage/hash_table.h"
#include "storage/value.h"

namespace gredis {

// whole keyspace detached in one move, for FLUSHALL ASYNC -- a
// thread-pool task owns this and drops it on a worker thread, never
// touching the live Database again
struct DetachedKeyspace {
    HashTable<std::string, Value> keyspace;
    HashTable<std::string, int64_t> expires;
};

class Database {
public:
    // every keyspace access must go through one of these two, so lazy
    // expiry (delete-on-access for a passed deadline) never gets
    // skipped. now_ms is passed in rather than read from the clock so
    // tests can control time. both do the same thing right now -- kept
    // as separate names in case read/write ever need to diverge.
    Value* lookup_read(std::string_view key, int64_t now_ms);
    Value* lookup_write(std::string_view key, int64_t now_ms);

    // always clears any existing TTL (no KEEPTTL) -- call expire_at()
    // after if you need one too
    void set(std::string key, Value v);

    // Removes `key` and any TTL it had. Returns whether it was present.
    bool del(std::string_view key);

    // like del() but moves the value out instead of freeing it --
    // caller decides whether to drop it inline or hand it to the
    // thread pool based on value_element_count() (UNLINK)
    std::optional<Value> take(std::string_view key, int64_t now_ms);

    // detaches keyspace + expires in one move, resets both to fresh
    // empty tables, clears the expiry heap (FLUSHALL ASYNC) -- caller
    // hands the result to the thread pool to actually free
    DetachedKeyspace take_all();

    size_t size() const { return keyspace_.size(); }
    bool empty() const { return keyspace_.empty(); }
    void clear();

    // deadline_ms is on the monotonic clock. a deadline at or before
    // now_ms deletes the key right away instead of storing a TTL
    // that'd just lazily expire on next access (Redis >=7 semantics
    // for EXPIRE with a non-positive/past time).
    bool expire_at(std::string_view key, int64_t deadline_ms, int64_t now_ms);

    // Removes key's TTL, if any. Returns whether a TTL was actually
    // removed (false for a missing key or one with no TTL).
    bool persist(std::string_view key, int64_t now_ms);

    // -2 if key doesn't exist, -1 if no TTL, else remaining ms
    int64_t pttl(std::string_view key, int64_t now_ms);

    // pops due entries off the expiry heap and deletes the still-live
    // ones (a mismatched expires_ entry means it got persisted/re-
    // expired/deleted since being pushed -- dropped silently, not an
    // error). bounded by max_keys or max_wall_ms, whichever hits
    // first, so one cycle can't stall the loop. returns count expired.
    size_t run_active_expiration_cycle(int64_t now_ms, size_t max_keys, double max_wall_ms);

    // pass-through so the cron tick doesn't need to know Database has
    // a HashTable inside it
    void rehash_step(size_t budget) { keyspace_.rehash_step(budget); }

    // visits every live key for snapshot encoding: fn(key, value,
    // expire_unix_ms_or_nullopt). a key already expired at now_mono is
    // skipped (not deleted -- can't mutate while for_each iterates).
    // TTL converted from internal monotonic deadline to persisted
    // unix-ms. takes Value& not const& only because ZSet::tree() isn't
    // const-qualified; callback only reads through it.
    void for_each_for_snapshot(int64_t now_mono, int64_t now_unix,
                                const std::function<void(const std::string&, Value&, std::optional<int64_t>)>& fn);

    // inserts key->value directly, for the snapshot loader only (never
    // a command handler). doesn't clear an existing TTL like set()
    // does since there's never an existing key here. expire_unix_ms
    // converts to a monotonic deadline like EXPIREAT; anything at or
    // before now_mono (including garbage from a corrupt file) drops
    // the key instead of inserting it.
    void load_key(std::string key, Value value, std::optional<int64_t> expire_unix_ms, int64_t now_mono,
                   int64_t now_unix);

    // test-only, so tests can check whether heap compaction actually fired
    size_t expiry_heap_size_for_testing() const { return expiry_heap_.size(); }

private:
    // deletes key from both tables and returns nullptr if its TTL has
    // passed; otherwise just keyspace_.find(key)
    Value* lookup_and_expire(std::string_view key, int64_t now_ms);

    // rebuilds the expiry heap from expires_ once it's grown too
    // stale-heavy (called once per active-expiration cycle)
    void maybe_compact_expiry_heap();

    HashTable<std::string, Value> keyspace_;
    HashTable<std::string, int64_t> expires_; // keys with a TTL only
    ExpiryHeap expiry_heap_;
};

} // namespace gredis
