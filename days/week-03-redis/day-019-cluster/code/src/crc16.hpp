// crc16.hpp
//
// Redis 的 CRC16：XMODEM 多项式 0x1021，初值 0。
// 已知向量：crc16("123456789") == 0x31C3。
//
// keyHashSlot 还会处理 {hash tag}：只哈希 {} 之间的内容。
// 这是让多 key 命令落到同一槽的唯一“官方后门”。

#pragma once

#include <cstdint>
#include <string_view>

#include "common.hpp"

namespace archdesign {

std::uint16_t crc16(std::string_view data);
Slot key_hash_slot(std::string_view key);

}  // namespace archdesign
