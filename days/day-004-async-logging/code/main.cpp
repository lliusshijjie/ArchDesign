// main.cpp
//
// Day 04 的异步日志行为测试：后台消费、flush、队列满策略、丢弃策略
// 和两种关闭语义。

#include "async_logger.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using archdesign::AsyncLogger;
using archdesign::LogLevel;
using archdesign::LogRecord;
using archdesign::LogSink;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class MemorySink final : public LogSink {
public:
    void write(const LogRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.push_back(record);
    }

    void flush() override {
        ++flushes_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_.size();
    }

    [[nodiscard]] std::vector<LogRecord> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_;
    }

    [[nodiscard]] int flushes() const {
        return flushes_.load();
    }

private:
    mutable std::mutex mutex_;
    std::vector<LogRecord> records_;
    std::atomic<int> flushes_{0};
};

// 第一次 write 会阻塞，用来稳定地制造“后台正在写、队列已满”的窗口。
class BlockingSink final : public LogSink {
public:
    void write(const LogRecord& record) override {
        std::unique_lock<std::mutex> lock(mutex_);
        started_ = true;
        started_cv_.notify_all();
        cv_.wait(lock, [this] { return released_; });
        records_.push_back(record);
    }

    void flush() override {}

    void wait_until_started() {
        std::unique_lock<std::mutex> lock(mutex_);
        started_cv_.wait(lock, [this] { return started_; });
    }

    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }

    [[nodiscard]] std::vector<LogRecord> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable started_cv_;
    std::condition_variable cv_;
    bool started_ = false;
    bool released_ = false;
    std::vector<LogRecord> records_;
};

class ThrowingSink final : public LogSink {
public:
    void write(const LogRecord&) override {
        throw std::runtime_error("sink failure");
    }

    void flush() override {}
};

void test_flush_and_batching() {
    MemorySink sink;
    AsyncLogger logger(sink, 128, 8);

    for (int i = 0; i < 100; ++i) {
        check(logger.log(LogLevel::Info, "message-" + std::to_string(i)),
              "log should be accepted");
    }

    logger.flush();

    check(sink.size() == 100, "flush should make all records visible");
    check(sink.flushes() >= 1, "flush should call the sink flush");
    const auto stats = logger.stats();
    check(stats.accepted == 100, "100 records should be accepted");
    check(stats.processed == 100, "100 records should be processed");
    check(stats.queue_depth == 0, "queue should be empty after flush");

    logger.shutdown(AsyncLogger::ShutdownMode::Drain);
}

void test_drop_new() {
    BlockingSink sink;
    AsyncLogger logger(sink, 1, 4, AsyncLogger::OverflowPolicy::DropNew);

    check(logger.log(LogLevel::Info, "A"), "A should be accepted");
    sink.wait_until_started();

    check(logger.log(LogLevel::Info, "B"), "B should fill the queue");
    check(!logger.log(LogLevel::Info, "C"), "C should be dropped");

    const auto before = logger.stats();
    check(before.dropped == 1, "DropNew should count one dropped record");

    sink.release();
    logger.shutdown(AsyncLogger::ShutdownMode::Drain);

    const auto records = sink.snapshot();
    check(records.size() == 2, "A and B should be written");
    check(records[0].message == "A", "first record should be A");
    check(records[1].message == "B", "second record should be B");
}

void test_drop_oldest() {
    BlockingSink sink;
    AsyncLogger logger(sink, 2, 4, AsyncLogger::OverflowPolicy::DropOldest);

    check(logger.log(LogLevel::Info, "A"), "A should be accepted");
    sink.wait_until_started();

    check(logger.log(LogLevel::Info, "B"), "B should be queued");
    check(logger.log(LogLevel::Info, "C"), "C should be queued");
    check(logger.log(LogLevel::Info, "D"), "D should replace B");

    const auto before = logger.stats();
    check(before.dropped == 1, "DropOldest should count one dropped record");

    sink.release();
    logger.shutdown(AsyncLogger::ShutdownMode::Drain);

    const auto records = sink.snapshot();
    check(records.size() == 3, "A, C and D should be written");
    check(records[0].message == "A", "first record should be A");
    check(records[1].message == "C", "second record should be C");
    check(records[2].message == "D", "third record should be D");
}

void test_block_policy() {
    BlockingSink sink;
    AsyncLogger logger(sink, 1, 4, AsyncLogger::OverflowPolicy::Block);

    check(logger.log(LogLevel::Info, "A"), "A should be accepted");
    sink.wait_until_started();
    check(logger.log(LogLevel::Info, "B"), "B should fill the queue");

    auto blocked = std::async(std::launch::async, [&] {
        return logger.log(LogLevel::Info, "C");
    });

    check(blocked.wait_for(50ms) == std::future_status::timeout,
          "C should block while the queue is full");

    sink.release();
    check(blocked.get(), "C should be accepted after space is available");

    logger.shutdown(AsyncLogger::ShutdownMode::Drain);
    const auto records = sink.snapshot();
    check(records.size() == 3, "A, B and C should be written");
}

void test_discard_shutdown() {
    BlockingSink sink;
    AsyncLogger logger(sink, 8, 4, AsyncLogger::OverflowPolicy::Block);

    check(logger.log(LogLevel::Info, "A"), "A should be accepted");
    sink.wait_until_started();
    check(logger.log(LogLevel::Info, "B"), "B should be queued");
    check(logger.log(LogLevel::Info, "C"), "C should be queued");

    std::thread stopper([&] {
        logger.shutdown(AsyncLogger::ShutdownMode::Discard);
    });

    for (int i = 0; i < 2000 && logger.stats().dropped < 2; ++i) {
        std::this_thread::sleep_for(1ms);
    }
    check(logger.stats().dropped == 2,
          "Discard should drop queued records");

    sink.release();
    stopper.join();

    const auto records = sink.snapshot();
    check(records.size() == 1, "only the in-flight record should be written");
    check(records[0].message == "A", "in-flight record should be A");

    const auto stats = logger.stats();
    check(stats.accepted == 3, "three records were accepted");
    check(stats.processed == 1, "only A was processed");
    check(stats.dropped == 2, "B and C were dropped");
}

void test_sink_error_isolated() {
    ThrowingSink sink;
    AsyncLogger logger(sink, 16, 4);

    check(logger.log(LogLevel::Error, "boom"), "record should be accepted");
    logger.flush();

    const auto stats = logger.stats();
    check(stats.processed == 1, "record should be counted as processed");
    check(stats.sink_errors == 1, "sink exception should be counted");

    logger.shutdown(AsyncLogger::ShutdownMode::Drain);
}

void test_stop_rejects_new_logs() {
    MemorySink sink;
    AsyncLogger logger(sink, 8, 4);

    logger.shutdown(AsyncLogger::ShutdownMode::Drain);
    check(!logger.log(LogLevel::Info, "after-stop"),
          "log after shutdown should be rejected");

    const auto stats = logger.stats();
    check(stats.accepted == 0, "no record should be accepted");
    check(stats.dropped == 1, "rejected record should be counted as dropped");

    logger.shutdown(AsyncLogger::ShutdownMode::Discard);  // 幂等
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"flush_and_batching", test_flush_and_batching},
        {"drop_new", test_drop_new},
        {"drop_oldest", test_drop_oldest},
        {"block_policy", test_block_policy},
        {"discard_shutdown", test_discard_shutdown},
        {"sink_error_isolated", test_sink_error_isolated},
        {"stop_rejects_new_logs", test_stop_rejects_new_logs},
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
