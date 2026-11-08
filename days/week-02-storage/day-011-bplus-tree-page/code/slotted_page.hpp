// slotted_page.hpp
//
// Day 11 的教学实现：B+ 树叶子页的物理布局（slotted page）。
//
// 页面布局（4 KiB）：
//
//   +----------------------------+ 0
//   | Header                     |
//   +----------------------------+ free_offset
//   | record 0 | record 1 | ...  |  （记录向高地址增长）
//   |          free space        |
//   +----------------------------+ slot area
//   | slot n-1 | ... | slot 1 | slot 0 |  （slot 从页尾向低地址增长）
//   +----------------------------+ 4096
//
//   slot 按 key 排序，记录本身不要求物理有序。
//   record: key_len(2) + value_len(2) + key + value
//   slot:   offset(2) + length(2)
//
// 省略：页校验和、压缩、页级 latch、B+ 树父节点/内部页、
// 崩溃恢复、free list、页分裂后的父节点更新。

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace archdesign {

class SlottedPage {
public:
    static constexpr std::size_t kPageSize = 4096;
    static constexpr std::uint32_t kMagic = 0x31504742u;  // "BGP1"
    static constexpr std::uint16_t kLeafPage = 1;

    using Bytes = std::array<std::byte, kPageSize>;

    explicit SlottedPage(std::uint64_t page_id = 0,
                         std::uint16_t level = 0) {
        Header header{};
        header.magic = kMagic;
        header.page_type = kLeafPage;
        header.level = level;
        header.n_slots = 0;
        header.free_offset = static_cast<std::uint16_t>(sizeof(Header));
        header.free_space =
            static_cast<std::uint16_t>(kPageSize - sizeof(Header));
        header.fragmentation = 0;
        header.page_id = page_id;
        header.next_page = kInvalidPage;
        header.prev_page = kInvalidPage;
        header.lsn = 0;
        // store_header(header);
        data_.fill(std::byte{0});
        store_header(header);
    }

    [[nodiscard]] std::uint64_t page_id() const {
        return read_header().page_id;
    }

    [[nodiscard]] std::uint64_t next_page() const {
        return read_header().next_page;
    }

    [[nodiscard]] std::uint64_t prev_page() const {
        return read_header().prev_page;
    }

    void set_next_page(std::uint64_t id) {
        auto header = read_header();
        header.next_page = id;
        store_header(header);
    }

    void set_prev_page(std::uint64_t id) {
        auto header = read_header();
        header.prev_page = id;
        store_header(header);
    }

    [[nodiscard]] std::size_t size() const {
        return read_header().n_slots;
    }

    [[nodiscard]] std::size_t free_space() const {
        return read_header().free_space;
    }

    [[nodiscard]] std::size_t fragmentation() const {
        return read_header().fragmentation;
    }

    // 插入唯一 key。空间不足/重复 key 返回 false。
    bool insert(std::string_view key, std::string_view value) {
        if (key.size() > kMaxKey || value.size() > kMaxValue) {
            return false;
        }
        const std::uint32_t record_size =
            static_cast<std::uint32_t>(sizeof(std::uint16_t) * 2 +
                                       key.size() + value.size());
        const std::size_t pos = lower_bound(key);
        if (pos < size() && key_at(pos) == key) {
            return false;  // B+ 树叶子 key 唯一（简化；真实索引可带 row id）
        }

        auto header = read_header();
        if (header.free_space < record_size + sizeof(Slot)) {
            return false;
        }
        if (static_cast<std::size_t>(header.free_offset) + record_size >
            slot_address(header.n_slots)) {
            return false;  // 防御：记录区与 slot 区重叠
        }

        const std::uint16_t offset = header.free_offset;
        write_record(offset, key, value);
        insert_slot(pos, offset, static_cast<std::uint16_t>(record_size));

        header = read_header();
        header.free_offset =
            static_cast<std::uint16_t>(offset + record_size);
        header.free_space = static_cast<std::uint16_t>(
            header.free_space - record_size - sizeof(Slot));
        store_header(header);
        return true;
    }

    bool erase(std::string_view key) {
        const std::size_t pos = lower_bound(key);
        if (pos >= size() || key_at(pos) != key) {
            return false;
        }

        auto header = read_header();
        const Slot slot = read_slot(pos);
        erase_slot(pos);
        header = read_header();
        header.n_slots = static_cast<std::uint16_t>(header.n_slots - 1);
        header.free_space =
            static_cast<std::uint16_t>(header.free_space + sizeof(Slot));
        // 记录数据成为洞；slot 目录的 4 B 被回收，记录区不立即移动。
        header.fragmentation =
            static_cast<std::uint16_t>(header.fragmentation + slot.length);
        store_header(header);
        return true;
    }

    [[nodiscard]] std::optional<std::string> find(std::string_view key) const {
        const std::size_t pos = lower_bound(key);
        if (pos >= size() || key_at(pos) != key) {
            return std::nullopt;
        }
        return value_at(pos);
    }

    // 按 slot 顺序返回所有记录（slot 已按 key 排序）。
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> entries() const {
        std::vector<std::pair<std::string, std::string>> result;
        result.reserve(size());
        for (std::size_t i = 0; i < size(); ++i) {
            result.emplace_back(std::string(key_at(i)), value_at(i));
        }
        return result;
    }

    // 回收删除留下的洞，把记录重新紧凑排列。
    void compact() {
        const auto all = entries();
        const auto old_header = read_header();
        const std::uint64_t next = old_header.next_page;
        const std::uint64_t prev = old_header.prev_page;
        const std::uint64_t id = old_header.page_id;
        const std::uint64_t lsn = old_header.lsn;

        SlottedPage rebuilt(id, old_header.level);
        rebuilt.set_next_page(next);
        rebuilt.set_prev_page(prev);
        auto header = rebuilt.read_header();
        header.lsn = lsn;
        rebuilt.store_header(header);

        for (const auto& [key, value] : all) {
            if (!rebuilt.insert(key, value)) {
                throw std::runtime_error("compact failed: entries do not fit");
            }
        }
        *this = std::move(rebuilt);
    }

    // 把一个满页从中间分裂；this 保留左半页，返回右半页和 separator key。
    std::pair<SlottedPage, std::string> split(std::uint64_t new_page_id) {
        auto all = entries();
        if (all.size() < 2) {
            throw std::logic_error("cannot split page with < 2 records");
        }
        const std::size_t mid = all.size() / 2;
        const std::string separator = all[mid].first;
        const auto old_header = read_header();

        SlottedPage left(old_header.page_id, old_header.level);
        left.set_prev_page(old_header.prev_page);
        left.set_next_page(new_page_id);
        for (std::size_t i = 0; i < mid; ++i) {
            if (!left.insert(all[i].first, all[i].second)) {
                throw std::runtime_error("left split page overflow");
            }
        }

        SlottedPage right(new_page_id, old_header.level);
        right.set_prev_page(old_header.page_id);
        right.set_next_page(old_header.next_page);
        for (std::size_t i = mid; i < all.size(); ++i) {
            if (!right.insert(all[i].first, all[i].second)) {
                throw std::runtime_error("right split page overflow");
            }
        }

        *this = std::move(left);
        return {std::move(right), separator};
    }

    [[nodiscard]] const Bytes& bytes() const noexcept {
        return data_;
    }

    static SlottedPage from_bytes(const Bytes& bytes) {
        SlottedPage page;
        page.data_ = bytes;
        const auto header = page.read_header();
        if (header.magic != kMagic || header.page_type != kLeafPage) {
            throw std::runtime_error("invalid slotted page");
        }
        if (header.n_slots > kMaxSlots ||
            header.free_offset < sizeof(Header) ||
            header.free_offset > kPageSize) {
            throw std::runtime_error("corrupt slotted page header");
        }
        return page;
    }

    [[nodiscard]] std::uint16_t level() const {
        return read_header().level;
    }

private:
    static constexpr std::uint64_t kInvalidPage =
        static_cast<std::uint64_t>(-1);
    static constexpr std::size_t kMaxSlots =
        (kPageSize - sizeof(std::uint32_t)) / sizeof(std::uint16_t) / 2;
    static constexpr std::size_t kMaxKey = 512;
    static constexpr std::size_t kMaxValue = 2048;

    struct Header {
        std::uint32_t magic;
        std::uint16_t page_type;
        std::uint16_t level;
        std::uint16_t n_slots;
        std::uint16_t free_offset;
        std::uint16_t free_space;
        std::uint16_t fragmentation;
        std::uint64_t page_id;
        std::uint64_t next_page;
        std::uint64_t prev_page;
        std::uint64_t lsn;
    };

    struct Slot {
        std::uint16_t offset;
        std::uint16_t length;
    };

    static_assert(sizeof(Header) <= 64);
    static_assert(sizeof(Slot) == 4);

    Header read_header() const {
        Header header{};
        std::memcpy(&header, data_.data(), sizeof(header));
        return header;
    }

    void store_header(const Header& header) {
        std::memcpy(data_.data(), &header, sizeof(header));
    }

    std::size_t slot_address(std::size_t index) const {
        return kPageSize - (index + 1) * sizeof(Slot);
    }

    Slot read_slot(std::size_t index) const {
        Slot slot{};
        std::memcpy(&slot, data_.data() + slot_address(index), sizeof(slot));
        return slot;
    }

    void write_slot(std::size_t index, Slot slot) {
        std::memcpy(data_.data() + slot_address(index), &slot, sizeof(slot));
    }

    void insert_slot(std::size_t pos, std::uint16_t offset,
                     std::uint16_t length) {
        const std::size_t n = size();
        // slot 数组向低地址增长：把 pos..n-1 依次挪到 pos+1..n。
        for (std::size_t i = n; i > pos; --i) {
            write_slot(i, read_slot(i - 1));
        }
        write_slot(pos, Slot{offset, length});
        auto header = read_header();
        header.n_slots = static_cast<std::uint16_t>(header.n_slots + 1);
        store_header(header);
    }

    void erase_slot(std::size_t pos) {
        const std::size_t n = size();
        // 把 pos+1..n-1 依次上移，覆盖 pos。
        for (std::size_t i = pos; i + 1 < n; ++i) {
            write_slot(i, read_slot(i + 1));
        }
    }

    std::string_view key_at(std::size_t index) const {
        const Slot slot = read_slot(index);
        std::uint16_t key_size = 0;
        std::memcpy(&key_size, data_.data() + slot.offset, sizeof(key_size));
        return std::string_view(
            reinterpret_cast<const char*>(data_.data() + slot.offset +
                                          sizeof(key_size) +
                                          sizeof(std::uint16_t)),
            key_size);
    }

    std::string value_at(std::size_t index) const {
        const Slot slot = read_slot(index);
        std::uint16_t key_size = 0;
        std::uint16_t value_size = 0;
        std::memcpy(&key_size, data_.data() + slot.offset, sizeof(key_size));
        std::memcpy(&value_size,
                    data_.data() + slot.offset + sizeof(key_size),
                    sizeof(value_size));
        return std::string(
            reinterpret_cast<const char*>(data_.data() + slot.offset +
                                          sizeof(key_size) +
                                          sizeof(value_size) + key_size),
            value_size);
    }

    void write_record(std::uint16_t offset, std::string_view key,
                      std::string_view value) {
        const std::uint16_t key_size = static_cast<std::uint16_t>(key.size());
        const std::uint16_t value_size =
            static_cast<std::uint16_t>(value.size());
        std::memcpy(data_.data() + offset, &key_size, sizeof(key_size));
        std::memcpy(data_.data() + offset + sizeof(key_size), &value_size,
                    sizeof(value_size));
        std::memcpy(data_.data() + offset + sizeof(key_size) +
                        sizeof(value_size),
                    key.data(), key.size());
        std::memcpy(data_.data() + offset + sizeof(key_size) +
                        sizeof(value_size) + key.size(),
                    value.data(), value.size());
    }

    std::size_t lower_bound(std::string_view key) const {
        std::size_t lo = 0;
        std::size_t hi = size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (key_at(mid) < key) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    Bytes data_{};
};

} // namespace archdesign
