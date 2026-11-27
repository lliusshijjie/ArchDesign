// tests_rewrite_model.cpp
//
// AOF rewrite 的三个崩溃点、RDB preamble、AOF/RDB 优先级、自动触发，
// 以及 500 步随机操作 + 反复恢复的参考模型。

#include <map>
#include <memory>
#include <random>
#include <string>

#include "test_support.hpp"

namespace archdesign::test {

void test_aof_rewrite() {
    MemFs fs;
    Engine engine(fs, base_config());
    engine.load(kEpoch);
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 200; ++i) engine.set(key_of(i), value_of(round * 1000 + i), kEpoch);
    }
    for (int i = 0; i < 50; ++i) engine.del(key_of(i), kEpoch);
    const std::size_t old_size = fs.size("appendonly.aof");

    engine.begin_rewrite(kEpoch);
    for (int i = 0; i < 20; ++i) engine.set(key_of(1000 + i), value_of(i), kEpoch);
    for (int i = 50; i < 70; ++i) engine.del(key_of(i), kEpoch);
    engine.set(key_of(70), "post-snapshot", kEpoch);
    check(engine.rewrite_active(), "rewrite is active");
    check(engine.rewrite_tail_bytes() > 0, "tail captured writes during rewrite");
    engine.finish_rewrite(kEpoch);

    const std::size_t new_size = fs.size("appendonly.aof");
    check(new_size < old_size, "rewrite shrinks the AOF");
    check(engine.stats().rewrites == 1, "one rewrite counted");

    fs.crash();
    Engine restored(fs, base_config());
    restored.load(kEpoch);
    require_same_live(engine.store(), restored.store(), kEpoch, "rewrite result");
    check(restored.get(key_of(70), kEpoch)->value == "post-snapshot",
          "write during rewrite is present");
}

void test_aof_rewrite_crash_before_commit() {
    MemFs fs;
    EngineConfig config = base_config();
    config.aof.fsync = FsyncPolicy::Always;
    Engine engine(fs, config);
    engine.load(kEpoch);
    seed(engine, 50, kEpoch);

    engine.begin_rewrite(kEpoch);
    for (int i = 0; i < 10; ++i) engine.set(key_of(100 + i), value_of(i), kEpoch);
    fs.crash();  // 模拟 rewrite 子进程/父进程在 rename 前掉电

    Engine restored(fs, config);
    restored.load(kEpoch);
    check(restored.size(kEpoch) == 60, "old AOF plus live writes recover everything");
    require_same_live(engine.store(), restored.store(), kEpoch, "pre-commit crash");
}

