// aries.hpp
//
// Day 13 的最小 ARIES 实现：Analysis / Redo / Undo 三阶段恢复，
// 物理日志 + pageLSN + recLSN + flushedLSN，CLR（补偿日志），模糊检查点。
//
// 这是教学实现，不是生产实现。刻意做的简化：
//   - 单线程，没有 latch / 锁管理器（靠“严格两阶段锁”的假设保证 undo 安全）；
//   - 页 = 8 个 64 位定长槽，日志里记的是槽的新旧值，不是任意字节；
//   - LSN 是日志记录的序号，不是字节偏移；
//   - 日志只增不截断；崩溃只丢“未 fsync 的尾部”，不模拟坏校验和/半条记录；
//   - 检查点写成一条记录（论文里是 begin_checkpoint + end_checkpoint）；
//   - 没有嵌套顶层动作（SMO）、没有 2PC、没有日志归档。
//
// 这些简化都在文章的“生产化缺口”里列出来了。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace archdesign {

// 日志序号。0 表示“没有”。
using Lsn = std::uint64_t;
using TxId = std::uint32_t;
using PageId = std::uint32_t;
using SlotId = std::uint32_t;

inline constexpr Lsn kNoLsn = 0;
inline constexpr PageId kNoPage = 0;
inline constexpr std::size_t kPageSlots = 8;
inline constexpr std::size_t kUnlimitedUndo = std::numeric_limits<std::size_t>::max();

enum class LogType : std::uint8_t {
    Begin,       // 事务开始（undo 链的头）
    Update,      // 物理更新：page/slot 的 before -> after
    Commit,      // 提交
    Abort,       // 事务结束（正常回滚完成，或恢复期撤销完成）
    Clr,         // 补偿日志：一条“撤销动作”的 redo-only 记录
    Checkpoint,  // 模糊检查点：携带 DPT + 活跃事务表
};

enum class LogForce {
    Yes,  // 等日志落盘（提交必须用这个）
    No,   // 不等 fsync（用来演示“提交没落盘 = 没提交”）
};

inline const char* to_string(LogType type) {
    switch (type) {
    case LogType::Begin:
        return "BEGIN";
    case LogType::Update:
        return "UPDATE";
    case LogType::Commit:
        return "COMMIT";
    case LogType::Abort:
        return "ABORT";
    case LogType::Clr:
        return "CLR";
    case LogType::Checkpoint:
        return "CHECKPOINT";
    }
    return "?";
}

// 一个数据页。page_lsn = 最后一次修改这个页的日志记录的 LSN（InnoDB 的 FIL_PAGE_LSN）。
struct Page {
    std::array<std::uint64_t, kPageSlots> slots{};
    Lsn page_lsn = kNoLsn;
};

// 一条日志记录。真实系统里 after/before 是任意字节；这里用 64 位值代替。
struct LogRecord {
    Lsn lsn = kNoLsn;
    Lsn prev_lsn = kNoLsn;  // 同一事务的上一条日志，构成 undo 链
    TxId txid = 0;
    LogType type = LogType::Begin;

    PageId page = kNoPage;
    SlotId slot = 0;
    std::uint64_t before = 0;  // 旧值：undo 时写回
    std::uint64_t after = 0;   // 新值：redo 时写入

    // 仅 CLR 使用：这条 CLR 撤掉之后，undo 游标该跳到哪里。
    Lsn undo_next_lsn = kNoLsn;

    // 仅 Checkpoint 使用。
    std::map<PageId, Lsn> dirty_page_table;  // page -> recLSN
    std::map<TxId, Lsn> active_table;        // tx -> lastLSN
};

