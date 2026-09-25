// fixed-size worker pool for background work that never touches the live
// database: freeing detached values (UNLINK/FLUSHALL ASYNC), snapshot I/O.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace gredis {

class ThreadPool {
public:
    explicit ThreadPool(size_t worker_count);

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // workers drain the queue before exiting, so a task queued right before
    // shutdown still runs instead of getting silently dropped
    ~ThreadPool();

    // task must be self-contained: move-captured or detached from the db
    // beforehand -- a worker must never hold a pointer into the live database
    void submit(std::function<void()> task);

private:
    void worker_loop();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    bool stop_ = false;
    // constructed last so worker_loop() never runs before mutex_/cv_/queue_/stop_ exist
    std::vector<std::thread> workers_;
};

} // namespace gredis
