// eviction_pool.hpp
//
// 固定 16 个槽，按 idle 升序保存「最值得淘汰」的候选。
// 池跨多轮淘汰保留（Redis 也一样），所以样本会累积，近似精度比单轮采样高。

#pragma once

#include <array>
#include <optional>
#include <string>

#include "common.hpp"

namespace archdesign {

class EvictionPool {
public:
    static constexpr int kCapacity = 16;

    struct Item {
        std::string key;
        u64 idle = 0;  // 分数越大越该淘汰
    };

    // 插入并按 idle 升序维护（池尾 = 最该淘汰）。
    // 池满时：比池里最小的还小就直接丢弃；否则丢掉最小的，再插入。
    void insert(std::string key, u64 idle);
    std::optional<Item> pop_best();
    void clear();

    int count() const;
    u64 inserts() const;
    u64 rejected() const;
    const Item& at(int i) const;

private:
    std::array<Item, static_cast<std::size_t>(kCapacity)> items_{};
    int count_ = 0;
    u64 inserts_ = 0;
    u64 rejected_ = 0;
};

}  // namespace archdesign