// 一次恢复的可观测结果。测试和文章里的数字都来自这里。
struct RecoveryReport {
    Lsn checkpoint_lsn = kNoLsn;    // 本次恢复用的检查点
    Lsn redo_start_lsn = kNoLsn;    // redo 起点 = min(recLSN)
    Lsn redo_end_lsn = kNoLsn;      // 日志末尾
    std::size_t analysis_records = 0;
    std::size_t redo_applied = 0;   // 真正重放的页更新数
    std::size_t redo_skipped = 0;   // 因为 pageLSN 已经更新而跳过的
    std::size_t undo_actions = 0;   // 撤销了几个 Update
    std::size_t clrs_written = 0;   // 写了几条 CLR
    bool undo_incomplete = false;   // 预算用完，undo 没做完
    std::vector<TxId> losers;       // 需要回滚的事务
    std::map<PageId, Lsn> dirty_pages;  // 分析阶段结束时的 DPT
};

class AriesEngine {
public:
    explicit AriesEngine(std::size_t frames = 4);

    // ---- 正常运行路径 ----
    TxId begin();
    void update(TxId tx, PageId page, SlotId slot, std::uint64_t value);
    void commit(TxId tx, LogForce force = LogForce::Yes);
    std::size_t abort(TxId tx);  // 正常回滚，返回写了多少条 CLR
    void checkpoint();           // 模糊检查点：只刷日志，不刷数据页
    void flush_page(PageId page);  // 遵守 WAL：先刷日志到 pageLSN，再写页
    void flush_all_pages();
    void flush_log_tail();       // 后台日志写线程把日志刷到末尾

    // ---- 故障与恢复 ----
    void crash();  // 丢掉缓冲池里的脏页 + 丢掉未 fsync 的日志尾部
    RecoveryReport recover(std::size_t max_undo_actions = kUnlimitedUndo);

    // ---- 观测 ----
    std::uint64_t read_slot(PageId page, SlotId slot);
    Lsn read_page_lsn(PageId page);  // 缓冲池里那份页的 pageLSN
    std::uint64_t disk_value(PageId page, SlotId slot) const;
    Lsn disk_page_lsn(PageId page) const;
    Lsn flushed_lsn() const { return durable_hw_; }
    Lsn last_lsn() const;
    Lsn last_checkpoint_lsn() const { return checkpoint_lsn_; }
    std::size_t log_records() const { return log_.size(); }
    std::size_t dirty_pages() const { return dirty_.size(); }
    std::size_t buffer_frames() const { return frames_.size(); }
    std::string dump_log() const;
    std::string dump_disk() const;

private:
    struct TxState {
        Lsn last_lsn = kNoLsn;
    };

    // 缓冲池
    Page& fetch(PageId page);
    void evict_if_needed();

    // 日志
    Lsn append(LogRecord record);
    void flush_log(Lsn upto);
    const LogRecord& log_at(Lsn lsn) const;

    // 撤销
    Lsn undo_one(const LogRecord& record, TxId tx);
    Lsn next_undo_target(Lsn cursor) const;  // 跳过 CLR/BEGIN，找到下一条 Update
    void write_end_record(TxId tx);

    std::size_t capacity_ = 4;

    std::map<PageId, Page> disk_;    // 磁盘上的页
    std::map<PageId, Page> frames_;  // 缓冲池里的页副本
    std::vector<PageId> order_;      // FIFO 驱逐顺序
    std::set<PageId> dirty_;         // 缓冲池里的脏页
    std::map<PageId, Lsn> rec_lsn_;  // 脏页第一次变脏的 LSN（DPT 的运行时版本）

    std::map<Lsn, LogRecord> log_;  // 内存日志（含未 fsync 的尾部）
    Lsn next_lsn_ = 1;              // LSN 单调递增，崩溃后不回退
    Lsn durable_hw_ = kNoLsn;       // flushedLSN：已经 fsync 到的最高 LSN

    std::map<TxId, TxState> active_;  // 活跃事务表
    TxId next_txid_ = 1;
    Lsn checkpoint_lsn_ = kNoLsn;  // 最近一次检查点
};

// ---------------------------------------------------------------------------
// 实现
// ---------------------------------------------------------------------------

inline AriesEngine::AriesEngine(std::size_t frames) : capacity_(frames == 0 ? 1 : frames) {}

inline std::uint64_t AriesEngine::read_slot(PageId page, SlotId slot) {
    if (slot >= kPageSlots) {
        throw std::out_of_range("slot out of range");
    }
    return fetch(page).slots[slot];
}

