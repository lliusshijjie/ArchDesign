#include "eviction_pool.hpp"

#include <utility>

namespace archdesign {

void EvictionPool::insert(std::string key, u64 idle) {
    if (count_ == kCapacity && idle <= items_[0].idle) {
        ++rejected_;
        return;
    }
    if (count_ == kCapacity) {
        // 丢掉最小的（下标 0），为后面的插入腾一个位置。
        for (int i = 0; i + 1 < count_; ++i) {
            items_[static_cast<std::size_t>(i)] =
                std::move(items_[static_cast<std::size_t>(i + 1)]);
        }
        --count_;
    }

    int pos = 0;
    while (pos < count_ && items_[static_cast<std::size_t>(pos)].idle < idle) ++pos;
    for (int i = count_; i > pos; --i) {
        items_[static_cast<std::size_t>(i)] = std::move(items_[static_cast<std::size_t>(i - 1)]);
    }
    items_[static_cast<std::size_t>(pos)] = Item{std::move(key), idle};
    ++count_;
    ++inserts_;
}

std::optional<EvictionPool::Item> EvictionPool::pop_best() {
    if (count_ == 0) return std::nullopt;
    --count_;
    Item item = std::move(items_[static_cast<std::size_t>(count_)]);
    items_[static_cast<std::size_t>(count_)] = Item{};
    return item;
}

void EvictionPool::clear() {
    for (auto& item : items_) item = Item{};
    count_ = 0;
}

int EvictionPool::count() const { return count_; }

u64 EvictionPool::inserts() const { return inserts_; }

u64 EvictionPool::rejected() const { return rejected_; }

const EvictionPool::Item& EvictionPool::at(int i) const {
    return items_[static_cast<std::size_t>(i)];
}

}  // namespace archdesign
