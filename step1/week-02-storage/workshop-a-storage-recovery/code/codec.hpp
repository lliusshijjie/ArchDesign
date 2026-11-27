// codec.hpp
//
// 工作坊 A 的最小序列化工具：只做三件事——CRC32、定点小端整数、字符串拼装。
//
// 为什么用 CRC32 而不是 std::hash：
//   - 校验和必须能跨进程/跨版本重算，std::hash 不保证稳定；
//   - 断电/部分写实验需要一个“逐字节都参与”的校验，CRC32 足够便宜；
//   - 它同时是本课的观察点：坏一条记录，CRC 立刻发现，而不是读出垃圾。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace arch {

// CRC-32（IEEE 802.3，反射多项式 0xEDB88320）。
inline std::uint32_t crc32(std::string_view data) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) != 0u ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            }
            result[index] = value;
        }
        return result;
    }();

    std::uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char byte : data) {
        crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

inline void put_u8(std::string& out, std::uint8_t value) {
    out.push_back(static_cast<char>(value));
}

inline void put_u32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFFu));
    out.push_back(static_cast<char>((value >> 8) & 0xFFu));
    out.push_back(static_cast<char>((value >> 16) & 0xFFu));
    out.push_back(static_cast<char>((value >> 24) & 0xFFu));
}

// 读取前必须保证 offset + n <= data.size()，否则 std::string_view::operator[] 越界。
inline std::uint8_t read_u8(std::string_view data, std::size_t offset) {
    return static_cast<std::uint8_t>(data[offset]);
}

inline std::uint32_t read_u32(std::string_view data, std::size_t offset) {
    const auto byte = [&](std::size_t index) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(data[index]));
    };
    return byte(offset) | (byte(offset + 1) << 8) | (byte(offset + 2) << 16) |
           (byte(offset + 3) << 24);
}

} // namespace arch