inline std::uint64_t AriesEngine::disk_value(PageId page, SlotId slot) const {
    if (slot >= kPageSlots) {
        throw std::out_of_range("slot out of range");
    }
    const auto it = disk_.find(page);
    return it == disk_.end() ? 0 : it->second.slots[slot];
}

inline Lsn AriesEngine::read_page_lsn(PageId page) {
    return fetch(page).page_lsn;
}

inline Lsn AriesEngine::disk_page_lsn(PageId page) const {
    const auto it = disk_.find(page);
    return it == disk_.end() ? kNoLsn : it->second.page_lsn;
}

inline Lsn AriesEngine::last_lsn() const {
    return log_.empty() ? kNoLsn : log_.rbegin()->first;
}

// ---- 缓冲池 ----

inline Page& AriesEngine::fetch(PageId page) {
    if (const auto it = frames_.find(page); it != frames_.end()) {
        return it->second;
    }
    evict_if_needed();

    Page loaded;
    if (const auto disk = disk_.find(page); disk != disk_.end()) {
        loaded = disk->second;  // 注意：pageLSN 一起从磁盘加载回来
    }
    const auto [pos, inserted] = frames_.emplace(page, loaded);
    (void)inserted;
    order_.push_back(page);
    return pos->second;
}

inline void AriesEngine::evict_if_needed() {
    while (frames_.size() >= capacity_ && !order_.empty()) {
        const PageId victim = order_.front();
        order_.erase(order_.begin());
        flush_page(victim);  // steal：可能把未提交的脏页写到磁盘
        frames_.erase(victim);
    }
}

inline void AriesEngine::flush_page(PageId page) {
    const auto it = frames_.find(page);
    if (it == frames_.end()) {
        return;
    }
    // WAL 规则：日志必须先于数据页落盘。
    flush_log(it->second.page_lsn);

    disk_[page] = it->second;
    dirty_.erase(page);
    rec_lsn_.erase(page);
}

inline void AriesEngine::flush_all_pages() {
    std::vector<PageId> pages;
    pages.reserve(frames_.size());
    for (const auto& entry : frames_) {
        pages.push_back(entry.first);
    }
    for (const PageId page : pages) {
        flush_page(page);
    }
}

// ---- 日志 ----

inline Lsn AriesEngine::append(LogRecord record) {
    record.lsn = next_lsn_++;
    const Lsn lsn = record.lsn;
    log_.emplace(lsn, std::move(record));
    return lsn;
}

inline void AriesEngine::flush_log(Lsn upto) {
    if (upto == kNoLsn) {
        return;
    }
    if (upto > last_lsn()) {
        throw std::logic_error("flush_log: LSN " + std::to_string(upto) + " does not exist");
    }
    if (upto > durable_hw_) {
        durable_hw_ = upto;
    }
}

inline void AriesEngine::flush_log_tail() {
    flush_log(last_lsn());
}

inline const LogRecord& AriesEngine::log_at(Lsn lsn) const {
    const auto it = log_.find(lsn);
    if (it == log_.end()) {
        throw std::logic_error("log_at: missing LSN " + std::to_string(lsn));
    }
    return it->second;
}

// ---- 正常路径 ----

inline TxId AriesEngine::begin() {
    const TxId tx = next_txid_++;
    LogRecord record;
    record.txid = tx;
    record.type = LogType::Begin;
    const Lsn lsn = append(std::move(record));
    active_[tx] = TxState{lsn};
    return tx;
}

inline void AriesEngine::update(TxId tx, PageId page, SlotId slot, std::uint64_t value) {
    const auto state = active_.find(tx);
    if (state == active_.end()) {
        throw std::logic_error("update: transaction " + std::to_string(tx) + " is not active");
    }
    if (slot >= kPageSlots) {
        throw std::out_of_range("slot out of range");
    }

    // 先把页读进来（可能触发驱逐，见 steal）。
    Page& frame = fetch(page);

    LogRecord record;
    record.prev_lsn = state->second.last_lsn;
    record.txid = tx;
    record.type = LogType::Update;
    record.page = page;
    record.slot = slot;
    record.before = frame.slots[slot];
    record.after = value;
    const Lsn lsn = append(std::move(record));

    frame.slots[slot] = value;
    frame.page_lsn = lsn;
    // 只有“第一次弄脏”才记 recLSN，而且必须取最小值，否则会漏 redo。
    if (dirty_.insert(page).second) {
        rec_lsn_[page] = lsn;
    }
    state->second.last_lsn = lsn;
}

