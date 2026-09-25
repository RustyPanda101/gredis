#include "storage/database.h"

#include <chrono>
#include <limits>
#include <utility>

#include "util/log.h"

namespace {

// duplicated from cmd_keys.cpp's identical helpers -- storage/ can't
// depend on commands/, and this is too small to justify a shared header
bool add_would_overflow(int64_t a, int64_t b) {
    if (b > 0 && a > std::numeric_limits<int64_t>::max() - b) {
        return true;
    }
    if (b < 0 && a < std::numeric_limits<int64_t>::min() - b) {
        return true;
    }
    return false;
}

bool sub_would_overflow(int64_t a, int64_t b) {
    if (b > 0 && a < std::numeric_limits<int64_t>::min() + b) {
        return true;
    }
    if (b < 0 && a > std::numeric_limits<int64_t>::max() + b) {
        return true;
    }
    return false;
}

} // namespace

namespace gredis {

Value* Database::lookup_and_expire(std::string_view key, int64_t now_ms) {
    const int64_t* deadline = expires_.find(key);
    if (deadline != nullptr && *deadline <= now_ms) {
        keyspace_.erase(key);
        expires_.erase(key);
        return nullptr;
    }
    return keyspace_.find(key);
}

Value* Database::lookup_read(std::string_view key, int64_t now_ms) {
    return lookup_and_expire(key, now_ms);
}

Value* Database::lookup_write(std::string_view key, int64_t now_ms) {
    return lookup_and_expire(key, now_ms);
}

void Database::set(std::string key, Value v) {
    expires_.erase(key);
    keyspace_.insert_or_assign(std::move(key), std::move(v));
}

bool Database::del(std::string_view key) {
    expires_.erase(key);
    return keyspace_.erase(key);
}

std::optional<Value> Database::take(std::string_view key, int64_t now_ms) {
    if (lookup_and_expire(key, now_ms) == nullptr) {
        return std::nullopt;
    }
    expires_.erase(key);
    return keyspace_.take(key);
}

DetachedKeyspace Database::take_all() {
    // move leaves keyspace_/expires_ already reset to fresh empty tables
    DetachedKeyspace detached{std::move(keyspace_), std::move(expires_)};
    expiry_heap_.clear();
    return detached;
}

void Database::clear() {
    keyspace_.clear();
    expires_.clear();
}

void Database::for_each_for_snapshot(int64_t now_mono, int64_t now_unix,
                                      const std::function<void(const std::string&, Value&, std::optional<int64_t>)>& fn) {
    keyspace_.for_each([&](const std::string& key, Value& value) {
        const int64_t* deadline = expires_.find(key);
        if (deadline == nullptr) {
            fn(key, value, std::nullopt);
            return;
        }
        if (*deadline <= now_mono) {
            return; // already expired, don't save it
        }
        fn(key, value, now_unix + (*deadline - now_mono));
    });
}

void Database::load_key(std::string key, Value value, std::optional<int64_t> expire_unix_ms, int64_t now_mono,
                         int64_t now_unix) {
    if (expire_unix_ms) {
        // a corrupt i64 from the file could overflow this conversion --
        // treat it like an already-expired key instead of risking UB
        if (sub_would_overflow(*expire_unix_ms, now_unix) ||
            add_would_overflow(now_mono, *expire_unix_ms - now_unix)) {
            return;
        }
        const int64_t deadline_mono = now_mono + (*expire_unix_ms - now_unix);
        if (deadline_mono <= now_mono) {
            return; // expired while the snapshot sat on disk
        }
        expires_.insert_or_assign(key, deadline_mono);
        expiry_heap_.push(deadline_mono, key);
    }
    keyspace_.insert_or_assign(std::move(key), std::move(value));
}

bool Database::expire_at(std::string_view key, int64_t deadline_ms, int64_t now_ms) {
    if (lookup_and_expire(key, now_ms) == nullptr) {
        return false;
    }
    if (deadline_ms <= now_ms) {
        keyspace_.erase(key);
        expires_.erase(key);
        return true;
    }
    expires_.insert_or_assign(std::string(key), deadline_ms);
    // stale heap entries (superseded by this push) get dropped lazily
    // by run_active_expiration_cycle(), not removed here
    expiry_heap_.push(deadline_ms, std::string(key));
    return true;
}

bool Database::persist(std::string_view key, int64_t now_ms) {
    if (lookup_and_expire(key, now_ms) == nullptr) {
        return false;
    }
    return expires_.erase(key);
}

int64_t Database::pttl(std::string_view key, int64_t now_ms) {
    if (lookup_and_expire(key, now_ms) == nullptr) {
        return -2;
    }
    const int64_t* deadline = expires_.find(key);
    if (deadline == nullptr) {
        return -1;
    }
    const int64_t remaining = *deadline - now_ms;
    return remaining > 0 ? remaining : 0;
}

size_t Database::run_active_expiration_cycle(int64_t now_ms, size_t max_keys, double max_wall_ms) {
    // monotonic_ms() is only ms-granular, too coarse for a sub-ms
    // budget check -- use steady_clock directly just for timing this loop
    const auto wall_start = std::chrono::steady_clock::now();
    size_t expired = 0;

    while (expired < max_keys && !expiry_heap_.empty()) {
        const ExpiryHeap::Entry& top = expiry_heap_.top();
        if (top.deadline_ms > now_ms) {
            break; // heap is ordered, nothing else is due yet either
        }
        const std::string key = top.key; // copy before pop() invalidates the reference
        const int64_t popped_deadline = top.deadline_ms;
        expiry_heap_.pop();

        const int64_t* live_deadline = expires_.find(key);
        const bool is_live = (live_deadline != nullptr && *live_deadline == popped_deadline);
        if (is_live) {
            keyspace_.erase(key);
            expires_.erase(key);
            ++expired;
        }
        // else: stale (persisted, re-expired to a different deadline,
        // or deleted since this entry was pushed) -- silently dropped.

        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall_start).count();
        if (elapsed_ms >= max_wall_ms) {
            break;
        }
    }

    maybe_compact_expiry_heap();
    return expired;
}

void Database::maybe_compact_expiry_heap() {
    if (expiry_heap_.size() > 2 * (expires_.size() + 1024)) {
        LOG_INFO("compacting expiry heap: %zu entries for %zu live TTLs", expiry_heap_.size(), expires_.size());
        expiry_heap_.rebuild_from(expires_);
    }
}

} // namespace gredis
