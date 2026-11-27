// kv.hpp
//
// MiniKV：只支持 string + 绝对 TTL。
//
// 它在 master 和 replica 上跑同一套 apply()；区别是：
//   - master 会在访问/主动过期时删除，并合成 DEL 进入复制流；
//   - replica 不自己删过期键，只对读隐藏，等 master 的 DEL。

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace archdesign {

struct Value {
    std::string value;
    i64 expire_at_ms = -1;  // -1 = 无 TTL
};

class Kv {
public:
    void set(std::string key, std::string value, i64 expire_at_ms = -1);
    bool del(std::string_view key);
    bool expire_at(std::string_view key, i64 when_ms);
    bool persist(std::string_view key);

    // 读路径：过期键不可见，但不会物理删除（replica 尤其依赖这一点）。
    std::optional<std::string> get(std::string_view key, i64 now_ms) const;
    std::optional<Value> raw(std::string_view key) const;

    // master 专用：真正删掉过期键；返回是否发生了删除。
    bool erase_if_expired(std::string_view key, i64 now_ms);
    std::vector<std::string> expired_keys(i64 now_ms) const;

    std::size_t size(i64 now_ms) const;
    std::size_t raw_size() const;

    // 全量同步用的“RDB”（长度前缀编码，够用且可校验）。
    std::string snapshot() const;
    void load(std::string_view snapshot);

    // 执行复制流里的一条命令（SET/DEL/PEXPIREAT/PERSIST）。
    void apply(const Command& command, i64 now_ms);

    std::vector<std::pair<std::string, Value>> entries() const;

private:
    std::map<std::string, Value> data_;
};

}  // namespace archdesign
