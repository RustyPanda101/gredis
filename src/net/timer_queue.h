// min-heap of one-shot timers, drives epoll_wait's timeout. not TTL-specific
// -- that's storage/expiry.h's ExpiryHeap; this is just for the cron tick etc.
#pragma once

#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_set>
#include <vector>

namespace gredis {

class TimerQueue {
public:
    using TimerId = uint64_t;
    using Callback = std::function<void()>;

    // no built-in repeat; a recurring timer re-arms itself from inside its own callback
    TimerId add_timer(int64_t deadline_ms, Callback cb);

    // lazy cancel: entry stays in the heap (can't remove an arbitrary heap element
    // cheaply) and gets dropped silently when it would reach the top. no-op if already ran.
    void cancel(TimerId id);

    // capped at 1000ms so the loop still gets a periodic wakeup with nothing scheduled;
    // -1 if no pending timers. drops cancelled entries found at the top along the way.
    int64_t next_timeout_ms(int64_t now_ms);

    // re-arming from inside a callback is fine -- new entry sorts by its new
    // deadline and isn't visited again in this call
    void run_due(int64_t now_ms);

    bool empty() const { return heap_.empty(); }

private:
    struct Entry {
        int64_t deadline_ms;
        TimerId id;
        Callback cb;
    };
    struct Greater {
        bool operator()(const Entry& a, const Entry& b) const { return a.deadline_ms > b.deadline_ms; }
    };
    using Heap = std::priority_queue<Entry, std::vector<Entry>, Greater>;

    void discard_cancelled_top();

    Heap heap_;
    std::unordered_set<TimerId> cancelled_;
    TimerId next_id_ = 1;
};

} // namespace gredis
