// bounded_thread_pool.hpp
//
// Day 01 的教学实现：固定工作线程 + 有界等待队列 + 显式拒绝策略 +
// 两阶段关闭 + 基本指标。
//
// 设计目标不是替代生产级执行器，而是把 ThreadPoolExecutor 中最值得
// 借鉴的几条边界显式化：
//   1. 队列有界，满时由策略决定“谁承担代价”；
//   2. 关闭分“排空”和“取消排队”两种语义；
//   3. worker 捕获任务异常，避免一个任务杀死整个线程；
//   4. 指标在锁内维护，读取时得到一致快照。
//
// 已知省略（见文章“生产化缺口”）：
//   - 动态调整 core/max、keepAlive；
//   - 优先级队列、延迟任务、work stealing；
//   - 每个任务的取消令牌/截止时间；
//   - 调用者排队（backpressure wait）而不是立即拒绝；
//   - 线程名、TLS/上下文传播、直方图指标。

#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace archdesign {

class RejectedExecutionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class BoundedThreadPool {
public:
    enum class RejectPolicy {
        Abort,          // 抛出 RejectedExecutionError（默认，最容易暴露问题）
        CallerRuns,     // 由提交线程同步执行，形成自然背压
        Discard,        // 静默丢弃新任务（只适合可丢的遥测/日志）
        DiscardOldest,  // 丢弃队列中最旧的任务，再放入新任务
    };

    enum class ShutdownMode {
        Drain,          // 停止接收新任务，已入队任务继续执行
        CancelPending,  // 停止接收，清空未开始的任务；运行中的任务无法强杀
    };

    struct Stats {
        std::uint64_t submitted = 0;  // 被接纳的任务数（含 CallerRuns 内联执行）
        std::uint64_t completed = 0;  // 已结束的任务数（含失败）
        std::uint64_t failed = 0;     // 抛出异常的任务数
        std::uint64_t rejected = 0;   // 因关闭/队列满被拒绝的任务数
        std::uint64_t dropped = 0;    // DiscardOldest/取消排队丢弃的任务数
        std::uint64_t max_queued = 0; // 历史最大排队长度
        std::size_t queued = 0;       // 当前排队长度（不含运行中）
        std::size_t active = 0;       // 当前正在执行任务的 worker 数
        std::size_t workers = 0;      // worker 总数
    };

    BoundedThreadPool(std::size_t worker_count,
                      std::size_t queue_capacity,
                      RejectPolicy policy = RejectPolicy::Abort)
        : capacity_(queue_capacity), policy_(policy) {
        if (worker_count == 0) {
            throw std::invalid_argument("worker_count must be > 0");
        }
        if (queue_capacity == 0) {
            throw std::invalid_argument("queue_capacity must be > 0");
        }

        workers_.reserve(worker_count);
        try {
            for (std::size_t i = 0; i < worker_count; ++i) {
                workers_.emplace_back([this] { worker_loop(); });
            }
        } catch (...) {
            // 部分 worker 已启动时，必须先把它们收回来，否则构造函数抛出后
            // std::thread 析构会触发 std::terminate。
            shutdown(ShutdownMode::CancelPending);
            throw;
        }
    }

    ~BoundedThreadPool() {
        try {
            shutdown(ShutdownMode::Drain);
        } catch (...) {
            // 析构函数不能抛异常。正常路径下只有“在 worker 内析构”会走到这里，
            // 那已经是未定义的生命周期用法，没有安全的补救方式。
        }
    }

    BoundedThreadPool(const BoundedThreadPool&) = delete;
    BoundedThreadPool& operator=(const BoundedThreadPool&) = delete;
    BoundedThreadPool(BoundedThreadPool&&) = delete;
    BoundedThreadPool& operator=(BoundedThreadPool&&) = delete;

    // 提交一个“即发即忘”的任务。
    // - Abort：队列满或池已关闭时抛 RejectedExecutionError；
    // - CallerRuns：队列满时在调用线程执行；池已关闭时不执行并返回 false；
    // - Discard / DiscardOldest：被丢弃时返回 false（新任务被丢弃才算 false）。
    // 返回 true 表示任务已被接纳（可能已内联执行完）。
    bool execute(std::function<void()> task) {
        if (!task) {
            throw std::invalid_argument("task must not be empty");
        }

        bool run_inline = false;
        Task discarded;  // 延迟到锁外析构，避免用户任务的析构函数重入池
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (!accepting_) {
                ++rejected_;
                if (policy_ == RejectPolicy::Abort) {
                    throw RejectedExecutionError("thread pool is shut down");
                }
                return false;
            }

            if (tasks_.size() < capacity_) {
                tasks_.push_back(std::move(task));
                ++submitted_;
                max_queued_ = std::max(max_queued_,
                                       static_cast<std::uint64_t>(tasks_.size()));
                not_empty_.notify_one();
                return true;
            }

            switch (policy_) {
            case RejectPolicy::Abort:
                ++rejected_;
                throw RejectedExecutionError("thread pool queue is full");
            case RejectPolicy::CallerRuns:
                ++submitted_;
                run_inline = true;
                break;
            case RejectPolicy::Discard:
                ++rejected_;
                return false;
            case RejectPolicy::DiscardOldest:
                discarded = std::move(tasks_.front());
                tasks_.pop_front();
                ++dropped_;
                tasks_.push_back(std::move(task));
                ++submitted_;
                max_queued_ = std::max(max_queued_,
                                       static_cast<std::uint64_t>(tasks_.size()));
                not_empty_.notify_one();
                return true;
            }
        }

