#include "net/timer_queue.h"

#include <algorithm>

namespace gredis {

TimerQueue::TimerId TimerQueue::add_timer(int64_t deadline_ms, Callback cb) {
    const TimerId id = next_id_++;
    heap_.push(Entry{deadline_ms, id, std::move(cb)});
    return id;
}

void TimerQueue::cancel(TimerId id) {
    cancelled_.insert(id);
}

void TimerQueue::discard_cancelled_top() {
    while (!heap_.empty()) {
        const auto it = cancelled_.find(heap_.top().id);
        if (it == cancelled_.end()) {
            break;
        }
        cancelled_.erase(it);
        heap_.pop();
    }
}

int64_t TimerQueue::next_timeout_ms(int64_t now_ms) {
    discard_cancelled_top();
    if (heap_.empty()) {
        return -1;
    }
    const int64_t deadline = heap_.top().deadline_ms;
    const int64_t remaining = deadline > now_ms ? deadline - now_ms : 0;
    return std::min<int64_t>(remaining, 1000);
}

void TimerQueue::run_due(int64_t now_ms) {
    for (;;) {
        discard_cancelled_top();
        if (heap_.empty() || heap_.top().deadline_ms > now_ms) {
            break;
        }
        // top() is a const ref, so this copies rather than moves -- fine,
        // there are only ever a handful of live timers
        Callback cb = heap_.top().cb;
        heap_.pop();
        cb();
    }
}

} // namespace gredis
