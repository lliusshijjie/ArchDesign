// main.cpp
//
// Day 06 的无锁环形队列测试：FIFO、回绕、MPSC、MPMC、关闭和背压。

#include "bounded_ring_buffer.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace std::chrono_literals;
using archdesign::BoundedRingBuffer;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_fifo_and_full() {
    BoundedRingBuffer<std::string> ring(8);

    check(ring.try_push("a"), "a should be accepted");
    check(ring.try_push("b"), "b should be accepted");
    check(ring.try_push("c"), "c should be accepted");

    std::string value;
    check(ring.try_pop(value) && value == "a", "FIFO should return a");
    check(ring.try_pop(value) && value == "b", "FIFO should return b");
    check(ring.try_pop(value) && value == "c", "FIFO should return c");
    check(!ring.try_pop(value), "empty ring should return false");

    for (int i = 0; i < 8; ++i) {
        check(ring.try_push(std::to_string(i)), "fill should succeed");
    }
    check(!ring.try_push("overflow"), "full ring should reject new item");

    for (int i = 0; i < 8; ++i) {
        check(ring.try_pop(value) && value == std::to_string(i),
              "drain should preserve FIFO");
    }
    check(ring.empty(), "ring should be empty after drain");
}

void test_wraparound() {
    BoundedRingBuffer<int> ring(4);

    for (int i = 0; i < 1000; ++i) {
        check(ring.try_push(i), "wraparound push should succeed");
        int value = -1;
        check(ring.try_pop(value), "wraparound pop should succeed");
        check(value == i, "wraparound should preserve order");
    }
}

void test_mpsc() {
    constexpr int producers = 4;
    constexpr int per_producer = 2000;
    BoundedRingBuffer<std::string> ring(1024);

    std::atomic<int> produced{0};
    std::vector<std::thread> producer_threads;
    for (int p = 0; p < producers; ++p) {
        producer_threads.emplace_back([&, p] {
            for (int i = 0; i < per_producer; ++i) {
                std::string value =
                    std::to_string(p) + ":" + std::to_string(i);
                if (!ring.push(std::move(value))) {
                    return;
                }
                ++produced;
            }
        });
    }

    std::vector<std::string> consumed;
    std::thread consumer([&] {
        std::string value;
        while (ring.pop(value)) {
            consumed.push_back(std::move(value));
        }
    });

    for (auto& thread : producer_threads) {
        thread.join();
    }
    ring.close();
    consumer.join();

    check(produced.load() == producers * per_producer,
          "all producers should publish");
    check(consumed.size() == static_cast<std::size_t>(producers * per_producer),
          "consumer should receive all messages");

    std::unordered_set<std::string> unique(consumed.begin(), consumed.end());
    check(unique.size() == consumed.size(), "MPSC messages should be unique");
}

void test_mpmc() {
    constexpr int producers = 4;
    constexpr int consumers = 4;
    constexpr int per_producer = 500;
    BoundedRingBuffer<int> ring(256);

    std::vector<std::thread> producer_threads;
    for (int p = 0; p < producers; ++p) {
        producer_threads.emplace_back([&, p] {
            for (int i = 0; i < per_producer; ++i) {
                ring.push(p * per_producer + i);
            }
        });
    }

    std::mutex mutex;
    std::vector<int> consumed;
    std::vector<std::thread> consumer_threads;
    for (int c = 0; c < consumers; ++c) {
        consumer_threads.emplace_back([&] {
            int value = 0;
            while (ring.pop(value)) {
                std::lock_guard<std::mutex> lock(mutex);
                consumed.push_back(value);
            }
        });
    }

    for (auto& thread : producer_threads) {
        thread.join();
    }
    ring.close();
    for (auto& thread : consumer_threads) {
        thread.join();
    }

    check(consumed.size() ==
              static_cast<std::size_t>(producers * per_producer),
          "MPMC consumers should receive all messages");
    std::unordered_set<int> unique(consumed.begin(), consumed.end());
    check(unique.size() == consumed.size(), "MPMC messages should be unique");
}

void test_backpressure_and_close() {
    BoundedRingBuffer<std::string> ring(2);

    check(ring.try_push("A"), "A should fill slot 0");
    check(ring.try_push("B"), "B should fill slot 1");
    auto blocked = std::async(std::launch::async, [&] {
        return ring.push("C");
    });
    check(blocked.wait_for(50ms) == std::future_status::timeout,
          "push should spin while the ring is full");

    std::string value;
    check(ring.try_pop(value) && value == "A", "pop should make space");
    check(blocked.get(), "blocked push should succeed after space appears");
    check(ring.try_pop(value) && value == "B", "B should be available");
    check(ring.try_pop(value) && value == "C", "C should be available");

    ring.close();
    check(!ring.try_push("D"), "closed ring should reject push");
    check(!ring.pop(value), "closed empty ring should stop pop");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"fifo_and_full", test_fifo_and_full},
        {"wraparound", test_wraparound},
        {"mpsc", test_mpsc},
        {"mpmc", test_mpmc},
        {"backpressure_and_close", test_backpressure_and_close},
    };

    int failed = 0;
    for (const auto& [name, run] : tests) {
        try {
            run();
            std::cout << "[  OK  ] " << name << '\n' << std::flush;
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAILED] " << name << ": " << error.what() << '\n'
                      << std::flush;
        } catch (...) {
            ++failed;
            std::cerr << "[FAILED] " << name << ": unknown exception\n"
                      << std::flush;
        }
    }

    if (failed != 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "\nAll " << tests.size() << " tests passed.\n";
    return 0;
}
