#include "util/thread_pool.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include "test_harness.h"

using gredis::ThreadPool;

TEST(submitted_tasks_all_run_exactly_once) {
    constexpr int kTasks = 10'000;
    std::atomic<int> counter{0};
    {
        ThreadPool pool(4);
        for (int i = 0; i < kTasks; ++i) {
            pool.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        }
    } // destructor drains the queue and joins
    CHECK_EQ(counter.load(), kTasks);
}

TEST(shutdown_drains_tasks_still_queued) {
    // 1 worker + a burst of tasks guarantees a real backlog when destructor runs
    std::atomic<int> counter{0};
    {
        ThreadPool pool(1);
        for (int i = 0; i < 1000; ++i) {
            pool.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        }
    }
    CHECK_EQ(counter.load(), 1000);
}

TEST(submitting_from_multiple_threads_works) {
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 500;
    std::atomic<int> counter{0};
    {
        ThreadPool pool(4);
        std::vector<std::thread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&pool, &counter] {
                for (int i = 0; i < kPerProducer; ++i) {
                    pool.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
                }
            });
        }
        for (std::thread& t : producers) {
            t.join();
        }
    }
    CHECK_EQ(counter.load(), kProducers * kPerProducer);
}

TEST(a_throwing_task_does_not_kill_the_worker) {
    std::atomic<int> ran_after{0};
    {
        ThreadPool pool(1);
        pool.submit([] { throw std::runtime_error("deliberate test exception"); });
        pool.submit([&ran_after] { ran_after.fetch_add(1, std::memory_order_relaxed); });
    }
    CHECK_EQ(ran_after.load(), 1);
}

// relies on ctest's per-test TIMEOUT: a hung join times out the binary
// instead of silently passing
TEST(destructor_joins_after_a_batch_of_slow_tasks) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(2);
        for (int i = 0; i < 50; ++i) {
            pool.submit([&counter] {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }
    }
    CHECK_EQ(counter.load(), 50);
}

TEST_MAIN()
