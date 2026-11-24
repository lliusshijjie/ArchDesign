// tests_wait_failover.cpp
//
// WAIT/ACK、master-driven expire、replid2 故障转移，以及大 backlog 的回绕。

#include <string>

#include "resp.hpp"
#include "test_support.hpp"

namespace archdesign::test {

void test_wait_acks() {
    Master master("R1", 512);
    master.set("a", "1", kEpoch);
    Replica replica("replica-1");
    check(replica.connect(master, kEpoch), "connect");
    check(replica.poll(master, kEpoch), "poll");
    replica.ack(master, kEpoch);

    const Offset target = master.offset();
    check(master.count_acks(target) == 1, "one replica has acked target");
    check(master.wait_for_acks(1, target, 100, kEpoch) == 1, "WAIT 1 returns immediately");
}

void test_wait_timeout() {
    Master master("R1", 512);
    master.set("a", "1", kEpoch);
    Replica replica("replica-1");
    check(replica.connect(master, kEpoch), "connect");
    replica.ack(master, kEpoch);

    master.set("b", "2", kEpoch);
    const Offset target = master.offset();
    check(master.count_acks(target) == 0, "replica has not acked the new write");
    check(master.wait_for_acks(1, target, 10, kEpoch) == 0, "WAIT times out with zero acks");
}

void test_failover_replid2() {
    Master old_master("R1", 1024);
    old_master.set("a", "1", kEpoch);
    old_master.set("b", "2", kEpoch);

    Replica promoted_replica("replica-b");
    Replica peer("replica-c");
    check(promoted_replica.connect(old_master, kEpoch), "B full sync");
    check(peer.connect(old_master, kEpoch), "C full sync");

    old_master.set("c", "3", kEpoch);
    check(promoted_replica.poll(old_master, kEpoch), "B poll c");
    check(peer.poll(old_master, kEpoch), "C poll c");

    peer.disconnect();
    old_master.set("d", "4", kEpoch);
    check(promoted_replica.poll(old_master, kEpoch), "B poll d");
    check(promoted_replica.kv().raw("d").has_value(), "B has d before promotion");

    // B 被提升：新 replid=R2，旧 replid=R1 作为 replid2，有效期到 B 的 offset+1。
    Master promoted("R2", "R1", promoted_replica.offset() + 1, 1024);
    promoted.seed_history(promoted_replica.offset(), old_master.backlog().start(),
                          old_master.backlog().read_from(old_master.backlog().start()),
                          promoted_replica.kv());

    check(peer.connect(promoted, kEpoch), "C reconnects to promoted master");
    check(peer.replid() == "R2", "C inherits the new replication history");
    require_same_kv(promoted.kv(), peer.kv(), kEpoch, "replid2 failover");
    check(peer.kv().raw("d").has_value(), "C catches up the write it missed");
}

void test_master_driven_expire() {
    Master master("R1", 256);
    master.set("k", "v", kEpoch, 100);
    Replica replica("replica-1");
    check(replica.connect(master, kEpoch), "connect");

    const i64 later = kEpoch + 1'000;
    check(!replica.kv().get("k", later).has_value(), "replica read hides expired key");
    check(replica.kv().raw("k").has_value(), "replica has not deleted it");
    check(master.expire_due(later) == 1, "master expires and synthesizes DEL");
    check(replica.poll(master, later), "replica receives DEL");
    check(!replica.kv().raw("k").has_value(), "replica deletes only after master DEL");
}

void test_backlog_wrap() {
    Master master("R1", 32);
    for (int i = 0; i < 100; ++i) {
        master.set("key-" + std::to_string(i), "value-" + std::to_string(i), kEpoch);
    }
    check(master.backlog().size() <= 32, "backlog is capped");
    check(master.backlog().start() > 0, "old bytes were trimmed");

    Replica replica("replica-1");
    check(replica.connect(master, kEpoch), "full sync after wrap");
    require_same_kv(master.kv(), replica.kv(), kEpoch, "wrapped backlog");
}

void test_command_stream() {
    const Command command{{"SET", "k", "v"}};
    const std::string bytes = resp_encode(command);
    const RespParseResult parsed = resp_parse(bytes);
    check(parsed.commands.size() == 1 && !parsed.truncated, "one complete command");
    check(parsed.consumed == bytes.size(), "consumed all bytes");

    Master master("R1", 128);
    master.set("k", "v", kEpoch);
    check(master.offset() == static_cast<Offset>(bytes.size()),
          "offset is byte-based, not command count");
    const PsyncResult at_end = master.psync("R1", master.offset(), kEpoch);
    check(at_end.type == PsyncResult::Type::Continue, "PSYNC at end is valid");
    check(at_end.payload.empty(), "nothing to send at end");
}

}  // namespace archdesign::test
