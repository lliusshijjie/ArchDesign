// test_support.hpp
//
// 所有测试组共用的小工具：断言、key/value 生成、引擎配置、快照比较。
// 这里不包含任何测试逻辑，避免每个 tests_*.cpp 重复造轮子。

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "bytes.hpp"
#include "common.hpp"
#include "engine.hpp"
#include "resp.hpp"

namespace archdesign::test {

using WallClock = std::chrono::steady_clock;

inline constexpr i64 kEpoch = 1'700'000'000'000LL;  // 2023-11-14T22:13:20Z

void check(bool condition, const std::string& message);

template <class Fn>
void expect_throw(Fn&& fn, const std::string& needle, const std::string& message) {
    try {
        fn();
    } catch (const std::exception& ex) {
        const std::string text = ex.what();
        check(text.find(needle) != std::string::npos,
              message + ": expected '" + needle + "' in '" + text + "'");
        return;
    }
    throw std::runtime_error(message + ": expected an exception");
}

std::string key_of(int index, const char* prefix = "k");
std::string value_of(int index, std::size_t size = 24);

EngineConfig base_config(bool aof_enabled = true, std::size_t pages = 16);
void seed(Engine& engine, int count, i64 now, i64 ttl_ms = -1);

std::optional<Entry> snapshot_key(const Snapshot& snapshot, std::string_view wanted);
void require_same_live(const PageStore& left, const PageStore& right, i64 now,
                       const std::string& context);

struct TestCase {
    const char* name;
    void (*fn)();
};

// 18 个用例的声明；定义分散在 tests_*.cpp，注册表在 test_registry.cpp。
void test_page_store_cow();
void test_rdb_roundtrip();
void test_rdb_ttl();
void test_rdb_checksum_and_truncation();
void test_rdb_bgsave_and_atomic_replace();
void test_aof_resp();
void test_aof_always();
void test_aof_everysec();
void test_aof_no();
void test_aof_truncated_tail();
void test_aof_corrupt();
void test_aof_rewrite();
void test_aof_rewrite_crash_before_commit();
void test_aof_rewrite_with_rdb_preamble();
void test_recovery_precedence();
void test_auto_rewrite();
void test_cow_cost();
void test_model();

const std::vector<TestCase>& test_cases();

void run_demo();
void run_bench();

}  // namespace archdesign::test