inline void AriesEngine::commit(TxId tx, LogForce force) {
    const auto state = active_.find(tx);
    if (state == active_.end()) {
        throw std::logic_error("commit: transaction " + std::to_string(tx) + " is not active");
    }

    LogRecord record;
    record.prev_lsn = state->second.last_lsn;
    record.txid = tx;
    record.type = LogType::Commit;
    const Lsn lsn = append(std::move(record));

    // 提交点：日志必须先落盘，数据页不用（no-force）。
    if (force == LogForce::Yes) {
        flush_log(lsn);
    }
    active_.erase(tx);
}

inline Lsn AriesEngine::next_undo_target(Lsn cursor) const {
    while (cursor != kNoLsn) {
        const LogRecord& record = log_at(cursor);
        if (record.type == LogType::Clr) {
            cursor = record.undo_next_lsn;  // CLR 不能被 undo，只能跳过去
            continue;
        }
        if (record.type != LogType::Update) {
            cursor = record.prev_lsn;  // BEGIN / COMMIT / ABORT：沿链继续往前
            continue;
        }
        return cursor;
    }
    return kNoLsn;
}

inline Lsn AriesEngine::undo_one(const LogRecord& record, TxId tx) {
    const auto state = active_.find(tx);
    if (state == active_.end()) {
        throw std::logic_error("undo: transaction " + std::to_string(tx) + " is not active");
    }

    LogRecord clr;
    clr.prev_lsn = state->second.last_lsn;
    clr.txid = tx;
    clr.type = LogType::Clr;
    clr.page = record.page;
    clr.slot = record.slot;
    clr.before = record.after;   // 被撤销掉的值
    clr.after = record.before;   // 撤销之后页上应该是什么（redo 写这个）
    clr.undo_next_lsn = record.prev_lsn;  // 下次从哪继续 undo
    const Lsn lsn = append(std::move(clr));

    Page& frame = fetch(record.page);
    frame.slots[record.slot] = record.before;
    frame.page_lsn = lsn;
    if (dirty_.insert(record.page).second) {
        rec_lsn_[record.page] = lsn;
    }
    state->second.last_lsn = lsn;
    return lsn;
}

inline void AriesEngine::write_end_record(TxId tx) {
    LogRecord record;
    record.prev_lsn = active_[tx].last_lsn;
    record.txid = tx;
    record.type = LogType::Abort;
    append(std::move(record));
    active_.erase(tx);
}

inline std::size_t AriesEngine::abort(TxId tx) {
    if (active_.find(tx) == active_.end()) {
        throw std::logic_error("abort: transaction " + std::to_string(tx) + " is not active");
    }

    std::size_t clrs = 0;
    Lsn cursor = active_[tx].last_lsn;
    while ((cursor = next_undo_target(cursor)) != kNoLsn) {
        const LogRecord& target = log_at(cursor);
        const Lsn clr_lsn = undo_one(target, tx);
        (void)clr_lsn;
        ++clrs;
        cursor = target.prev_lsn;
    }

    // 回滚的“决定”也要落盘，否则崩溃后这条事务会被当成 loser 再撤一次（结果一样，但白做功）。
    write_end_record(tx);
    flush_log_tail();
    return clrs;
}

inline void AriesEngine::checkpoint() {
    LogRecord record;
    record.type = LogType::Checkpoint;
    record.dirty_page_table = rec_lsn_;  // 模糊：脏页还在内存里，不刷
    for (const auto& entry : active_) {
        record.active_table[entry.first] = entry.second.last_lsn;
    }
    const Lsn lsn = append(std::move(record));

    // 检查点记录本身要落盘，否则崩溃后“以为有这个检查点”是错的。
    flush_log(lsn);
    checkpoint_lsn_ = lsn;
}

