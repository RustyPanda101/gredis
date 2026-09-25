#include "util/thread_pool.h"

#include <exception>

#include "util/log.h"

namespace gredis {

ThreadPool::ThreadPool(size_t worker_count) {
    workers_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (std::thread& w : workers_) {
        w.join();
    }
}

void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void ThreadPool::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) {
                return; // predicate guarantees stop_ is true here, queue is drained
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        // an exception escaping std::thread's entry function calls std::terminate
        try {
            task();
        } catch (const std::exception& e) {
            LOG_ERROR("thread pool task threw: %s", e.what());
        } catch (...) {
            LOG_ERROR("thread pool task threw a non-std::exception value");
        }
    }
}

} // namespace gredis
