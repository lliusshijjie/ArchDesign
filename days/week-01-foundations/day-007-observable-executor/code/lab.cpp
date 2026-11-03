#include "lab.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "bounded_ring_buffer.hpp"
#include "event_loop.hpp"

namespace archdesign::lab {
namespace {

using Clock = std::chrono::steady_clock;

Config checked(Config config) {
    if (config.workers == 0 || config.worker_queue == 0 ||
        config.ingress_capacity == 0 || config.max_in_flight == 0 ||
        config.dispatch_budget == 0 || config.completion_budget == 0 ||
        config.log_capacity == 0 || config.max_payload_bytes == 0 ||
        config.timer_tick <= Milliseconds::zero()) {
        throw std::invalid_argument("capacities, budgets and timer tick must be positive");
    }
    if (config.ring_capacity < 2 ||
        (config.ring_capacity & (config.ring_capacity - 1)) != 0 ||
        config.ring_capacity < config.max_in_flight) {
        throw std::invalid_argument("ring capacity must be a power of two >= max_in_flight and >= 2");
    }
    return config;
}

class PooledBytes {
public:
    explicit PooledBytes(std::size_t size)
        : data_(static_cast<unsigned char*>(SizeClassAllocator::instance().allocate(size))),
          size_(size) {}
    ~PooledBytes() { SizeClassAllocator::instance().deallocate(data_); }
    PooledBytes(const PooledBytes&) = delete;
    PooledBytes& operator=(const PooledBytes&) = delete;

    std::uint64_t fill_and_sum(std::uint64_t id) {
        const auto byte = static_cast<unsigned char>(id & 0xffu);
        std::fill_n(data_, size_, byte);
        return static_cast<std::uint64_t>(byte) * size_;
    }

private:
    unsigned char* data_;
    std::size_t size_;
};

struct Job {
    explicit Job(TaskSpec input)
        : spec(std::move(input)), payload(spec.payload_bytes), admitted_at(Clock::now()),
          deadline(admitted_at + spec.timeout) {}

    TaskSpec spec;
    PooledBytes payload;
    Clock::time_point admitted_at;
    Clock::time_point deadline;
    std::atomic<bool> timeout_hint{false};
    std::shared_ptr<HashedWheelTimer::Timeout> timeout;
    bool responded = false; // 仅 owner 线程读写。
};

static_assert(alignof(Job) <= alignof(std::max_align_t));

struct JobDeleter {
    void operator()(Job* job) const {
        job->~Job();
        SizeClassAllocator::instance().deallocate(job);
    }
};

std::shared_ptr<Job> make_job(TaskSpec spec) {
    auto& allocator = SizeClassAllocator::instance();
    void* storage = allocator.allocate(sizeof(Job));
    Job* job = nullptr;
    try {
        job = ::new (storage) Job(std::move(spec));
    } catch (...) {
        allocator.deallocate(storage);
        throw;
    }
    // 控制块分配失败时，shared_ptr 构造函数会调用传入的 deleter。
    return std::shared_ptr<Job>(job, JobDeleter{});
}

struct Completion {
    std::shared_ptr<Job> job;
    std::uint64_t checksum = 0;
    bool failed = false;
};

} // namespace

std::string_view to_string(Status status) {
    switch (status) {
    case Status::Succeeded: return "succeeded";
    case Status::Failed: return "failed";
    case Status::TimedOut: return "timed_out";
    case Status::IngressRejected: return "ingress_rejected";
    case Status::AdmissionRejected: return "admission_rejected";
    case Status::PoolRejected: return "pool_rejected";
    case Status::AdmissionError: return "admission_error";
    case Status::Stopped: return "stopped";
    }
    return "unknown";
}

class Pipeline::Impl {
public:
    Impl(Config config, LogSink& sink, ResultHandler handler)
        : config_(checked(config)), handler_(std::move(handler)),
          owner_(std::this_thread::get_id()),
          allocator_before_(SizeClassAllocator::instance().stats()),
          pool_(config_.workers, config_.worker_queue, BoundedThreadPool::RejectPolicy::Abort),
          timer_(config_.timer_tick, 64, config_.max_in_flight),
          logger_(sink, config_.log_capacity, 1, AsyncLogger::OverflowPolicy::DropNew),
          completions_(config_.ring_capacity) {
        if (!handler_) throw std::invalid_argument("result handler must not be empty");
        auto poller = std::make_unique<MockPoller>();
        poller_ = poller.get();
        loop_ = std::make_unique<EventLoop>(std::move(poller));
        loop_->set_before_sleep([this] {
            collect_completions();
            observe_deadlines();
        });
        loop_->add_file(kInputFd, kReadable,
                        [this](int, std::uint32_t) { dispatch_inputs(); }, {});
    }

