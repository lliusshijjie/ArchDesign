// tests_aof.cpp
//
// RESP、fsync 策略、尾部截断、中间损坏：AOF 的写入与加载不变量。

#include <string>

#include "test_support.hpp"

namespace archdesign::test {

void test_aof_resp() {
    Command set{"SET", "k", "v"};
    check(resp_encode(set) == "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n",
          "RESP SET encoding");
    Command pexpireat{"PEXPIREAT", "k", "1700000001000"};
    check(resp_encode(pexpireat) ==
              "*3\r\n$9\r\nPEXPIREAT\r\n$1\r\nk\r\n$13\r\n1700000001000\r\n",
          "RESP PEXPIREAT encoding");

    const std::string two = resp_encode(set) + resp_encode(Command{"DEL", "k"});
    const RespParseResult complete = resp_parse(two);
    check(complete.commands.size() == 2 && !complete.truncated, "two complete commands");
    check(complete.commands[0].args[0] == "SET", "first verb");
    check(complete.consumed == two.size(), "all bytes consumed");

    const RespParseResult cut = resp_parse(two.substr(0, two.size() - 4));
    check(cut.commands.size() == 1 && cut.truncated, "last command is truncated");
    check(cut.consumed == resp_encode(set).size(),
          "consumed points at the last complete command");

    const std::string annotated = "#TS:1700000000\r\n" + resp_encode(set);
    const RespParseResult with_annotation = resp_parse(annotated);
    check(with_annotation.commands.size() == 1, "timestamp annotation is skipped");
}

void test_aof_always() {
    MemFs fs;
    EngineConfig config = base_config();
    config.aof.fsync = FsyncPolicy::Always;
    Engine engine(fs, config);
    engine.load(kEpoch);
    seed(engine, 20, kEpoch);

    fs.crash();
    Engine restored(fs, config);
    restored.load(kEpoch);
    check(restored.stats().loaded_from == "aof", "loaded from AOF");
    require_same_live(engine.store(), restored.store(), kEpoch, "always policy");
    check(restored.size(kEpoch) == 20, "all 20 keys survived");
}

void test_aof_everysec() {
    MemFs fs;
    EngineConfig config = base_config();
    config.aof.fsync = FsyncPolicy::EverySec;
    Engine engine(fs, config);
    engine.load(kEpoch);

    engine.set("k0", "lost", kEpoch);
    fs.crash();
    Engine after_crash(fs, config);
    after_crash.load(kEpoch);
    check(after_crash.size(kEpoch) == 0, "un-fsynced tail is lost after crash");

    engine.set("k1", "saved", kEpoch + 500);
    engine.aof().tick(kEpoch + 1500);  // 模拟 1 秒定时 fsync
    fs.crash();
    Engine after_tick(fs, config);
    after_tick.load(kEpoch + 1500);
    check(after_tick.get("k1", kEpoch + 1500).has_value(), "fsync makes the tail durable");
    check(!after_tick.get("k0", kEpoch + 1500).has_value(), "the earlier lost key stays lost");
}

void test_aof_no() {
    MemFs fs;
    EngineConfig config = base_config();
    config.aof.fsync = FsyncPolicy::No;
    Engine engine(fs, config);
    engine.load(kEpoch);
    engine.set("k", "v", kEpoch);
    fs.crash();
    Engine lost(fs, config);
    lost.load(kEpoch);
    check(lost.size(kEpoch) == 0, "no fsync: page cache is gone");

    engine.set("k2", "v2", kEpoch + 100);
    engine.aof().fsync_now(kEpoch + 200);
    fs.crash();
    Engine saved(fs, config);
    saved.load(kEpoch + 200);
    check(saved.get("k2", kEpoch + 200).has_value(), "explicit fsync saves the data");
}

void test_aof_truncated_tail() {
    MemFs fs;
    Engine engine(fs, base_config());
    engine.load(kEpoch);
    seed(engine, 3, kEpoch);

    std::string data = fs.read("appendonly.aof");
    fs.truncate("appendonly.aof", data.size() - 5);

    Engine recovered(fs, base_config());
    recovered.load(kEpoch);
    check(recovered.size(kEpoch) == 2, "only the first two commands are replayed");
    check(recovered.stats().aof_truncated_bytes > 0, "discarded tail bytes counted");
    check(fs.size("appendonly.aof") < data.size(), "AOF was repaired to the valid prefix");

    EngineConfig strict = base_config();
    strict.aof.load_truncated = false;
    fs.truncate("appendonly.aof", 0);
    fs.append("appendonly.aof", "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\nva");
    expect_throw([&] { Engine rejected(fs, strict); rejected.load(kEpoch); }, "truncated tail",
                 "strict AOF load must reject a partial command");
}

void test_aof_corrupt() {
    MemFs fs;
    Engine engine(fs, base_config());
    engine.load(kEpoch);
    seed(engine, 3, kEpoch);
    std::string data = fs.read("appendonly.aof");
    data[0] = 'X';  // 破坏第一条命令的 '*'
    fs.write_file("appendonly.aof", data);
    fs.fsync("appendonly.aof");

    Engine broken(fs, base_config());
    expect_throw([&] { broken.load(kEpoch); }, "expected '*'", "middle corruption must fail");
}

}  // namespace archdesign::test
