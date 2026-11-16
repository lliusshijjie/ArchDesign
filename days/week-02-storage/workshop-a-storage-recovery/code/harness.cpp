// harness.cpp
//
// 工作坊 A：存储引擎压测与崩溃恢复。
//
// 这个程序不是“跑一遍看看”，而是把结论做成可断言的东西：
//   1. 场景矩阵：在每一个顺序边界上注入崩溃，重新打开，检查不变量；
//   2. 不变量：确认过的写入必须还在（acked ⊆ recovered）、没有幽灵键、
//      没有错误的值、没有已删除的键复活；
//   3. 预期失败：故意关掉 fsync / 提前截断 WAL，验证“丢多少”，
//      以及恢复后能不能继续正常工作；
//   4. 基准：写放大、fsync 对 p99 的影响、恢复时间随数据量增长。
//
// 输出的表格就是报告里“实验记录”一节的原始数据。

#include "engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace arch;
namespace fs = std::filesystem;

// ---------------------------------------------------------------- 基础工具

class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned long long> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("archdesign-workshop-a-" + std::to_string(stamp) + "-" +
                 std::to_string(counter.fetch_add(1)));
        std::error_code error;
        fs::remove_all(path_, error);
    }

    ~TempDir() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

double ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

std::uint64_t file_size_of(const fs::path& path) {
    std::error_code error;
    const auto bytes = fs::file_size(path, error);
    if (error) {
        throw std::runtime_error("cannot stat " + path.string());
    }
    return bytes;
}

void append_bytes(const fs::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) {
        throw std::runtime_error("cannot append to " + path.string());
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out) {
        throw std::runtime_error("append failed: " + path.string());
    }
}

void flip_byte(const fs::path& path, std::uint64_t offset) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        throw std::runtime_error("cannot open " + path.string());
    }
    file.seekg(static_cast<std::streamoff>(offset));
    char byte = 0;
    file.read(&byte, 1);
    if (!file) {
        throw std::runtime_error("flip offset out of range");
    }
    file.clear();
    file.seekp(static_cast<std::streamoff>(offset));
    byte = static_cast<char>(byte ^ 0x5A);
    file.write(&byte, 1);
    file.flush();
    if (!file) {
        throw std::runtime_error("flip write failed");
    }
}

fs::path first_sst(const fs::path& directory) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("sst-") && name.ends_with(".dat")) {
            files.push_back(entry.path());
        }
    }
    if (files.empty()) {
        throw std::runtime_error("no sst file to damage");
    }
    std::sort(files.begin(), files.end());
    return files.front();
}

// ---------------------------------------------------------------- 负载

// 键单调递增，避免“重复写同一个 key 导致 memtable 不再增长”；
// 每隔 delete_every 次删一个更早的 key，用来看 tombstone 会不会复活。
struct Workload {
    std::size_t ops = 120;
    std::size_t value_size = 24;
    std::size_t delete_every = 0;

    [[nodiscard]] std::string key(std::size_t index) const {
        char buffer[24];
        std::snprintf(buffer, sizeof(buffer), "key-%06zu", index);
        return buffer;
    }

    [[nodiscard]] bool is_delete(std::size_t index) const {
        return delete_every != 0 && index % delete_every == delete_every - 1 && index > 0;
    }

    [[nodiscard]] std::string target(std::size_t index) const {
        // 删除一个更早写入的 key，保证“删除”之后不会又被重新写入。
        return is_delete(index) ? key(index / 2) : key(index);
    }

    [[nodiscard]] std::string value(std::size_t index) const {
        std::string text = "v" + std::to_string(index) + "-";
        if (text.size() < value_size) {
            text.append(value_size - text.size(), 'x');
        }
        return text;
    }
};

// 记录“哪些操作已经向调用方确认过”，以及“这个 key 到底被写过哪些值”。
//
// pending 只存放崩溃那一刻正在执行、还没返回给调用方的那一次操作：
// 它可能已经落盘（属于合法的“未确认但已生效”），也可能没落盘。
// 不把它单独记下来，就会把这种情况误判成“确认过的数据丢了”。
struct Tracker {
    std::map<std::string, std::optional<std::string>> acked;
    std::map<std::string, std::optional<std::string>> pending;
    std::map<std::string, std::set<std::optional<std::string>>> written;
};

