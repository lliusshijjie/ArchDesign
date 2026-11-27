// store_evict.cpp
//
// maxmemory 检查、采样填池、单次淘汰、批量 perform_evictions。
// 策略只决定“候选集合 + 评分函数”，删除仍然复用 delete_key。

#include "store.hpp"

#include <algorithm>
#include <limits>

namespace archdesign {

bool Store::ensure_room(u64 extra_bytes) {
    if (cfg_.maxmemory == 0) return true;
    if (memory_used_ + extra_bytes <= cfg_.maxmemory) return true;
    const u64 need = memory_used_ + extra_bytes - cfg_.maxmemory;
    (void)perform_evictions(need);
    return memory_used_ + extra_bytes <= cfg_.maxmemory;
}

u64 Store::eviction_score(const Entry& e, i64 now_ms) const {
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

bool Store::populate_eviction_pool() {
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

bool Store::evict_one() {
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

Store::EvictResult Store::perform_evictions(u64 bytes_to_free) {
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

}  // namespace archdesign
