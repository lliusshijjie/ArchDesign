// tests_redirect.cpp
//
// MOVED、客户端缓存、ASK、TRYAGAIN，以及“迁移完成后才更新缓存”。

#include <string>

#include "test_support.hpp"

namespace archdesign::test {

void test_moved() {
    Cluster cluster = three_masters();
    const std::string key = key_owned_by(cluster, "M2");
    cluster.node("M2")->data[key] = "value";

    const Reply reply = cluster.execute("M1", Command{{"GET", key}});
    check(reply.type == ReplyType::Moved, "request on wrong node -> MOVED");
    check(reply.target == "M2", "MOVED points at the slot owner");
    check(reply.slot == key_hash_slot(key), "MOVED includes the slot");
}

void test_client_cache() {
    Cluster cluster = three_masters();
    const std::string key = key_owned_by(cluster, "M2");
    cluster.node("M2")->data[key] = "value";

    ClusterClient client(cluster);
    client.learn_slot(key_hash_slot(key), "M1");  // 过期缓存
    const Reply first = client.execute(Command{{"GET", key}});
    check(first.type == ReplyType::Ok && first.value == "value", "client follows MOVED");
    check(client.moved_count() == 1, "one MOVED");

    const Reply second = client.execute(Command{{"GET", key}});
    check(second.type == ReplyType::Ok, "cache is updated");
    check(client.moved_count() == 1, "no second MOVED for the same slot");
}

void test_ask_migration() {
    Cluster cluster = three_masters();
    const std::string key = key_owned_by(cluster, "M1");
    const Slot slot = key_hash_slot(key);
    cluster.node("M1")->data[key] = "moving";

    cluster.begin_migration(slot, "M1", "M2");
    check(cluster.migrate_keys(slot, "M1", "M2") == 1, "key moved to target");
    check(cluster.node("M2")->data[key] == "moving", "target has the key");

    const Reply ask = cluster.execute("M1", Command{{"GET", key}});
    check(ask.type == ReplyType::Ask, "source without key -> ASK");
    check(ask.target == "M2", "ASK points at target");

    ClusterClient client(cluster);
    client.learn_slot(slot, "M1");
    const Reply got = client.execute(Command{{"GET", key}});
    check(got.type == ReplyType::Ok && got.value == "moving", "client follows ASK");
    check(client.ask_count() == 1, "one ASK");
    check(client.moved_count() == 0, "ASK does not update the slot cache");

    cluster.finish_migration(slot, "M2");
    const Reply after = client.execute(Command{{"GET", key}});
    check(after.type == ReplyType::Ok && after.value == "moving", "after finish, data still works");
    check(client.moved_count() == 1, "finish causes a MOVED, then cache updates");
}

void test_tryagain() {
    Cluster cluster = three_masters();
    std::string tag;
    for (int i = 0; i < 10000; ++i) {
        tag = "tag" + std::to_string(i);
        if (cluster.slots().owner(key_hash_slot("{" + tag + "}a")) == "M1") break;
    }
    const std::string key1 = "{" + tag + "}a";
    const std::string key2 = "{" + tag + "}b";
    check(key_hash_slot(key1) == key_hash_slot(key2), "same hash tag");
    const Slot slot = key_hash_slot(key1);
    cluster.node("M1")->data[key1] = "1";
    cluster.node("M1")->data[key2] = "2";

    cluster.begin_migration(slot, "M1", "M2");
    check(cluster.migrate_keys(slot, "M1", "M2", 1) == 1, "only one key moved");
    const Reply reply = cluster.execute("M1", Command{{"MGET", key1, key2}});
    check(reply.type == ReplyType::TryAgain, "partial key set during migration -> TRYAGAIN");
}

void test_ask_target_requires_asking() {
    Cluster cluster = three_masters();
    const std::string key = key_owned_by(cluster, "M1");
    const Slot slot = key_hash_slot(key);
    cluster.node("M1")->data[key] = "v";
    cluster.begin_migration(slot, "M1", "M2");
    cluster.migrate_keys(slot, "M1", "M2");

    const Reply no_asking = cluster.execute("M2", Command{{"GET", key}}, false);
    check(no_asking.type == ReplyType::Moved, "target without ASKING -> MOVED source");
    check(no_asking.target == "M1", "MOVED points back at the source");

    const Reply with_asking = cluster.execute("M2", Command{{"GET", key}}, true);
    check(with_asking.type == ReplyType::Ok && with_asking.value == "v",
          "target with ASKING serves the key");
}

}  // namespace archdesign::test
