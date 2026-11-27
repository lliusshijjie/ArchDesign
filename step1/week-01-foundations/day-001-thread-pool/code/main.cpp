// main.cpp
//
// Day 01 的可运行实验：验证 BoundedThreadPool 的接纳、拒绝、异常隔离、
// 两阶段关闭和指标。使用最朴素的断言，避免引入测试框架。

#include "bounded_thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using archdesign::BoundedThreadPool;
using archdesign::RejectedExecutionError;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_basic_futures() {
    BoundedThreadPool pool(4, 64);

    std::vector<std::future<int>> futures;
    futures.reserve(32);
    for (int i = 0; i < 32; ++i) {
        futures.push_back(pool.submit([i] { return i * i; }));
    }

    long long sum = 0;
    for (auto& future : futures) {
        sum += future.get();
    }
    check(sum == 10416, "sum of squares 0..31 should be 10416");

    pool.shutdown(BoundedThreadPool::ShutdownMode::Drain);
    const auto stats = pool.stats();
    check(stats.submitted == 32, "32 tasks should be submitted");
    check(stats.completed == 32, "32 tasks should be completed");
    check(stats.failed == 0, "no task should fail");
    check(stats.queued == 0, "queue should be empty after shutdown");
}

void test_submit_propagates_exception() {
    BoundedThreadPool pool(1, 8);

    auto ok = pool.submit([](int value) { return value + 1; }, 41);
    check(ok.get() == 42, "submit should forward arguments and return value");

    auto void_task = pool.submit([] {});
    void_task.get();  // 编译期同时验证 R=void 的返回路径

    auto bad = pool.submit([]() -> int { throw std::runtime_error("expected"); });
    bool caught = false;
    try {
        (void)bad.get();
    } catch (const std::runtime_error&) {
        caught = true;
    }
    check(caught, "exception should be rethrown by future::get");

    pool.shutdown(BoundedThreadPool::ShutdownMode::Drain);
}

void test_worker_survives_task_exception() {
    BoundedThreadPool pool(1, 8);

    pool.execute([] { throw std::runtime_error("boom"); });
    auto after = pool.submit([] { return 7; });
    check(after.get() == 7, "worker should survive an execute() exception");

    pool.shutdown(BoundedThreadPool::ShutdownMode::Drain);
    const auto stats = pool.stats();
    check(stats.failed == 1, "failed execute task should be counted");
    check(stats.completed == 2, "both tasks should be completed");
}

void test_abort_when_full() {
    BoundedThreadPool pool(1, 1, BoundedThreadPool::RejectPolicy::Abort);

    std::promise<void> started;
    std::promise<void> release;
    auto blocker = pool.submit([&] {
        started.set_value();
        release.get_future().wait();
    });
    started.get_future().wait();

    pool.execute([] {});  // 占满唯一的等待槽位

    bool rejected = false;
    try {
        pool.execute([] {});
    } catch (const RejectedExecutionError&) {
        rejected = true;
    }
    check(rejected, "third task should be rejected when queue is full");

    release.set_value();
    blocker.get();
    pool.shutdown(BoundedThreadPool::ShutdownMode::Drain);

    const auto stats = pool.stats();
    check(stats.rejected == 1, "one rejection should be recorded");
    check(stats.completed == 2, "blocker and queued task should complete");
}

void test_caller_runs_on_submitter() {
    BoundedThreadPool pool(1, 1, BoundedThreadPool::RejectPolicy::CallerRuns);

    std::promise<void> started;
    std::promise<void> release;
    auto blocker = pool.submit([&] {
        started.set_value();
        release.get_future().wait();
    });
    started.get_future().wait();

    pool.execute([] {});  // 占满等待槽位

    const auto submitter = std::this_thread::get_id();
    std::thread::id executed_on;
    pool.execute([&] { executed_on = std::this_thread::get_id(); });

    check(executed_on == submitter,
          "CallerRuns should execute the task on the submitting thread");

    release.set_value();
    blocker.get();
    pool.shutdown(BoundedThreadPool::ShutdownMode::Drain);

    const auto stats = pool.stats();
    check(stats.submitted == 3, "all three accepted tasks should be counted");
    check(stats.completed == 3, "inline task should also be counted as completed");
}

