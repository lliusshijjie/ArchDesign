// main.cpp
//
// Day 03 的哈希时间轮测试：一次性任务、跨轮任务、取消、异常隔离、
// pending 上限和停止语义。

#include "hashed_wheel_timer.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using archdesign::HashedWheelTimer;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_one_shot() {
    HashedWheelTimer timer(10ms, 8);

    std::promise<void> fired;
    auto future = fired.get_future();
    timer.new_timeout(25ms, [&] { fired.set_value(); });

    check(future.wait_for(1s) == std::future_status::ready,
          "one-shot timer should fire");
    timer.stop();

    const auto stats = timer.stats();
    check(stats.scheduled == 1, "one task should be scheduled");
    check(stats.executed == 1, "one task should execute");
    check(stats.failed == 0, "no task should fail");
    check(stats.pending == 0, "no task should remain pending");
}

void test_order_and_rounds() {
    // tick=10ms, wheel=8 => 一轮 80ms。100ms 和 180ms 都会落在
    // 同一个 bucket（2），但 rounds 分别是 1 和 2。
    HashedWheelTimer timer(10ms, 8);

    std::mutex mutex;
    std::vector<int> order;
    std::promise<void> done;
    auto future = done.get_future();

    const auto add = [&](int id, int delay_ms) {
        timer.new_timeout(std::chrono::milliseconds(delay_ms), [&, id] {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(id);
            if (order.size() == 3) {
                done.set_value();
            }
        });
    };

    add(1, 20);
    add(2, 100);
    add(3, 180);

    check(future.wait_for(2s) == std::future_status::ready,
          "all ordered timers should fire");
    timer.stop();

    std::lock_guard<std::mutex> lock(mutex);
    check(order == std::vector<int>({1, 2, 3}),
          "timers should fire in deadline order across wheel rounds");
}

void test_cancel() {
    HashedWheelTimer timer(10ms, 8);

    std::atomic<bool> fired{false};
    auto timeout = timer.new_timeout(100ms, [&] { fired = true; });

    check(timer.cancel(timeout), "first cancel should succeed");
    check(!timer.cancel(timeout), "second cancel should be a no-op");
    timer.stop();

    check(!fired.load(), "cancelled task must not run");
    const auto stats = timer.stats();
    check(stats.canceled == 1, "one task should be cancelled");
    check(stats.executed == 0, "cancelled task should not be executed");
    check(stats.pending == 0, "cancelled task should not remain pending");
}

void test_callback_exception_isolation() {
    HashedWheelTimer timer(10ms, 8);

    timer.new_timeout(5ms, [] { throw std::runtime_error("boom"); });

    std::promise<void> fired;
    auto future = fired.get_future();
    timer.new_timeout(25ms, [&] { fired.set_value(); });

    check(future.wait_for(1s) == std::future_status::ready,
          "timer thread should survive a task exception");
    timer.stop();

    const auto stats = timer.stats();
    check(stats.executed == 2, "both tasks should be executed");
    check(stats.failed == 1, "one task should be counted as failed");
}

void test_pending_limit() {
    HashedWheelTimer timer(10ms, 8, 2);

    auto first = timer.new_timeout(1s, [] {});
    auto second = timer.new_timeout(1s, [] {});

    bool rejected = false;
    try {
        timer.new_timeout(1s, [] {});
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "third pending timer should be rejected");

    check(timer.cancel(first), "first pending task should be cancelable");
    check(timer.cancel(second), "second pending task should be cancelable");
    timer.stop();

    const auto stats = timer.stats();
    check(stats.scheduled == 2, "only two tasks should be scheduled");
    check(stats.canceled == 2, "both tasks should be cancelled");
    check(stats.pending == 0, "cancelled tasks should not remain pending");
}

void test_stop_rejects_new_timeouts() {
    HashedWheelTimer timer(10ms, 8);
    timer.stop();

    bool rejected = false;
    try {
        timer.new_timeout(1ms, [] {});
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "new_timeout after stop should be rejected");

    timer.stop();  // 幂等
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"one_shot", test_one_shot},
        {"order_and_rounds", test_order_and_rounds},
        {"cancel", test_cancel},
        {"callback_exception_isolation", test_callback_exception_isolation},
        {"pending_limit", test_pending_limit},
        {"stop_rejects_new_timeouts", test_stop_rejects_new_timeouts},
    };

    int failed = 0;
    for (const auto& [name, run] : tests) {
        try {
            run();
            std::cout << "[  OK  ] " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAILED] " << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failed;
            std::cerr << "[FAILED] " << name << ": unknown exception\n";
        }
    }

    if (failed != 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "\nAll " << tests.size() << " tests passed.\n";
    return 0;
}
