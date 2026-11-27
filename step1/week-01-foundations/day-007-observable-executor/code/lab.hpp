#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string_view>

#include "async_logger.hpp"
#include "bounded_thread_pool.hpp"
#include "hashed_wheel_timer.hpp"
#include "size_class_allocator.hpp"

namespace archdesign::lab {

using Milliseconds = std::chrono::milliseconds;

struct Config {
    std::size_t workers = 2;
    std::size_t worker_queue = 16;
    std::size_t ingress_capacity = 16;
    std::size_t max_in_flight = 16;
    std::size_t ring_capacity = 16;
    std::size_t dispatch_budget = 16;
    std::size_t completion_budget = 16;
    std::size_t log_capacity = 64;
    std::size_t max_payload_bytes = 8192;
    Milliseconds timer_tick{2};
};

struct TaskSpec {
    std::uint64_t id = 0;
    Milliseconds work{0};
    Milliseconds timeout{1000};
    std::size_t payload_bytes = 64;
    bool fail = false;

    // 实验注入点：先通知“已开始”，再等待 gate。正常业务不设置它们。
    std::shared_ptr<std::promise<void>> started;
    std::shared_future<void> gate;
};

enum class Status {
    Succeeded,
    Failed,
    TimedOut,
    IngressRejected,
    AdmissionRejected,
    PoolRejected,
    AdmissionError,
    Stopped,
};

std::string_view to_string(Status status);

struct Result {
    std::uint64_t id = 0;
    Status status = Status::Failed;
    std::uint64_t checksum = 0;
    std::int64_t elapsed_us = 0;
};

struct Snapshot {
    std::uint64_t offered = 0;
    std::uint64_t admitted = 0;
    std::uint64_t succeeded = 0;
    std::uint64_t failed = 0;
    std::uint64_t timed_out = 0;
    std::uint64_t ingress_rejected = 0;
    std::uint64_t admission_rejected = 0;
    std::uint64_t pool_rejected = 0;
    std::uint64_t admission_errors = 0;
    std::uint64_t stopped_rejected = 0;
    std::uint64_t retired = 0;
    std::uint64_t late_completed = 0;
    std::uint64_t observer_errors = 0;
    std::uint64_t rounds = 0;
    std::size_t ingress = 0;
    std::size_t in_flight = 0;
    std::size_t max_ingress = 0;
    std::size_t max_active = 0;
    BoundedThreadPool::Stats pool;
    HashedWheelTimer::Stats timer;
    AsyncLogger::Stats logger;
    SizeClassAllocator::Stats allocator;
    SizeClassAllocator::Stats allocator_before;
};

// 单 owner 线程调用；结果回调也在 owner 线程中执行，且不得重入管线。
// 工作线程只处理任务、发布完成事件；时间轮线程只设置超时提示。
class Pipeline {
public:
    using ResultHandler = std::function<void(const Result&)>;

    Pipeline(Config config, LogSink& sink, ResultHandler handler);
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // true 仅表示进入入站队列。最终接纳/拒绝通过 ResultHandler 通知。
    bool offer(TaskSpec spec);
    void pump();
    bool drain_for(Milliseconds timeout);
    void shutdown();
    [[nodiscard]] Snapshot snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace archdesign::lab
