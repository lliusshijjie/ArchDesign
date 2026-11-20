// expire_evict.hpp
//
// Day 16 的最小实现：Redis 风格的「过期删除 + 内存淘汰」。
//
// 目标不是复刻 redis/src/expire.c 和 evict.c 的每一行，而是把两条机制里
// 真正可迁移的部分做成能跑、能测、能破坏的结构：
//
//   1. 惰性过期：读/写路径上顺手检查 TTL，过期就删；没人访问的过期键会一直占内存。
//   2. 主动过期：cron 定期从 expires 表里随机抽样，删掉过期键；按「抽样」而不是
//      「全表扫描」工作，用时间预算兜底。
//   3. 内存淘汰：超过 maxmemory 时，按策略（LRU/LFU/RANDOM/TTL）从样本里挑
//      「最该走」的键；用固定大小的淘汰池提高近似精度；没有候选就 OOM。
//
// 与 Redis 7.2 的对应关系写在 README 第 3 节。刻意的简化：
//   - 单线程、无锁、无事件循环（Day 02 已经讲过网络层）；
//   - expires 表复制了一份 key（Redis 复用主表的 sds），所以内存账更悲观；
//   - 用稠密槽位 + 空闲链表模拟 dict，随机抽样是「随机槽位」而不是「随机桶」；
//   - 不做 lazyfree/UNLINK，删除是同步的；lazyfree 的代价在文章里讨论。

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace archdesign {

using i64 = std::int64_t;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8 = std::uint8_t;

// ---------------------------------------------------------------------------
// 逻辑时钟：生产里是 mstime()/steady_clock，测试里是手动推进的变量。
// 把时间注入进来，过期/淘汰的测试才能不 sleep、不 flaky。
// ---------------------------------------------------------------------------
class Clock {
public:
    static Clock system() {
        return Clock([] {
            return static_cast<i64>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
        });
    }

    // 指向外部变量；调用方保证生命周期覆盖 Store。
    static Clock manual(const i64* now_ms) {
        return Clock([now_ms] { return *now_ms; });
    }

    i64 now() const { return fn_(); }

private:
    explicit Clock(std::function<i64()> fn) : fn_(std::move(fn)) {}
    std::function<i64()> fn_;
};

// ---------------------------------------------------------------------------
// 淘汰策略：名字与 redis.conf 的 maxmemory-policy 一一对应。
// ---------------------------------------------------------------------------
enum class Policy : u8 {
    NoEviction,
    AllKeysLru,
    AllKeysLfu,
    AllKeysRandom,
    VolatileLru,
    VolatileLfu,
    VolatileRandom,
    VolatileTtl,
};

constexpr bool is_volatile_policy(Policy p) {
    return p == Policy::VolatileLru || p == Policy::VolatileLfu ||
           p == Policy::VolatileRandom || p == Policy::VolatileTtl;
}

constexpr bool is_lfu_policy(Policy p) {
    return p == Policy::AllKeysLfu || p == Policy::VolatileLfu;
}

constexpr bool is_lru_policy(Policy p) {
    return p == Policy::AllKeysLru || p == Policy::VolatileLru;
}

constexpr bool is_random_policy(Policy p) {
    return p == Policy::AllKeysRandom || p == Policy::VolatileRandom;
}

inline const char* policy_name(Policy p) {
    switch (p) {
        case Policy::NoEviction: return "noeviction";
        case Policy::AllKeysLru: return "allkeys-lru";
        case Policy::AllKeysLfu: return "allkeys-lfu";
        case Policy::AllKeysRandom: return "allkeys-random";
        case Policy::VolatileLru: return "volatile-lru";
        case Policy::VolatileLfu: return "volatile-lfu";
        case Policy::VolatileRandom: return "volatile-random";
        case Policy::VolatileTtl: return "volatile-ttl";
    }
    return "unknown";
}

inline std::optional<Policy> policy_from_name(std::string_view name) {
    for (Policy p : {Policy::NoEviction, Policy::AllKeysLru, Policy::AllKeysLfu,
                     Policy::AllKeysRandom, Policy::VolatileLru, Policy::VolatileLfu,
                     Policy::VolatileRandom, Policy::VolatileTtl}) {
        if (name == policy_name(p)) return p;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// EXPIRE 的选项：与 Redis 6.0+ 的 NX/XX/GT/LT 对齐。
// ---------------------------------------------------------------------------
enum class ExpireFlags : u8 { None = 0, Nx = 1, Xx = 2, Gt = 4, Lt = 8 };

constexpr ExpireFlags operator|(ExpireFlags a, ExpireFlags b) {
    return ExpireFlags(static_cast<u8>(static_cast<u8>(a) | static_cast<u8>(b)));
}

constexpr bool has_flag(ExpireFlags value, ExpireFlags flag) {
    return (static_cast<u8>(value) & static_cast<u8>(flag)) != 0;
}

struct Config {
    Policy policy = Policy::NoEviction;
    u64 maxmemory = 0;  // 0 = 不限制
    int maxmemory_samples = 5;
    int lfu_log_factor = 10;
    int lfu_decay_time = 1;      // 分钟；0 = 不衰减
    int hz = 10;                 // cron 频率；Redis 默认 10
    int active_expire_effort = 1;  // 1..10；每轮抽样 20*effort 个
    bool replica = false;        // 从库不自己删过期键，只把它们「藏起来」
    std::uint32_t seed = 42;     // 抽样/概率递增用，测试可复现
};

struct Entry {
    std::string key;
    std::string value;
    i64 expire_at_ms = -1;  // -1 = 无 TTL
    // 一个字段两种解释（和 Redis 的 robj->lru 一样）：
    //   LRU 策略：低 24 位逻辑时钟（秒分辨率）
    //   LFU 策略：高 16 位「上次衰减的分钟数」+ 低 8 位计数器
    u32 lru = 0;
};

// 诊断用的删除原因：同一段删除逻辑，统计口径不同。
enum class DeleteReason { Expired, Evicted, Del };

// ---------------------------------------------------------------------------
// 淘汰池：固定 16 个槽，按 idle 升序保存「最值得淘汰」的候选。
// 池跨多轮淘汰保留（Redis 也一样），所以样本会累积，近似精度比单轮采样高。
// ---------------------------------------------------------------------------
class EvictionPool {
public:
    static constexpr int kCapacity = 16;

    struct Item {
        std::string key;
        u64 idle = 0;  // 分数越大越该淘汰
    };

    // 插入并按 idle 升序维护（池尾 = 最该淘汰）。
    // 池满时：比池里最小的还小就直接丢弃；否则丢掉最小的，再插入。
    void insert(std::string key, u64 idle) {
        if (count_ == kCapacity && idle <= items_[0].idle) {
            ++rejected_;
            return;
        }
        if (count_ == kCapacity) {
            // 丢掉最小的（下标 0），为后面的插入腾一个位置。
            for (int i = 0; i + 1 < count_; ++i) {
                items_[static_cast<std::size_t>(i)] = std::move(items_[static_cast<std::size_t>(i + 1)]);
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

    // 取出分数最高的候选（池尾）。
    std::optional<Item> pop_best() {
        if (count_ == 0) return std::nullopt;
        --count_;
        Item item = std::move(items_[static_cast<std::size_t>(count_)]);
        items_[static_cast<std::size_t>(count_)] = Item{};
        return item;
    }

    int count() const { return count_; }
    u64 inserts() const { return inserts_; }
    u64 rejected() const { return rejected_; }
    const Item& at(int i) const { return items_[static_cast<std::size_t>(i)]; }
    void clear() {
        for (auto& item : items_) item = Item{};
        count_ = 0;
    }

private:
    std::array<Item, static_cast<std::size_t>(kCapacity)> items_{};
    int count_ = 0;
    u64 inserts_ = 0;
    u64 rejected_ = 0;
};

// ---------------------------------------------------------------------------
// Store：一个极小的「单线程 keyspace」。
// ---------------------------------------------------------------------------
class Store {
public:
    struct Stats {
        u64 expired_keys = 0;   // 惰性 + 主动删除的过期键总数
        u64 lazy_expired = 0;   // 被访问时才发现过期
        u64 active_expired = 0; // 被 cron 抽样删掉
        u64 replica_hidden = 0; // 从库上「逻辑过期但不删」的次数
        u64 evicted_keys = 0;
        u64 del_keys = 0;
        u64 oom_errors = 0;
        u64 set_keys = 0;
        u64 lookups = 0;
        u64 hits = 0;
        u64 misses = 0;
        u64 active_cycles = 0;
        u64 active_samples = 0;
        u64 eviction_rounds = 0;
    };

    struct ActiveExpireResult {
        u64 sampled = 0;
        u64 expired = 0;
        u64 iterations = 0;   // 一轮周期内重复抽样的次数
        u64 elapsed_us = 0;
        bool time_limit_exit = false;
        bool empty = false;
    };

    struct EvictResult {
        bool ok = true;        // 是否达到目标内存
        u64 evicted = 0;
        u64 freed_bytes = 0;
        u64 elapsed_us = 0;
        bool no_candidates = false;
    };

    // Redis 的常量（7.2）：一轮 20 个 key；最多 16 轮；过期比例 >=10% 就继续抽；
    // slow cycle 最多占 25% CPU；淘汰池 16 个。
    static constexpr int kKeysPerLoop = 20;
    static constexpr int kMaxIterations = 16;
    static constexpr int kAcceptableStalePct = 10;
    static constexpr int kSlowTimePerc = 25;
    static constexpr u8 kLfuInitVal = 5;
    static constexpr u32 kLruBits = 24;
    static constexpr u32 kLruClockMax = (1u << kLruBits) - 1;

    Store(Config cfg, Clock clock)
        : cfg_(cfg), clock_(std::move(clock)), rng_(cfg.seed) {}

    Store() : Store(Config{}, Clock::system()) {}

    // ---- 只读信息 ---------------------------------------------------------
    const Config& config() const { return cfg_; }
    const Stats& stats() const { return stats_; }
    std::size_t size() const { return index_.size(); }
    std::size_t expires_size() const { return expires_index_.size(); }
    u64 memory_used() const { return memory_used_; }
    bool raw_contains(const std::string& key) const { return index_.count(key) != 0; }
    const EvictionPool& eviction_pool() const { return pool_; }

    void set_policy(Policy p) { cfg_.policy = p; }
    void set_maxmemory(u64 bytes) { cfg_.maxmemory = bytes; }
    void set_replica(bool v) { cfg_.replica = v; }

    // ---- 命令 -------------------------------------------------------------
    std::optional<std::string> get(const std::string& key);
    bool exists(const std::string& key);
    bool set(const std::string& key, std::string value, bool keep_ttl = false);

    // DEL：和 Redis 一样先 expireIfNeeded；主库上删过期键返回 0（它已经「不算存在」）。
    int del(const std::string& key);

    // 返回 1 设置成功，0 未设置，-1 选项组合非法。
    int expire_at(const std::string& key, i64 when_ms, ExpireFlags flags = ExpireFlags::None);
    int expire_in(const std::string& key, i64 ttl_ms, ExpireFlags flags = ExpireFlags::None) {
        return expire_at(key, clock_.now() + ttl_ms, flags);
    }
    int persist(const std::string& key);

    // -2 = key 不存在（或已过期），-1 = 没有 TTL，>=0 = 剩余毫秒。
    i64 pttl(const std::string& key);
    // Redis 的 TTL 是四舍五入到秒：(ms + 500) / 1000。
    i64 ttl(const std::string& key);

    std::optional<u64> object_idletime(const std::string& key);  // ms；仅 LRU
    std::optional<u8> object_freq(const std::string& key);       // 仅 LFU

    // ---- 主动过期 ---------------------------------------------------------
    // budget_us < 0 时按 hz 计算：1s * 25% / hz（hz=10 => 25ms）。
    ActiveExpireResult active_expire_cycle(i64 budget_us = -1);

    // ---- 淘汰 -------------------------------------------------------------
    // 释放至少 bytes_to_free 字节：反复挑候选删除，直到内存降到目标以下。
    EvictResult perform_evictions(u64 bytes_to_free);

    // ---- 测试/教学辅助 -----------------------------------------------------
    u64 memory_recomputed() const;
    bool check_consistency(std::string* error = nullptr) const;

    // 不触发惰性过期的原始视图，用来证明「从库只是藏起来，没有删」。
    std::optional<std::string> peek_raw(const std::string& key) const;
    bool raw_has_expire(const std::string& key) const;

    // 近似 LRU 的空闲时间，等价于 Redis 的 estimateObjectIdleTime。
    u64 estimate_idle_ms(i64 now_ms, u32 lru) const;

private:
    static constexpr u32 kNoPos = std::numeric_limits<u32>::max();

    // 内存模型：只求数量级与相对关系正确，不追求和 jemalloc 字节级一致。
    static constexpr u64 kEntryAllocOverhead = 32;
    static constexpr u64 kExpiresAllocOverhead = 24;

    struct Slot {
        Entry entry;
        bool used = false;
    };

    struct ExpiresNode {
        std::string key;
        i64 when_ms = 0;
        bool used = false;
    };

    // 主表
    std::vector<Slot> slots_;
    std::vector<u32> free_slots_;
    std::vector<u32> slots_live_;
    std::vector<u32> slots_pos_;
    std::unordered_map<std::string, u32> index_;

    // expires 表（对应 db->expires）
    std::vector<ExpiresNode> expires_nodes_;
    std::vector<u32> expires_free_;
    std::vector<u32> expires_live_;
    std::vector<u32> expires_pos_;
    std::unordered_map<std::string, u32> expires_index_;

    Config cfg_;
    Clock clock_;
    std::mt19937 rng_;
    Stats stats_;
    EvictionPool pool_;
    u64 memory_used_ = 0;

    // ---- 哈希表小工具 -----------------------------------------------------
    static u64 entry_bytes(const Entry& e);
    static u64 entry_bytes(const std::string& key, const std::string& value);
    static u64 expires_bytes(const std::string& key);

    u32 add_slot(Entry e);
    void erase_slot(u32 id);
    bool expires_set(const std::string& key, i64 when_ms);
    bool expires_erase(const std::string& key);

    Entry* find_entry(const std::string& key);
    const Entry* find_entry(const std::string& key) const;

    // 返回 true 表示「逻辑上已过期」；主库上会顺手删除，从库上只隐藏。
    bool expire_if_needed(const std::string& key);
    void delete_key(const std::string& key, DeleteReason reason);
    void touch_entry(Entry& e);
    void init_lru(Entry& e) const;

    bool ensure_room(u64 extra_bytes);
    bool evict_one();
    bool populate_eviction_pool();
    u64 eviction_score(const Entry& e, i64 now_ms) const;
    std::string random_key(bool volatile_only);

    std::vector<u32> sample_slot_ids(int count);
    std::vector<u32> sample_expires_ids(int count);

    // ---- LRU / LFU 基础函数 ----------------------------------------------
    static u32 lru_clock(i64 now_ms) {
        return static_cast<u32>((now_ms / 1000) & kLruClockMax);
    }
    static u32 lfu_minutes(i64 now_ms) {
        return static_cast<u32>((now_ms / 60000) & 0xFFFF);
    }
    static u64 lfu_time_elapsed(u32 now_minutes, u32 ldt) {
        return static_cast<u64>((now_minutes - ldt) & 0xFFFF);
    }
    u8 lfu_counter_after_decay(u32 lru, i64 now_ms) const;
    u8 lfu_log_incr(u8 counter);
    double random01() {
        return std::generate_canonical<double, 32>(rng_);
    }

    static u64 elapsed_us(std::chrono::steady_clock::time_point start) {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
                                    .count());
    }
};

// ===========================================================================
// 实现
// ===========================================================================

inline u64 Store::entry_bytes(const Entry& e) {
    return static_cast<u64>(sizeof(Entry)) + e.key.size() + 1 + e.value.size() + 1 +
           kEntryAllocOverhead;
}

inline u64 Store::entry_bytes(const std::string& key, const std::string& value) {
    return static_cast<u64>(sizeof(Entry)) + key.size() + 1 + value.size() + 1 +
           kEntryAllocOverhead;
}

inline u64 Store::expires_bytes(const std::string& key) {
    return static_cast<u64>(sizeof(ExpiresNode)) + key.size() + 1 + kExpiresAllocOverhead;
}

inline u32 Store::add_slot(Entry e) {
    u32 id = 0;
    if (!free_slots_.empty()) {
        id = free_slots_.back();
        free_slots_.pop_back();
        slots_[id].entry = std::move(e);
        slots_[id].used = true;
    } else {
        id = static_cast<u32>(slots_.size());
        slots_.push_back(Slot{std::move(e), true});
    }
    if (slots_pos_.size() <= id) slots_pos_.resize(static_cast<std::size_t>(id) + 1, kNoPos);
    slots_pos_[id] = static_cast<u32>(slots_live_.size());
    slots_live_.push_back(id);
    return id;
}

inline void Store::erase_slot(u32 id) {
    const u32 pos = slots_pos_[id];
    const u32 last = slots_live_.back();
    slots_live_[pos] = last;
    slots_pos_[last] = pos;
    slots_live_.pop_back();
    slots_pos_[id] = kNoPos;

    slots_[id].entry = Entry{};
    slots_[id].used = false;
    free_slots_.push_back(id);
}

inline bool Store::expires_set(const std::string& key, i64 when_ms) {
    auto it = expires_index_.find(key);
    if (it != expires_index_.end()) {
        expires_nodes_[it->second].when_ms = when_ms;
        return false;
    }
    u32 id = 0;
    if (!expires_free_.empty()) {
        id = expires_free_.back();
        expires_free_.pop_back();
        expires_nodes_[id] = ExpiresNode{key, when_ms, true};
    } else {
        id = static_cast<u32>(expires_nodes_.size());
        expires_nodes_.push_back(ExpiresNode{key, when_ms, true});
    }
    if (expires_pos_.size() <= id) {
        expires_pos_.resize(static_cast<std::size_t>(id) + 1, kNoPos);
    }
    expires_pos_[id] = static_cast<u32>(expires_live_.size());
    expires_live_.push_back(id);
    expires_index_[key] = id;
    return true;
}

inline bool Store::expires_erase(const std::string& key) {
    auto it = expires_index_.find(key);
    if (it == expires_index_.end()) return false;
    const u32 id = it->second;
    const u32 pos = expires_pos_[id];
    const u32 last = expires_live_.back();
    expires_live_[pos] = last;
    expires_pos_[last] = pos;
    expires_live_.pop_back();
    expires_pos_[id] = kNoPos;

    expires_nodes_[id].key.clear();
    expires_nodes_[id].when_ms = 0;
    expires_nodes_[id].used = false;
    expires_free_.push_back(id);
    expires_index_.erase(it);
    return true;
}

inline Entry* Store::find_entry(const std::string& key) {
    auto it = index_.find(key);
    if (it == index_.end()) return nullptr;
    return &slots_[it->second].entry;
}

inline const Entry* Store::find_entry(const std::string& key) const {
    auto it = index_.find(key);
    if (it == index_.end()) return nullptr;
    return &slots_[it->second].entry;
}

inline void Store::init_lru(Entry& e) const {
    if (is_lfu_policy(cfg_.policy)) {
        e.lru = (lfu_minutes(clock_.now()) << 8) | kLfuInitVal;
    } else {
        e.lru = lru_clock(clock_.now());
    }
}

inline u8 Store::lfu_counter_after_decay(u32 lru, i64 now_ms) const {
    u8 counter = static_cast<u8>(lru & 0xFF);
    if (cfg_.lfu_decay_time <= 0) return counter;
    const u32 ldt = (lru >> 8) & 0xFFFF;
    const u64 periods = lfu_time_elapsed(lfu_minutes(now_ms), ldt) /
                        static_cast<u64>(cfg_.lfu_decay_time);
    if (periods == 0) return counter;
    if (periods >= counter) return 0;
    return static_cast<u8>(counter - periods);
}

inline u8 Store::lfu_log_incr(u8 counter) {
    if (counter == 255) return 255;
    const double base = counter > kLfuInitVal ? static_cast<double>(counter - kLfuInitVal) : 0.0;
    const double probability = 1.0 / (base * static_cast<double>(cfg_.lfu_log_factor) + 1.0);
    if (random01() < probability) ++counter;
    return counter;
}

inline void Store::touch_entry(Entry& e) {
    if (is_lfu_policy(cfg_.policy)) {
        u8 counter = lfu_counter_after_decay(e.lru, clock_.now());
        counter = lfu_log_incr(counter);
        e.lru = (lfu_minutes(clock_.now()) << 8) | counter;
    } else {
        // Redis 即使当前不是 LRU 策略也会更新 lru 字段：策略可以运行时切换。
        e.lru = lru_clock(clock_.now());
    }
}

inline u64 Store::estimate_idle_ms(i64 now_ms, u32 lru) const {
    const u32 now_clock = lru_clock(now_ms);
    const u32 then = lru & kLruClockMax;
    if (now_clock >= then) {
        return static_cast<u64>(now_clock - then) * 1000ULL;
    }
    // 24 位时钟回绕：now 已经绕到 0，then 还在最大值附近。
    return (static_cast<u64>(now_clock) + (kLruClockMax - static_cast<u64>(then))) * 1000ULL;
}

inline bool Store::expire_if_needed(const std::string& key) {
    Entry* e = find_entry(key);
    if (e == nullptr) return false;
    if (e->expire_at_ms < 0) return false;
    if (clock_.now() <= e->expire_at_ms) return false;  // Redis: now > when 才算过期

    if (cfg_.replica) {
        // 从库：逻辑上不可见，但不删。删除权在主库，否则主从会分叉。
        ++stats_.replica_hidden;
        return true;
    }

    delete_key(key, DeleteReason::Expired);
    ++stats_.lazy_expired;
    return true;
}

inline void Store::delete_key(const std::string& key, DeleteReason reason) {
    auto it = index_.find(key);
    if (it == index_.end()) return;

    const u32 id = it->second;
    Entry& e = slots_[id].entry;
    memory_used_ -= entry_bytes(e);
    if (e.expire_at_ms >= 0) {
        memory_used_ -= expires_bytes(key);
        expires_erase(key);
    }

    index_.erase(it);
    erase_slot(id);

    switch (reason) {
        case DeleteReason::Expired: ++stats_.expired_keys; break;
        case DeleteReason::Evicted: ++stats_.evicted_keys; break;
        case DeleteReason::Del: ++stats_.del_keys; break;
    }
}

inline std::optional<std::string> Store::get(const std::string& key) {
    ++stats_.lookups;
    if (expire_if_needed(key)) {
        ++stats_.misses;
        return std::nullopt;
    }
    Entry* e = find_entry(key);
    if (e == nullptr) {
        ++stats_.misses;
        return std::nullopt;
    }
    touch_entry(*e);
    ++stats_.hits;
    return e->value;
}

inline bool Store::exists(const std::string& key) {
    ++stats_.lookups;
    if (expire_if_needed(key)) {
        ++stats_.misses;
        return false;
    }
    if (find_entry(key) == nullptr) {
        ++stats_.misses;
        return false;
    }
    ++stats_.hits;
    return true;
}

inline bool Store::ensure_room(u64 extra_bytes) {
    if (cfg_.maxmemory == 0) return true;
    if (memory_used_ + extra_bytes <= cfg_.maxmemory) return true;
    const u64 need = memory_used_ + extra_bytes - cfg_.maxmemory;
    (void)perform_evictions(need);
    return memory_used_ + extra_bytes <= cfg_.maxmemory;
}

inline bool Store::set(const std::string& key, std::string value, bool keep_ttl) {
    // 写路径也检查过期：否则会把一个「逻辑上已经不存在」的键当成更新。
    (void)expire_if_needed(key);

    u64 old_bytes = 0;
    bool had_ttl = false;
    if (const Entry* old = find_entry(key)) {
        old_bytes = entry_bytes(*old) +
                    (old->expire_at_ms >= 0 ? expires_bytes(key) : 0);
        had_ttl = old->expire_at_ms >= 0;
    }
    const bool keep = keep_ttl && had_ttl;
    const u64 new_bytes = entry_bytes(key, value) + (keep ? expires_bytes(key) : 0);

    if (new_bytes > old_bytes) {
        if (!ensure_room(new_bytes - old_bytes)) {
            ++stats_.oom_errors;
            return false;
        }
    }

    // 淘汰可能刚好把 key 自己删掉了，所以这里必须重新查找。
    if (Entry* e = find_entry(key)) {
        memory_used_ -= entry_bytes(*e);
        if (e->expire_at_ms >= 0 && !keep) {
            memory_used_ -= expires_bytes(key);
            expires_erase(key);
            e->expire_at_ms = -1;
        }
        e->value = std::move(value);
        touch_entry(*e);
        memory_used_ += entry_bytes(*e);
    } else {
        Entry fresh;
        fresh.key = key;
        fresh.value = std::move(value);
        init_lru(fresh);
        const u32 id = add_slot(std::move(fresh));
        index_[key] = id;
        memory_used_ += entry_bytes(slots_[id].entry);
    }
    ++stats_.set_keys;
    return true;
}

inline int Store::del(const std::string& key) {
    (void)expire_if_needed(key);  // 主库上会先删掉过期键，于是下面的 DEL 返回 0
    if (index_.count(key) == 0) return 0;
    delete_key(key, DeleteReason::Del);
    return 1;
}

inline int Store::expire_at(const std::string& key, i64 when_ms, ExpireFlags flags) {
    const bool nx = has_flag(flags, ExpireFlags::Nx);
    const bool xx = has_flag(flags, ExpireFlags::Xx);
    const bool gt = has_flag(flags, ExpireFlags::Gt);
    const bool lt = has_flag(flags, ExpireFlags::Lt);
    if ((nx && (xx || gt || lt)) || (xx && (gt || lt)) || (gt && lt)) return -1;

    (void)expire_if_needed(key);
    Entry* e = find_entry(key);
    if (e == nullptr) return 0;

    const bool has_ttl = e->expire_at_ms >= 0;
    if (nx && has_ttl) return 0;
    if (xx && !has_ttl) return 0;
    if (gt && (!has_ttl || when_ms <= e->expire_at_ms)) return 0;
    if (lt && has_ttl && when_ms >= e->expire_at_ms) return 0;

    // 主库上把过期时间设到过去 = 立刻删除；从库只记录，等主库发 DEL。
    if (when_ms <= clock_.now() && !cfg_.replica) {
        delete_key(key, DeleteReason::Del);
        return 1;
    }

    if (!has_ttl) memory_used_ += expires_bytes(key);
    expires_set(key, when_ms);
    e->expire_at_ms = when_ms;
    return 1;
}

inline int Store::persist(const std::string& key) {
    (void)expire_if_needed(key);
    Entry* e = find_entry(key);
    if (e == nullptr || e->expire_at_ms < 0) return 0;
    memory_used_ -= expires_bytes(key);
    expires_erase(key);
    e->expire_at_ms = -1;
    return 1;
}

inline i64 Store::pttl(const std::string& key) {
    if (expire_if_needed(key)) return -2;
    const Entry* e = find_entry(key);
    if (e == nullptr) return -2;
    if (e->expire_at_ms < 0) return -1;
    const i64 remaining = e->expire_at_ms - clock_.now();
    return remaining > 0 ? remaining : 0;
}

inline i64 Store::ttl(const std::string& key) {
    const i64 ms = pttl(key);
    if (ms < 0) return ms;
    return (ms + 500) / 1000;
}

inline std::optional<u64> Store::object_idletime(const std::string& key) {
    if (is_lfu_policy(cfg_.policy)) return std::nullopt;  // Redis 在 LFU 下不跟踪空闲时间
    if (expire_if_needed(key)) return std::nullopt;
    const Entry* e = find_entry(key);
    if (e == nullptr) return std::nullopt;
    return estimate_idle_ms(clock_.now(), e->lru);
}

inline std::optional<u8> Store::object_freq(const std::string& key) {
    if (!is_lfu_policy(cfg_.policy)) return std::nullopt;
    if (expire_if_needed(key)) return std::nullopt;
    const Entry* e = find_entry(key);
    if (e == nullptr) return std::nullopt;
    return lfu_counter_after_decay(e->lru, clock_.now());
}

inline std::vector<u32> Store::sample_slot_ids(int count) {
    std::vector<u32> out;
    if (count <= 0 || slots_live_.empty()) return out;
    if (static_cast<std::size_t>(count) >= slots_live_.size()) return slots_live_;

    out.reserve(static_cast<std::size_t>(count));
    std::unordered_set<u32> seen;
    seen.reserve(static_cast<std::size_t>(count) * 2);
    std::uniform_int_distribution<std::size_t> dist(0, slots_live_.size() - 1);
    while (out.size() < static_cast<std::size_t>(count)) {
        const u32 id = slots_live_[dist(rng_)];
        if (seen.insert(id).second) out.push_back(id);
    }
    return out;
}

inline std::vector<u32> Store::sample_expires_ids(int count) {
    std::vector<u32> out;
    if (count <= 0 || expires_live_.empty()) return out;
    if (static_cast<std::size_t>(count) >= expires_live_.size()) return expires_live_;

    out.reserve(static_cast<std::size_t>(count));
    std::unordered_set<u32> seen;
    seen.reserve(static_cast<std::size_t>(count) * 2);
    std::uniform_int_distribution<std::size_t> dist(0, expires_live_.size() - 1);
    while (out.size() < static_cast<std::size_t>(count)) {
        const u32 id = expires_live_[dist(rng_)];
        if (seen.insert(id).second) out.push_back(id);
    }
    return out;
}

inline std::string Store::random_key(bool volatile_only) {
    if (volatile_only) {
        if (expires_live_.empty()) return {};
        const u32 id = expires_live_[rng_() % expires_live_.size()];
        return expires_nodes_[id].key;
    }
    if (slots_live_.empty()) return {};
    const u32 id = slots_live_[rng_() % slots_live_.size()];
    return slots_[id].entry.key;
}

inline Store::ActiveExpireResult Store::active_expire_cycle(i64 budget_us) {
    ActiveExpireResult result;
    if (expires_index_.empty()) {
        result.empty = true;
        return result;
    }
    if (budget_us < 0) {
        budget_us = 1'000'000LL * kSlowTimePerc / 100 /
                    static_cast<i64>(std::max(1, cfg_.hz));
    }
    const int keys_per_loop =
        kKeysPerLoop * static_cast<int>(std::clamp(cfg_.active_expire_effort, 1, 10));

    const auto start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
        int sampled = 0;
        int expired = 0;
        for (int i = 0; i < keys_per_loop; ++i) {
            if (expires_live_.empty()) break;
            const u32 id = expires_live_[rng_() % expires_live_.size()];
            if (!expires_nodes_[id].used) continue;
            ++sampled;
            if (clock_.now() > expires_nodes_[id].when_ms) {
                // 先拷贝 key：delete_key 会把 expires 槽位清空。
                const std::string victim = expires_nodes_[id].key;
                delete_key(victim, DeleteReason::Expired);
                ++stats_.active_expired;
                ++expired;
            }
        }
        if (sampled == 0) {
            result.empty = true;
            break;
        }
        result.sampled += static_cast<u64>(sampled);
        result.expired += static_cast<u64>(expired);
        ++result.iterations;
        stats_.active_samples += static_cast<u64>(sampled);

        // 样本里过期比例低，说明过期键已经稀疏，本轮收工。
        if (expired * 100 / sampled < kAcceptableStalePct) break;
        if (elapsed_us(start) >= static_cast<u64>(budget_us)) {
            result.time_limit_exit = true;
            break;
        }
    }
    result.elapsed_us = elapsed_us(start);
    ++stats_.active_cycles;
    return result;
}

inline u64 Store::eviction_score(const Entry& e, i64 now_ms) const {
    switch (cfg_.policy) {
        case Policy::AllKeysLru:
        case Policy::VolatileLru:
            return estimate_idle_ms(now_ms, e.lru);
        case Policy::AllKeysLfu:
        case Policy::VolatileLfu: {
            // 分数越大越该走：counter 越小，255-counter 越大。
            return 255 - static_cast<u64>(lfu_counter_after_decay(e.lru, now_ms));
        }
        case Policy::VolatileTtl:
            // 剩余 TTL 越短越该走；用 max - expire_at 把「小」变成「大」。
            return std::numeric_limits<u64>::max() -
                   static_cast<u64>(std::max<i64>(0, e.expire_at_ms));
        default:
            return 0;
    }
}

inline bool Store::populate_eviction_pool() {
    const bool volatile_only = is_volatile_policy(cfg_.policy);
    const int samples = std::max(1, cfg_.maxmemory_samples);
    const i64 now_ms = clock_.now();

    if (volatile_only) {
        const std::vector<u32> ids = sample_expires_ids(samples);
        for (const u32 id : ids) {
            if (!expires_nodes_[id].used) continue;
            const std::string key = expires_nodes_[id].key;
            const Entry* e = find_entry(key);
            if (e == nullptr) continue;
            pool_.insert(key, eviction_score(*e, now_ms));
        }
    } else {
        const std::vector<u32> ids = sample_slot_ids(samples);
        for (const u32 id : ids) {
            if (!slots_[id].used) continue;
            const Entry& e = slots_[id].entry;
            pool_.insert(e.key, eviction_score(e, now_ms));
        }
    }
    return pool_.count() > 0;
}

inline bool Store::evict_one() {
    if (is_random_policy(cfg_.policy)) {
        const std::string victim = random_key(is_volatile_policy(cfg_.policy));
        if (victim.empty()) return false;
        delete_key(victim, DeleteReason::Evicted);
        return true;
    }

    // 每一轮都补充样本（Redis 的 performEvictions 也是每轮 populate 一次）。
    // 如果只在池空时补充，池里「最不该淘汰」的那个候选会在每批末尾被误杀。
    (void)populate_eviction_pool();

    for (;;) {
        std::optional<EvictionPool::Item> item = pool_.pop_best();
        if (!item) return false;
        // 池是跨轮保留的，可能残留已经被删除的 key。
        if (index_.count(item->key) == 0) continue;
        delete_key(item->key, DeleteReason::Evicted);
        return true;
    }
}

inline Store::EvictResult Store::perform_evictions(u64 bytes_to_free) {
    EvictResult result;
    if (cfg_.maxmemory == 0 || bytes_to_free == 0) return result;
    if (cfg_.policy == Policy::NoEviction) {
        result.ok = false;
        return result;
    }

    const u64 target = memory_used_ > bytes_to_free ? memory_used_ - bytes_to_free : 0;
    const u64 before = memory_used_;
    const auto start = std::chrono::steady_clock::now();

    // 防御性上限：正常情况下每次 evict_one 都会让内存严格下降。
    int guard = 0;
    while (memory_used_ > target) {
        if (++guard > 200000) {
            result.ok = false;
            result.no_candidates = true;
            break;
        }
        if (!evict_one()) {
            result.ok = false;
            result.no_candidates = true;
            break;
        }
        ++result.evicted;
    }
    result.ok = memory_used_ <= target;
    result.freed_bytes = before - memory_used_;
    result.elapsed_us = elapsed_us(start);
    ++stats_.eviction_rounds;
    return result;
}

inline u64 Store::memory_recomputed() const {
    u64 total = 0;
    for (const u32 id : slots_live_) {
        const Entry& e = slots_[id].entry;
        total += entry_bytes(e);
        if (e.expire_at_ms >= 0) total += expires_bytes(e.key);
    }
    return total;
}

inline bool Store::check_consistency(std::string* error) const {
    auto fail = [error](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };

    if (index_.size() != slots_live_.size()) return fail("index size != live slots");
    if (expires_index_.size() != expires_live_.size()) return fail("expires index size mismatch");
    if (memory_used_ != memory_recomputed()) return fail("memory counter drift");

    for (const auto& [key, id] : index_) {
        if (id >= slots_.size() || !slots_[id].used) return fail("index points at a dead slot");
        if (slots_[id].entry.key != key) return fail("slot key mismatch");
        if (slots_pos_[id] >= slots_live_.size() || slots_live_[slots_pos_[id]] != id) {
            return fail("slot position index broken");
        }
    }
    for (const auto& [key, id] : expires_index_) {
        if (id >= expires_nodes_.size() || !expires_nodes_[id].used) {
            return fail("expires index points at a dead node");
        }
        if (expires_nodes_[id].key != key) return fail("expires key mismatch");
        if (expires_pos_[id] >= expires_live_.size() || expires_live_[expires_pos_[id]] != id) {
            return fail("expires position index broken");
        }
        const Entry* e = find_entry(key);
        if (e == nullptr) return fail("expires entry without a main entry");
        if (e->expire_at_ms != expires_nodes_[id].when_ms) {
            return fail("expires timestamp mismatch");
        }
    }
    for (const u32 id : slots_live_) {
        const Entry& e = slots_[id].entry;
        if (e.expire_at_ms >= 0) {
            auto it = expires_index_.find(e.key);
            if (it == expires_index_.end()) return fail("TTL entry missing from expires table");
            if (expires_nodes_[it->second].when_ms != e.expire_at_ms) {
                return fail("TTL timestamp mismatch");
            }
        }
    }
    return true;
}

inline std::optional<std::string> Store::peek_raw(const std::string& key) const {
    const Entry* e = find_entry(key);
    if (e == nullptr) return std::nullopt;
    return e->value;
}

inline bool Store::raw_has_expire(const std::string& key) const {
    const Entry* e = find_entry(key);
    return e != nullptr && e->expire_at_ms >= 0;
}

}  // namespace archdesign
