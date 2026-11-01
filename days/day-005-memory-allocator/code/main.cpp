// main.cpp
//
// Day 05 的尺寸分级分配器测试：基本分配、尺寸类、large fallback、
// 多线程竞争和统计。

#include "size_class_allocator.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using archdesign::SizeClassAllocator;
using namespace std::chrono_literals;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_basic() {
    auto& allocator = SizeClassAllocator::instance();
    const auto before = allocator.stats();

    const std::vector<std::size_t> sizes = {0, 1, 15, 16, 17, 63, 64, 100, 1024, 4096};
    std::vector<void*> pointers;
    pointers.reserve(sizes.size());

    for (const auto size : sizes) {
        void* pointer = allocator.allocate(size);
        check(pointer != nullptr, "allocation should not return null");
        check(reinterpret_cast<std::uintptr_t>(pointer) % 16 == 0,
              "allocation should be 16-byte aligned");

        const auto bytes = std::max<std::size_t>(size, 1);
        std::memset(pointer, 0xAB, bytes);
        pointers.push_back(pointer);
    }

    for (void* pointer : pointers) {
        allocator.deallocate(pointer);
    }

    const auto after = allocator.stats();
    check(after.active == 0, "all blocks should be released");
    check(after.allocations - before.allocations == sizes.size(),
          "allocation count should match");
    check(after.deallocations - before.deallocations == sizes.size(),
          "deallocation count should match");
}

void test_size_classes_and_reuse() {
    auto& allocator = SizeClassAllocator::instance();
    const auto before = allocator.stats();

    constexpr int count = 256;
    std::vector<void*> pointers;
    pointers.reserve(count);

    for (int i = 0; i < count; ++i) {
        void* pointer = allocator.allocate(64);
        std::memset(pointer, i & 0xFF, 64);
        pointers.push_back(pointer);
    }

    for (void* pointer : pointers) {
        allocator.deallocate(pointer);
    }

    // 第二轮应复用线程缓存/central free list，而不是每次都向 OS 申请。
    for (int i = 0; i < count; ++i) {
        void* pointer = allocator.allocate(64);
        check(pointer != nullptr, "reused allocation should not be null");
        pointers[i] = pointer;
    }
    for (void* pointer : pointers) {
        allocator.deallocate(pointer);
    }

    const auto after = allocator.stats();
    check(after.active == 0, "all size-class blocks should be released");
    check(after.allocations - before.allocations == count * 2,
          "two rounds of allocations should be counted");
}

void test_large_allocation() {
    auto& allocator = SizeClassAllocator::instance();
    const auto before = allocator.stats();

    constexpr std::size_t size = 128 * 1024;
    auto* pointer = static_cast<unsigned char*>(allocator.allocate(size));
    check(pointer != nullptr, "large allocation should not return null");

    for (std::size_t i = 0; i < size; i += 4096) {
        pointer[i] = static_cast<unsigned char>(i / 4096);
    }
    for (std::size_t i = 0; i < size; i += 4096) {
        check(pointer[i] == static_cast<unsigned char>(i / 4096),
              "large allocation content should be stable");
    }

    allocator.deallocate(pointer);

    const auto after = allocator.stats();
    check(after.active == 0, "large block should be released");
    check(after.large_allocations - before.large_allocations == 1,
          "large allocation should be counted");
}

void test_multithreaded() {
    auto& allocator = SizeClassAllocator::instance();
    const auto before = allocator.stats();

    constexpr int thread_count = 4;
    constexpr int iterations = 4000;

    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(0xC0FFEEu + static_cast<unsigned>(t));
            std::uniform_int_distribution<std::size_t> dist(1, 4096);
            std::vector<void*> live;

            for (int i = 0; i < iterations; ++i) {
                const auto size = dist(rng);
                void* pointer = allocator.allocate(size);
                if (pointer == nullptr) {
                    ++errors;
                    return;
                }
                auto* bytes = static_cast<unsigned char*>(pointer);
                bytes[0] = static_cast<unsigned char>(t);
                bytes[size - 1] = static_cast<unsigned char>(i);
                live.push_back(pointer);

                if (live.size() >= 64) {
                    allocator.deallocate(live.front());
                    live.erase(live.begin());
                }
            }

            for (void* pointer : live) {
                allocator.deallocate(pointer);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    check(errors.load() == 0, "all concurrent allocations should succeed");
    const auto after = allocator.stats();
    check(after.active == 0, "all concurrent blocks should be released");
    check(after.allocations - before.allocations ==
              static_cast<std::uint64_t>(thread_count) * iterations,
          "all concurrent allocations should be counted");
    check(after.deallocations - before.deallocations ==
              static_cast<std::uint64_t>(thread_count) * iterations,
          "all concurrent deallocations should be counted");
}

void test_stats() {
    auto& allocator = SizeClassAllocator::instance();

    const auto before = allocator.stats();
    void* a = allocator.allocate(32);
    void* b = allocator.allocate(33);
    void* c = allocator.allocate(8192);

    auto during = allocator.stats();
    check(during.active == before.active + 3, "three blocks should be active");
    check(during.large_allocations >= before.large_allocations + 1,
          "large allocation should be counted");

    allocator.deallocate(a);
    allocator.deallocate(b);
    allocator.deallocate(c);

    const auto after = allocator.stats();
    check(after.active == before.active, "all blocks should be released");
    check(after.allocations - before.allocations == 3,
          "three allocations should be counted");
}

void benchmark() {
    // 只是教学用的单线程微基准：真实项目请用 perf + 多线程 + 真实负载。
    constexpr int iterations = 200000;
    auto& allocator = SizeClassAllocator::instance();

    volatile void* sink = nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        void* pointer = allocator.allocate(64);
        sink = pointer;
        allocator.deallocate(pointer);
    }
    const auto t1 = std::chrono::steady_clock::now();

    const auto allocator_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() /
        iterations;

    const auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        void* pointer = ::operator new(64);
        sink = pointer;
        ::operator delete(pointer);
    }
    const auto t3 = std::chrono::steady_clock::now();

    const auto new_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count() /
        iterations;

    std::cout << "micro-benchmark (single thread, 64 B alloc+free):\n"
              << "  size-class allocator: " << allocator_ns << " ns/op\n"
              << "  operator new/delete:  " << new_ns << " ns/op\n";
    (void)sink;
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"basic", test_basic},
        {"size_classes_and_reuse", test_size_classes_and_reuse},
        {"large_allocation", test_large_allocation},
        {"multithreaded", test_multithreaded},
        {"stats", test_stats},
    };

    int failed = 0;
    for (const auto& [name, run] : tests) {
        try {
            run();
            std::cout << "[  OK  ] " << name << '\n' << std::flush;
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAILED] " << name << ": " << error.what() << '\n' << std::flush;
        } catch (...) {
            ++failed;
            std::cerr << "[FAILED] " << name << ": unknown exception\n" << std::flush;
        }
    }

    if (failed != 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }

    std::cout << "\nAll " << tests.size() << " tests passed.\n";
    benchmark();
    return 0;
}
