// store_access.cpp
//
// LRU/LFU 的“一个字段两种解释”：热度初始化、访问更新、LFU 衰减/概率递增、
// 近似空闲时间。淘汰策略只消费这里算出的分数，不直接解释 lru 字段。

#include "store.hpp"

#include <algorithm>

namespace archdesign {

u32 Store::lru_clock(i64 now_ms) {
    return static_cast<u32>((now_ms / 1000) & kLruClockMax);
}

u32 Store::lfu_minutes(i64 now_ms) {
    return static_cast<u32>((now_ms / 60000) & 0xFFFF);
}

u64 Store::lfu_time_elapsed(u32 now_minutes, u32 ldt) {
    return static_cast<u64>((now_minutes - ldt) & 0xFFFF);
}

double Store::random01() { return std::generate_canonical<double, 32>(rng_); }

void Store::init_lru(Entry& e) const {
    if (is_lfu_policy(cfg_.policy)) {
        e.lru = (lfu_minutes(clock_.now()) << 8) | kLfuInitVal;
    } else {
        e.lru = lru_clock(clock_.now());
    }
}

u8 Store::lfu_counter_after_decay(u32 lru, i64 now_ms) const {
    u8 counter = static_cast<u8>(lru & 0xFF);
    if (cfg_.lfu_decay_time <= 0) return counter;
    const u32 ldt = (lru >> 8) & 0xFFFF;
    const u64 periods =
        lfu_time_elapsed(lfu_minutes(now_ms), ldt) / static_cast<u64>(cfg_.lfu_decay_time);
    if (periods == 0) return counter;
    if (periods >= counter) return 0;
    return static_cast<u8>(counter - periods);
}

u8 Store::lfu_log_incr(u8 counter) {
    if (counter == 255) return 255;
    const double base = counter > kLfuInitVal ? static_cast<double>(counter - kLfuInitVal) : 0.0;
    const double probability =
        1.0 / (base * static_cast<double>(cfg_.lfu_log_factor) + 1.0);
    if (random01() < probability) ++counter;
    return counter;
}

void Store::touch_entry(Entry& e) {
    if (is_lfu_policy(cfg_.policy)) {
        u8 counter = lfu_counter_after_decay(e.lru, clock_.now());
        counter = lfu_log_incr(counter);
        e.lru = (lfu_minutes(clock_.now()) << 8) | counter;
    } else {
        // Redis 即使当前不是 LRU 策略也会更新 lru 字段：策略可以运行时切换。
        e.lru = lru_clock(clock_.now());
    }
}

u64 Store::estimate_idle_ms(i64 now_ms, u32 lru) const {
    const u32 now_clock = lru_clock(now_ms);
    const u32 then = lru & kLruClockMax;
    if (now_clock >= then) {
        return static_cast<u64>(now_clock - then) * 1000ULL;
    }
    // 24 位时钟回绕：now 已经绕到 0，then 还在最大值附近。
    return (static_cast<u64>(now_clock) + (kLruClockMax - static_cast<u64>(then))) * 1000ULL;
}

std::optional<u64> Store::object_idletime(const std::string& key) {
    if (is_lfu_policy(cfg_.policy)) return std::nullopt;  // Redis 在 LFU 下不跟踪空闲时间
    if (expire_if_needed(key)) return std::nullopt;
    const Entry* e = find_entry(key);
    if (e == nullptr) return std::nullopt;
    return estimate_idle_ms(clock_.now(), e->lru);
}

std::optional<u8> Store::object_freq(const std::string& key) {
    if (!is_lfu_policy(cfg_.policy)) return std::nullopt;
    if (expire_if_needed(key)) return std::nullopt;
    const Entry* e = find_entry(key);
    if (e == nullptr) return std::nullopt;
    return lfu_counter_after_decay(e->lru, clock_.now());
}

}  // namespace archdesign
