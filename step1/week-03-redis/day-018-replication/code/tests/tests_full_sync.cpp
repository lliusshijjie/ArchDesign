// tests_full_sync.cpp
//
// 全量同步：快照、offset、TTL，以及 full sync 期间追加的 tail。

#include <string>

#include "resp.hpp"
#include "test_support.hpp"

namespace archdesign::test {

void test_full_sync() {
    Master master("R1", 256);
    master.set("a", "1", kEpoch);
    master.set("b", "2", kEpoch);

    Replica replica("replica-1");
    check(replica.connect(master, kEpoch), "replica connects");
    check(replica.replid() == "R1", "replica inherits replid");
    check(replica.offset() == master.offset(), "replica catches up to master offset");
    require_same_kv(master.kv(), replica.kv(), kEpoch, "full sync");
}

void test_incremental() {
    Master master("R1", 256);
    master.set("a", "1", kEpoch);
    Replica replica("replica-1");
    check(replica.connect(master, kEpoch), "initial full sync");
    const Offset before = replica.offset();

    master.set("b", "2", kEpoch);
    master.del("a");
    check(replica.poll(master, kEpoch), "replica polls incremental stream");
    check(replica.offset() > before, "offset advances");
    require_same_kv(master.kv(), replica.kv(), kEpoch, "incremental");
    check(!replica.kv().raw("a").has_value(), "DEL is applied");
}

void test_full_sync_tail() {
    Master master("R1", 256);
    master.set("base", "1", kEpoch);

    const PsyncResult full = master.psync("?", -1, kEpoch);
    check(full.type == PsyncResult::Type::Full, "new replica gets FULLRESYNC");

    // 模拟 RDB 正在传输时，master 继续接收写入。
    master.set("during", "2", kEpoch);
    const std::string tail = master.finish_full_sync(full.full_sync_id);
    check(!tail.empty(), "writes during full sync are buffered");

    Kv replica;
    replica.load(full.payload);
    const RespParseResult parsed = resp_parse(tail);
    check(!parsed.truncated, "tail is a complete command stream");
    for (const Command& command : parsed.commands) replica.apply(command, kEpoch);
    check(replica.get("base", kEpoch).value() == "1", "snapshot has base");
    check(replica.get("during", kEpoch).value() == "2", "tail has during");
}

void test_full_sync_ttl() {
    Master master("R1", 256);
    master.set("ttl", "v", kEpoch, 1'000);

    Replica replica("replica-1");
    check(replica.connect(master, kEpoch), "full sync with TTL");
    check(replica.kv().get("ttl", kEpoch + 500).has_value(), "TTL alive at +500ms");
    check(!replica.kv().get("ttl", kEpoch + 2'000).has_value(), "read hides expired key");
    check(replica.kv().raw("ttl").has_value(), "replica keeps expired key in memory");
    check(replica.kv().raw("ttl")->expire_at_ms == kEpoch + 1'000, "absolute TTL preserved");
}

}  // namespace archdesign::test
