// main.cpp
//
// Day 02 的事件循环行为测试。用 MockPoller 注入就绪事件，避免依赖
// 平台 socket；测试重点是可观察的调度语义，而不是内核 I/O。

#include "event_loop.hpp"

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using archdesign::Duration;
using archdesign::EventLoop;
using archdesign::kReadable;
using archdesign::kWritable;
using archdesign::MockPoller;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_file_read_event() {
    auto poller = std::make_unique<MockPoller>();
    auto* mock = poller.get();
    EventLoop loop(std::move(poller));

    int calls = 0;
    int seen_fd = -1;
    loop.add_file(
        7,
        kReadable,
        [&](int fd, std::uint32_t mask) {
            ++calls;
            seen_fd = fd;
            check((mask & kReadable) != 0, "read mask should be set");
        },
        {});

    mock->inject(7, kReadable);
    loop.run_once();

    check(calls == 1, "read callback should run once");
    check(seen_fd == 7, "read callback should receive fd 7");
    check(!mock->last_timeout().has_value(),
          "no timers means the poller may block indefinitely");
}

void test_read_write_order() {
    const auto run_order = [](bool barrier) {
        auto poller = std::make_unique<MockPoller>();
        auto* mock = poller.get();
        EventLoop loop(std::move(poller));

        std::string order;
        loop.add_file(
            9,
            kReadable | kWritable,
            [&](int, std::uint32_t) { order += 'R'; },
            [&](int, std::uint32_t) { order += 'W'; },
            barrier);

        mock->inject(9, kReadable | kWritable);
        loop.run_once();
        return order;
    };

    check(run_order(false) == "RW", "default order should be read then write");
    check(run_order(true) == "WR", "AE_BARRIER should write then read");
}

void test_one_shot_timer() {
    auto poller = std::make_unique<MockPoller>();
    auto* mock = poller.get();
    EventLoop loop(std::move(poller));

    int fired = 0;
    loop.add_timer(100ms, [&]() -> std::optional<Duration> {
        ++fired;
        return std::nullopt;
    });

    loop.run_once();
    check(fired == 0, "timer should not fire before its deadline");
    check(mock->last_timeout().has_value(), "timer should bound the poll timeout");
    check(mock->last_timeout().value() > Duration::zero(),
          "timeout should be positive before the deadline");
    check(mock->last_timeout().value() <= 100ms,
          "timeout should not exceed the nearest timer");

    std::this_thread::sleep_for(120ms);
    loop.run_once();

    check(fired == 1, "one-shot timer should fire once");
    check(loop.timer_count() == 0, "one-shot timer should remove itself");
}

void test_recurring_timer() {
    auto poller = std::make_unique<MockPoller>();
    EventLoop loop(std::move(poller));

    int fired = 0;
    loop.add_timer(40ms, [&]() -> std::optional<Duration> {
        ++fired;
        if (fired < 2) {
            return 40ms;
        }
        return std::nullopt;
    });

    std::this_thread::sleep_for(50ms);
    loop.run_once();
    check(fired == 1, "recurring timer should fire the first time");

    std::this_thread::sleep_for(50ms);
    loop.run_once();
    check(fired == 2, "recurring timer should fire the second time");
    check(loop.timer_count() == 0, "timer should stop after returning nullopt");
}

void test_cancel_timer() {
    auto poller = std::make_unique<MockPoller>();
    auto* mock = poller.get();
    EventLoop loop(std::move(poller));

    int fired = 0;
    const auto id = loop.add_timer(200ms, [&]() -> std::optional<Duration> {
        ++fired;
        return std::nullopt;
    });

    check(loop.cancel_timer(id), "cancel_timer should find an active timer");
    check(loop.timer_count() == 0, "cancelled timer should leave the active map");

    loop.run_once();
    check(fired == 0, "cancelled timer must not fire");
    check(!mock->last_timeout().has_value(),
          "cancelled timer should not affect poll timeout");
}

void test_before_after_sleep_order() {
    auto poller = std::make_unique<MockPoller>();
    auto* mock = poller.get();
    EventLoop loop(std::move(poller));

    std::string order;
    loop.add_file(
        3,
        kReadable,
        [&](int, std::uint32_t) { order += 'R'; },
        {});

    loop.set_before_sleep([&] {
        order += 'B';
        mock->inject(3, kReadable);  // 模拟 beforeSleep 把待写数据挂到 fd
    });
    loop.set_after_sleep([&] { order += 'A'; });

    loop.run_once();
    check(order == "BAR",
          "order should be beforeSleep -> poll -> afterSleep -> file event");
}

void test_remove_file() {
    auto poller = std::make_unique<MockPoller>();
    auto* mock = poller.get();
    EventLoop loop(std::move(poller));

    int calls = 0;
    loop.add_file(4, kReadable, [&](int, std::uint32_t) { ++calls; }, {});
    check(loop.remove_file(4), "remove_file should find the fd");
    check(loop.file_count() == 0, "fd should be removed from the loop");

    mock->inject(4, kReadable);
    loop.run_once();
    check(calls == 0, "removed fd must not receive events");
}

void test_stop_skips_round() {
    auto poller = std::make_unique<MockPoller>();
    EventLoop loop(std::move(poller));

    bool before_sleep_called = false;
    loop.set_before_sleep([&] { before_sleep_called = true; });
    loop.stop();
    loop.run_once();

    check(!before_sleep_called, "stopped loop should not run a round");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"file_read_event", test_file_read_event},
        {"read_write_order", test_read_write_order},
        {"one_shot_timer", test_one_shot_timer},
        {"recurring_timer", test_recurring_timer},
        {"cancel_timer", test_cancel_timer},
        {"before_after_sleep_order", test_before_after_sleep_order},
        {"remove_file", test_remove_file},
        {"stop_skips_round", test_stop_skips_round},
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
