// crash.hpp
//
// 可复现的崩溃注入。
//
// 为什么不用“随机时刻 kill 进程”：
//   - 随机 kill 不可复现，失败时无法重放；
//   - 真正有价值的崩溃点是设计里的“顺序边界”：
//     日志写完了但没 fsync、SST 写完了但 manifest 没提交、manifest 提交了但 WAL 没截断……
//   - 所以这里把崩溃点做成命名点位 + 第几次命中，命中的瞬间抛异常，
//     让引擎像“进程突然消失”一样离开，不执行任何清理。

#pragma once

#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace arch {

// 崩溃点位常量。名字就是设计里的顺序边界，报告和表格里直接引用它们。
inline constexpr std::string_view kWalBeforeAppend = "wal.before-append";
inline constexpr std::string_view kWalAfterAppend = "wal.after-append";
inline constexpr std::string_view kWalAfterDurability = "wal.after-durability";
inline constexpr std::string_view kWalAfterMemtable = "wal.after-memtable";

inline constexpr std::string_view kFlushSstBeforeRename = "flush.sst.before-rename";
inline constexpr std::string_view kFlushSstAfterRename = "flush.sst.after-rename";
inline constexpr std::string_view kFlushManifestBeforeCommit = "flush.manifest.before-commit";
inline constexpr std::string_view kFlushManifestAfterCommit = "flush.manifest.after-commit";
inline constexpr std::string_view kFlushWalBeforeTruncate = "flush.wal.before-truncate";
inline constexpr std::string_view kFlushWalAfterTruncate = "flush.wal.after-truncate";

inline constexpr std::string_view kCompactSstBeforeRename = "compact.sst.before-rename";
inline constexpr std::string_view kCompactSstAfterRename = "compact.sst.after-rename";
inline constexpr std::string_view kCompactManifestBeforeCommit = "compact.manifest.before-commit";
inline constexpr std::string_view kCompactManifestAfterCommit = "compact.manifest.after-commit";
inline constexpr std::string_view kCompactOldDeleted = "compact.old-deleted";

// 模拟“进程在这一点上被杀掉”：不是 std::terminate，而是可捕获的异常。
// 捕获方不会调用 close()，之后由 FaultDisk 模拟“未落盘的字节消失”。
class CrashSignal : public std::exception {
public:
    explicit CrashSignal(std::string point) : point_(std::move(point)) {}

    [[nodiscard]] const std::string& point() const noexcept { return point_; }
    [[nodiscard]] const char* what() const noexcept override { return "simulated crash"; }

private:
    std::string point_;
};

class CrashInjection {
public:
    struct Plan {
        std::string point;      // 空 = 永不崩溃
        int occurrence = 1;     // 第几次命中该点位时崩溃
    };

    CrashInjection() = default;
    explicit CrashInjection(Plan plan) : plan_(std::move(plan)) {}

    void hit(std::string_view point) {
        if (plan_.point.empty() || triggered_) {
            return;
        }
        if (point != std::string_view(plan_.point)) {
            return;
        }
        ++seen_;
        if (seen_ >= plan_.occurrence) {
            triggered_ = true;
            throw CrashSignal(std::string(point));
        }
    }

    [[nodiscard]] bool armed() const { return !plan_.point.empty(); }
    [[nodiscard]] bool triggered() const { return triggered_; }
    [[nodiscard]] int seen() const { return seen_; }

private:
    Plan plan_;
    int seen_ = 0;
    bool triggered_ = false;
};

} // namespace arch