void test_aof_rewrite_with_rdb_preamble() {
    MemFs fs;
    EngineConfig config = base_config();
    config.aof.use_rdb_preamble = true;
    Engine engine(fs, config);
    engine.load(kEpoch);
    for (int i = 0; i < 30; ++i) engine.set(key_of(i), value_of(i), kEpoch, 10'000);
    engine.begin_rewrite(kEpoch);
    engine.set("during", "rewrite", kEpoch, 20'000);
    engine.finish_rewrite(kEpoch);

    const std::string data = fs.read("appendonly.aof");
    check(std::string_view(data).substr(0, 9) == "REDIS0011", "AOF starts with RDB preamble");
    check(data.find("during") != std::string::npos, "tail is appended after preamble");

    fs.crash();
    Engine restored(fs, config);
    restored.load(kEpoch);
    check(restored.stats().loaded_from == "aof (rdb preamble)", "loader detects preamble");
    require_same_live(engine.store(), restored.store(), kEpoch, "RDB preamble roundtrip");
    check(restored.get("during", kEpoch)->expire_at_ms == kEpoch + 20'000,
          "TTL in tail is absolute");
}

void test_recovery_precedence() {
    MemFs fs;

    EngineConfig rdb_only = base_config(false, 8);
    Engine rdb_engine(fs, rdb_only);
    rdb_engine.load(kEpoch);
    rdb_engine.set("from-rdb", "r", kEpoch);
    rdb_engine.save_rdb(kEpoch);

    EngineConfig aof_config = base_config(true, 8);
    Engine aof_engine(fs, aof_config);
    aof_engine.load(kEpoch);  // AOF 文件还不存在，回退 RDB
    check(aof_engine.get("from-rdb", kEpoch).has_value(), "initial load falls back to RDB");
    aof_engine.set("from-aof", "a", kEpoch);

    Engine both(fs, aof_config);
    both.load(kEpoch);
    check(both.stats().loaded_from == "aof", "AOF wins when both exist");
    check(both.get("from-aof", kEpoch).has_value(), "AOF data loaded");
    check(!both.get("from-rdb", kEpoch).has_value(), "RDB data not replayed on top of AOF");

    Engine only_rdb(fs, rdb_only);
    only_rdb.load(kEpoch);
    check(only_rdb.stats().loaded_from == "rdb", "AOF disabled -> RDB");
    check(only_rdb.get("from-rdb", kEpoch).has_value(), "RDB data loaded");
    check(!only_rdb.get("from-aof", kEpoch).has_value(), "AOF ignored when disabled");
}

void test_auto_rewrite() {
    MemFs fs;
    EngineConfig config = base_config();
    config.auto_rewrite_min_size = 256;
    config.auto_rewrite_percentage = 100;
    Engine engine(fs, config);
    engine.load(kEpoch);

    for (int i = 0; i < 300; ++i) engine.set("same", value_of(i), kEpoch);
    check(engine.maybe_auto_rewrite(kEpoch), "growth triggers auto rewrite");
    check(engine.rewrite_active(), "auto rewrite starts a session");
    engine.finish_rewrite(kEpoch);
    check(engine.stats().rewrites == 1, "auto rewrite completed");
    check(!engine.maybe_auto_rewrite(kEpoch), "fresh base does not trigger again");
}

struct ModelValue {
    std::string value;
    i64 expire_at_ms = -1;
};

void test_model() {
    MemFs fs;
    EngineConfig config = base_config();
    auto engine = std::make_unique<Engine>(fs, config);
    engine->load(kEpoch);

    std::map<std::string, ModelValue> model;
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> key_dist(0, 49);
    std::uniform_int_distribution<int> op_dist(0, 9);
    i64 now = kEpoch;

    auto apply_model = [&](const std::string& key, int op, int index) {
        if (op < 6) {
            model[key] = ModelValue{value_of(index), -1};
        } else if (op == 6) {
            model.erase(key);
        } else if (op == 7) {
            auto it = model.find(key);
            if (it != model.end()) it->second.expire_at_ms = now + 5'000;
        } else {
            auto it = model.find(key);
            if (it != model.end()) it->second.expire_at_ms = -1;
        }
    };

    auto verify = [&](const std::string& context) {
        std::size_t live = 0;
        for (const auto& [key, value] : model) {
            if (value.expire_at_ms != -1 && value.expire_at_ms <= now) continue;
            ++live;
            const auto actual = engine->get(key, now);
            check(actual.has_value(), context + ": missing key " + key);
            check(actual->value == value.value, context + ": value mismatch for " + key);
            check(actual->expire_at_ms == value.expire_at_ms, context + ": TTL mismatch for " + key);
        }
        check(engine->size(now) == live, context + ": live count mismatch");
    };

    for (int step = 0; step < 500; ++step) {
        now += 100;
        const std::string key = key_of(key_dist(rng));
        const int op = op_dist(rng);
        if (op < 6) {
            engine->set(key, value_of(step), now);
        } else if (op == 6) {
            engine->del(key, now);
        } else if (op == 7) {
            engine->expire(key, now, 5'000);
        } else {
            engine->persist(key, now);
        }
        apply_model(key, op, step);

        if (step % 50 == 49) {
            engine->save_rdb(now);
            engine->begin_rewrite(now);
            engine->set("during-rewrite", value_of(step), now);
            model["during-rewrite"] = ModelValue{value_of(step), -1};
            engine->finish_rewrite(now);

            fs.crash();
            engine = std::make_unique<Engine>(fs, config);
            engine->load(now);
            verify("after reload");
            check(engine->size(now) == engine->store().live_key_count(now),
                  "reloaded live count matches store");
        }
    }
    verify("final");
}

}  // namespace archdesign::test
