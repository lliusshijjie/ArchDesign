// demo.cpp
//
// 槽、MOVED、ASK 迁移、replid2 式故障转移的讲解式输出。

#include <iomanip>
#include <iostream>
#include <string>

#include "tests/test_support.hpp"

namespace archdesign::test {

void run_demo() {
    Cluster cluster = three_masters();

    std::cout << "== 1. 槽与 hash tag ==\n";
    const std::string a = "{user1000}.profile";
    const std::string b = "{user1000}.orders";
    std::cout << "  " << a << " -> slot " << key_hash_slot(a) << "\n"
              << "  " << b << " -> slot " << key_hash_slot(b) << "（同一 tag，同槽）\n\n";

    std::cout << "== 2. MOVED ==\n";
    const std::string key = key_owned_by(cluster, "M2");
    cluster.node("M2")->data[key] = "Alice";
    const Reply moved = cluster.execute("M1", Command{{"GET", key}});
    std::cout << "  GET " << key << " 发到 M1 -> "
              << (moved.type == ReplyType::Moved ? "MOVED" : "OK") << " " << moved.slot
              << " -> " << moved.target << "\n\n";

    std::cout << "== 3. 迁移中的 ASK ==\n";
    const std::string moving_key = key_owned_by(cluster, "M1");
    const Slot slot = key_hash_slot(moving_key);
    cluster.node("M1")->data[moving_key] = "Bob";
    cluster.begin_migration(slot, "M1", "M2");
    cluster.migrate_keys(slot, "M1", "M2");

    ClusterClient client(cluster);
    client.learn_slot(slot, "M1");
    const Reply ask = client.execute(Command{{"GET", moving_key}});
    std::cout << "  迁移中 GET 发到 M1 -> ASK；客户端带 ASKING 去 M2 -> "
              << (ask.type == ReplyType::Ok ? ask.value : "FAIL") << "\n";
    cluster.finish_migration(slot, "M2");
    const Reply after = client.execute(Command{{"GET", moving_key}});
    std::cout << "  迁移完成后再 GET -> MOVED 学习到新 owner，值=" << after.value << "\n\n";

    std::cout << "== 4. gossip + quorum 故障转移 ==\n";
    const std::string fail_key = key_owned_by(cluster, "M3");
    cluster.node("M3")->data[fail_key] = "Carol";
    cluster.add_replica("R3", "M3");
    cluster.sync_replica("R3");

    cluster.report_failure("M1", "M3");
    std::cout << "  1 个 master 举报 M3：" << (cluster.failed("M3") ? "FAIL" : "还不到 quorum") << "\n";
    cluster.report_failure("M2", "M3");
    std::cout << "  2 个 master 举报 M3：" << (cluster.failed("M3") ? "FAIL" : "还不到 quorum") << "\n";
    cluster.begin_failover("R3");
    cluster.grant_vote("M1", "R3");
    cluster.grant_vote("M2", "R3");
    const bool promoted = cluster.promote("R3");
    std::cout << "  R3 拿到 " << cluster.votes("R3") << " 票，promote=" << (promoted ? "yes" : "no")
              << "，slot(" << key_hash_slot(fail_key) << ") -> "
              << cluster.slots().owner(key_hash_slot(fail_key)) << "\n";
    std::cout << "  promoted 后 healthy=" << (cluster.healthy() ? "yes" : "no") << "\n";
}

}  // namespace archdesign::test
