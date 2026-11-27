// bounded_ring_buffer.hpp
//
// Day 06 的教学实现：有界无锁环形队列。
//
// 说明：这不是 LMAX Disruptor 的完整实现（没有 Sequencer、SequenceBarrier、
// 消费者依赖图/WorkerPool），而是把 Disruptor 最核心的机械 sympathy
// 单独拆出来：
//   - 2 的幂容量 + 位掩码取模；
//   - 每个 slot 一个 sequence，生产者/消费者用 CAS 推进位置；
//   - release/acquire 建立“数据先写、sequence 后发布”的可见性；
//   - cache line padding 避免生产/消费位置互相踩 cache line；
//   - 有界，满了由调用方决定自旋、丢弃还是阻塞。
//
// 算法采用 Dmitry Vyukov 的 bounded MPMC queue 结构，支持多生产者/多消费者。
// 对日志这种 MPSC 场景，可以把消费者数量限制为 1。

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace archdesign {

template <class T>
class BoundedRingBuffer {
    static_assert(std::is_default_constructible_v<T>,
                  "T must be default constructible");
    static_assert(std::is_nothrow_move_assignable_v<T>,
                  "T must be nothrow move assignable");

public:
    static constexpr std::size_t kCacheLine = 64;

    explicit BoundedRingBuffer(std::size_t capacity)
        : capacity_(capacity),
          mask_(capacity - 1),
          cells_(capacity) {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument(
                "capacity must be a power of two and at least 2");
        }

        for (std::size_t i = 0; i < capacity_; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    BoundedRingBuffer(const BoundedRingBuffer&) = delete;
    BoundedRingBuffer& operator=(const BoundedRingBuffer&) = delete;

    // 非阻塞入队。队列满/已关闭返回 false。
    bool try_push(T value) {
        return try_push_impl(value);
    }

    // 自旋等待空位；队列关闭时返回 false。
    bool push(T value) {
        for (;;) {
            if (try_push_impl(value)) {
                return true;
            }
            if (closed_.load(std::memory_order_acquire)) {
                return false;
            }
            std::this_thread::yield();
        }
    }

    // 非阻塞出队。队列空返回 false。
    bool try_pop(T& out) {
        std::size_t pos = dequeue_pos_.value.load(std::memory_order_relaxed);

        for (;;) {
            Cell& cell = cells_[pos & mask_];
            const std::size_t seq =
                cell.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) -
                              static_cast<std::ptrdiff_t>(pos + 1);

            if (diff == 0) {
                if (dequeue_pos_.value.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) {
                    out = std::move(cell.data);
                    cell.sequence.store(pos + capacity_,
                                        std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;  // 没有已发布的数据
            } else {
                pos = dequeue_pos_.value.load(std::memory_order_relaxed);
            }
        }
    }

    // 自旋等待数据；close() 且队列为空时返回 false。
    bool pop(T& out) {
        for (;;) {
            if (try_pop(out)) {
                return true;
            }
            if (closed_.load(std::memory_order_acquire) && empty()) {
                return false;
            }
            std::this_thread::yield();
        }
    }

    void close() {
        closed_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool closed() const {
        return closed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool empty() const {
        return size_approx() == 0;
    }

    [[nodiscard]] bool full() const {
        return size_approx() >= capacity_;
    }

    [[nodiscard]] std::size_t size_approx() const {
        const auto enqueue = enqueue_pos_.value.load(std::memory_order_relaxed);
        const auto dequeue = dequeue_pos_.value.load(std::memory_order_relaxed);
        return static_cast<std::size_t>(enqueue - dequeue);
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

private:
    struct alignas(kCacheLine) PaddedIndex {
        std::atomic<std::size_t> value{0};
        char padding[kCacheLine - sizeof(std::atomic<std::size_t>)]{};
    };

    struct alignas(kCacheLine) Cell {
        std::atomic<std::size_t> sequence{0};
        T data{};
    };

    bool try_push_impl(T& value) {
        if (closed_.load(std::memory_order_acquire)) {
            return false;
        }

        std::size_t pos = enqueue_pos_.value.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[pos & mask_];
            const std::size_t seq =
                cell.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) -
                              static_cast<std::ptrdiff_t>(pos);

            if (diff == 0) {
                if (enqueue_pos_.value.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) {
                    cell.data = std::move(value);
                    // 数据写完后再发布 sequence：消费者 acquire 到这个值，
                    // 才能安全读取 cell.data。
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;  // 队列满
            } else {
                pos = enqueue_pos_.value.load(std::memory_order_relaxed);
            }
        }
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    std::vector<Cell> cells_;

    PaddedIndex enqueue_pos_;
    PaddedIndex dequeue_pos_;
    std::atomic<bool> closed_{false};
};

} // namespace archdesign
