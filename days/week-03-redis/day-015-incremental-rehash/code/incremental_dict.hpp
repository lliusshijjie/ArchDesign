// incremental_dict.hpp
//
// Day 15 的最小实现：Redis dict.c 的“双表 + rehashidx + 每次操作搬一个桶”。
//
// 只保留教学需要的骨架：
//   * 链地址法（每桶一条链），表大小始终是 2 的幂；
//   * 扩容/缩容都走同一套渐进式迁移；
//   * 每次 lookup / insert / delete 顺手搬一格；
//   * 暂停 rehash（对应 Redis 的 dictPauseRehashing）；
//   * 反序二进制游标 SCAN（对应 dictScan）。
//
// 和 Redis 的差异（故意保留，便于把注意力放在迁移逻辑上）：
//   * 用 std::list 表示桶，splice 就是“整条链搬走”，不用手写指针重连；
//   * 哈希函数用 FNV-1a（确定、无密钥），Redis 用带随机种子的 SipHash；
//   * 没有 fork/COW、maxmemory、active-defrag 这些策略层，只保留机制。
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace archdesign {

class IncrementalDict;

// RAII 守卫：暂停期间不搬桶。
// 对应 Redis 的 dictPauseRehashing()/dictResumeRehashing()，用计数器支持嵌套。
class RehashPause {
public:
    explicit RehashPause(IncrementalDict& dict);
    ~RehashPause();

    RehashPause(const RehashPause&) = delete;
    RehashPause& operator=(const RehashPause&) = delete;

private:
    IncrementalDict& dict_;
};

class IncrementalDict {
public:
    // Redis: DICT_HT_INITIAL_SIZE / DICT_HT_INITIAL_EXP = 4 / 2。
    static constexpr std::size_t kMinSlots = 4;
    // Redis: dictRehash() 里 “n*10” 的空桶上限。
    static constexpr std::size_t kEmptyVisitFactor = 10;

    struct Entry {
        std::string key;
        std::string value;
    };

    // 观测指标：真实系统里这些值应该进 INFO / metrics，而不是只给测试用。
    struct Stats {
        std::size_t slots[2] = {0, 0};      // 两张表的桶数
        std::size_t used[2] = {0, 0};       // 两张表各自装了多少条目
        std::int64_t rehash_index = -1;     // -1 表示没在搬；否则是旧表下一个待搬的桶
        std::size_t buckets_moved = 0;      // 累计搬走的桶数
        std::size_t entries_moved = 0;      // 累计搬走的条目数
        std::size_t steps = 0;              // 实际执行的 rehash 步数
        std::size_t max_buckets_per_step = 0;
        std::size_t empty_buckets_skipped = 0;
        std::size_t step_time_ns = 0;       // 所有步的总耗时
        std::size_t max_step_time_ns = 0;
    };

    explicit IncrementalDict(std::size_t initial_slots = 0);

    // 插入（key 已存在返回 false，和 Redis dictAdd 语义一致）。
    bool insert(std::string key, std::string value);
    // 删除；key 不存在返回 false。
    bool erase(std::string_view key);
    // 查找。注意：不是 const 的——一次查找也会推进 rehash。
    Entry* find(std::string_view key);

    std::size_t size() const { return tables_[0].used + tables_[1].used; }
    std::size_t slots(int table) const;
    std::size_t used(int table) const;
    bool rehashing() const { return rehash_index_ >= 0; }
    std::int64_t rehash_index() const { return rehash_index_; }

    // 手工扩容/缩容。min_slots 会向上取整到 2 的幂；已在 rehash 中则返回 false。
    bool expand(std::size_t min_slots);
    // 负载因子低于 10% 时缩容（对应 server.c 的 htNeedsResize + dictResize）。
    bool shrink();
    bool needs_shrink() const;

    // 搬最多 buckets 个非空桶；返回是否仍在 rehash。
    bool rehash_buckets(std::size_t buckets);
    // 按时间预算搬（对应 dictRehashMilliseconds，serverCron 每轮给 1ms）。
    std::size_t rehash_milliseconds(int milliseconds);
    // 一次性搬完：只用来对照“停世界”版本的最大停顿。
    void full_rehash_now();

    void pause_rehashing() { ++pause_depth_; }
    void resume_rehashing();

    // SCAN。无状态游标：cursor 从 0 开始，返回 0 表示结束。
    // 保证：遍历全程都在的 key 至少被访问一次；rehash 期间可能重复。
    std::uint64_t scan(std::uint64_t cursor, const std::function<void(const Entry&)>& visit);

    Stats stats() const;
    std::size_t longest_chain() const;
    // 走一遍两张表数条目：用来验证 used 计数没有漂移。
    std::size_t count_entries() const;
    // 调试用：返回某张表每个桶里的 key。
    std::vector<std::vector<std::string>> bucket_keys(int table) const;

    // 上一步搬了哪个桶 / 搬走几个条目（-1 表示这一步没搬桶），供 trace 用。
    std::int64_t last_moved_bucket() const { return last_moved_bucket_; }
    std::size_t last_moved_entries() const { return last_moved_entries_; }

private:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Table {
        std::vector<std::list<Entry>> buckets;
        std::size_t used = 0;
    };

    static std::uint64_t hash(std::string_view key);
    static std::uint64_t reverse_bits(std::uint64_t value);
    static std::size_t next_pow2(std::size_t slots);

    std::uint64_t mask(int table) const;
    void step();
    void finish_rehash();
    void record_step(TimePoint start, std::size_t moved_buckets);

    // 新表里的落点：
    //   扩容：必须重算 hash（多出来的高位决定了落在 0/1 两份的哪一份）；
    //   缩容：旧桶号就是 hash & mask_old，新掩码是它的子集，直接掩码即可。
    std::size_t destination_index(std::string_view key, std::size_t old_index) const;
    bool table_contains(int table, std::size_t index, std::string_view key) const;
    void push_front(int table, std::size_t index, std::string key, std::string value);
    void emit_bucket(int table, std::uint64_t index,
                     const std::function<void(const Entry&)>& visit) const;

    Table tables_[2];
    std::int64_t rehash_index_ = -1;
    int pause_depth_ = 0;

    std::int64_t last_moved_bucket_ = -1;
    std::size_t last_moved_entries_ = 0;

    std::size_t buckets_moved_ = 0;
    std::size_t entries_moved_ = 0;
    std::size_t steps_ = 0;
    std::size_t max_buckets_per_step_ = 0;
    std::size_t empty_buckets_skipped_ = 0;
    std::size_t step_time_ns_ = 0;
    std::size_t max_step_time_ns_ = 0;
};

// ------------------------------ 实现 --------------------------------------

inline RehashPause::RehashPause(IncrementalDict& dict) : dict_(dict) {
    dict_.pause_rehashing();
}

inline RehashPause::~RehashPause() { dict_.resume_rehashing(); }

inline IncrementalDict::IncrementalDict(std::size_t initial_slots) {
    if (initial_slots != 0) {
        tables_[0].buckets.resize(next_pow2(initial_slots));
    }
}

inline std::uint64_t IncrementalDict::hash(std::string_view key) {
    // FNV-1a 64：确定、便宜、分布够好。Redis 用带随机种子的 SipHash，
    // 目的是让攻击者无法构造同桶冲突（hash flooding）。
    std::uint64_t value = 1469598103934665603ULL;
    for (const unsigned char byte : key) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    return value;
}

inline std::uint64_t IncrementalDict::reverse_bits(std::uint64_t value) {
    // 等价于 Redis 那个 while 循环版的 rev()，只是把 6 层交换写死。
    value = ((value >> 1) & 0x5555555555555555ULL) | ((value & 0x5555555555555555ULL) << 1);
    value = ((value >> 2) & 0x3333333333333333ULL) | ((value & 0x3333333333333333ULL) << 2);
    value = ((value >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((value & 0x0F0F0F0F0F0F0F0FULL) << 4);
    value = ((value >> 8) & 0x00FF00FF00FF00FFULL) | ((value & 0x00FF00FF00FF00FFULL) << 8);
    value = ((value >> 16) & 0x0000FFFF0000FFFFULL) | ((value & 0x0000FFFF0000FFFFULL) << 16);
    return (value >> 32) | (value << 32);
}

inline std::size_t IncrementalDict::next_pow2(std::size_t slots) {
    std::size_t size = kMinSlots;
    while (size < slots) {
        if (size > std::numeric_limits<std::size_t>::max() / 2) {
            throw std::length_error("IncrementalDict: table size overflow");
        }
        size <<= 1;
    }
    return size;
}

inline std::size_t IncrementalDict::slots(int table) const {
    return tables_[table].buckets.size();
}

inline std::size_t IncrementalDict::used(int table) const { return tables_[table].used; }

inline std::uint64_t IncrementalDict::mask(int table) const {
    const std::size_t count = tables_[table].buckets.size();
    return count == 0 ? 0 : static_cast<std::uint64_t>(count - 1);
}

inline void IncrementalDict::resume_rehashing() {
    if (pause_depth_ == 0) {
        throw std::logic_error("IncrementalDict: resume_rehashing without pause");
    }
    --pause_depth_;
}

inline bool IncrementalDict::table_contains(int table, std::size_t index,
                                            std::string_view key) const {
    for (const Entry& entry : tables_[table].buckets[index]) {
        if (entry.key == key) {
            return true;
        }
    }
    return false;
}

inline void IncrementalDict::push_front(int table, std::size_t index, std::string key,
                                        std::string value) {
    // Redis 把新条目插到链头：刚写入的 key 更可能被马上读到。
    tables_[table].buckets[index].push_front(Entry{std::move(key), std::move(value)});
    ++tables_[table].used;
}

inline bool IncrementalDict::insert(std::string key, std::string value) {
    step();

    // 对应 _dictExpandIfNeeded：负载因子到 1 才扩，扩到“装得下 used+1”的 2 的幂。
    if (!rehashing() && (tables_[0].buckets.empty() || tables_[0].used >= slots(0))) {
        expand(tables_[0].used + 1);
    }

    const std::uint64_t hashed = hash(key);
    if (tables_[0].buckets.empty()) {
        // expand 失败时（极端内存情况）仍然允许直接初始化。
        tables_[0].buckets.resize(kMinSlots);
    }
    const std::size_t index0 = static_cast<std::size_t>(hashed) & mask(0);
    if (table_contains(0, index0, key)) {
        return false;
    }
    if (rehashing()) {
        const std::size_t index1 = static_cast<std::size_t>(hashed) & mask(1);
        if (table_contains(1, index1, key)) {
            return false;
        }
        push_front(1, index1, std::move(key), std::move(value));
    } else {
        push_front(0, index0, std::move(key), std::move(value));
    }
    return true;
}

inline IncrementalDict::Entry* IncrementalDict::find(std::string_view key) {
    if (size() == 0) {
        return nullptr;
    }
    step();

    const std::uint64_t hashed = hash(key);
    for (int table = 0; table < 2; ++table) {
        const std::size_t index = static_cast<std::size_t>(hashed) & mask(table);
        for (Entry& entry : tables_[table].buckets[index]) {
            if (entry.key == key) {
                return &entry;
            }
        }
        // 没在搬就只可能有一张表。
        if (!rehashing()) {
            break;
        }
    }
    return nullptr;
}

inline bool IncrementalDict::erase(std::string_view key) {
    if (size() == 0) {
        return false;
    }
    step();

    const std::uint64_t hashed = hash(key);
    for (int table = 0; table < 2; ++table) {
        std::list<Entry>& bucket =
            tables_[table].buckets[static_cast<std::size_t>(hashed) & mask(table)];
        for (auto it = bucket.begin(); it != bucket.end(); ++it) {
            if (it->key == key) {
                bucket.erase(it);
                --tables_[table].used;
                return true;
            }
        }
        if (!rehashing()) {
            break;
        }
    }
    return false;
}

inline bool IncrementalDict::expand(std::size_t min_slots) {
    if (rehashing()) {
        return false;
    }
    const std::size_t target = next_pow2(min_slots == 0 ? kMinSlots : min_slots);

    // 第一次插入：只是把表建出来，不算 rehash。
    if (tables_[0].buckets.empty()) {
        tables_[0].buckets.resize(target);
        return true;
    }
    // size 比现有元素还小，没有意义（Redis 的 _dictExpand 直接返回错误）。
    if (tables_[0].used > min_slots) {
        return false;
    }
    if (target == tables_[0].buckets.size()) {
        return false;
    }

    tables_[1].buckets.assign(target, std::list<Entry>{});
    tables_[1].used = 0;
    rehash_index_ = 0;
    return true;
}

inline bool IncrementalDict::needs_shrink() const {
    const std::size_t count = tables_[0].buckets.size();
    // 对应 htNeedsResize: size > 4 && used*100/size < 10。
    return count > kMinSlots && tables_[0].used * 100 < count * 10;
}

inline bool IncrementalDict::shrink() {
    if (rehashing()) {
        return false;
    }
    return expand(std::max<std::size_t>(tables_[0].used, kMinSlots));
}

inline void IncrementalDict::step() {
    if (pause_depth_ == 0) {
        rehash_buckets(1);
    }
}

inline std::size_t IncrementalDict::destination_index(std::string_view key,
                                                      std::size_t old_index) const {
    if (slots(1) > slots(0)) {
        // 扩容：新掩码多了高位，必须用完整 hash 重新算落点。
        return static_cast<std::size_t>(hash(key)) & mask(1);
    }
    // 缩容：旧桶号本身就是 hash & mask_old，而 mask_new 是 mask_old 的子集，
    // 所以 hash & mask_new == old_index & mask_new，不必再算一次 hash。
    return old_index & mask(1);
}

inline void IncrementalDict::finish_rehash() {
    // 旧表一定空了：整张表被释放，新表“升格”为唯一的一张表。
    tables_[0] = std::move(tables_[1]);
    tables_[1] = Table{};
    rehash_index_ = -1;
}

inline void IncrementalDict::record_step(TimePoint start, std::size_t moved_buckets) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    const auto nanoseconds = static_cast<std::size_t>(elapsed);
    ++steps_;
    buckets_moved_ += moved_buckets;
    max_buckets_per_step_ = std::max(max_buckets_per_step_, moved_buckets);
    step_time_ns_ += nanoseconds;
    max_step_time_ns_ = std::max(max_step_time_ns_, nanoseconds);
}

inline bool IncrementalDict::rehash_buckets(std::size_t buckets) {
    if (buckets == 0 || pause_depth_ > 0 || !rehashing()) {
        return rehashing();
    }

    const TimePoint start = std::chrono::steady_clock::now();
    last_moved_bucket_ = -1;
    last_moved_entries_ = 0;
    std::size_t moved_buckets = 0;
    // 空桶上限：避免稀疏大表在一次调用里扫过成千上万个空桶，
    // 那样“渐进式”就退化成了“一次长停顿”。
    std::size_t empty_visits = buckets * kEmptyVisitFactor;
    std::vector<std::list<Entry>>& old_buckets = tables_[0].buckets;

    while (buckets > 0 && tables_[0].used != 0) {
        --buckets;
        std::size_t index = static_cast<std::size_t>(rehash_index_);

        while (index < old_buckets.size() && old_buckets[index].empty()) {
            ++index;
            ++rehash_index_;
            ++empty_buckets_skipped_;
            if (--empty_visits == 0) {
                record_step(start, moved_buckets);
                return true;
            }
        }
        if (index >= old_buckets.size()) {
            // 理论上不可达：used0 > 0 时一定还有非空桶。防御性收尾。
            finish_rehash();
            record_step(start, moved_buckets);
            return false;
        }

        // 整条链一起搬：不重新分配条目，只重接 next 指针。
        std::list<Entry>& bucket = old_buckets[index];
        const std::size_t entries = bucket.size();
        while (!bucket.empty()) {
            auto it = bucket.begin();
            const std::size_t destination = destination_index(it->key, index);
            tables_[1].buckets[destination].splice(tables_[1].buckets[destination].begin(),
                                                   bucket, it);
            ++tables_[1].used;
            --tables_[0].used;
        }
        ++rehash_index_;
        ++moved_buckets;
        last_moved_bucket_ = static_cast<std::int64_t>(index);
        last_moved_entries_ = entries;
        entries_moved_ += entries;
    }

    if (tables_[0].used == 0) {
        finish_rehash();
    }
    record_step(start, moved_buckets);
    return rehashing();
}

inline std::size_t IncrementalDict::rehash_milliseconds(int milliseconds) {
    if (pause_depth_ > 0 || !rehashing()) {
        return 0;
    }
    const TimePoint start = std::chrono::steady_clock::now();
    const std::size_t before = buckets_moved_;
    const int budget = milliseconds < 0 ? 0 : milliseconds;

    while (rehashing()) {
        rehash_buckets(100);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        if (elapsed >= budget) {
            break;
        }
    }
    return buckets_moved_ - before;
}

inline void IncrementalDict::full_rehash_now() {
    if (!rehashing()) {
        return;
    }
    // 一次搬完。空桶预算也按桶数放大，确保真的搬完。
    rehash_buckets(slots(0) + 1);
}

inline std::uint64_t IncrementalDict::scan(std::uint64_t cursor,
                                           const std::function<void(const Entry&)>& visit) {
    if (size() == 0) {
        return 0;
    }
    // SCAN 期间不搬桶：否则同一条链可能在两张表里各被访问一次，
    // 或者刚被搬走的 key 从游标后面漏掉。
    const RehashPause guard(*this);

    if (!rehashing()) {
        const std::uint64_t mask0 = mask(0);
        emit_bucket(0, cursor & mask0, visit);

        // 反序二进制游标：在掩码位之外全部置 1，反向、+1、再反向。
        cursor |= ~mask0;
        cursor = reverse_bits(cursor);
        ++cursor;
        cursor = reverse_bits(cursor);
        return cursor;
    }

    // 两张表时：小表在前，大表按“同一游标的全部扩展”逐个访问。
    int small = 0;
    int large = 1;
    if (slots(small) > slots(large)) {
        std::swap(small, large);
    }
    const std::uint64_t mask_small = mask(small);
    const std::uint64_t mask_large = mask(large);

    emit_bucket(small, cursor & mask_small, visit);
    do {
        emit_bucket(large, cursor & mask_large, visit);
        cursor |= ~mask_large;
        cursor = reverse_bits(cursor);
        ++cursor;
        cursor = reverse_bits(cursor);
    } while ((cursor & (mask_small ^ mask_large)) != 0);
    return cursor;
}

inline void IncrementalDict::emit_bucket(int table, std::uint64_t index,
                                         const std::function<void(const Entry&)>& visit) const {
    const std::size_t slot = static_cast<std::size_t>(index);
    if (slot >= tables_[table].buckets.size()) {
        return;
    }
    for (const Entry& entry : tables_[table].buckets[slot]) {
        visit(entry);
    }
}

inline IncrementalDict::Stats IncrementalDict::stats() const {
    Stats result;
    result.slots[0] = tables_[0].buckets.size();
    result.slots[1] = tables_[1].buckets.size();
    result.used[0] = tables_[0].used;
    result.used[1] = tables_[1].used;
    result.rehash_index = rehash_index_;
    result.buckets_moved = buckets_moved_;
    result.entries_moved = entries_moved_;
    result.steps = steps_;
    result.max_buckets_per_step = max_buckets_per_step_;
    result.empty_buckets_skipped = empty_buckets_skipped_;
    result.step_time_ns = step_time_ns_;
    result.max_step_time_ns = max_step_time_ns_;
    return result;
}

inline std::size_t IncrementalDict::longest_chain() const {
    std::size_t longest = 0;
    for (const Table& table : tables_) {
        for (const std::list<Entry>& bucket : table.buckets) {
            longest = std::max(longest, bucket.size());
        }
    }
    return longest;
}

inline std::size_t IncrementalDict::count_entries() const {
    std::size_t total = 0;
    for (const Table& table : tables_) {
        for (const std::list<Entry>& bucket : table.buckets) {
            total += bucket.size();
        }
    }
    return total;
}

inline std::vector<std::vector<std::string>> IncrementalDict::bucket_keys(int table) const {
    std::vector<std::vector<std::string>> result;
    result.reserve(tables_[table].buckets.size());
    for (const std::list<Entry>& bucket : tables_[table].buckets) {
        std::vector<std::string> keys;
        keys.reserve(bucket.size());
        for (const Entry& entry : bucket) {
            keys.push_back(entry.key);
        }
        result.push_back(std::move(keys));
    }
    return result;
}

}  // namespace archdesign
