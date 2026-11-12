// main.cpp
//
// Day 13 的 ARIES 行为测试：redo、undo、steal、no-force、CLR 的可重启动、
// 检查点对 redo 起点的影响，以及“没 fsync 的提交不算提交”。
//
// 编译运行：
//   cmake -S . -B build -G Ninja
//   cmake --build build
//   ctest --test-dir build --output-on-failure
//   ./build/aries_demo            # 跑全部测试
//   ./build/aries_demo --trace    # 打印一次完整的崩溃恢复日志

#include "aries.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using archdesign::AriesEngine;
using archdesign::LogForce;
using archdesign::Lsn;
using archdesign::RecoveryReport;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void print_report(const RecoveryReport& report) {
    std::cout << "  checkpoint   = " << report.checkpoint_lsn << '\n'
              << "  redo window  = " << report.redo_start_lsn << " .. " << report.redo_end_lsn
              << '\n'
              << "  analysis     = " << report.analysis_records << " records\n"
              << "  redo         = " << report.redo_applied << " applied, "
              << report.redo_skipped << " skipped\n"
              << "  undo         = " << report.undo_actions << " actions, "
              << report.clrs_written << " CLRs, incomplete="
              << (report.undo_incomplete ? "yes" : "no") << '\n';

    std::cout << "  losers       = ";
    if (report.losers.empty()) {
        std::cout << "(none)\n";
    } else {
        for (std::size_t index = 0; index < report.losers.size(); ++index) {
            std::cout << (index == 0 ? "tx" : ", tx") << report.losers[index];
        }
        std::cout << '\n';
    }
}

// 1. no-force：提交只要求日志落盘，数据页还脏；崩溃后必须靠 redo 重放。
void test_redo_replays_committed_updates() {
    AriesEngine db(4);

    const auto tx = db.begin();
    db.update(tx, 1, 0, 100);
    db.update(tx, 1, 1, 200);
    db.commit(tx);

    check(db.dirty_pages() == 1, "commit must not force the dirty page out");
    check(db.disk_page_lsn(1) == 0, "the page must still be absent from disk");

    db.crash();
    const auto report = db.recover();

    check(db.read_slot(1, 0) == 100, "redo must restore slot 0");
    check(db.read_slot(1, 1) == 200, "redo must restore slot 1");
    check(report.redo_applied == 2, "both updates must be redone");
    check(report.undo_actions == 0, "a committed transaction is never undone");
    check(report.losers.empty(), "a committed transaction is not a loser");
}

// 2. steal + no-force：未提交的脏页被驱逐写回磁盘，恢复时必须撤销。
void test_steal_writes_uncommitted_page_then_undo() {
    AriesEngine db(1);  // 只有一帧，必然发生驱逐

    const auto a = db.begin();
    db.update(a, 1, 0, 42);  // 未提交

    const auto b = db.begin();
    db.update(b, 2, 0, 7);  // 需要新帧 -> 页 1 被写回磁盘

    check(db.disk_value(1, 0) == 42, "steal: an uncommitted value must reach disk");
    check(db.dirty_pages() == 1, "only page 2 should still be dirty");

    db.flush_log_tail();
    db.crash();
    const auto report = db.recover();

    check(report.losers.size() == 2, "both open transactions are losers");
    check(report.undo_actions == 2, "one undo action per logged update");
    check(report.clrs_written == 2, "every undo action is logged as a CLR");
    check(db.read_slot(1, 0) == 0, "undo must restore the before-image");
    check(db.read_slot(2, 0) == 0, "undo must clear the second page too");
    check(db.disk_value(1, 0) == 0, "the durable state must be the undone state");
}

// 3. 幂等：恢复跑两遍，第二遍什么都不该改。
void test_redo_is_idempotent() {
    AriesEngine db(4);

    const auto tx = db.begin();
    db.update(tx, 1, 0, 5);
    db.commit(tx);
    db.crash();

    const auto first = db.recover();
    check(first.redo_applied == 1, "first recovery must replay the update");
    check(db.read_slot(1, 0) == 5, "value after the first recovery");

    const auto second = db.recover();
    check(second.redo_applied == 0, "second recovery must skip everything via pageLSN");
    check(second.redo_skipped >= 1, "the update must be skipped, not applied");
    check(second.undo_actions == 0, "there is nothing left to undo");
    check(db.read_slot(1, 0) == 5, "idempotent recovery must not change the value");
}

