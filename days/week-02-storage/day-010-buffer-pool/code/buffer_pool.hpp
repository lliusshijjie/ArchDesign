// buffer_pool.hpp
//
// Day 10 的教学实现：Buffer Pool Manager。
//
// 结构：
//   PageStore（磁盘接口） <- BufferPool -> frames + page table + LRU young/old
//
// 尽量贴近 InnoDB 的关键机制：
//   - 固定大小的 page frame（4 KiB）；
//   - page table: page_id -> frame；
//   - pin count：被使用的页不能被淘汰；
//   - young/old 两段 LRU：新页从 midpoint（old 头部）进入；
//   - 在 old 中被再次访问的页提升到 young；
//   - young 超预算时把尾部降级回 old；
//   - 淘汰 old 尾部（其次 young 尾部），脏页先写回；
//   - flush_all / 统计。
//
// 省略：
//   - latch/并发控制；
//   - flush list + checkpoint + 后台 page cleaner；
//   - doublewrite buffer；
//   - 预读、压缩页、buffer pool instances；
//   - WAL 顺序（Day 09）与真正的崩溃一致性。

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace archdesign {

inline constexpr std::size_t kPageSize = 4096;
using PageId = std::uint64_t;
using PageData = std::array<std::byte, kPageSize>;

class PageStore {
public:
    virtual ~PageStore() = default;
    virtual void read(PageId id, PageData& out) = 0;
    virtual void write(PageId id, const PageData& data) = 0;
};

class MemoryPageStore final : public PageStore {
public:
    void read(PageId id, PageData& out) override {
        const auto it = pages_.find(id);
        if (it == pages_.end()) {
            out.fill(std::byte{0});
        } else {
            out = it->second;
        }
        ++reads_;
    }

    void write(PageId id, const PageData& data) override {
        pages_[id] = data;
        ++writes_;
    }

    [[nodiscard]] bool has(PageId id) const {
        return pages_.find(id) != pages_.end();
    }

    [[nodiscard]] PageData get(PageId id) const {
        const auto it = pages_.find(id);
        if (it == pages_.end()) {
            throw std::out_of_range("page not found");
        }
        return it->second;
    }

    [[nodiscard]] std::uint64_t reads() const {
        return reads_;
    }

    [[nodiscard]] std::uint64_t writes() const {
        return writes_;
    }

private:
    std::unordered_map<PageId, PageData> pages_;
    std::uint64_t reads_ = 0;
    std::uint64_t writes_ = 0;
};

class BufferPoolFull : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class BufferPool {
private:
    struct Page;
    struct Frame;

public:
    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::uint64_t flushes = 0;
        std::size_t resident = 0;
        std::size_t young = 0;
        std::size_t old = 0;
    };

    // old_blocks_percent 近似 InnoDB 的 innodb_old_blocks_pct：
    // 新页先进入 old 段，避免全表扫描把 hot set 冲掉。
    explicit BufferPool(PageStore& store,
                        std::size_t capacity,
                        std::size_t old_blocks_percent = 37)
        : store_(store), capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("capacity must be > 0");
        }
        if (old_blocks_percent == 0 || old_blocks_percent >= 100) {
            throw std::invalid_argument("old_blocks_percent must be in (0, 100)");
        }

        old_target_ = std::max<std::size_t>(
            1, capacity * old_blocks_percent / 100);
        old_target_ = std::min(old_target_, capacity);
        young_target_ = capacity - old_target_;

        frames_.reserve(capacity);
        for (std::size_t i = 0; i < capacity; ++i) {
            frames_.push_back(std::make_unique<Frame>());
            free_.push_back(frames_.back().get());
        }
    }

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // 返回的 Page 处于 pinned 状态；调用方用完必须 unpin。
    Page* fetch(PageId id) {
        auto found = page_table_.find(id);
        if (found != page_table_.end()) {
            ++stats_.hits;
            Frame* frame = found->second;
            ++frame->page.pin_count;
            touch(frame);
            return &frame->page;
        }

        ++stats_.misses;
        Frame* frame = acquire_frame();
        try {
            store_.read(id, frame->page.data);
        } catch (...) {
            free_.push_back(frame);
            throw;
        }

        frame->page.id = id;
        frame->page.pin_count = 1;
        frame->page.dirty = false;
        page_table_[id] = frame;
        insert_old_front(frame);
        return &frame->page;
    }

    void unpin(PageId id, bool dirty = false) {
        Frame* frame = find_frame(id);
        if (frame->page.pin_count == 0) {
            throw std::logic_error("unpin without pin");
        }
        --frame->page.pin_count;
        frame->page.dirty = frame->page.dirty || dirty;
    }

    // 返回是否真的写盘（不是 dirty 则 false）。
    bool flush_page(PageId id) {
        Frame* frame = find_frame(id);
        if (!frame->page.dirty) {
            return false;
        }
        store_.write(id, frame->page.data);
        frame->page.dirty = false;
        ++stats_.flushes;
        return true;
    }

    void flush_all() {
        for (auto& frame : frames_) {
            if (frame->page.id == kInvalidPageId || !frame->page.dirty) {
                continue;
            }
            store_.write(frame->page.id, frame->page.data);
            frame->page.dirty = false;
            ++stats_.flushes;
        }
    }

    [[nodiscard]] bool contains(PageId id) const {
        return page_table_.find(id) != page_table_.end();
    }

    [[nodiscard]] std::size_t pin_count(PageId id) const {
        return find_frame(id)->page.pin_count;
    }

    [[nodiscard]] bool dirty(PageId id) const {
        return find_frame(id)->page.dirty;
    }

    [[nodiscard]] Stats stats() const {
        Stats result = stats_;
        result.resident = page_table_.size();
        result.young = young_.size();
        result.old = old_.size();
        return result;
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

private:
    static constexpr PageId kInvalidPageId = static_cast<PageId>(-1);

    struct Page {
        PageId id = kInvalidPageId;
        PageData data{};
        std::size_t pin_count = 0;
        bool dirty = false;
    };

    struct Frame {
        Page page;
        bool in_young = false;
        std::list<Frame*>::iterator lru_it;
    };

    [[nodiscard]] Frame* find_frame(PageId id) const {
        const auto it = page_table_.find(id);
        if (it == page_table_.end()) {
            throw std::out_of_range("page is not resident");
        }
        return it->second;
    }

    void insert_old_front(Frame* frame) {
        frame->in_young = false;
        frame->lru_it = old_.insert(old_.begin(), frame);
    }

    void remove_from_lru(Frame* frame) {
        if (frame->in_young) {
            young_.erase(frame->lru_it);
        } else {
            old_.erase(frame->lru_it);
        }
    }

    void touch(Frame* frame) {
        remove_from_lru(frame);
        frame->in_young = true;
        frame->lru_it = young_.insert(young_.begin(), frame);

        // young 超预算：把 young 尾部降级到 old 头部（midpoint 附近）。
        while (young_.size() > young_target_ && !young_.empty()) {
            Frame* demoted = young_.back();
            young_.pop_back();
            demoted->in_young = false;
            demoted->lru_it = old_.insert(old_.begin(), demoted);
        }
    }

    Frame* acquire_frame() {
        if (!free_.empty()) {
            Frame* frame = free_.back();
            free_.pop_back();
            return frame;
        }

        Frame* victim = nullptr;
        for (auto it = old_.rbegin(); it != old_.rend(); ++it) {
            if ((*it)->page.pin_count == 0) {
                victim = *it;
                break;
            }
        }
        if (victim == nullptr) {
            for (auto it = young_.rbegin(); it != young_.rend(); ++it) {
                if ((*it)->page.pin_count == 0) {
                    victim = *it;
                    break;
                }
            }
        }
        if (victim == nullptr) {
            throw BufferPoolFull("all frames are pinned");
        }

        if (victim->page.dirty) {
            store_.write(victim->page.id, victim->page.data);
            ++stats_.flushes;
        }

        page_table_.erase(victim->page.id);
        remove_from_lru(victim);
        ++stats_.evictions;

        victim->page.id = kInvalidPageId;
        victim->page.pin_count = 0;
        victim->page.dirty = false;
        victim->page.data.fill(std::byte{0});
        return victim;
    }

    PageStore& store_;
    const std::size_t capacity_;
    std::size_t old_target_ = 0;
    std::size_t young_target_ = 0;

    std::vector<std::unique_ptr<Frame>> frames_;
    std::vector<Frame*> free_;
    std::unordered_map<PageId, Frame*> page_table_;
    std::list<Frame*> young_;
    std::list<Frame*> old_;
    Stats stats_;
};

} // namespace archdesign