// ---------------------------------------------------------------- 场景定义

enum class Damage {
    None,
    TornHeader,    // WAL 尾部只写了半条记录头
    GarbageTail,   // WAL 尾部是纯垃圾
    MidFileFlip,   // WAL 中间一个字节翻转
    SstFlip,       // 已落盘的 SST 中间一个字节翻转
};

struct Spec {
    std::string group;
    std::string id;
    std::string name;
    Engine::Options options;
    Workload workload;
    std::string crash_point;
    int occurrence = 1;
    bool power_loss_at_end = false;
    bool close_cleanly = false;
    bool strict = true;              // 要求“确认过的写入一条都不少”
    bool expect_corruption = false;  // 期望恢复时检测到损坏并拒绝启动
    bool verify_continuation = true;
    Damage damage = Damage::None;
    std::uint64_t min_lost = 0;
    std::uint64_t max_lost = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t min_truncated = 0;
    std::uint64_t min_replayed = 0;
    std::string note;
};

struct Result {
    std::string id;
    std::string name;
    std::string point;
    int occurrence = 1;
    std::uint64_t acked = 0;
    std::uint64_t present = 0;
    std::uint64_t lost = 0;
    std::uint64_t unacked_applied = 0;
    std::uint64_t resurrected = 0;
    std::uint64_t phantom = 0;
    std::uint64_t bad_values = 0;
    std::uint64_t truncated = 0;
    std::uint64_t replayed = 0;
    std::uint64_t fsyncs = 0;
    std::uint64_t flushes = 0;
    std::uint64_t compactions = 0;
    std::size_t ssts = 0;
    double recovery_ms = 0.0;
    bool crashed = false;
    bool corruption_detected = false;
    bool passed = true;
    std::string note;
};

void apply_damage(Damage damage, const fs::path& directory, Result& result) {
    const auto wal = directory / std::string(Engine::wal_file);
    switch (damage) {
    case Damage::None:
        return;
    case Damage::TornHeader: {
        // magic(4) + type(1) + key_len(4) + value_len(4) = 13 字节，恰好半条记录头。
        std::string torn;
        torn += "WAL1";
        torn.push_back(static_cast<char>(0));
        put_u32(torn, 3);
        put_u32(torn, 0);
        append_bytes(wal, torn);
        result.note = "尾部追加 13 字节半条记录头";
        return;
    }
    case Damage::GarbageTail:
        append_bytes(wal, "xxxxx");
        result.note = "尾部追加 5 字节垃圾";
        return;
    case Damage::MidFileFlip: {
        const auto size = file_size_of(wal);
        flip_byte(wal, size / 2);
        result.note = "WAL 中间翻转 1 字节（模拟扇区静默损坏）";
        return;
    }
    case Damage::SstFlip: {
        const auto sst = first_sst(directory);
        const auto size = file_size_of(sst);
        if (size < 25) {
            throw std::runtime_error("sst too small to damage");
        }
        flip_byte(sst, 20);
        result.note = "SST 第 20 字节翻转（模拟落盘后损坏）";
        return;
    }
    }
}

