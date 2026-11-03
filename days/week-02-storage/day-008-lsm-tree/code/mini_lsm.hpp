// mini_lsm.hpp
//
// Day 08 的教学实现：一个最小 LSM-tree 存储引擎。
//
// 结构：
//   WAL -> MemTable -> Immutable（本实现同步 flush）-> SSTable
//   get: MemTable -> 新 SST -> 旧 SST（从新到旧）
//   compaction: 合并全部 SST，保留每个 key 的最新版本，丢弃 tombstone
//
// 刻意省略：
//   - skiplist/并发 MemTable；
//   - Manifest / version set / 分层 compaction；
//   - block index、Bloom filter、checksum、压缩；
//   - fsync/原子 rename/崩溃恢复的完整协议（Day 09 深入 WAL）；
//   - 后台 flush/compaction 线程；
//   - MVCC/sequence number/快照读。

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace archdesign {

class MiniLSM {
public:
    struct Options {
        std::size_t memtable_limit = 4096;  // 估算字节数
        std::size_t compaction_trigger = 3; // SST 数量达到后合并
    };

    struct Stats {
        std::uint64_t puts = 0;
        std::uint64_t removes = 0;
        std::uint64_t flushes = 0;
        std::uint64_t compactions = 0;
        std::uint64_t recovered_wal_records = 0;
        std::size_t memtable_bytes = 0;
        std::size_t sstables = 0;
    };

    explicit MiniLSM(std::filesystem::path directory)
        : MiniLSM(std::move(directory), Options{}) {}

    MiniLSM(std::filesystem::path directory, Options options)
        : directory_(std::move(directory)), options_(options) {
        if (options_.memtable_limit == 0) {
            throw std::invalid_argument("memtable_limit must be > 0");
        }
        if (options_.compaction_trigger < 2) {
            throw std::invalid_argument("compaction_trigger must be >= 2");
        }

        std::error_code ec;
        std::filesystem::create_directories(directory_, ec);
        if (ec) {
            throw std::runtime_error("cannot create LSM directory: " + ec.message());
        }

        wal_path_ = directory_ / "wal.log";
        recover();
        open_wal_append();
    }

    ~MiniLSM() {
        // WAL 每条记录都已 flush；析构只关闭文件，不强制把 memtable 写成 SST。
        // 这样即使进程退出，下一次打开也能从 WAL 恢复。
        try {
            close_wal();
        } catch (...) {
            // 析构函数不能抛。
        }
    }

    MiniLSM(const MiniLSM&) = delete;
    MiniLSM& operator=(const MiniLSM&) = delete;

    void put(const std::string& key, const std::string& value) {
        ensure_open();
        append_wal(false, key, value);
        set_memtable(key, value);
        ++stats_.puts;
        maybe_flush();
    }

    void remove(const std::string& key) {
        ensure_open();
        append_wal(true, key, "");
        set_memtable(key, std::nullopt);
        ++stats_.removes;
        maybe_flush();
    }

    [[nodiscard]] std::optional<std::string> get(const std::string& key) const {
        auto it = memtable_.find(key);
        if (it != memtable_.end()) {
            return it->second;
        }

        // 从新到旧扫描 SST；第一个命中的版本决定结果（tombstone 也是命中）。
        for (auto sst = sstables_.rbegin(); sst != sstables_.rend(); ++sst) {
            const auto& entries = sst->entries;
            const auto found = std::lower_bound(
                entries.begin(), entries.end(), key,
                [](const Entry& entry, const std::string& value) {
                    return entry.key < value;
                });
            if (found != entries.end() && found->key == key) {
                if (found->tombstone) {
                    return std::nullopt;
                }
                return found->value;
            }
        }
        return std::nullopt;
    }

    // 同步把 memtable 写成 SST，并清空 WAL。生产实现应后台 flush。
    void flush() {
        ensure_open();
        if (memtable_.empty()) {
            return;
        }

        auto sst = write_sst(memtable_, false);
        sstables_.push_back(std::move(sst));
        memtable_.clear();
        memtable_bytes_ = 0;
        reset_wal();
        ++stats_.flushes;

        if (sstables_.size() >= options_.compaction_trigger) {
            compact();
        }
    }

    void close() {
        if (closed_) {
            return;
        }
        flush();
        close_wal();
        closed_ = true;
    }

    [[nodiscard]] Stats stats() const {
        Stats result = stats_;
        result.memtable_bytes = memtable_bytes_;
        result.sstables = sstables_.size();
        return result;
    }

private:
    struct Entry {
        std::string key;
        std::string value;
        bool tombstone = false;
    };

    struct SSTable {
        std::uint64_t id = 0;
        std::filesystem::path path;
        std::vector<Entry> entries;
    };

    static constexpr std::uint32_t kSstMagic = 0x31545353u;  // "SST1"
    static constexpr std::uint32_t kMaxField = 0xFFFFFFFFu;

    void ensure_open() const {
        if (closed_) {
            throw std::logic_error("MiniLSM is closed");
        }
    }

    void set_memtable(const std::string& key, std::optional<std::string> value) {
        auto it = memtable_.find(key);
        if (it != memtable_.end()) {
            memtable_bytes_ -= estimate(it->first, it->second);
        }

        if (value.has_value()) {
            memtable_[key] = std::move(value);
        } else {
            memtable_[key] = std::nullopt;
        }
        memtable_bytes_ += estimate(key, memtable_[key]);
    }

    static std::size_t estimate(const std::string& key,
                                const std::optional<std::string>& value) {
        return key.size() + (value.has_value() ? value->size() : 0) + 32;
    }

    void maybe_flush() {
        if (memtable_bytes_ >= options_.memtable_limit) {
            flush();
        }
    }

    void append_wal(bool tombstone,
                    const std::string& key,
                    const std::string& value) {
        if (!wal_out_.is_open()) {
            throw std::runtime_error("WAL is not open");
        }
        if (key.size() > kMaxField || value.size() > kMaxField) {
            throw std::length_error("key/value too large for teaching WAL");
        }

        const std::uint32_t key_size = static_cast<std::uint32_t>(key.size());
        const std::uint32_t value_size = static_cast<std::uint32_t>(value.size());
        const std::uint8_t flag = tombstone ? 1 : 0;

        wal_out_.write(reinterpret_cast<const char*>(&key_size), sizeof(key_size));
        wal_out_.write(reinterpret_cast<const char*>(&value_size), sizeof(value_size));
        wal_out_.write(reinterpret_cast<const char*>(&flag), sizeof(flag));
        wal_out_.write(key.data(), static_cast<std::streamsize>(key.size()));
        if (!tombstone) {
            wal_out_.write(value.data(), static_cast<std::streamsize>(value.size()));
        }
        wal_out_.flush();
        if (!wal_out_) {
            throw std::runtime_error("failed to write WAL");
        }
    }

    void reset_wal() {
        if (wal_out_.is_open()) {
            wal_out_.close();
        }

        std::ofstream truncate(wal_path_, std::ios::binary | std::ios::trunc);
        truncate.close();
        open_wal_append();
    }

    void open_wal_append() {
        wal_out_.open(wal_path_, std::ios::binary | std::ios::app);
        if (!wal_out_) {
            throw std::runtime_error("cannot open WAL for append");
        }
    }

    void close_wal() {
        if (wal_out_.is_open()) {
            wal_out_.flush();
            wal_out_.close();
        }
    }

    std::filesystem::path sst_path(std::uint64_t id) const {
        char name[32];
        std::snprintf(name, sizeof(name), "sst-%06llu.dat",
                      static_cast<unsigned long long>(id));
        return directory_ / name;
    }

    SSTable write_sst(
        const std::map<std::string, std::optional<std::string>>& entries,
        bool drop_tombstones) {
        SSTable sst;
        sst.id = next_sst_id_++;
        sst.path = sst_path(sst.id);

        for (const auto& [key, value] : entries) {
            if (!value.has_value() && drop_tombstones) {
                continue;
            }
            sst.entries.push_back(Entry{key, value.value_or(""), !value.has_value()});
        }
        write_sst_file(sst);
        return sst;
    }

    void write_sst_file(const SSTable& sst) const {
        std::ofstream out(sst.path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot create SST");
        }

        const std::uint32_t count =
            static_cast<std::uint32_t>(sst.entries.size());
        out.write(reinterpret_cast<const char*>(&kSstMagic), sizeof(kSstMagic));
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));

        for (const auto& entry : sst.entries) {
            const std::uint32_t key_size =
                static_cast<std::uint32_t>(entry.key.size());
            const std::uint32_t value_size =
                static_cast<std::uint32_t>(entry.value.size());
            const std::uint8_t tombstone = entry.tombstone ? 1 : 0;

            out.write(reinterpret_cast<const char*>(&key_size), sizeof(key_size));
            out.write(reinterpret_cast<const char*>(&value_size), sizeof(value_size));
            out.write(reinterpret_cast<const char*>(&tombstone), sizeof(tombstone));
            out.write(entry.key.data(), static_cast<std::streamsize>(entry.key.size()));
            out.write(entry.value.data(),
                      static_cast<std::streamsize>(entry.value.size()));
        }

        out.flush();
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(sst.path, ec);
            throw std::runtime_error("failed to write SST");
        }
    }

    SSTable load_sst(const std::filesystem::path& path) const {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot open SST: " + path.string());
        }

        std::uint32_t magic = 0;
        std::uint32_t count = 0;
        in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (magic != kSstMagic) {
            throw std::runtime_error("bad SST magic: " + path.string());
        }

        SSTable sst;
        sst.path = path;
        sst.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t key_size = 0;
            std::uint32_t value_size = 0;
            std::uint8_t tombstone = 0;
            in.read(reinterpret_cast<char*>(&key_size), sizeof(key_size));
            in.read(reinterpret_cast<char*>(&value_size), sizeof(value_size));
            in.read(reinterpret_cast<char*>(&tombstone), sizeof(tombstone));

            Entry entry;
            entry.key.resize(key_size);
            entry.value.resize(value_size);
            entry.tombstone = tombstone != 0;
            in.read(entry.key.data(), static_cast<std::streamsize>(key_size));
            in.read(entry.value.data(), static_cast<std::streamsize>(value_size));
            if (!in) {
                throw std::runtime_error("truncated SST: " + path.string());
            }
            sst.entries.push_back(std::move(entry));
        }
        return sst;
    }

    void recover() {
        std::vector<std::pair<std::uint64_t, std::filesystem::path>> files;
        for (const auto& item : std::filesystem::directory_iterator(directory_)) {
            if (!item.is_regular_file()) {
                continue;
            }
            const auto name = item.path().filename().string();
            if (name.rfind("sst-", 0) != 0 || item.path().extension() != ".dat") {
                continue;
            }
            const auto id_text = name.substr(4, name.find('.') - 4);
            try {
                files.emplace_back(std::stoull(id_text), item.path());
            } catch (...) {
                // 忽略不是本实现命名的文件。
            }
        }

        std::sort(files.begin(), files.end());
        for (const auto& [id, path] : files) {
            auto sst = load_sst(path);
            sst.id = id;
            next_sst_id_ = std::max(next_sst_id_, id + 1);
            sstables_.push_back(std::move(sst));
        }

        std::ifstream wal(wal_path_, std::ios::binary);
        if (!wal) {
            return;
        }

        for (;;) {
            std::uint32_t key_size = 0;
            std::uint32_t value_size = 0;
            std::uint8_t tombstone = 0;
            if (!wal.read(reinterpret_cast<char*>(&key_size), sizeof(key_size))) {
                break;
            }
            if (!wal.read(reinterpret_cast<char*>(&value_size), sizeof(value_size)) ||
                !wal.read(reinterpret_cast<char*>(&tombstone), sizeof(tombstone))) {
                break;  // 部分尾部记录，忽略
            }
            if (key_size > 1u << 24 || value_size > 1u << 28) {
                break;  // 防御损坏/异常长度
            }

            std::string key(key_size, '\0');
            std::string value(value_size, '\0');
            if (!wal.read(key.data(), static_cast<std::streamsize>(key_size))) {
                break;
            }
            if (!wal.read(value.data(), static_cast<std::streamsize>(value_size))) {
                break;
            }

            set_memtable(key, tombstone ? std::nullopt
                                       : std::optional<std::string>(std::move(value)));
            ++stats_.recovered_wal_records;
        }
    }

    void compact() {
        if (sstables_.size() < 2) {
            return;
        }

        // 新 SST 在前，所以每个 key 第一次出现就是最新版本。
        std::map<std::string, std::optional<std::string>> merged;
        for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
            for (const auto& entry : it->entries) {
                if (merged.find(entry.key) != merged.end()) {
                    continue;
                }
                if (entry.tombstone) {
                    merged.emplace(entry.key, std::nullopt);
                } else {
                    merged.emplace(entry.key, entry.value);
                }
            }
        }

        const auto old_files = sstables_;
        auto compacted = write_sst(merged, true);  // 全量合并，可丢 tombstone
        sstables_.clear();
        if (!compacted.entries.empty()) {
            sstables_.push_back(std::move(compacted));
        }

        for (const auto& old : old_files) {
            std::error_code ec;
            std::filesystem::remove(old.path, ec);
        }
        ++stats_.compactions;
    }

    std::filesystem::path directory_;
    std::filesystem::path wal_path_;
    Options options_;
    std::map<std::string, std::optional<std::string>> memtable_;
    std::vector<SSTable> sstables_;
    std::ofstream wal_out_;
    std::uint64_t next_sst_id_ = 1;
    std::size_t memtable_bytes_ = 0;
    mutable Stats stats_;
    bool closed_ = false;
};

} // namespace archdesign