// ---- 故障 ----

inline void AriesEngine::crash() {
    // 缓冲池里的脏页全丢；磁盘上只有之前写回的版本。
    frames_.clear();
    order_.clear();
    dirty_.clear();
    rec_lsn_.clear();
    active_.clear();  // 事务表在内存里，也丢

    // 日志只保留已 fsync 的部分；尾部丢掉。LSN 不回退（真实系统用 timeline 区分分叉）。
    for (auto it = log_.begin(); it != log_.end();) {
        if (it->first > durable_hw_) {
            it = log_.erase(it);
        } else {
            ++it;
        }
    }
    if (checkpoint_lsn_ > durable_hw_) {
        checkpoint_lsn_ = kNoLsn;  // 检查点记录本身丢了
    }
}

// ---- 恢复 ----

inline RecoveryReport AriesEngine::recover(std::size_t max_undo_actions) {
    RecoveryReport report;

    // ---------- 阶段 1：Analysis ----------
    // 目标：重建 DPT（脏页 -> recLSN）和 ATT（活跃事务 -> lastLSN），确定 redo 起点和 losers。
    std::map<PageId, Lsn> dpt;
    std::map<TxId, Lsn> att;

    auto cursor = log_.begin();
    if (checkpoint_lsn_ != kNoLsn) {
        const auto ckpt = log_.find(checkpoint_lsn_);
        if (ckpt == log_.end()) {
            checkpoint_lsn_ = kNoLsn;
        } else {
            dpt = ckpt->second.dirty_page_table;
            att = ckpt->second.active_table;
            report.checkpoint_lsn = checkpoint_lsn_;
            cursor = std::next(ckpt);  // 从检查点之后开始扫
        }
    }

    for (; cursor != log_.end(); ++cursor) {
        const LogRecord& record = cursor->second;
        ++report.analysis_records;

        switch (record.type) {
        case LogType::Update:
        case LogType::Clr:
            att[record.txid] = record.lsn;  // 最新的 undo 起点
            if (record.page != kNoPage) {
                // 只记第一次，也就是最小的 recLSN。
                dpt.emplace(record.page, record.lsn);
            }
            break;
        case LogType::Commit:
        case LogType::Abort:
            att.erase(record.txid);  // 已结束，不是 loser
            break;
        case LogType::Checkpoint:
            // 只从最新的检查点开始扫，正常不会遇到；遇到就以它为准。
            dpt = record.dirty_page_table;
            att = record.active_table;
            break;
        case LogType::Begin:
            att.emplace(record.txid, record.lsn);
            break;
        }
    }

    for (const auto& entry : att) {
        report.losers.push_back(entry.first);
    }

    // redo 起点 = 所有脏页里最小的 recLSN。这一步是 ARIES 的关键：
    // “从最后一次检查点开始重放”是错的，必须从最早的脏页开始。
    Lsn redo_start = kNoLsn;
    for (const auto& entry : dpt) {
        if (redo_start == kNoLsn || entry.second < redo_start) {
            redo_start = entry.second;
        }
    }
    if (redo_start == kNoLsn) {
        redo_start = last_lsn() + 1;  // 一个脏页都没有：没有东西要 redo
    }

    report.redo_start_lsn = redo_start;
    report.redo_end_lsn = last_lsn();
    report.dirty_pages = dpt;

    // ---------- 阶段 2：Redo ----------
    // 重复历史：所有更新都重放一遍，包括之后要回滚的事务（rolling back）。
    // 幂等性靠 pageLSN：页上已经 >= 这条日志的 LSN，就说明它（以及之前的）已经生效。
    rec_lsn_ = dpt;
    dirty_.clear();

    for (auto it = log_.lower_bound(redo_start); it != log_.end(); ++it) {
        const LogRecord& record = it->second;
        if (record.type != LogType::Update && record.type != LogType::Clr) {
            continue;
        }

        const auto rec = dpt.find(record.page);
        if (rec == dpt.end() || rec->second > record.lsn) {
            // 这个页在崩溃时是干净的（已经刷过），最多重放到 recLSN 就够。
            ++report.redo_skipped;
            continue;
        }

        Page& frame = fetch(record.page);
        if (frame.page_lsn >= record.lsn) {
            ++report.redo_skipped;
            continue;
        }

        frame.slots[record.slot] = record.after;
        frame.page_lsn = record.lsn;
        if (dirty_.insert(record.page).second) {
            rec_lsn_[record.page] = record.lsn;
        }
        ++report.redo_applied;
    }

    // ---------- 阶段 3：Undo ----------
    // 从所有 loser 里挑“最大 lastLSN”先撤，保证全局逆序。
    active_.clear();
    for (const auto& entry : att) {
        active_[entry.first] = TxState{entry.second};
    }

    std::map<Lsn, TxId> pending;  // undoNextLSN -> txid
    for (const auto& entry : att) {
        pending.emplace(entry.second, entry.first);
    }

    std::size_t budget = max_undo_actions;
    while (!pending.empty()) {
        auto pick = std::prev(pending.end());  // LSN 最大的那个 loser
        const TxId tx = pick->second;
        const Lsn target = next_undo_target(pick->first);

        if (target == kNoLsn) {
            pending.erase(pick);
            write_end_record(tx);  // 这个 loser 撤完了
            continue;
        }
        if (budget == 0) {
            report.undo_incomplete = true;
            break;
        }
        --budget;

        pending.erase(pick);
        const LogRecord copy = log_at(target);  // undo_one 会写新记录，先拷一份
        const Lsn clr_lsn = undo_one(copy, tx);
        ++report.undo_actions;
        ++report.clrs_written;
        pending.emplace(clr_lsn, tx);  // 下一轮从 CLR 的 undoNextLSN 继续
    }

    // 恢复结束：完整跑完时把恢复过的页写回，让磁盘终态自洽（真实系统是异步刷的）。
    // 没跑完（undo_incomplete）就不写页：等价于“页写入器还没来得及动手就又崩了”，
    // 这正是 CLR 存在的理由——页上还没有撤销效果，下次恢复只能靠日志里的 CLR 重放出来。
    flush_log_tail();  // CLR 必须落盘，否则这次撤销就白做了

    if (!report.undo_incomplete) {
        flush_all_pages();
    }
    return report;
}