// 4. CLR 是 redo-only：回滚过的修改，崩溃后不能“复活”。
void test_aborted_change_does_not_resurrect() {
    AriesEngine db(4);

    const auto tx = db.begin();
    db.update(tx, 1, 0, 99);
    const auto clrs = db.abort(tx);

    check(clrs == 1, "abort must write one CLR");
    check(db.read_slot(1, 0) == 0, "abort must restore the before-image");

    db.crash();  // 脏页丢掉：CLR 的效果只能靠 redo 重放
    const auto report = db.recover();

    check(report.losers.empty(), "an aborted transaction is finished, not a loser");
    check(report.undo_actions == 0, "recovery must not undo it a second time");
    check(report.redo_applied == 2, "the update and its CLR are both replayed");
    check(db.read_slot(1, 0) == 0, "redo of the CLR must keep the rollback");
}

// 5. 恢复途中再崩：CLR 的 undoNextLSN 让第二次恢复从断点继续。
void test_crash_during_undo_resumes_with_clr() {
    AriesEngine db(4);

    const auto tx = db.begin();
    db.update(tx, 1, 0, 10);
    db.update(tx, 1, 1, 20);
    db.update(tx, 1, 0, 30);  // slot 0: 0 -> 10 -> 30
    db.flush_log_tail();
    db.crash();

    const auto first = db.recover(1);  // 只允许撤销一步
    check(first.undo_incomplete, "one step cannot roll back three updates");
    check(first.undo_actions == 1, "exactly one undo action");
    check(db.read_slot(1, 0) == 10, "the newest update is undone first");
    check(db.read_slot(1, 1) == 20, "older updates are still applied");

    db.crash();  // 恢复做着做着又崩了
    const auto second = db.recover();

    check(!second.undo_incomplete, "the resumed recovery must finish");
    check(second.undo_actions == 2, "the CLR tells recovery where to resume");
    check(second.redo_applied >= 1, "redo must replay the CLR it finds in the log");
    check(db.read_slot(1, 0) == 0, "slot 0 must be fully undone");
    check(db.read_slot(1, 1) == 0, "slot 1 must be fully undone");
}

// 6a. 检查点时没有脏页：redo 可以从检查点之后开始。
void test_clean_checkpoint_advances_redo_start() {
    AriesEngine db(4);

    const auto a = db.begin();
    db.update(a, 1, 0, 11);
    db.commit(a);
    db.flush_all_pages();  // 页 1 干净了
    db.checkpoint();
    const Lsn checkpoint = db.last_checkpoint_lsn();

    const auto b = db.begin();
    db.update(b, 2, 0, 22);
    db.commit(b);
    db.crash();

    const auto report = db.recover();
    check(report.checkpoint_lsn == checkpoint, "recovery must use the checkpoint");
    check(report.redo_start_lsn > checkpoint, "clean checkpoint lets redo start after it");
    check(report.redo_applied == 1, "only the post-checkpoint update needs redo");
    check(db.read_slot(1, 0) == 11, "the pre-checkpoint page is already durable");
    check(db.read_slot(2, 0) == 22, "the post-checkpoint page is replayed");
}

// 6b. 检查点时还有脏页：redo 必须回到脏页的 recLSN，也就是检查点之前。
void test_dirty_checkpoint_keeps_redo_start_early() {
    AriesEngine db(4);

    const auto a = db.begin();
    db.update(a, 1, 0, 11);  // 脏页，未提交，也没刷
    db.checkpoint();
    const Lsn checkpoint = db.last_checkpoint_lsn();
    db.crash();

    const auto report = db.recover();
    check(report.redo_start_lsn < checkpoint, "a dirty page keeps redo before the checkpoint");
    check(report.losers.size() == 1, "the open transaction is a loser");
    check(db.read_slot(1, 0) == 0, "the uncommitted update is redone and then undone");
}