Result execute(const Spec& spec) {
    Result result;
    result.id = spec.id;
    result.name = spec.name;
    result.point = spec.crash_point.empty() ? "-" : spec.crash_point;
    result.occurrence = spec.occurrence;
    result.note = spec.note;

    TempDir directory;
    Tracker tracker;
    CrashInjection crash;
    if (!spec.crash_point.empty()) {
        crash = CrashInjection(CrashInjection::Plan{spec.crash_point, spec.occurrence});
    }

    {
        std::unique_ptr<Engine> engine;
        try {
            engine = std::make_unique<Engine>(directory.path(), spec.options,
                                              spec.crash_point.empty() ? nullptr : &crash);
        } catch (const std::exception& error) {
            result.passed = false;
            result.note = std::string("initial open failed: ") + error.what();
            return result;
        }

        for (std::size_t index = 0; index < spec.workload.ops; ++index) {
            const auto key = spec.workload.target(index);
            const bool is_delete = spec.workload.is_delete(index);
            std::optional<std::string> op_value;
            if (!is_delete) {
                op_value = spec.workload.value(index);
            }
            tracker.written[key].insert(op_value);
            tracker.pending[key] = op_value;

            try {
                if (is_delete) {
                    engine->erase(key);
                } else {
                    engine->put(key, *op_value);
                }
                tracker.acked[key] = op_value;
                tracker.pending.erase(key);
            } catch (const CrashSignal&) {
                result.crashed = true;
                break;
            }
        }

        if (result.crashed || spec.power_loss_at_end) {
            engine->power_loss();
        } else if (spec.close_cleanly) {
            engine->close();
        }

        const auto stats = engine->stats();
        result.fsyncs = stats.fsyncs;
        result.flushes = stats.flushes;
        result.compactions = stats.compactions;
    }

    apply_damage(spec.damage, directory.path(), result);

    const auto start = std::chrono::steady_clock::now();
    std::unique_ptr<Engine> recovered;
    try {
        recovered = std::make_unique<Engine>(directory.path(), spec.options, nullptr);
    } catch (const Corruption& error) {
        result.recovery_ms = ms_since(start);
        result.corruption_detected = true;
        result.acked = tracker.acked.size();
        result.passed = spec.expect_corruption;
        if (!spec.expect_corruption) {
            result.note = std::string("unexpected corruption: ") + error.what();
        } else {
            result.note = std::string("fail-stop：拒绝启动（") + error.what() + "）";
        }
        return result;
    } catch (const std::exception& error) {
        result.recovery_ms = ms_since(start);
        result.passed = false;
        result.note = std::string("reopen threw: ") + error.what();
        return result;
    }
    result.recovery_ms = ms_since(start);

    if (spec.expect_corruption) {
        result.passed = false;
        result.note = "期望检测到损坏，但恢复成功了（静默返回不可信数据更危险）";
        return result;
    }

    const auto stats = recovered->stats();
    result.truncated = stats.wal_truncated_bytes;
    result.replayed = stats.wal_records_replayed;
    result.ssts = stats.ssts;
    result.acked = tracker.acked.size();

    const auto snapshot = recovered->dump();

    // 1) 幽灵键 / 写不出来的值：任何情况下都不允许。
    for (const auto& [key, value] : snapshot) {
        const auto written = tracker.written.find(key);
        if (written == tracker.written.end()) {
            ++result.phantom;
            continue;
        }
        if (written->second.find(value) == written->second.end()) {
            ++result.bad_values;
        }
        if (value.has_value()) {
            ++result.present;
        }
    }

    // 2) 确认过的写入：严格模式下必须一条不少。
    //    唯一的例外是“崩溃时正在执行的那次操作已经生效”：那是允许的
    //    （未确认的操作可以提交，但不能凭空消失）。
    for (const auto& [key, value] : tracker.acked) {
        const auto it = snapshot.find(key);
        const std::optional<std::string> actual =
            it == snapshot.end() ? std::nullopt : it->second;
        if (actual == value) {
            continue;
        }
        const auto pending = tracker.pending.find(key);
        if (pending != tracker.pending.end() && pending->second == actual) {
            ++result.unacked_applied;
            continue;
        }
        ++result.lost;
        if (!value.has_value() && actual.has_value()) {
            ++result.resurrected;
        }
    }

    // 3) 恢复后继续写、再重开：引擎不能“带着坏状态”继续工作。
    if (spec.verify_continuation) {
        const auto expected = snapshot;
        for (int index = 0; index < 3; ++index) {
            recovered->put("post-" + std::to_string(index),
                           "post-value-" + std::to_string(index));
        }
        recovered->close();
        recovered.reset();
        try {
            Engine again(directory.path(), spec.options);
            const auto after = again.dump();
            for (const auto& [key, value] : expected) {
                const auto it = after.find(key);
                const std::optional<std::string> actual =
                    it == after.end() ? std::nullopt : it->second;
                if (actual != value) {
                    result.passed = false;
                    result.note = "continuation lost " + key;
                    break;
                }
            }
            for (int index = 0; index < 3; ++index) {
                const auto expected_value =
                    std::optional<std::string>("post-value-" + std::to_string(index));
                if (again.get("post-" + std::to_string(index)) != expected_value) {
                    result.passed = false;
                    result.note = "continuation lost post-" + std::to_string(index);
                    break;
                }
            }
        } catch (const std::exception& error) {
            result.passed = false;
            result.note = std::string("continuation reopen threw: ") + error.what();
        }
    }

    if (result.phantom != 0 || result.bad_values != 0) {
        result.passed = false;
    }
    if (spec.strict && (result.lost != 0 || result.resurrected != 0)) {
        result.passed = false;
    }
    if (result.lost < spec.min_lost) {
        result.passed = false;
        result.note += " [期望至少丢 " + std::to_string(spec.min_lost) + " 条，实际 " +
                       std::to_string(result.lost) + " 条]";
    }
    if (result.lost > spec.max_lost) {
        result.passed = false;
        result.note += " [丢失 " + std::to_string(result.lost) + " 条，超出预期窗口 " +
                       std::to_string(spec.max_lost) + " 条]";
    }
    if (result.truncated < spec.min_truncated) {
        result.passed = false;
        result.note += " [期望至少截断 " + std::to_string(spec.min_truncated) + " 字节]";
    }
    if (result.replayed < spec.min_replayed) {
        result.passed = false;
        result.note += " [期望至少重放 " + std::to_string(spec.min_replayed) + " 条记录]";
    }
    return result;
}

