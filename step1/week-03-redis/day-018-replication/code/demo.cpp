// demo.cpp
//
// 复制机制讲解：全量 → 增量 → 断线续传 → backlog 溢出 → WAIT → replid2。

#include <iomanip>
#include <iostream>
#include <string>

#include "tests/test_support.hpp"

namespace archdesign::test {

void run_demo() {
    std::cout << "== 1. 全量同步 + 增量流 ==\n";
    Master master("R1", 512);
    std::cout << "  初始 replid=" << master.replid() << " offset=" << master.offset() << "\n";
    master.set("user:1", "Alice", kEpoch);
    master.set("user:2", "Bob", kEpoch);
    std::cout << "  master 写入 2 条后 offset=" << master.offset() << "\n";

    Replica replica("replica-1");
    replica.connect(master, kEpoch);
    std::cout << "  replica FULLRESYNC -> replid=" << replica.replid()
              << " offset=" << replica.offset() << "\n";

    master.set("user:3", "Carol", kEpoch);
    replica.poll(master, kEpoch);
    replica.ack(master, kEpoch);
    std::cout << "  增量写入 + poll 后 replica offset=" << replica.offset()
              << "，WAIT(1)=" << master.wait_for_acks(1, master.offset(), 100, kEpoch) << "\n\n";

    std::cout << "== 2. 断线续传 ==\n";
    replica.disconnect();
    master.set("user:4", "Dave", kEpoch);
    const Offset lagging = replica.offset();
    replica.connect(master, kEpoch);
    std::cout << "  断线时 replica offset=" << lagging << "，重连后 offset=" << replica.offset()
              << "（PSYNC +CONTINUE）\n\n";

    std::cout << "== 3. backlog 溢出 -> FULLRESYNC ==\n";
    Master small("R1", 64);
    small.set("seed", "before-backlog", kEpoch);
    Replica behind("replica-2");
    behind.connect(small, kEpoch);
    behind.disconnect();
    for (int i = 0; i < 50; ++i) {
        small.set("overflow-" + std::to_string(i), std::string(32, 'x'), kEpoch);
    }
    const Offset old_offset = behind.offset();
    behind.connect(small, kEpoch);
    std::cout << "  backlog 只有 64B，断线期间写入 50 条；replica 从 offset=" << old_offset
              << " 重连 -> FULLRESYNC，新 offset=" << behind.offset() << "\n\n";

    std::cout << "== 4. 提升 + replid2 ==\n";
    Master old_master("R1", 1024);
    old_master.set("a", "1", kEpoch);
    Replica b("replica-b");
    Replica c("replica-c");
    b.connect(old_master, kEpoch);
    c.connect(old_master, kEpoch);
    old_master.set("b", "2", kEpoch);
    b.poll(old_master, kEpoch);
    c.poll(old_master, kEpoch);
    c.disconnect();
    old_master.set("c", "3", kEpoch);
    b.poll(old_master, kEpoch);

    Master promoted("R2", "R1", b.offset() + 1, 1024);
    promoted.seed_history(b.offset(), old_master.backlog().start(),
                          old_master.backlog().read_from(old_master.backlog().start()), b.kv());
    c.connect(promoted, kEpoch);
    std::cout << "  旧主 R1 -> 新主 R2；C 用 R1 的 offset 续传，新 replid=" << c.replid()
              << "，有 c=" << (c.kv().raw("c").has_value() ? "yes" : "no") << "\n";
}

}  // namespace archdesign::test
