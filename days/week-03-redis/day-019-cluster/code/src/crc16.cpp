#include "crc16.hpp"

namespace archdesign {

std::uint16_t crc16(std::string_view data) {
    std::uint16_t crc = 0;
    for (unsigned char byte : data) {
        crc ^= static_cast<std::uint16_t>(byte) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000) != 0) {
                crc = static_cast<std::uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc = static_cast<std::uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

Slot key_hash_slot(std::string_view key) {
    const std::size_t open = key.find('{');
    if (open == std::string_view::npos) return crc16(key) & 0x3FFF;

    const std::size_t close = key.find('}', open + 1);
    if (close == std::string_view::npos || close == open + 1) {
        return crc16(key) & 0x3FFF;  // 没有闭合，或 {} 为空：哈希整个 key
    }
    return crc16(key.substr(open + 1, close - open - 1)) & 0x3FFF;
}

}  // namespace archdesign
