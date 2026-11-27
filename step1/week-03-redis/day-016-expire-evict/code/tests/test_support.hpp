// test_support.hpp
//
// Day 16 测试/演示/基准共用的小工具。测试逻辑分散在 tests_*.cpp，
// 注册表在 test_registry.cpp，避免一个 800 行的 main.cpp。

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "clock.hpp"
#include "common.hpp"
#include "eviction_pool.hpp"
#include "store.hpp"

namespace archdesign::test {

using WallClock = std::chrono::steady_clock;

inline constexpr i64 kEpoch = 1'700'000'000'000LL;

void check(bool condition, const std::string& message);
std::string key_of(int index, const char* prefix = "k");
std::string value_of(std::size_t size);

std::size_t elapsed_ms(WallClock::time_point start);
double as_mb(u64 bytes);
std::string human_bytes(u64 bytes);

struct TestCase {
    const char* name;
    void (*fn)();
};

void test_semantics();
void test_lazy_get();
void test_lazy_write();
void test_active_drain();
void test_active_budget();
void test_active_ratio_gate();
void test_evict_noeviction();
void test_evict_allkeys_lru();
void test_evict_volatile_only();
void test_evict_lfu();
void test_evict_ttl();
void test_replica_hidden();
void test_pool_order();
void test_model();

const std::vector<TestCase>& test_cases();

void run_demo();
void run_bench();

}  // namespace archdesign::test