    void require_owner() const {
        if (std::this_thread::get_id() != owner_ || pumping_ || notifying_) {
            throw std::logic_error("pipeline operations require its owner thread and no reentry");
        }
    }

    bool offer(TaskSpec spec) {
        require_owner();
        if (spec.id == 0 || spec.work < Milliseconds::zero() ||
            spec.work > Milliseconds{60000} || spec.timeout <= Milliseconds::zero() ||
            spec.timeout > Milliseconds{60000} || spec.payload_bytes == 0 ||
            spec.payload_bytes > config_.max_payload_bytes) {
            throw std::invalid_argument("invalid task id, duration or payload size");
        }
        if (jobs_.contains(spec.id) || std::any_of(inputs_.begin(), inputs_.end(),
                [&spec](const TaskSpec& pending) { return pending.id == spec.id; })) {
            throw std::invalid_argument("task id is already pending");
        }
        ++metrics_.offered;
        if (!accepting_) {
            ++metrics_.stopped_rejected;
            notify({spec.id, Status::Stopped, 0, 0});
            return false;
        }
        if (inputs_.size() >= config_.ingress_capacity) {
            ++metrics_.ingress_rejected;
            notify({spec.id, Status::IngressRejected, 0, 0});
            return false;
        }
        inputs_.push_back(std::move(spec));
        metrics_.max_ingress = std::max(metrics_.max_ingress, inputs_.size());
        return true;
    }

    void pump() {
        require_owner();
        if (stopped_) return;
        pumping_ = true;
        try {
            if (!inputs_.empty()) poller_->inject(kInputFd, kReadable);
            loop_->run_once();
            ++metrics_.rounds;
        } catch (...) {
            pumping_ = false;
            throw;
        }
        pumping_ = false;
    }

    bool drain_for(Milliseconds timeout) {
        require_owner();
        const auto until = Clock::now() + timeout;
        while (!inputs_.empty() || !jobs_.empty()) {
            pump();
            if (inputs_.empty() && jobs_.empty()) return true;
            if (Clock::now() >= until) return false;
            std::this_thread::sleep_for(Milliseconds{1});
        }
        return true;
    }

    void shutdown() {
        require_owner();
        if (stopped_) return;
        accepting_ = false;
        // 先继续消费完成事件。若先 join worker，再排空 ring，会形成循环等待。
        while (!inputs_.empty() || !jobs_.empty()) {
            pump();
            std::this_thread::sleep_for(Milliseconds{1});
        }
        timer_.stop();
        pool_.shutdown(BoundedThreadPool::ShutdownMode::Drain);
        completions_.close(); // 此时所有生产者均已退出。
        loop_->remove_file(kInputFd);
        loop_->stop();
        logger_.shutdown(AsyncLogger::ShutdownMode::Drain);
        stopped_ = true;
    }

    Snapshot snapshot() const {
        require_owner();
        auto result = metrics_;
        result.ingress = inputs_.size();
        result.in_flight = jobs_.size();
        result.pool = pool_.stats();
        result.timer = timer_.stats();
        result.logger = logger_.stats();
        result.allocator = SizeClassAllocator::instance().stats();
        result.allocator_before = allocator_before_;
        return result;
    }

private:
    void notify(const Result& result) {
        notifying_ = true;
        try {
            handler_(result);
        } catch (...) {
            ++metrics_.observer_errors;
        }
        notifying_ = false;
    }

    void log(std::uint64_t id, std::string_view event) noexcept {
        try {
            logger_.log(LogLevel::Info, "id=" + std::to_string(id) + " event=" + std::string(event));
        } catch (...) {
            // 诊断日志的分配失败也不应该阻断完成事件的回收。
        }
    }

    void dispatch_inputs() {
        for (std::size_t i = 0; i < config_.dispatch_budget && !inputs_.empty(); ++i) {
            TaskSpec spec = std::move(inputs_.front());
            inputs_.pop_front();
            if (jobs_.size() >= config_.max_in_flight) {
                ++metrics_.admission_rejected;
                notify({spec.id, Status::AdmissionRejected, 0, 0});
                continue;
            }
            admit(std::move(spec));
        }
    }

