// common.hpp
//
// Day 17 的基础类型与小型文本工具。这里只放没有业务含义的东西，
// 让 page_store / rdb / aof / engine 各自只依赖自己需要的模块。

#pragma once

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace archdesign {

using i64 = std::int64_t;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8 = std::uint8_t;

inline std::string upper_ascii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

inline i64 parse_i64(std::string_view text, const char* what) {
    i64 value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        throw std::runtime_error(std::string("invalid integer for ") + what + ": '" +
                                 std::string(text) + "'");
    }
    return value;
}

}  // namespace archdesign
