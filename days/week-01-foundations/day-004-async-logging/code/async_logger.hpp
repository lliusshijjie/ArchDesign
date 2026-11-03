// async_logger.hpp
//
// Day 04 的教学实现：有界队列 + 后台批量消费的异步日志器。
//
// 设计重点不是“格式化字符串”，而是日志链路的边界：
//   - 调用线程只捕获不可变 LogRecord，不做磁盘 I/O；
//   - 有界队列，满时由 OverflowPolicy 决定丢/等/覆盖；
//   - 后台线程批量取出，减少锁和 sink 调用次数；
//   - flush 等待队列排空并刷 sink；
//   - shutdown 分 Drain / Discard 两种语义。
//
// 生产实现（Log4j2 AsyncLogger / spdlog async_logger）通常会把这里的
// mutex 队列换成 Disruptor/环形队列，或者把格式化放到后台；本课先把
// “异步”的语义讲清楚，Day 06 再拆 Disruptor。

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace archdesign {

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error,
};

inline std::string_view to_string(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "UNKNOWN";
}

struct LogRecord {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level = LogLevel::Info;
    std::thread::id thread_id;
    std::string message;
};

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() = 0;
};

class AsyncLogger {
public:
    enum class OverflowPolicy {
        Block,      // 队列满时阻塞调用者（不丢日志，但可能拖慢业务）
        DropNew,    // 丢弃新日志（低延迟，允许丢失）
        DropOldest, // 丢弃最旧日志，保留最新（低延迟，保留近期现场）
    };

    enum class ShutdownMode {
        Drain,   // 停止接收，消费完队列再退出
        Discard, // 停止接收，丢弃尚未开始的日志
    };

    struct Stats {
        std::uint64_t accepted = 0;
        std::uint64_t dropped = 0;
        std::uint64_t processed = 0;
        std::uint64_t sink_errors = 0;
        std::size_t queue_depth = 0;
    };

    AsyncLogger(LogSink& sink,
                std::size_t capacity,
                std::size_t batch_size = 64,
                OverflowPolicy policy = OverflowPolicy::Block)
        : sink_(sink),
          capacity_(capacity),
          batch_size_(batch_size),
          policy_(policy) {
        if (capacity == 0) {
            throw std::invalid_argument("capacity must be > 0");
        }
        if (batch_size == 0) {
            throw std::invalid_argument("batch_size must be > 0");
        }
        worker_ = std::thread([this] { worker_loop(); });
    }

    ~AsyncLogger() {
        try {
            shutdown(ShutdownMode::Drain);
        } catch (...) {
            // 析构函数不能抛；sink 的错误不应该让进程终止。
        }
    }

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    // 返回 false 表示日志被丢弃或 logger 已停止。
    bool log(LogLevel level, std::string message) {
        LogRecord record{std::chrono::system_clock::now(),
                         level,
                         std::this_thread::get_id(),
                         std::move(message)};

        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_) {
            ++dropped_;
            return false;
        }

        if (queue_.size() >= capacity_) {
            switch (policy_) {
            case OverflowPolicy::Block:
                not_full_.wait(lock, [this] {
                    return stopping_ || queue_.size() < capacity_;
                });
                if (stopping_) {
                    ++dropped_;
                    return false;
                }
                break;
            case OverflowPolicy::DropNew:
                ++dropped_;
                return false;
            case OverflowPolicy::DropOldest:
                queue_.pop_front();
                ++dropped_;
                break;
            }
        }

        queue_.push_back(std::move(record));
        ++accepted_;
        not_empty_.notify_one();
        return true;
    }

    // 等待当前已入队日志被 sink 消费，并调用 sink.flush()。
    // 注意：并发生产者持续写入时，flush 可能等待较久；它不是全局屏障。
    void flush() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!worker_.joinable()) {
            sink_.flush();
            return;
        }

        idle_cv_.wait(lock, [this] {
            return queue_.empty() && !processing_;
        });
        sink_.flush();
    }

    // 幂等关闭。第一次调用的 mode 生效；调用后 log() 返回 false。
    void shutdown(ShutdownMode mode = ShutdownMode::Drain) {
        std::call_once(shutdown_once_, [this, mode] {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stopping_ = true;
                if (mode == ShutdownMode::Discard) {
                    dropped_ += static_cast<std::uint64_t>(queue_.size());
                    queue_.clear();
                }
                not_empty_.notify_all();
                not_full_.notify_all();
                idle_cv_.notify_all();
            }

            if (worker_.joinable()) {
                worker_.join();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                sink_.flush();
            }
        });
    }

    [[nodiscard]] Stats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Stats result;
        result.accepted = accepted_.load(std::memory_order_relaxed);
        result.dropped = dropped_.load(std::memory_order_relaxed);
        result.processed = processed_.load(std::memory_order_relaxed);
        result.sink_errors = sink_errors_.load(std::memory_order_relaxed);
        result.queue_depth = queue_.size();
        return result;
    }

    [[nodiscard]] OverflowPolicy policy() const noexcept {
        return policy_;
    }

private:
    void worker_loop() {
        for (;;) {
            std::vector<LogRecord> batch;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                not_empty_.wait(lock, [this] {
                    return stopping_ || !queue_.empty();
                });

                if (queue_.empty()) {
                    if (stopping_) {
                        break;
                    }
                    continue;
                }

                const auto count = std::min(batch_size_, queue_.size());
                batch.reserve(count);
                for (std::size_t i = 0; i < count; ++i) {
                    batch.push_back(std::move(queue_.front()));
                    queue_.pop_front();
                }

                processing_ = true;
                not_full_.notify_all();
            }

            for (const auto& record : batch) {
                try {
                    sink_.write(record);
                } catch (...) {
                    ++sink_errors_;
                }
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                processing_ = false;
                processed_ += static_cast<std::uint64_t>(batch.size());
                idle_cv_.notify_all();
            }
        }
    }

    LogSink& sink_;
    const std::size_t capacity_;
    const std::size_t batch_size_;
    const OverflowPolicy policy_;

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::condition_variable idle_cv_;
    std::deque<LogRecord> queue_;
    bool processing_ = false;
    bool stopping_ = false;
    std::thread worker_;
    std::once_flag shutdown_once_;

    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> processed_{0};
    std::atomic<std::uint64_t> sink_errors_{0};
};

} // namespace archdesign
