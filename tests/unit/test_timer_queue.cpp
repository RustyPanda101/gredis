#include "net/timer_queue.h"

#include <vector>

#include "test_harness.h"

using gredis::TimerQueue;

TEST(empty_queue_reports_no_next_timeout) {
    TimerQueue q;
    CHECK(q.empty());
    CHECK_EQ(q.next_timeout_ms(0), int64_t{-1});
}

TEST(next_timeout_ms_reflects_the_nearest_deadline) {
    TimerQueue q;
    q.add_timer(500, [] {});
    q.add_timer(100, [] {});
    q.add_timer(900, [] {});
    CHECK_EQ(q.next_timeout_ms(0), int64_t{100});
}

TEST(next_timeout_ms_is_zero_for_an_already_due_timer) {
    TimerQueue q;
    q.add_timer(50, [] {});
    CHECK_EQ(q.next_timeout_ms(200), int64_t{0});
}

TEST(next_timeout_ms_is_capped_at_1000) {
    TimerQueue q;
    q.add_timer(1'000'000, [] {});
    CHECK_EQ(q.next_timeout_ms(0), int64_t{1000});
}

TEST(run_due_fires_callbacks_in_deadline_order) {
    TimerQueue q;
    std::vector<int> fired;
    q.add_timer(300, [&] { fired.push_back(3); });
    q.add_timer(100, [&] { fired.push_back(1); });
    q.add_timer(200, [&] { fired.push_back(2); });
    q.run_due(1000);
    CHECK_EQ(fired.size(), size_t{3});
    CHECK_EQ(fired[0], 1);
    CHECK_EQ(fired[1], 2);
    CHECK_EQ(fired[2], 3);
}

TEST(run_due_only_fires_timers_at_or_before_now) {
    TimerQueue q;
    int fired = 0;
    q.add_timer(100, [&] { ++fired; });
    q.add_timer(500, [&] { ++fired; });
    q.run_due(200);
    CHECK_EQ(fired, 1);
    CHECK(!q.empty()); // the 500ms timer is still pending
    q.run_due(500);
    CHECK_EQ(fired, 2);
    CHECK(q.empty());
}

TEST(equal_deadlines_both_fire_and_neither_is_lost) {
    TimerQueue q;
    int count = 0;
    q.add_timer(100, [&] { ++count; });
    q.add_timer(100, [&] { ++count; });
    q.run_due(100);
    CHECK_EQ(count, 2);
}

TEST(cancel_a_pending_timer_skips_its_callback) {
    TimerQueue q;
    bool fired = false;
    const auto id = q.add_timer(100, [&] { fired = true; });
    q.cancel(id);
    q.run_due(1000);
    CHECK(!fired);
    CHECK(q.empty());
}

TEST(cancel_one_of_several_only_skips_that_one) {
    TimerQueue q;
    std::vector<int> fired;
    const auto id_b = q.add_timer(200, [&] { fired.push_back(2); });
    q.add_timer(100, [&] { fired.push_back(1); });
    q.add_timer(300, [&] { fired.push_back(3); });
    q.cancel(id_b);
    q.run_due(1000);
    CHECK_EQ(fired.size(), size_t{2});
    CHECK_EQ(fired[0], 1);
    CHECK_EQ(fired[1], 3);
}

TEST(cancel_after_already_fired_is_a_safe_no_op) {
    TimerQueue q;
    int fired = 0;
    const auto id = q.add_timer(100, [&] { ++fired; });
    q.run_due(100);
    CHECK_EQ(fired, 1);
    q.cancel(id); // already gone -- must not crash or affect anything else
    CHECK(q.empty());
}

TEST(a_callback_that_re_arms_itself_is_not_visited_again_in_the_same_run_due) {
    TimerQueue q;
    int fired = 0;
    std::function<void()> tick = [&] {
        ++fired;
        q.add_timer(1000, [&] { ++fired; }); // schedules far in the future
    };
    q.add_timer(100, tick);
    q.run_due(100); // only the first firing should happen here
    CHECK_EQ(fired, 1);
    CHECK(!q.empty());
}

TEST_MAIN()