// ---------------------------------------------------------------- 场景清单

std::vector<Spec> build_scenarios() {
    std::vector<Spec> specs;
    const auto add = [&specs](std::string group, std::string id, std::string name) -> Spec& {
        Spec spec;
        spec.group = std::move(group);
        spec.id = std::move(id);
        spec.name = std::move(name);
        specs.push_back(std::move(spec));
        return specs.back();
    };

    // ---- A. 基线：正常关闭 vs 断电 --------------------------------
    {
        auto& spec = add("A. 基线", "B00-clean-reopen", "正常关闭后重开");
        spec.workload = Workload{120, 24, 7};
        spec.options.memtable_limit = 512;
        spec.options.compaction_trigger = 3;
        spec.close_cleanly = true;
        spec.note = "对照组：所有写入都确认并且正常关闭，重开后一条不少";
    }
    {
        auto& spec = add("A. 基线", "B01-nofsync-power-loss", "WAL 不 fsync 时断电");
        spec.workload = Workload{100, 32, 0};
        spec.options.memtable_limit = 1u << 20;
        spec.options.fsync_wal = false;
        spec.power_loss_at_end = true;
        spec.strict = false;
        spec.min_lost = 100;
        spec.note = "预期：确认过的 100 条写入全部丢失（没有 fsync 就没有持久化）";
    }
    {
        auto& spec = add("A. 基线", "B02-fsync-power-loss", "WAL 每条 fsync 时断电");
        spec.workload = Workload{100, 32, 0};
        spec.options.memtable_limit = 1u << 20;
        spec.power_loss_at_end = true;
        spec.min_lost = 0;
        spec.note = "预期：0 丢失（put 返回前已经 fsync）";
    }
    {
        auto& spec = add("A. 基线", "B03-group-commit-8", "group commit=8 的丢失窗口");
        spec.workload = Workload{100, 32, 0};
        spec.options.memtable_limit = 1u << 20;
        spec.options.group_commit = 8;
        spec.power_loss_at_end = true;
        spec.strict = false;
        spec.min_lost = 1;
        spec.max_lost = 7;
        spec.note = "预期：丢 1–7 条，等于最后一个 fsync 之后的窗口";
    }

    // ---- B. WAL 崩溃点位：每个点位 × 第 1/5/37 次 ----------------
    const std::array<std::string_view, 4> wal_points = {
        kWalBeforeAppend, kWalAfterAppend, kWalAfterDurability, kWalAfterMemtable};
    const std::array<int, 3> wal_occurrences = {1, 5, 37};
    for (const auto point : wal_points) {
        for (const int occurrence : wal_occurrences) {
            auto& spec = add("B. WAL 崩溃点位",
                             "W-" + std::string(point) + "@" + std::to_string(occurrence),
                             "WAL 写路径崩溃");
            spec.workload = Workload{120, 24, 7};
            spec.options.memtable_limit = 512;
            spec.options.compaction_trigger = 3;
            spec.crash_point = std::string(point);
            spec.occurrence = occurrence;
            spec.note = "严格：确认过的写入必须一条不少";
        }
    }

    // ---- C. flush 崩溃点位：第 1/2 次 flush ----------------------
    const std::array<std::string_view, 6> flush_points = {
        kFlushSstBeforeRename,        kFlushSstAfterRename,
        kFlushManifestBeforeCommit,   kFlushManifestAfterCommit,
        kFlushWalBeforeTruncate,      kFlushWalAfterTruncate};
    const std::array<int, 2> flush_occurrences = {1, 2};
    for (const auto point : flush_points) {
        for (const int occurrence : flush_occurrences) {
            auto& spec = add("C. flush 崩溃点位",
                             "F-" + std::string(point) + "@" + std::to_string(occurrence),
                             "flush 顺序边界崩溃");
            spec.workload = Workload{200, 24, 5};
            spec.options.memtable_limit = 384;
            spec.options.compaction_trigger = 4;
            spec.crash_point = std::string(point);
            spec.occurrence = occurrence;
            if (point == kFlushManifestAfterCommit && occurrence == 1) {
                spec.min_replayed = 1;
                spec.note = "manifest 已提交但 WAL 未截断：重放重复记录，靠幂等覆盖";
            } else {
                spec.note = "严格：SST/manifest 落盘后才允许截断 WAL";
            }
        }
    }

    // ---- D. compaction 崩溃点位 --------------------------------
    const std::array<std::string_view, 4> compact_points = {
        kCompactSstBeforeRename, kCompactSstAfterRename,
        kCompactManifestAfterCommit, kCompactOldDeleted};
    const std::array<int, 2> compact_occurrences = {1, 2};
    for (const auto point : compact_points) {
        for (const int occurrence : compact_occurrences) {
            auto& spec = add("D. compaction 崩溃点位",
                             "C-" + std::string(point) + "@" + std::to_string(occurrence),
                             "compaction 顺序边界崩溃");
            spec.workload = Workload{260, 24, 9};
            spec.options.memtable_limit = 320;
            spec.options.compaction_trigger = 3;
            spec.crash_point = std::string(point);
            spec.occurrence = occurrence;
            spec.note = "严格：新 SST 与 manifest 一起提交，孤儿文件启动时清理";
        }
    }

    // ---- E. WAL / SST 尾部破损 ---------------------------------
    {
        auto& spec = add("E. 尾部破损", "D01-torn-header", "WAL 尾部只有半条记录头");
        spec.workload = Workload{60, 24, 0};
        spec.options.memtable_limit = 1u << 20;
        spec.damage = Damage::TornHeader;
        spec.min_truncated = 13;
        spec.note = "预期：截断 13 字节，前 60 条记录全部保留";
    }
    {
        auto& spec = add("E. 尾部破损", "D02-garbage-tail", "WAL 尾部是垃圾字节");
        spec.workload = Workload{60, 24, 0};
        spec.options.memtable_limit = 1u << 20;
        spec.damage = Damage::GarbageTail;
        spec.min_truncated = 5;
        spec.note = "预期：截断 5 字节，然后还能继续追加";
    }
    {
        auto& spec = add("E. 尾部破损", "D03-midfile-flip", "WAL 中间一个字节翻转");
        spec.workload = Workload{60, 24, 0};
        spec.options.memtable_limit = 1u << 20;
        spec.damage = Damage::MidFileFlip;
        spec.strict = false;
        spec.min_truncated = 1;
        spec.min_lost = 1;
        spec.note = "预期：从损坏记录起截断，后面的确认写入丢失（保守但正确）";
    }
    {
        auto& spec = add("E. 尾部破损", "D04-sst-corrupt", "已落盘 SST 损坏");
        spec.workload = Workload{120, 24, 3};
        spec.options.memtable_limit = 512;
        spec.options.compaction_trigger = 3;
        spec.close_cleanly = true;
        spec.damage = Damage::SstFlip;
        spec.expect_corruption = true;
        spec.verify_continuation = false;
        spec.min_lost = 0;
        spec.strict = false;
        spec.note = "预期：检测到 CRC 不匹配并拒绝启动，而不是返回错误数据";
    }

    // ---- F. 错误顺序 / 缺 fsync 的配置 -------------------------
    {
        auto& spec = add("F. 错误配置", "X01-wal-cut-before-sst-durable",
                         "SST 没落盘就截断 WAL");
        spec.workload = Workload{120, 24, 5};
        spec.options.memtable_limit = 384;
        spec.options.compaction_trigger = 4;
        spec.options.fsync_sst = false;
        spec.options.fsync_manifest = false;
        spec.crash_point = std::string(kFlushWalAfterTruncate);
        spec.occurrence = 2;
        spec.strict = false;
        spec.min_lost = 1;
        spec.note = "预期：丢数据。WAL 先被清空，而 SST/manifest 还在 page cache 里";
    }
    {
        auto& spec = add("F. 错误配置", "X02-manifest-not-synced",
                         "manifest 不 fsync");
        spec.workload = Workload{120, 24, 5};
        spec.options.memtable_limit = 384;
        spec.options.compaction_trigger = 4;
        spec.options.fsync_manifest = false;
        spec.crash_point = std::string(kFlushWalAfterTruncate);
        spec.occurrence = 3;
        spec.strict = false;
        spec.min_lost = 1;
        spec.note = "预期：丢数据。manifest 的 rename 不是持久化屏障，SST 变成孤儿";
    }
    {
        auto& spec = add("F. 错误配置", "X03-sst-not-synced",
                         "manifest 已提交但 SST 没 fsync");
        spec.workload = Workload{120, 24, 5};
        spec.options.memtable_limit = 384;
        spec.options.compaction_trigger = 4;
        spec.options.fsync_sst = false;
        spec.crash_point = std::string(kFlushWalAfterTruncate);
        spec.occurrence = 2;
        spec.expect_corruption = true;
        spec.verify_continuation = false;
        spec.strict = false;
        spec.note = "预期：fail-stop。manifest 指向一个 0 字节 SST，恢复必须拒绝启动";
    }

    return specs;
}