    void admit(TaskSpec spec) {
        const auto id = spec.id;
        std::shared_ptr<Job> job;
        bool accepted = false;
        Status rejected = Status::AdmissionError;
        try {
            job = make_job(std::move(spec));
            jobs_.emplace(id, job);
            std::weak_ptr<Job> weak = job;
            job->timeout = timer_.new_timeout(job->spec.timeout, [weak] {
                // 不碰 EventLoop、业务状态或 logger，也不等待 ring。
                if (auto current = weak.lock()) {
                    current->timeout_hint.store(true, std::memory_order_release);
                }
            });
            pool_.execute([this, job] { perform(job); });
            accepted = true;
        } catch (const RejectedExecutionError&) {
            rejected = Status::PoolRejected;
        } catch (...) {
            rejected = Status::AdmissionError;
        }

        if (accepted) {
            ++metrics_.admitted;
            metrics_.max_active = std::max(metrics_.max_active, jobs_.size());
            log(id, "admitted");
            return;
        }
        if (job) timer_.cancel(job->timeout);
        jobs_.erase(id);
        if (rejected == Status::PoolRejected) ++metrics_.pool_rejected;
        else ++metrics_.admission_errors;
        notify({id, rejected, 0, 0});
    }

    void perform(const std::shared_ptr<Job>& job) {
        Completion completion;
        completion.job = job;
        try {
            if (job->spec.started) job->spec.started->set_value();
            if (job->spec.gate.valid()) job->spec.gate.wait();
            std::this_thread::sleep_for(job->spec.work);
            if (job->spec.fail) throw std::runtime_error("injected task failure");
            completion.checksum = job->payload.fill_and_sum(job->spec.id);
        } catch (...) {
            completion.failed = true;
        }
        // 每个接纳任务只发布一个完成事件；容量 >= 在途上限。
        // 生命周期协议保证 join 之前 ring 不关闭。
        if (!completions_.push(std::move(completion))) std::terminate();
    }

    void respond(const std::shared_ptr<Job>& job, Status status, std::uint64_t checksum = 0) {
        job->responded = true;
        if (status == Status::Succeeded) ++metrics_.succeeded;
        else if (status == Status::Failed) ++metrics_.failed;
        else ++metrics_.timed_out;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - job->admitted_at).count();
        notify({job->spec.id, status, checksum, elapsed});
        log(job->spec.id, to_string(status));
    }

    void collect_completions() {
        for (std::size_t i = 0; i < config_.completion_budget; ++i) {
            Completion completion;
            if (!completions_.try_pop(completion)) break;
            const auto& job = completion.job;
            if (job->responded) {
                ++metrics_.late_completed;
            } else if (Clock::now() >= job->deadline) {
                respond(job, Status::TimedOut);
            } else {
                respond(job, completion.failed ? Status::Failed : Status::Succeeded,
                        completion.checksum);
            }
            timer_.cancel(job->timeout);
            jobs_.erase(job->spec.id);
            ++metrics_.retired;
        }
    }

    void observe_deadlines() {
        const auto now = Clock::now();
        for (const auto& [id, job] : jobs_) {
            (void)id;
            if (!job->responded && job->timeout_hint.load(std::memory_order_acquire) &&
                now >= job->deadline) {
                // 时间轮是提示；绝对 deadline 再校验，避免 tick 量化导致提前超时。
                respond(job, Status::TimedOut);
            }
        }
    }

    static constexpr int kInputFd = 7; // MockPoller 的逻辑 fd，不是 OS socket。
    Config config_;
    ResultHandler handler_;
    std::thread::id owner_;
    SizeClassAllocator::Stats allocator_before_;
    BoundedThreadPool pool_;
    HashedWheelTimer timer_;
    AsyncLogger logger_;
    BoundedRingBuffer<Completion> completions_;
    MockPoller* poller_ = nullptr;
    std::unique_ptr<EventLoop> loop_;
    std::deque<TaskSpec> inputs_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Job>> jobs_;
    Snapshot metrics_;
    bool accepting_ = true;
    bool stopped_ = false;
    bool pumping_ = false;
    bool notifying_ = false;
};

Pipeline::Pipeline(Config config, LogSink& sink, ResultHandler handler)
    : impl_(std::make_unique<Impl>(config, sink, std::move(handler))) {}

Pipeline::~Pipeline() {
    try { impl_->shutdown(); }
    catch (...) { std::terminate(); }
}

bool Pipeline::offer(TaskSpec spec) { return impl_->offer(std::move(spec)); }
void Pipeline::pump() { impl_->pump(); }
bool Pipeline::drain_for(Milliseconds timeout) { return impl_->drain_for(timeout); }
void Pipeline::shutdown() { impl_->shutdown(); }
Snapshot Pipeline::snapshot() const { return impl_->snapshot(); }

} // namespace archdesign::lab
