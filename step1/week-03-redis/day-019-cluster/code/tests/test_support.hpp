// test_support.hpp
//
// Day 19 测试组共用：三主拓扑、按槽找 key、断言、注册表。

#pragma once

#include <string>
#include <vector>

#include "client.hpp"
#include "cluster.hpp"
#include "crc16.hpp"

namespace archdesign::test {

void check(bool condition, const std::string& message);

Cluster three_masters();
std::string key_owned_by(const Cluster& cluster, const std::string& node,
                         const std::string& prefix = "key-");
std::string key_in_range(Slot begin, Slot end, const std::string& prefix = "key-");

struct TestCase {
    const char* name;
    void (*fn)();
};

void test_slot_hash();
void test_slot_map();
void test_moved();
void test_client_cache();
void test_crossslot();
void test_clusterdown();
void test_ask_migration();
void test_tryagain();
void test_ask_target_requires_asking();
void test_gossip_quorum();
void test_failover();
void test_failover_no_quorum();
void test_epoch_vote_once();
void test_replica_data();

const std::vector<TestCase>& test_cases();

void run_demo();
void run_bench();

}  // namespace archdesign::test