// ---------------------------------------------------------------- 输出

std::string format_ms(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    return buffer;
}

void print_group(const std::string& title) {
    std::cout << "\n-- " << title << " --\n";
    std::cout << std::left << std::setw(34) << "SCENARIO" << std::right << std::setw(6) << "ACK"
              << std::setw(5) << "LOST" << std::setw(5) << "RES" << std::setw(7) << "TRUNC"
              << std::setw(7) << "REPLAY" << std::setw(9) << "REOPEN" << "  VERDICT\n";
}

void print_result(const Result& result) {
    std::cout << std::left << std::setw(34) << result.id << std::right << std::setw(6)
              << result.acked << std::setw(5) << result.lost << std::setw(5)
              << result.resurrected << std::setw(7) << result.truncated << std::setw(7)
              << result.replayed << std::setw(9) << format_ms(result.recovery_ms) << "  "
              << (result.passed ? (result.corruption_detected ? "PASS*" : "PASS") : "FAIL")
              << '\n';
    if (!result.passed || result.lost > 0 || result.corruption_detected ||
        result.unacked_applied > 0) {
        std::cout << "    " << result.name << " | " << result.note;
        if (result.unacked_applied > 0) {
            std::cout << "（另有 " << result.unacked_applied
                      << " 条未确认的操作已生效，属于允许的提交）";
        }
        std::cout << '\n';
    }
}

