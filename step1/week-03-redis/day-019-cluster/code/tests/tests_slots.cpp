// tests_slots.cpp
//
// 槽计算、hash tag、槽映射、CROSSSLOT、CLUSTERDOWN。

#include <string>

#include "test_support.hpp"

namespace archdesign::test {

void test_slot_hash() {
    check(crc16("123456789") == 0x31C3, "CRC16 XMODEM known vector");
    check((key_hash_slot("{user1000}.profile") ==
           (crc16("user1000") & 0x3FFF)),
          "hash tag uses only the content inside {}");
    check(key_hash_slot("{user1000}.profile") == key_hash_slot("{user1000}.orders"),
          "same hash tag -> same slot");
    check(key_hash_slot("{}foo") == (crc16("{}foo") & 0x3FFF),
          "empty hash tag hashes the whole key");
    check(key_hash_slot("foo{bar") == (crc16("foo{bar") & 0x3FFF),
          "unclosed brace hashes the whole key");
}

void test_slot_map() {
    Cluster cluster = three_masters();
    check(cluster.slots().slot_count() == 16384, "cluster has 16384 slots");
    check(cluster.slots().assigned_count() == 16384, "all slots assigned");
    check(cluster.healthy(), "three complete masters -> healthy");
    check(cluster.slots().owner(0) == "M1", "slot 0 -> M1");
    check(cluster.slots().owner(5461) == "M2", "slot 5461 -> M2");
    check(cluster.slots().owner(16383) == "M3", "slot 16383 -> M3");
}

void test_crossslot() {
    Cluster cluster = three_masters();
    const std::string key1 = "{a}one";
    const std::string key2 = "{b}two";
    check(key_hash_slot(key1) != key_hash_slot(key2), "different tags -> different slots");

    const NodeId receiving = cluster.slots().owner(key_hash_slot(key1));
    const Reply reply = cluster.execute(receiving, Command{{"MGET", key1, key2}});
    check(reply.type == ReplyType::CrossSlot, "different slots -> CROSSSLOT");

    const std::string same1 = "{same}one";
    const std::string same2 = "{same}two";
    const Reply ok = cluster.execute(cluster.slots().owner(key_hash_slot(same1)),
                                     Command{{"MGET", same1, same2}});
    check(ok.type == ReplyType::Ok, "same tag -> multikey accepted");
}

void test_clusterdown() {
    Cluster cluster;
    cluster.add_master("M1");
    cluster.add_master("M2");
    cluster.assign_range(0, 100, "M1");
    cluster.assign_range(101, 200, "M2");
    check(!cluster.healthy(), "incomplete slot coverage");
    const Reply reply = cluster.execute("M1", Command{{"SET", "k", "v"}});
    check(reply.type == ReplyType::ClusterDown, "uncovered cluster -> CLUSTERDOWN");
}

}  // namespace archdesign::test
