// test_support.hpp
//
// Day 18 测试组共用断言、MiniKV 比较、用例注册表。

#pragma once

#include <string>
#include <vector>

#include "kv.hpp"
#include "master.hpp"
#include "replica.hpp"

namespace archdesign::test {

inline constexpr i64 kEpoch = 1'700'000'000'000LL;

void check(bool condition, const std::string& message);
void require_same_kv(const Kv& left, const Kv& right, i64 now, const std::string& context);

struct TestCase {
    const char* name;
    void (*fn)();
};

void test_full_sync();
void test_incremental();
void test_full_sync_tail();
void test_full_sync_ttl();
void test_partial_resync();
void test_backlog_overflow();
void test_psync_boundary();
void test_wrong_replid();
void test_wait_acks();
void test_wait_timeout();
void test_failover_replid2();
void test_master_driven_expire();
void test_backlog_wrap();
void test_command_stream();

const std::vector<TestCase>& test_cases();

void run_demo();
void run_bench();

}  // namespace archdesign::test