// ---- 调试输出 ----

inline std::string AriesEngine::dump_log() const {
    std::ostringstream out;
    for (const auto& entry : log_) {
        const LogRecord& record = entry.second;
        out << "LSN " << record.lsn << "  tx" << record.txid << "  " << to_string(record.type);
        if (record.type == LogType::Update || record.type == LogType::Clr) {
            out << "  page=" << record.page << " slot=" << record.slot << "  " << record.before
                << " -> " << record.after;
        }
        if (record.type == LogType::Clr) {
            out << "  undoNext=" << record.undo_next_lsn;
        }
        out << "  (prev=" << record.prev_lsn << ")";
        if (record.type == LogType::Checkpoint) {
            out << "  DPT={";
            bool first = true;
            for (const auto& page : record.dirty_page_table) {
                out << (first ? "" : ",") << page.first << "@" << page.second;
                first = false;
            }
            out << "}";
        }
        out << '\n';
    }
    return out.str();
}

inline std::string AriesEngine::dump_disk() const {
    std::ostringstream out;
    for (const auto& entry : disk_) {
        std::vector<std::string> values;
        for (std::size_t slot = 0; slot < kPageSlots; ++slot) {
            if (entry.second.slots[slot] != 0) {
                values.push_back("[" + std::to_string(slot) + "]=" +
                                 std::to_string(entry.second.slots[slot]));
            }
        }
        out << "disk page " << entry.first << "  pageLSN=" << entry.second.page_lsn << "  ";
        if (values.empty()) {
            out << "(empty)";
        } else {
            for (std::size_t index = 0; index < values.size(); ++index) {
                out << (index == 0 ? "" : " ") << values[index];
            }
        }
        out << '\n';
    }
    return out.str();
}

}  // namespace archdesign
