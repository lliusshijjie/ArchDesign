// hashed_wheel_timer.hpp
//
// Day 03 的教学实现：单层哈希时间轮（Hashed Wheel Timer）。
//
// 对应 Netty HashedWheelTimer 的核心思想：
//   - 固定 tick 时长和 wheelSize（2 的幂）；
//   - 每个 bucket 是一条任务链表；
//   - 任务按 deadline 放入 bucket，并用 rounds 跳过整轮；
//   - 取消是 O(1) 的 lazy cancellation；
//   - 单个 worker 线程按 tick 推进并批量执行到期任务。
//
// 与 Netty 的差异：
//   - Netty 用 remainingRounds 的经典公式；本实现从“下一次访问该 bucket 的
//     tick”推导 rounds，避免在边界处多等/少等一轮；
//   - 这里只做单层轮，不实现 overflow wheel；超长延迟靠 rounds 跨轮，
//     Kafka 的层级时间轮则用多级 wheel 处理超大时间范围。

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace archdesign {

class HashedWheelTimer {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    using Task = std::function<void()>;

    struct Timeout {
        enum class State : std::uint8_t {
            Pending,
            Canceled,
            Executing,
            Done,
        };

        std::uint64_t id = 0;
        std::uint64_t deadline_tick = 0;
        std::uint64_t rounds = 0;
        Task task;
        std::atomic<State> state{State::Pending};
    };

    struct Stats {
        std::uint64_t scheduled = 0;
        std::uint64_t executed = 0;
        std::uint64_t failed = 0;
        std::uint64_t canceled = 0;
        std::uint64_t pending = 0;
    };

    // tick：时间轮的最小推进单位；wheel_size 必须是 2 的幂。
    // max_pending=0 表示不限制；非 0 时用于防止 timer 无限堆积。
    explicit HashedWheelTimer(Duration tick,
                              std::size_t wheel_size,
                              std::uint64_t max_pending = 0)
        : tick_(tick),
          wheel_(wheel_size),
          mask_(wheel_size - 1),
          max_pending_(max_pending),
          next_tick_(Clock::now() + tick) {
        if (tick <= Duration::zero()) {
            throw std::invalid_argument("tick must be positive");
        }
        if (wheel_size == 0 || (wheel_size & (wheel_size - 1)) != 0) {
            throw std::invalid_argument("wheel_size must be a power of two");
        }

        worker_ = std::thread([this] { worker_loop(); });
    }

    ~HashedWheelTimer() {
        try {
            stop();
        } catch (...) {
            // 析构函数不能抛；正常使用不会走到这里。
        }
    }

    HashedWheelTimer(const HashedWheelTimer&) = delete;
    HashedWheelTimer& operator=(const HashedWheelTimer&) = delete;

    std::shared_ptr<Timeout> new_timeout(Duration delay, Task task) {
        if (!task) {
            throw std::invalid_argument("task must not be empty");
        }
        if (delay < Duration::zero()) {
            delay = Duration::zero();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            throw std::runtime_error("timer has been stopped");
        }
        if (max_pending_ != 0 && stats().pending >= max_pending_) {
            throw std::runtime_error("too many pending timers");
        }

        // 向上取整到 tick；延迟为 0 也至少等一个 tick，避免在同一个
        // bucket 的处理过程中插入又立刻执行，语义更可预测。
        const auto rounded = std::chrono::ceil<Duration>(delay);
        const auto ticks = std::max<std::uint64_t>(
            1, static_cast<std::uint64_t>(rounded / tick_));

        const std::uint64_t target = current_tick_ + ticks;
        const std::uint64_t wheel_size = wheel_.size();
        const std::uint64_t bucket = static_cast<std::uint64_t>(target & mask_);
        const std::uint64_t rounds = (ticks - 1) / wheel_size;

        auto timeout = std::make_shared<Timeout>();
        timeout->id = next_id_++;
        timeout->deadline_tick = target;
        timeout->rounds = rounds;
        timeout->task = std::move(task);
        wheel_[bucket].push_back(timeout);
        ++scheduled_;
        return timeout;
    }

    bool cancel(const std::shared_ptr<Timeout>& timeout) {
        if (!timeout) {
            return false;
        }
        auto expected = Timeout::State::Pending;
        if (timeout->state.compare_exchange_strong(expected,
                                                   Timeout::State::Canceled)) {
            ++canceled_;
            return true;
        }
        return false;
    }

    void stop() {
        if (worker_.get_id() == std::this_thread::get_id()) {
            throw std::logic_error("stop() must not be called from timer thread");
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();

        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] Stats stats() const {
        Stats result;
        result.scheduled = scheduled_.load(std::memory_order_relaxed);
        result.executed = executed_.load(std::memory_order_relaxed);
        result.failed = failed_.load(std::memory_order_relaxed);
        result.canceled = canceled_.load(std::memory_order_relaxed);
        const auto done = result.executed + result.canceled;
        result.pending = result.scheduled > done ? result.scheduled - done : 0;
        return result;
    }

    [[nodiscard]] Duration tick() const noexcept {
        return tick_;
    }

    [[nodiscard]] std::size_t wheel_size() const noexcept {
        return wheel_.size();
    }

private:
    void worker_loop() {
        std::unique_lock<std::mutex> lock(mutex_);

        while (!stopping_) {
            if (cv_.wait_until(lock, next_tick_, [this] { return stopping_; })) {
                break;
            }
            if (stopping_) {
                break;
            }

            ++current_tick_;
            auto& bucket = wheel_[current_tick_ & mask_];

            std::vector<std::shared_ptr<Timeout>> due;
            for (auto it = bucket.begin(); it != bucket.end();) {
                auto timeout = *it;

                if (timeout->state.load(std::memory_order_relaxed) ==
                    Timeout::State::Canceled) {
                    it = bucket.erase(it);
                    continue;
                }
                if (timeout->rounds > 0) {
                    --timeout->rounds;
                    ++it;
                    continue;
                }
                if (timeout->deadline_tick <= current_tick_) {
                    due.push_back(timeout);
                    it = bucket.erase(it);
                } else {
                    // 正常情况下不会走到这里；防御 Deadline 计算错误。
                    ++it;
                }
            }

            lock.unlock();
            for (auto& timeout : due) {
                auto expected = Timeout::State::Pending;
                if (!timeout->state.compare_exchange_strong(
                        expected, Timeout::State::Executing)) {
                    // 在出队和执行之间被取消。
                    continue;
                }
                try {
                    timeout->task();
                } catch (...) {
                    ++failed_;
                }
                timeout->state.store(Timeout::State::Done,
                                     std::memory_order_release);
                ++executed_;
            }
            lock.lock();

            // 回调执行可能超过一个 tick：不补跑错过的 tick，避免一次
            // 慢回调引发“定时器风暴”。代价是 timer 会向后漂移。
            const auto now = Clock::now();
            next_tick_ += tick_;
            if (next_tick_ <= now) {
                next_tick_ = now + tick_;
            }
        }
    }

    Duration tick_;
    std::vector<std::list<std::shared_ptr<Timeout>>> wheel_;
    std::size_t mask_;
    std::uint64_t max_pending_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;

    Clock::time_point next_tick_;
    std::uint64_t current_tick_ = 0;
    std::uint64_t next_id_ = 1;
    bool stopping_ = false;

    std::atomic<std::uint64_t> scheduled_{0};
    std::atomic<std::uint64_t> executed_{0};
    std::atomic<std::uint64_t> failed_{0};
    std::atomic<std::uint64_t> canceled_{0};
};

} // namespace archdesign
