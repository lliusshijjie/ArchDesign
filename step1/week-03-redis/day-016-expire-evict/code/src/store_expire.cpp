// store_expire.cpp
//
// 惰性过期、统一删除、随机抽样与 active_expire_cycle。
// 所有删除路径都走 delete_key，保证主表/expires/内存账/统计一起更新。

#include "store.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_set>

namespace archdesign {

bool Store::expire_if_needed(const std::string& key) {
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

void Store::delete_key(const std::string& key, DeleteReason reason) {
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

std::vector<u32> Store::sample_slot_ids(int count) {
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

std::vector<u32> Store::sample_expires_ids(int count) {
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

std::string Store::random_key(bool volatile_only) {
    if (volatile_only) {
        if (expires_live_.empty()) return {};
        const u32 id = expires_live_[rng_() % expires_live_.size()];
        return expires_nodes_[id].key;
    }
    if (slots_live_.empty()) return {};
    const u32 id = slots_live_[rng_() % slots_live_.size()];
    return slots_[id].entry.key;
}

Store::ActiveExpireResult Store::active_expire_cycle(i64 budget_us) {
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

}  // namespace archdesign
