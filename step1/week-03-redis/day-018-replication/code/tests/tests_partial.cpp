// tests_partial.cpp
//
// 断线续传：offset 在 backlog 内 -> +CONTINUE；超出/ID 不匹配 -> FULLRESYNC。

#include <string>

#include "test_support.hpp"

namespace archdesign::test {

void test_partial_resync() {
    Master master("R1", 512);
    master.set("a", "1", kEpoch);

    Replica replica("replica-1");
    check(replica.connect(master, kEpoch), "initial full sync");
    master.set("b", "2", kEpoch);
    check(replica.poll(master, kEpoch), "replica sees b");
    replica.disconnect();

    master.set("c", "3", kEpoch);
    const Offset before = replica.offset();
    check(replica.connect(master, kEpoch), "reconnect");
    check(replica.offset() > before, "replica catches up after reconnect");
    require_same_kv(master.kv(), replica.kv(), kEpoch, "partial resync");
    check(replica.kv().raw("c").has_value(), "offline write is recovered");
}

void test_backlog_overflow() {
    Master master("R1", 64);
    Replica replica("replica-1");
    check(replica.connect(master, kEpoch), "initial full sync");
    replica.disconnect();

    // 让离线期间产生的复制流超过 backlog 容量。
    for (int i = 0; i < 50; ++i) {
        master.set("key-" + std::to_string(i), "value-" + std::to_string(i), kEpoch);
    }
    check(replica.connect(master, kEpoch), "reconnect with overflow");
    require_same_kv(master.kv(), replica.kv(), kEpoch, "backlog overflow full resync");
    check(replica.offset() == master.offset(), "offset resets to full sync point");
}

void test_psync_boundary() {
    Master master("R1", 128);
    master.set("a", "1", kEpoch);
    const Offset end = master.offset();

    const PsyncResult at_end = master.psync("R1", end, kEpoch);
    check(at_end.type == PsyncResult::Type::Continue, "offset == end is a valid Continue");
    check(at_end.payload.empty(), "no bytes to send at end");

    const PsyncResult old = master.psync("R1", end + 1, kEpoch);
    check(old.type == PsyncResult::Type::Full, "offset beyond master forces full sync");
}

void test_wrong_replid() {
    Master master("R1", 128);
    master.set("a", "1", kEpoch);

    Replica replica("replica-1", "R-other");
    check(replica.connect(master, kEpoch), "connect with unknown replid");
    check(replica.replid() == "R1", "full sync replaces replid");
    require_same_kv(master.kv(), replica.kv(), kEpoch, "wrong replid");
}

}  // namespace archdesign::test