// 7. 没 fsync 的提交不是提交：数据页已经在磁盘上，事务照样被回滚。
void test_unflushed_commit_is_lost() {
    AriesEngine db(4);

    const auto tx = db.begin();
    db.update(tx, 1, 0, 77);
    db.flush_page(1);  // 数据页先落盘了（WAL 保证日志也落到了这条更新）

    check(db.disk_value(1, 0) == 77, "the stolen page already carries the value");

    db.commit(tx, LogForce::No);  // 提交记录没等 fsync
    db.crash();                   // 掉电：提交记录丢了

    const auto report = db.recover();
    check(report.losers.size() == 1, "without a durable commit record the tx is a loser");
    check(report.undo_actions == 1, "its update must be undone");
    check(db.read_slot(1, 0) == 0, "an unflushed commit must not survive");
    check(db.disk_value(1, 0) == 0, "the durable value must be rolled back");
}

// 8. WAL：数据页落盘之前，日志必须已经落盘。
void test_wal_flushes_log_before_page() {
    AriesEngine db(4);

    const auto tx = db.begin();
    db.update(tx, 1, 0, 5);

    check(db.disk_page_lsn(1) == 0, "the page has not been written yet");
    check(db.flushed_lsn() < db.read_page_lsn(1), "the log is still behind the page");

    db.flush_page(1);

    check(db.disk_page_lsn(1) == db.read_page_lsn(1), "page and frame must agree");
    check(db.flushed_lsn() >= db.disk_page_lsn(1),
          "WAL: flushedLSN must be at least the page LSN before the page hits disk");
    check(db.disk_value(1, 0) == 5, "the page content is on disk");
}

}  // namespace

namespace {

int run_trace() {
    std::cout << "== 1. 正常运行：两个事务交错，缓冲池只有 1 帧（强制 steal）==\n";
    AriesEngine db(1);

    const auto a = db.begin();
    db.update(a, 1, 0, 111);
    const auto b = db.begin();
    db.update(b, 2, 0, 222);
    db.update(b, 1, 1, 333);
    db.flush_log_tail();

    std::cout << db.dump_log();
    std::cout << "flushedLSN = " << db.flushed_lsn() << '\n';
    std::cout << db.dump_disk();

    std::cout << "\n== 2. 掉电：脏页和未 fsync 的日志尾部一起消失 ==\n";
    db.crash();

    std::cout << "\n== 3. 恢复 ==\n";
    const auto report = db.recover();
    print_report(report);

    std::cout << "\n恢复后的日志（多了 CLR 和 ABORT）：\n" << db.dump_log();
    std::cout << "恢复后的磁盘：\n" << db.dump_disk();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--trace") == 0) {
        return run_trace();
    }

    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"redo_replays_committed_updates", test_redo_replays_committed_updates},
        {"steal_writes_uncommitted_page_then_undo", test_steal_writes_uncommitted_page_then_undo},
        {"redo_is_idempotent", test_redo_is_idempotent},
        {"aborted_change_does_not_resurrect", test_aborted_change_does_not_resurrect},
        {"crash_during_undo_resumes_with_clr", test_crash_during_undo_resumes_with_clr},
        {"clean_checkpoint_advances_redo_start", test_clean_checkpoint_advances_redo_start},
        {"dirty_checkpoint_keeps_redo_start_early", test_dirty_checkpoint_keeps_redo_start_early},
        {"unflushed_commit_is_lost", test_unflushed_commit_is_lost},
        {"wal_flushes_log_before_page", test_wal_flushes_log_before_page},
    };

    int failed = 0;
    for (const auto& [name, run] : tests) {
        try {
            run();
            std::cout << "[  OK  ] " << name << '\n' << std::flush;
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAILED] " << name << ": " << error.what() << '\n' << std::flush;
        } catch (...) {
            ++failed;
            std::cerr << "[FAILED] " << name << ": unknown exception\n" << std::flush;
        }
    }

    if (failed != 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "\nAll " << tests.size() << " tests passed.\n";
    return 0;
}