// ---------------------------------------------------------------- 基准

void print_bench_header() {
    std::cout << std::left << std::setw(16) << "CONFIG" << std::right << std::setw(9)
              << "USER_B" << std::setw(10) << "WAL_B" << std::setw(11) << "SST_B" << std::setw(10)
              << "MFT_B" << std::setw(12) << "WRITE_B" << std::setw(8) << "AMP" << std::setw(7)
              << "SSTS" << std::setw(8) << "COMP" << std::setw(10) << "WALL_MS" << '\n';
}

bool bench_write_amplification() {
    std::cout << "\n-- G. 写放大：compaction 触发阈值的影响（1500 条写入，不 fsync） --\n";
    print_bench_header();
    bool ok = true;
    for (const std::size_t trigger : {std::size_t{2}, std::size_t{4}, std::size_t{8},
                                      std::size_t{1000000}}) {
        Engine::Options options;
        options.memtable_limit = 512;
        options.compaction_trigger = trigger;
        options.fsync_wal = false;
        options.fsync_sst = false;
        options.fsync_manifest = false;

        TempDir directory;
        const Workload workload{1500, 32, 0};
        const auto start = std::chrono::steady_clock::now();
        auto engine = std::make_unique<Engine>(directory.path(), options, nullptr);
        for (std::size_t index = 0; index < workload.ops; ++index) {
            engine->put(workload.key(index), workload.value(index));
        }
        engine->close();
        const auto elapsed = ms_since(start);
        const auto stats = engine->stats();

        const auto amplification = stats.user_bytes == 0
                                       ? 0.0
                                       : static_cast<double>(stats.total_write_bytes()) /
                                             static_cast<double>(stats.user_bytes);
        std::cout << std::left << std::setw(16)
                  << ("trigger=" + (trigger > 1000 ? std::string("off")
                                                  : std::to_string(trigger)))
                  << std::right << std::setw(9) << stats.user_bytes << std::setw(10)
                  << stats.wal_bytes << std::setw(11) << stats.sst_bytes << std::setw(10)
                  << stats.manifest_bytes << std::setw(12) << stats.total_write_bytes()
                  << std::setw(8) << std::fixed << std::setprecision(2) << amplification
                  << std::setprecision(6) << std::setw(7) << stats.ssts << std::setw(8)
                  << stats.compactions << std::setw(10) << format_ms(elapsed) << '\n';

        if (stats.total_write_bytes() < stats.user_bytes) {
            ok = false;
        }
        if (trigger <= 8 && stats.compactions == 0) {
            ok = false;
        }
    }
    return ok;
}

