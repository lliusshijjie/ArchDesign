// mvcc_store.hpp
//
// Day 12 的教学实现：一个最小 MVCC 键值存储。
//
// 直觉：
//   每次写入不是覆盖旧值，而是追加一个“版本”；
//   每个事务开始时保存一个快照（哪些事务还没结束）；
//   读取时从新到旧找“对这个快照可见”的第一个版本。
//
// 版本字段：
//   xmin  = 创建这个版本的事务
//   xmax  = 删除/更新这个版本的事务（0 表示还没被删）
//   value / tombstone
//
// 可见性规则（简化 PostgreSQL 风格）：
//   创建者：自己的写可见；已提交、且在快照之前、且不在快照活跃集合中，才可见。
//   删除者：自己的删除可见；已提交、且在快照之前、且不在活跃集合中，才算删掉。
//
// 省略：
//   - 真正的 undo/rollback 链、WAL/redo；
//   - 可串行化冲突检测、间隙锁；
//   - 索引可见性、范围扫描；
//   - 长事务 horizon、增量 purge、VACUUM；
//   - 多线程 latch。

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace archdesign {

using TxId = std::uint64_t;

class MVCCWriteConflict : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class MVCCStore {
public:
    struct Snapshot {
        TxId xmax = 0;  // 快照开始时下一个事务 id
        std::unordered_set<TxId> active;
    };

    struct Version {
        TxId xmin = 0;
        TxId xmax = 0;
        std::string value;
        bool tombstone = false;
    };

    struct Stats {
        std::size_t keys = 0;
        std::size_t versions = 0;
        std::size_t active_transactions = 0;
    };

    TxId begin() {
        const TxId id = next_txid_++;
        Snapshot snapshot;
        snapshot.xmax = id;
        snapshot.active = active_;

        transactions_.emplace(
            id, Transaction{id, std::move(snapshot), Status::Active});
        active_.insert(id);
        return id;
    }

    void commit(TxId txid) {
        auto& tx = find_active(txid);
        tx.status = Status::Committed;
        active_.erase(txid);
    }

    void abort(TxId txid) {
        auto& tx = find_active(txid);
        tx.status = Status::Aborted;
        active_.erase(txid);
    }

    void put(TxId txid, const std::string& key, std::string value) {
        auto& tx = find_active(txid);
        auto& versions = data_[key];
        check_write_conflict(versions, txid, tx);

        if (!versions.empty()) {
            auto& latest = versions.back();
            if (latest.xmax == 0 || status_of(latest.xmax) == Status::Aborted ||
                latest.xmax == txid) {
                latest.xmax = txid;
            }
        }
        versions.push_back(Version{txid, 0, std::move(value), false});
    }

    void erase(TxId txid, const std::string& key) {
        auto& tx = find_active(txid);
        auto& versions = data_[key];
        check_write_conflict(versions, txid, tx);

        if (!versions.empty()) {
            auto& latest = versions.back();
            if (latest.xmax == 0 || status_of(latest.xmax) == Status::Aborted ||
                latest.xmax == txid) {
                latest.xmax = txid;
            }
        }
        versions.push_back(Version{txid, 0, "", true});
    }

    [[nodiscard]] std::optional<std::string> get(TxId txid,
                                                 const std::string& key) const {
        const auto tx_it = transactions_.find(txid);
        if (tx_it == transactions_.end() ||
            tx_it->second.status != Status::Active) {
            throw std::logic_error("get from inactive transaction");
        }
        const Transaction& tx = tx_it->second;

        const auto data_it = data_.find(key);
        if (data_it == data_.end()) {
            return std::nullopt;
        }

        const auto& versions = data_it->second;
        for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
            if (visible(*it, tx)) {
                if (it->tombstone) {
                    return std::nullopt;
                }
                return it->value;
            }
        }
        return std::nullopt;
    }

    // 没有活跃事务时，做一次保守清理：只保留每个 key 最新的已提交版本。
    // 若最新版本是 tombstone，则整个 key 可以删掉。
    std::size_t purge() {
        if (!active_.empty()) {
            return 0;  // 有活跃快照，保守不清理
        }

        std::size_t removed = 0;
        for (auto it = data_.begin(); it != data_.end();) {
            auto& versions = it->second;

            // 先移除创建者已回滚的版本。
            versions.erase(
                std::remove_if(versions.begin(), versions.end(),
                               [&](const Version& version) {
                                   return status_of(version.xmin) ==
                                          Status::Aborted;
                               }),
                versions.end());

            if (versions.empty()) {
                it = data_.erase(it);
                continue;
            }

            const Version& latest = versions.back();
            if (latest.tombstone) {
                removed += versions.size();
                it = data_.erase(it);
                continue;
            }

            removed += versions.size() - 1;
            versions.erase(versions.begin(), versions.end() - 1);
            ++it;
        }
        return removed;
    }

    [[nodiscard]] std::size_t version_count(const std::string& key) const {
        const auto it = data_.find(key);
        return it == data_.end() ? 0 : it->second.size();
    }

    [[nodiscard]] Stats stats() const {
        Stats result;
        result.keys = data_.size();
        result.active_transactions = active_.size();
        for (const auto& [key, versions] : data_) {
            (void)key;
            result.versions += versions.size();
        }
        return result;
    }

private:
    enum class Status { Active, Committed, Aborted };

    struct Transaction {
        TxId id = 0;
        Snapshot snapshot;
        Status status = Status::Active;
    };

    [[nodiscard]] Transaction& find_active(TxId txid) {
        const auto it = transactions_.find(txid);
        if (it == transactions_.end() || it->second.status != Status::Active) {
            throw std::logic_error("transaction is not active");
        }
        return it->second;
    }

    [[nodiscard]] Status status_of(TxId txid) const {
        if (txid == 0) {
            return Status::Committed;
        }
        const auto it = transactions_.find(txid);
        if (it == transactions_.end()) {
            throw std::logic_error("unknown transaction id");
        }
        return it->second.status;
    }

    [[nodiscard]] bool visible(const Version& version,
                               const Transaction& tx) const {
        const TxId self = tx.id;
        const auto& snapshot = tx.snapshot;

        bool creator_visible = false;
        if (version.xmin == self) {
            creator_visible = true;
        } else if (status_of(version.xmin) == Status::Committed &&
                   version.xmin < snapshot.xmax &&
                   snapshot.active.find(version.xmin) ==
                       snapshot.active.end()) {
            creator_visible = true;
        }
        if (!creator_visible) {
            return false;
        }

        if (version.xmax == 0) {
            return true;
        }
        if (version.xmax == self) {
            return false;
        }
        if (status_of(version.xmax) != Status::Committed) {
            return true;  // 删除者回滚/未提交，旧版本仍然可见
        }
        if (version.xmax >= snapshot.xmax ||
            snapshot.active.find(version.xmax) != snapshot.active.end()) {
            return true;  // 删除发生在快照之后，不影响本快照
        }
        return false;
    }

    void check_write_conflict(const std::vector<Version>& versions,
                              TxId txid,
                              const Transaction& tx) const {
        (void)tx;
        if (versions.empty()) {
            return;
        }
        const Version& latest = versions.back();
        if (latest.xmin != txid &&
            status_of(latest.xmin) == Status::Active) {
            throw MVCCWriteConflict("another transaction is writing this key");
        }
        if (latest.xmax != 0 && latest.xmax != txid &&
            status_of(latest.xmax) == Status::Active) {
            throw MVCCWriteConflict("another transaction is deleting this key");
        }
    }

    TxId next_txid_ = 1;
    std::unordered_map<TxId, Transaction> transactions_;
    std::unordered_set<TxId> active_;
    std::map<std::string, std::vector<Version>> data_;
};

} // namespace archdesign
