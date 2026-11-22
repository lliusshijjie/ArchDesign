// page_store.hpp
//
// 页式键空间：fork/COW 的模型。
//
// 每个 key 通过 FNV-1a 映射到固定数量的页。写一个 key 时，如果这一页正被
// 某个快照共享（use_count > 1），就先复制整页，再改副本。快照持有旧的
// shared_ptr，所以它永远看到 fork 那一刻的内容。

#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace archdesign {

struct Entry {
    std::string value;
    i64 expire_at_ms = -1;  // -1 = 无 TTL；否则是绝对毫秒时间戳
};

struct Page {
    std::map<std::string, Entry> entries;
};

std::size_t page_bytes(const Page& page);

struct CowStats {
    u64 snapshots = 0;
    u64 pages_copied = 0;
    u64 bytes_copied = 0;
    u64 writes = 0;
};

class Snapshot {
public:
    Snapshot();
    Snapshot(std::vector<std::shared_ptr<const Page>> pages, u64 version,
             std::size_t page_count);

    std::size_t page_count() const;
    u64 version() const;
    std::size_t key_count() const;
    std::size_t expires_count() const;

    template <class Fn>
    void for_each(Fn&& fn) const {
        for (const auto& page : pages_) {
            for (const auto& [key, entry] : page->entries) fn(key, entry);
        }
    }

private:
    std::vector<std::shared_ptr<const Page>> pages_;
    u64 version_ = 0;
    std::size_t page_count_ = 0;
};

class PageStore {
public:
    explicit PageStore(std::size_t page_count = 16);

    std::optional<Entry> get(std::string_view key) const;
    std::optional<Entry> get_live(std::string_view key, i64 now_ms) const;

    void set(std::string key, std::string value, i64 expire_at_ms = -1);
    bool del(std::string_view key);
    bool expire_at(std::string_view key, i64 when_ms);
    bool persist(std::string_view key);

    Snapshot fork() const;
    void clear();

    std::size_t key_count() const;
    std::size_t live_key_count(i64 now_ms) const;
    std::size_t page_count() const;

    // 仅用于测试/演示：key 落在哪一页，以及 COW 统计。
    std::size_t page_of(std::string_view key) const;
    const CowStats& stats() const;
    void reset_stats();

    std::vector<std::string> keys() const;

private:
    Page& writable_page(std::size_t index);

    std::vector<std::shared_ptr<Page>> pages_;
    std::size_t page_count_;
    mutable u64 version_ = 0;
    mutable CowStats stats_;
};

}  // namespace archdesign
