// common.hpp
//
// Day 16 的配置与数据模型：策略、EXPIRE 选项、Entry。
// 这些类型被 Store / EvictionPool / 测试共用，但不包含任何算法。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace archdesign {

using i64 = std::int64_t;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8 = std::uint8_t;

// 名字与 redis.conf 的 maxmemory-policy 一一对应。
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

// 与 Redis 6.0+ 的 EXPIRE NX/XX/GT/LT 对齐。
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
    int lfu_decay_time = 1;        // 分钟；0 = 不衰减
    int hz = 10;                   // cron 频率；Redis 默认 10
    int active_expire_effort = 1;  // 1..10；每轮抽样 20*effort 个
    bool replica = false;          // 从库不自己删过期键，只把它们「藏起来」
    std::uint32_t seed = 42;       // 抽样/概率递增用，测试可复现
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

}  // namespace archdesign