bool bench_latency() {
    std::cout << "\n-- H. fsync 对写入延迟的影响（1000 条写入，memtable 不 flush） --\n";
    std::cout << std::left << std::setw(18) << "MODE" << std::right << std::setw(7) << "OPS"
              << std::setw(10) << "P50_US" << std::setw(10) << "P99_US" << std::setw(10)
              << "MAX_US" << std::setw(12) << "WALL_MS" << std::setw(9) << "FSYNCS" << '\n';

    bool ok = true;
    for (const bool fsync : {false, true}) {
        Engine::Options options;
        options.memtable_limit = 1u << 20;
        options.compaction_trigger = 1000000;
        options.fsync_wal = fsync;
        options.fsync_sst = fsync;
        options.fsync_manifest = fsync;

        TempDir directory;
        const Workload workload{1000, 64, 0};
        std::vector<double> latencies;
        latencies.reserve(workload.ops);

        const auto start = std::chrono::steady_clock::now();
        auto engine = std::make_unique<Engine>(directory.path(), options, nullptr);
        for (std::size_t index = 0; index < workload.ops; ++index) {
            const auto op_start = std::chrono::steady_clock::now();
            engine->put(workload.key(index), workload.value(index));
            latencies.push_back(
                std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - op_start)
                    .count());
        }
        engine->close();
        const auto elapsed = ms_since(start);
        const auto stats = engine->stats();

        std::sort(latencies.begin(), latencies.end());
        const auto percentile = [&latencies](double ratio) {
            const auto index = static_cast<std::size_t>(
                static_cast<double>(latencies.size() - 1) * ratio);
            return latencies[index];
        };

        std::cout << std::left << std::setw(18)
                  << (fsync ? "fsync=on" : "fsync=off") << std::right << std::setw(7)
                  << workload.ops << std::setw(10) << std::fixed << std::setprecision(1)
                  << percentile(0.50) << percentile(0.99) << latencies.back()
                  << std::setprecision(2) << std::setw(12) << format_ms(elapsed)
                  << std::setw(9) << stats.fsyncs << '\n';

        // close() 里截断 WAL 一定会 fsync 一次，所以 off 也不是严格的 0。
        if (fsync && stats.fsyncs < workload.ops) {
            ok = false;
        }
        if (!fsync && stats.fsyncs > 2) {
            ok = false;
        }
    }
    return ok;
}

