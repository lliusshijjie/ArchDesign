// store.hpp
//
// 一个极小的「单线程 keyspace」：主表 + expires 表 + 惰性过期 + 抽样回收 + 淘汰。
//
// 这里只有接口和状态；实现按职责拆在：
//   store_core.cpp   —— 槽位/内存账、SET/GET/DEL/TTL、一致性检查
//   store_expire.cpp —— 惰性过期、删除、抽样、active_expire_cycle
//   store_evict.cpp  —— 淘汰池填充、单次淘汰、perform_evictions
//   store_access.cpp —— LRU/LFU 的时钟、衰减、热度更新

#pragma once

#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "clock.hpp"
#include "common.hpp"
#include "eviction_pool.hpp"

namespace archdesign {

class Store {
public:
    struct Stats {
        u64 expired_keys = 0;    // 惰性 + 主动删除的过期键总数
        u64 lazy_expired = 0;    // 被访问时才发现过期
        u64 active_expired = 0;  // 被 cron 抽样删掉
        u64 replica_hidden = 0;  // 从库上「逻辑过期但不删」的次数
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
        u64 iterations = 0;  // 一轮周期内重复抽样的次数
        u64 elapsed_us = 0;
        bool time_limit_exit = false;
        bool empty = false;
    };

    struct EvictResult {
        bool ok = true;  // 是否达到目标内存
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

    Store(Config cfg, Clock clock);
    Store();

    // ---- 只读信息 ---------------------------------------------------------
    const Config& config() const;
    const Stats& stats() const;
    std::size_t size() const;
    std::size_t expires_size() const;
    u64 memory_used() const;
    bool raw_contains(const std::string& key) const;
    const EvictionPool& eviction_pool() const;

    void set_policy(Policy p);
    void set_maxmemory(u64 bytes);
    void set_replica(bool v);

    // ---- 命令 -------------------------------------------------------------
    std::optional<std::string> get(const std::string& key);
    bool exists(const std::string& key);
    bool set(const std::string& key, std::string value, bool keep_ttl = false);

    // DEL：和 Redis 一样先 expireIfNeeded；主库上删过期键返回 0（它已经「不算存在」）。
    int del(const std::string& key);

    // 返回 1 设置成功，0 未设置，-1 选项组合非法。
    int expire_at(const std::string& key, i64 when_ms, ExpireFlags flags = ExpireFlags::None);
    int expire_in(const std::string& key, i64 ttl_ms, ExpireFlags flags = ExpireFlags::None);
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
    static u32 lru_clock(i64 now_ms);
    static u32 lfu_minutes(i64 now_ms);
    static u64 lfu_time_elapsed(u32 now_minutes, u32 ldt);
    u8 lfu_counter_after_decay(u32 lru, i64 now_ms) const;
    u8 lfu_log_incr(u8 counter);
    double random01();

    static u64 elapsed_us(std::chrono::steady_clock::time_point start);
};

}  // namespace archdesign