void test_drain_shutdown() {
    BoundedThreadPool pool(2, 16);
    std::atomic<int> completed{0};

    for (int i = 0; i < 16; ++i) {
        pool.execute([&] { ++completed; });
    }

    pool.shutdown(BoundedThreadPool::ShutdownMode::Drain);
    check(completed.load() == 16, "Drain should finish all queued tasks");

    const auto stats = pool.stats();
    check(stats.completed == 16, "stats should see all completions");
    check(stats.dropped == 0, "Drain should not drop tasks");
}

void test_cancel_pending_shutdown() {
    BoundedThreadPool pool(1, 8);
    std::promise<void> started;
    std::promise<void> release;
    std::atomic<int> ran{0};

    pool.execute([&] {
        started.set_value();
        release.get_future().wait();
        ++ran;
    });
    started.get_future().wait();

    for (int i = 0; i < 5; ++i) {
        pool.execute([&] { ++ran; });
    }

    // shutdown 会 join 阻塞中的 worker，因此放到另一个线程，主线程等
    // “排队任务已经被丢弃”这个可观察状态出现后再放行。
    std::thread stopper([&] { pool.shutdown(BoundedThreadPool::ShutdownMode::CancelPending); });

    for (int i = 0; i < 2000 && pool.stats().dropped < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(pool.stats().dropped == 5, "CancelPending should drop all queued tasks");

    release.set_value();
    stopper.join();

    check(ran.load() == 1, "only the running task should finish");
    const auto stats = pool.stats();
    check(stats.completed == 1, "running task should complete");
    check(stats.queued == 0, "queue should be empty");
}

void test_discard_oldest() {
    BoundedThreadPool pool(1, 1, BoundedThreadPool::RejectPolicy::DiscardOldest);
    std::promise<void> started;
    std::promise<void> release;
    std::atomic<int> first{0};
    std::atomic<int> second{0};

    pool.execute([&] {
        started.set_value();
        release.get_future().wait();
    });
    started.get_future().wait();

    pool.execute([&] { ++first; });
    pool.execute([&] { ++second; });  // 挤掉 first

    release.set_value();
    pool.shutdown(BoundedThreadPool::ShutdownMode::Drain);

    check(first.load() == 0, "oldest queued task should be discarded");
    check(second.load() == 1, "new task should run");
    check(pool.stats().dropped == 1, "one dropped task should be recorded");
}

void test_shutdown_from_worker_is_rejected() {
    BoundedThreadPool pool(1, 4);
    std::atomic<bool> rejected{false};

    pool.execute([&] {
        try {
            pool.shutdown();
        } catch (const std::logic_error&) {
            rejected = true;
        }
    });

    auto after = pool.submit([] { return 11; });
    check(after.get() == 11, "worker should survive the rejected shutdown call");
    check(rejected.load(), "shutdown from a worker should throw logic_error");

    pool.shutdown(BoundedThreadPool::ShutdownMode::Drain);
}

void test_reject_after_shutdown() {
    BoundedThreadPool pool(1, 1);

    pool.shutdown(BoundedThreadPool::ShutdownMode::Drain);

    bool rejected = false;
    try {
        pool.execute([] {});
    } catch (const RejectedExecutionError&) {
        rejected = true;
    }
    check(rejected, "Abort policy should reject submissions after shutdown");
    check(pool.stats().rejected == 1, "post-shutdown rejection should be counted");

    // 幂等：第二次 shutdown 不应阻塞或抛异常。
    pool.shutdown(BoundedThreadPool::ShutdownMode::CancelPending);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"basic_futures", test_basic_futures},
        {"submit_propagates_exception", test_submit_propagates_exception},
        {"worker_survives_task_exception", test_worker_survives_task_exception},
        {"abort_when_full", test_abort_when_full},
        {"caller_runs_on_submitter", test_caller_runs_on_submitter},
        {"drain_shutdown", test_drain_shutdown},
        {"cancel_pending_shutdown", test_cancel_pending_shutdown},
        {"discard_oldest", test_discard_oldest},
        {"shutdown_from_worker_is_rejected", test_shutdown_from_worker_is_rejected},
        {"reject_after_shutdown", test_reject_after_shutdown},
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