bool bench_recovery() {
    std::cout << "\n-- I. 恢复时间随数据量增长（memtable=4KB，compaction=8） --\n";
    std::cout << std::left << std::setw(10) << "OPS" << std::right << std::setw(12)
              << "ONDISK_B" << std::setw(7) << "SSTS" << std::setw(10) << "REPLAY"
              << std::setw(12) << "REOPEN_MS" << std::setw(12) << "WRITE_MS" << '\n';

    bool ok = true;
    for (const std::size_t ops : {std::size_t{500}, std::size_t{2000}, std::size_t{5000}}) {
        Engine::Options options;
        options.memtable_limit = 4096;
        options.compaction_trigger = 8;
        options.fsync_wal = false;
        options.fsync_sst = false;
        options.fsync_manifest = false;

        TempDir directory;
        const Workload workload{ops, 32, 0};
        auto engine = std::make_unique<Engine>(directory.path(), options, nullptr);
        const auto write_start = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < workload.ops; ++index) {
            engine->put(workload.key(index), workload.value(index));
        }
        engine->close();
        const auto write_ms = ms_since(write_start);
        engine.reset();

        std::uint64_t on_disk = 0;
        for (const auto& entry : fs::directory_iterator(directory.path())) {
            if (entry.is_regular_file()) {
                on_disk += file_size_of(entry.path());
            }
        }

        const auto open_start = std::chrono::steady_clock::now();
        Engine recovered(directory.path(), options, nullptr);
        const auto open_ms = ms_since(open_start);
        const auto stats = recovered.stats();

        std::cout << std::left << std::setw(10) << ops << std::right << std::setw(12)
                  << on_disk << std::setw(7) << stats.ssts << std::setw(10)
                  << stats.wal_records_replayed << std::setw(12) << std::fixed
                  << std::setprecision(2) << open_ms << std::setw(12) << write_ms << '\n';

        if (stats.ssts == 0 && ops > 100) {
            ok = false;
        }
    }
    return ok;
}

} // namespace

int main() {
    std::cout << "工作坊 A · 存储引擎压测与崩溃恢复\n";
    std::cout << "编译环境: C++20 / " << (sizeof(void*) * 8) << "-bit\n";

    const auto program_start = std::chrono::steady_clock::now();
    const auto specs = build_scenarios();

    std::vector<Result> results;
    results.reserve(specs.size());
    for (const auto& spec : specs) {
        try {
            results.push_back(execute(spec));
        } catch (const std::exception& error) {
            Result result;
            result.id = spec.id;
            result.name = spec.name;
            result.passed = false;
            result.note = std::string("harness threw: ") + error.what();
            results.push_back(std::move(result));
        }
    }

    std::string current_group;
    std::size_t failed = 0;
    std::size_t strict_total = 0;
    std::size_t strict_failed = 0;
    std::size_t demos = 0;
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& spec = specs[index];
        const auto& result = results[index];
        if (spec.group != current_group) {
            current_group = spec.group;
            print_group(current_group);
        }
        print_result(result);

        if (!result.passed) {
            ++failed;
        }
        if (spec.strict) {
            ++strict_total;
            if (!result.passed) {
                ++strict_failed;
            }
        }
        if (!spec.strict || spec.expect_corruption) {
            ++demos;
        }
    }

    std::cout << "\n== 基准 ==\n";
    const bool bench_amp_ok = bench_write_amplification();
    const bool bench_latency_ok = bench_latency();
    const bool bench_recovery_ok = bench_recovery();

    std::cout << "\n== 汇总 ==\n";
    std::cout << "场景数: " << results.size() << "（其中严格不变量场景 " << strict_total
              << "，预期失败/降级演示 " << demos << "）\n";
    std::cout << "严格不变量断言失败: " << strict_failed << " / " << strict_total << '\n';
    std::cout << "场景失败: " << failed << " / " << results.size() << '\n';
    std::cout << "基准自检: 写放大 " << (bench_amp_ok ? "OK" : "FAILED") << " / 延迟 "
              << (bench_latency_ok ? "OK" : "FAILED") << " / 恢复 "
              << (bench_recovery_ok ? "OK" : "FAILED") << '\n';
    std::cout << "总耗时: " << format_ms(ms_since(program_start)) << " ms\n";

    if (failed != 0 || !bench_amp_ok || !bench_latency_ok || !bench_recovery_ok) {
        std::cout << "\nFAILED\n";
        return 1;
    }
    std::cout << "\nAll " << results.size() << " scenarios passed.\n";
    return 0;
}
