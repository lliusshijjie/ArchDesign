// tests_failover.cpp
//
// gossip 故障举报、quorum、epoch 投票、提升后槽接管、副本数据。

#include <string>

#include "test_support.hpp"

namespace archdesign::test {

void test_gossip_quorum() {
    Cluster cluster = three_masters();
    cluster.report_failure("M2", "M1");
    check(!cluster.failed("M1"), "one report is not quorum");
    cluster.report_failure("M3", "M1");
    check(cluster.failed("M1"), "two reports from three masters -> FAIL");
}

void test_failover() {
    Cluster cluster = three_masters();
    const std::string key = key_owned_by(cluster, "M1");
    const Slot slot = key_hash_slot(key);
    cluster.node("M1")->data[key] = "value";
    cluster.add_replica("R1", "M1");
    cluster.sync_replica("R1");

    cluster.report_failure("M2", "M1");
    cluster.report_failure("M3", "M1");
    const Epoch epoch = cluster.begin_failover("R1");
    check(epoch == 1, "failover epoch increments");
    check(cluster.grant_vote("M2", "R1"), "M2 votes");
    check(cluster.grant_vote("M3", "R1"), "M3 votes");
    check(cluster.votes("R1") == 2, "two votes");
    check(cluster.promote("R1"), "quorum promotes replica");

    check(cluster.node("R1")->master, "R1 is now a master");
    check(!cluster.node("M1")->master, "old master is demoted");
    check(cluster.slots().owner(slot) == "R1", "slots are taken over");
    check(cluster.healthy(), "coverage restored after failover");
    const Reply reply = cluster.execute("R1", Command{{"GET", key}});
    check(reply.type == ReplyType::Ok && reply.value == "value", "promoted replica serves data");
}

void test_failover_no_quorum() {
    Cluster cluster = three_masters();
    cluster.add_replica("R1", "M1");
    cluster.report_failure("M2", "M1");
    cluster.report_failure("M3", "M1");
    cluster.begin_failover("R1");
    check(cluster.grant_vote("M2", "R1"), "one vote");
    check(!cluster.promote("R1"), "one vote is not quorum");
    check(!cluster.healthy(), "failed master still leaves slots uncovered");
}

void test_epoch_vote_once() {
    Cluster cluster = three_masters();
    cluster.add_replica("R1", "M1");
    cluster.report_failure("M2", "M1");
    cluster.report_failure("M3", "M1");
    cluster.begin_failover("R1");

    check(cluster.grant_vote("M2", "R1"), "first vote in epoch");
    check(!cluster.grant_vote("M2", "R1"), "same voter cannot vote twice in one epoch");
    check(cluster.votes("R1") == 1, "one vote counted");

    cluster.begin_failover("R1");
    check(cluster.grant_vote("M2", "R1"), "new epoch allows the voter to vote again");
    check(cluster.votes("R1") == 1, "votes are reset for the new epoch");
}

void test_replica_data() {
    Cluster cluster = three_masters();
    const std::string key = key_owned_by(cluster, "M1");
    cluster.node("M1")->data[key] = "replicated";
    cluster.add_replica("R1", "M1");
    cluster.sync_replica("R1");
    check(cluster.node("R1")->data[key] == "replicated", "replica receives master data");
}

}  // namespace archdesign::test
