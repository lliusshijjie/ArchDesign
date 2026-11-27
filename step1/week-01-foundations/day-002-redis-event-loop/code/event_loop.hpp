// event_loop.hpp
//
// Day 02 的教学实现：单线程 Reactor / 事件循环。
//
// 对应 Redis `ae.c` 的几个核心抽象：
//   - Poller      ≈ aeApiPoll（epoll_wait / kqueue / select）
//   - FileEvent   ≈ aeFileEvent（fd + 读写 mask + 回调）
//   - TimerEvent  ≈ aeTimeEvent（when + callback + 是否续期）
//   - beforeSleep ≈ beforesleep（poll 前处理延迟工作）
//
// 为了让测试不依赖平台 socket，本实现使用可注入就绪事件的 MockPoller；
// 真实实现只需把 Poller::wait 换成 epoll_wait/kqueue/select，并维护
// fd -> mask 的注册表。事件循环本体不需要改。

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace archdesign {

using Clock = std::chrono::steady_clock;
using Duration = Clock::duration;
using TimePoint = Clock::time_point;

inline constexpr std::uint32_t kReadable = 1u << 0;
inline constexpr std::uint32_t kWritable = 1u << 1;
inline constexpr std::uint32_t kAllEvents = kReadable | kWritable;

struct ReadyEvent {
    int fd = -1;
    std::uint32_t mask = 0;
};

// 对应 aeApiPoll 的抽象。生产实现可以是 epoll/kqueue/select；
// 这里用接口把事件循环和 OS 解耦，便于确定性地测试调度逻辑。
class Poller {
public:
    virtual ~Poller() = default;

    virtual void add(int fd, std::uint32_t mask) = 0;
    virtual void remove(int fd) = 0;
    virtual std::vector<ReadyEvent> wait(std::optional<Duration> timeout) = 0;
};

// 测试用 Poller：不阻塞、不碰 socket，由测试注入“哪些 fd 就绪”。
class MockPoller final : public Poller {
public:
    void add(int fd, std::uint32_t mask) override {
        masks_[fd] = mask;
    }

    void remove(int fd) override {
        masks_.erase(fd);
    }

    std::vector<ReadyEvent> wait(std::optional<Duration> timeout) override {
        last_timeout_ = timeout;
        auto ready = std::move(ready_);
        ready_.clear();
        return ready;
    }

    void inject(int fd, std::uint32_t mask) {
        auto it = masks_.find(fd);
        if (it == masks_.end()) {
            return;
        }
        const auto effective = mask & it->second;
        if (effective != 0) {
            ready_.push_back(ReadyEvent{fd, effective});
        }
    }

    [[nodiscard]] bool registered(int fd) const {
        return masks_.find(fd) != masks_.end();
    }

    [[nodiscard]] std::optional<Duration> last_timeout() const {
        return last_timeout_;
    }

private:
    std::unordered_map<int, std::uint32_t> masks_;
    std::vector<ReadyEvent> ready_;
    std::optional<Duration> last_timeout_;
};

class EventLoop {
public:
    using FileCallback = std::function<void(int fd, std::uint32_t mask)>;
    // 返回 nullopt 表示一次性定时器；返回 Duration 表示在本次触发后，
    // 以“当前时间 + 返回值”为下一次触发时间继续。
    using TimerCallback = std::function<std::optional<Duration>()>;
    using SleepCallback = std::function<void()>;

    explicit EventLoop(std::unique_ptr<Poller> poller)
        : poller_(std::move(poller)) {
        if (!poller_) {
            throw std::invalid_argument("poller must not be null");
        }
    }

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void add_file(int fd,
                  std::uint32_t mask,
                  FileCallback read_callback,
                  FileCallback write_callback,
                  bool barrier = false) {
        if (fd < 0) {
            throw std::invalid_argument("fd must be non-negative");
        }
        if (mask == 0 || (mask & ~kAllEvents) != 0) {
            throw std::invalid_argument("invalid file event mask");
        }
        if ((mask & kReadable) != 0 && !read_callback) {
            throw std::invalid_argument("read callback is required");
        }
        if ((mask & kWritable) != 0 && !write_callback) {
            throw std::invalid_argument("write callback is required");
        }
        if (files_.find(fd) != files_.end()) {
            throw std::logic_error("fd is already registered");
        }

        poller_->add(fd, mask);
        files_.emplace(fd, FileHandler{mask,
                                       std::move(read_callback),
                                       std::move(write_callback),
                                       barrier});
    }

    bool remove_file(int fd) {
        auto it = files_.find(fd);
        if (it == files_.end()) {
            return false;
        }
        poller_->remove(fd);
        files_.erase(it);
        return true;
    }

    std::uint64_t add_timer(Duration delay, TimerCallback callback) {
        if (!callback) {
            throw std::invalid_argument("timer callback is required");
        }
        if (delay < Duration::zero()) {
            delay = Duration::zero();
        }

        const auto id = next_timer_id_++;
        const auto when = Clock::now() + delay;
        auto timer = std::make_shared<Timer>(Timer{id, when, std::move(callback)});
        timers_.emplace(id, timer);
        timer_heap_.push(TimerRef{when, id});
        return id;
    }

    bool cancel_timer(std::uint64_t id) {
        return timers_.erase(id) != 0;
    }

    void set_before_sleep(SleepCallback callback) {
        before_sleep_ = std::move(callback);
    }

    void set_after_sleep(SleepCallback callback) {
        after_sleep_ = std::move(callback);
    }

    // 执行一轮：beforeSleep -> 计算 timeout -> poll -> afterSleep
    // -> 文件事件 -> 时间事件。顺序与 Redis aeProcessEvents 对齐。
    void run_once() {
        if (stopped_) {
            return;
        }

        if (before_sleep_) {
            before_sleep_();
        }
        if (stopped_) {
            return;
        }

        // 注意：Redis 在 beforeSleep 之前计算 timeout；这里放在之后，
        // 这样 beforeSleep 新增的 timer 也能缩短本次等待。差异会在文中说明。
        const auto timeout = next_timeout();
        auto ready = poller_->wait(timeout);

        if (after_sleep_) {
            after_sleep_();
        }

        for (const auto& event : ready) {
            dispatch_file_event(event);
        }

        process_timers();
    }

    void run() {
        while (!stopped_) {
            run_once();
        }
    }

    void stop() {
        stopped_ = true;
    }

    [[nodiscard]] bool stopped() const {
        return stopped_;
    }

    [[nodiscard]] std::size_t file_count() const {
        return files_.size();
    }

    [[nodiscard]] std::size_t timer_count() const {
        return timers_.size();
    }

private:
    struct FileHandler {
        std::uint32_t mask;
        FileCallback read_callback;
        FileCallback write_callback;
        bool barrier;
    };

    struct Timer {
        std::uint64_t id;
        TimePoint when;
        TimerCallback callback;
    };

    struct TimerRef {
        TimePoint when;
        std::uint64_t id;

        bool operator>(const TimerRef& other) const {
            if (when != other.when) {
                return when > other.when;
            }
            return id > other.id;
        }
    };

    void discard_stale_timer_refs() {
        while (!timer_heap_.empty()) {
            const auto ref = timer_heap_.top();
            const auto it = timers_.find(ref.id);
            if (it != timers_.end() && it->second->when == ref.when) {
                break;
            }
            timer_heap_.pop();
        }
    }

    std::optional<Duration> next_timeout() {
        discard_stale_timer_refs();
        if (timer_heap_.empty()) {
            return std::nullopt;
        }
        const auto when = timer_heap_.top().when;
        const auto now = Clock::now();
        if (when <= now) {
            return Duration::zero();
        }
        return when - now;
    }

    void dispatch_file_event(const ReadyEvent& event) {
        auto it = files_.find(event.fd);
        if (it == files_.end()) {
            return;
        }

        // 拷贝一份 handler：回调里可能 remove_file/add_file，不能继续持有
        // unordered_map 的引用。
        const auto handler = it->second;
        const bool want_read = (handler.mask & event.mask & kReadable) != 0;
        const bool want_write = (handler.mask & event.mask & kWritable) != 0;

        if (handler.barrier) {
            // AE_BARRIER：先写后读。
            if (want_write && handler.write_callback) {
                handler.write_callback(event.fd, event.mask);
            }
            if (want_read && handler.read_callback) {
                handler.read_callback(event.fd, event.mask);
            }
        } else {
            if (want_read && handler.read_callback) {
                handler.read_callback(event.fd, event.mask);
            }
            if (want_write && handler.write_callback) {
                handler.write_callback(event.fd, event.mask);
            }
        }
    }

    void process_timers() {
        const auto now = Clock::now();

        for (;;) {
            discard_stale_timer_refs();
            if (timer_heap_.empty()) {
                break;
            }

            const auto ref = timer_heap_.top();
            if (ref.when > now) {
                break;
            }
            timer_heap_.pop();

            auto it = timers_.find(ref.id);
            if (it == timers_.end() || it->second->when != ref.when) {
                continue;  // 被取消，或已被续期产生的旧堆项
            }

            auto timer = it->second;
            timers_.erase(it);

            const auto next = timer->callback();
            if (next.has_value()) {
                // 防止返回 0 导致同一轮循环里无限触发；Redis 的 serverCron
                // 返回固定的 1000/hz，不会走这个边界。
                const auto delay =
                    std::max<Duration>(*next, std::chrono::milliseconds(1));
                timer->when = Clock::now() + delay;
                timers_.emplace(timer->id, timer);
                timer_heap_.push(TimerRef{timer->when, timer->id});
            }

            if (stopped_) {
                break;
            }
        }
    }

    std::unique_ptr<Poller> poller_;
    std::unordered_map<int, FileHandler> files_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Timer>> timers_;
    std::priority_queue<TimerRef, std::vector<TimerRef>, std::greater<TimerRef>>
        timer_heap_;

    std::uint64_t next_timer_id_ = 1;
    SleepCallback before_sleep_;
    SleepCallback after_sleep_;
    bool stopped_ = false;
};

} // namespace archdesign