        // 关键：CallerRuns 必须在释放队列锁之后执行。否则任务里如果再提交
        // 任务、或者任务执行很久，会把整个池的提交路径锁死。
        if (run_inline) {
            bool failed = false;
            try {
                task();
            } catch (...) {
                failed = true;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            ++completed_;
            if (failed) {
                ++failed_;
            }
        }
        return true;
    }

    // 提交一个可获取结果/异常的任务。被策略丢弃时主动抛异常，避免返回一个
    // 永远不会 ready 的 future（broken promise 只会在析构时暴露，很难排查）。
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

        auto bound = [fn = std::forward<F>(f),
                      tuple = std::make_tuple(std::forward<Args>(args)...)]() mutable -> R {
            // 任务只执行一次，因此可以安全地把捕获的值 move 进可调用对象。
            return std::apply(std::move(fn), std::move(tuple));
        };

        auto task = std::make_shared<std::packaged_task<R()>>(std::move(bound));
        auto future = task->get_future();

        const bool accepted = execute([task]() mutable { (*task)(); });
        if (!accepted) {
            throw RejectedExecutionError("task was discarded by rejection policy");
        }
        return future;
    }

    // 幂等关闭。第一次调用的 mode 生效；后续调用会等待第一次关闭完成。
    // 禁止在 worker 内部调用：join 自己会死锁/抛异常。
    void shutdown(ShutdownMode mode = ShutdownMode::Drain) {
        if (tls_worker_pool_ == this) {
            throw std::logic_error(
                "BoundedThreadPool::shutdown must not be called from its worker");
        }

        std::call_once(shutdown_once_, [this, mode] {
            std::deque<Task> pending;  // 同样在锁外析构
            {
                std::lock_guard<std::mutex> lock(mutex_);
                accepting_ = false;
                stopping_ = true;
                if (mode == ShutdownMode::CancelPending) {
                    pending.swap(tasks_);
                    dropped_ += static_cast<std::uint64_t>(pending.size());
                }
                // 唤醒所有 worker：Drain 模式让它们继续清空队列，CancelPending
                // 模式让它们看到空队列后退出。
                not_empty_.notify_all();
            }

            pending.clear();

            for (auto& worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
        });
    }

    [[nodiscard]] Stats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return Stats{
            submitted_,
            completed_,
            failed_,
            rejected_,
            dropped_,
            max_queued_,
            tasks_.size(),
            active_,
            workers_.size(),
        };
    }

    [[nodiscard]] RejectPolicy policy() const noexcept { return policy_; }

private:
    using Task = std::function<void()>;

    void worker_loop() {
        tls_worker_pool_ = this;

        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                not_empty_.wait(lock, [this] {
                    return stopping_ || !tasks_.empty();
                });

                if (tasks_.empty()) {
                    // 只有 stopping_ 才能让空队列成为退出条件；Drain 模式下
                    // 队列非空时仍然会继续取任务。
                    if (stopping_) {
                        break;
                    }
                    continue;
                }

                task = std::move(tasks_.front());
                tasks_.pop_front();
                ++active_;
            }

            bool failed = false;
            try {
                task();
            } catch (...) {
                // 任务异常必须在这里终止：让异常逃出线程函数会触发
                // std::terminate，整个进程退出。submit 的异常已经被
                // packaged_task 捕获到 future 中，不会走到这里。
                failed = true;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                --active_;
                ++completed_;
                if (failed) {
                    ++failed_;
                }
            }
        }

        tls_worker_pool_ = nullptr;
    }

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::deque<Task> tasks_;

    const std::size_t capacity_;
    const RejectPolicy policy_;

    bool accepting_ = true;
    bool stopping_ = false;

    std::uint64_t submitted_ = 0;
    std::uint64_t completed_ = 0;
    std::uint64_t failed_ = 0;
    std::uint64_t rejected_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t max_queued_ = 0;
    std::size_t active_ = 0;

    std::vector<std::thread> workers_;
    std::once_flag shutdown_once_;

    // 每个 worker 线程记录自己所属的池，用于阻止 worker 内 shutdown 自己。
    inline static thread_local const BoundedThreadPool* tls_worker_pool_ = nullptr;
};

} // namespace archdesign
