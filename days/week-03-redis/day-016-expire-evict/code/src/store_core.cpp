// store_core.cpp
//
// Store 的基础部分：槽位/expires 表、内存账、命令语义、一致性检查。

#include "store.hpp"

#include <algorithm>
#include <utility>

namespace archdesign {

Store::Store(Config cfg, Clock clock)
    : cfg_(std::move(cfg)), clock_(std::move(clock)), rng_(cfg_.seed) {}

Store::Store() : Store(Config{}, Clock::system()) {}

const Config& Store::config() const { return cfg_; }

const Store::Stats& Store::stats() const { return stats_; }

std::size_t Store::size() const { return index_.size(); }

std::size_t Store::expires_size() const { return expires_index_.size(); }

u64 Store::memory_used() const { return memory_used_; }

bool Store::raw_contains(const std::string& key) const { return index_.count(key) != 0; }

const EvictionPool& Store::eviction_pool() const { return pool_; }

void Store::set_policy(Policy p) { cfg_.policy = p; }

void Store::set_maxmemory(u64 bytes) { cfg_.maxmemory = bytes; }

void Store::set_replica(bool v) { cfg_.replica = v; }

u64 Store::entry_bytes(const Entry& e) {
    return static_cast<u64>(sizeof(Entry)) + e.key.size() + 1 + e.value.size() + 1 +
           kEntryAllocOverhead;
}

u64 Store::entry_bytes(const std::string& key, const std::string& value) {
    return static_cast<u64>(sizeof(Entry)) + key.size() + 1 + value.size() + 1 +
           kEntryAllocOverhead;
}

u64 Store::expires_bytes(const std::string& key) {
    return static_cast<u64>(sizeof(ExpiresNode)) + key.size() + 1 + kExpiresAllocOverhead;
}

u32 Store::add_slot(Entry e) {
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

void Store::erase_slot(u32 id) {
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

bool Store::expires_set(const std::string& key, i64 when_ms) {
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

bool Store::expires_erase(const std::string& key) {
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

Entry* Store::find_entry(const std::string& key) {
    auto it = index_.find(key);
    if (it == index_.end()) return nullptr;
    return &slots_[it->second].entry;
}

const Entry* Store::find_entry(const std::string& key) const {
    auto it = index_.find(key);
    if (it == index_.end()) return nullptr;
    return &slots_[it->second].entry;
}

bool Store::set(const std::string& key, std::string value, bool keep_ttl) {
    // 写路径也检查过期：否则会把一个「逻辑上已经不存在」的键当成更新。
    (void)expire_if_needed(key);

    u64 old_bytes = 0;
    bool had_ttl = false;
    if (const Entry* old = find_entry(key)) {
        old_bytes = entry_bytes(*old) + (old->expire_at_ms >= 0 ? expires_bytes(key) : 0);
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

std::optional<std::string> Store::get(const std::string& key) {
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

bool Store::exists(const std::string& key) {
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

int Store::del(const std::string& key) {
    (void)expire_if_needed(key);  // 主库上会先删掉过期键，于是下面的 DEL 返回 0
    if (index_.count(key) == 0) return 0;
    delete_key(key, DeleteReason::Del);
    return 1;
}

int Store::expire_at(const std::string& key, i64 when_ms, ExpireFlags flags) {
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

int Store::expire_in(const std::string& key, i64 ttl_ms, ExpireFlags flags) {
    return expire_at(key, clock_.now() + ttl_ms, flags);
}

int Store::persist(const std::string& key) {
    (void)expire_if_needed(key);
    Entry* e = find_entry(key);
    if (e == nullptr || e->expire_at_ms < 0) return 0;
    memory_used_ -= expires_bytes(key);
    expires_erase(key);
    e->expire_at_ms = -1;
    return 1;
}

i64 Store::pttl(const std::string& key) {
    if (expire_if_needed(key)) return -2;
    const Entry* e = find_entry(key);
    if (e == nullptr) return -2;
    if (e->expire_at_ms < 0) return -1;
    const i64 remaining = e->expire_at_ms - clock_.now();
    return remaining > 0 ? remaining : 0;
}

i64 Store::ttl(const std::string& key) {
    const i64 ms = pttl(key);
    if (ms < 0) return ms;
    return (ms + 500) / 1000;
}

u64 Store::memory_recomputed() const {
    u64 total = 0;
    for (const u32 id : slots_live_) {
        const Entry& e = slots_[id].entry;
        total += entry_bytes(e);
        if (e.expire_at_ms >= 0) total += expires_bytes(e.key);
    }
    return total;
}

bool Store::check_consistency(std::string* error) const {
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

std::optional<std::string> Store::peek_raw(const std::string& key) const {
    const Entry* e = find_entry(key);
    if (e == nullptr) return std::nullopt;
    return e->value;
}

bool Store::raw_has_expire(const std::string& key) const {
    const Entry* e = find_entry(key);
    return e != nullptr && e->expire_at_ms >= 0;
}

u64 Store::elapsed_us(std::chrono::steady_clock::time_point start) {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
}

}  // namespace archdesign
