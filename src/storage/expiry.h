// min-heap of (deadline, key) for active expiration. dumb ordering
// structure only -- doesn't know what "stale" means, Database handles
// recognizing stale entries when popping (see
// run_active_expiration_cycle()). entries are never removed on a TTL
// change, just left to be skipped later or cleared by rebuild_from().
#pragma once

#include <cstdint>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "storage/hash_table.h"

namespace gredis {

class ExpiryHeap {
public:
    struct Entry {
        int64_t deadline_ms;
        std::string key;
    };

    void push(int64_t deadline_ms, std::string key) { heap_.push(Entry{deadline_ms, std::move(key)}); }

    bool empty() const { return heap_.empty(); }
    size_t size() const { return heap_.size(); }

    // requires !empty(). reference invalidated by the next push()/pop()
    const Entry& top() const { return heap_.top(); }
    void pop() { heap_.pop(); }

    // discards everything and rebuilds one entry per live TTL. O(n),
    // meant to be rare, not called every cycle.
    void rebuild_from(const HashTable<std::string, int64_t>& live_ttls) {
        Heap fresh;
        live_ttls.for_each([&fresh](const std::string& key, const int64_t& deadline_ms) {
            fresh.push(Entry{deadline_ms, key});
        });
        heap_ = std::move(fresh);
    }

    // used by FLUSHALL ASYNC when every TTL is dropped at once
    void clear() { heap_ = Heap(); }

private:
    struct Greater {
        bool operator()(const Entry& a, const Entry& b) const { return a.deadline_ms > b.deadline_ms; }
    };
    using Heap = std::priority_queue<Entry, std::vector<Entry>, Greater>;
    Heap heap_;
};

} // namespace gredis
