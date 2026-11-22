#include "bytes.hpp"

#include <stdexcept>

namespace archdesign {

void put_u32_be(std::string& out, u32 v) {
    out.push_back(static_cast<char>((v >> 24) & 0xff));
    out.push_back(static_cast<char>((v >> 16) & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>(v & 0xff));
}

void put_u64_le(std::string& out, u64 v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

void put_u64_be(std::string& out, u64 v) {
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

u32 read_u32_be(std::string_view data, std::size_t pos) {
    if (pos + 4 > data.size()) throw std::runtime_error("read_u32_be: out of bounds");
    return (static_cast<u32>(static_cast<u8>(data[pos])) << 24) |
           (static_cast<u32>(static_cast<u8>(data[pos + 1])) << 16) |
           (static_cast<u32>(static_cast<u8>(data[pos + 2])) << 8) |
           static_cast<u32>(static_cast<u8>(data[pos + 3]));
}

u64 read_u64_le(std::string_view data, std::size_t pos) {
    if (pos + 8 > data.size()) throw std::runtime_error("read_u64_le: out of bounds");
    u64 v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<u64>(static_cast<u8>(data[pos + static_cast<std::size_t>(i)]))
             << (8 * i);
    }
    return v;
}

u64 read_u64_be(std::string_view data, std::size_t pos) {
    if (pos + 8 > data.size()) throw std::runtime_error("read_u64_be: out of bounds");
    u64 v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) |
            static_cast<u64>(static_cast<u8>(data[pos + static_cast<std::size_t>(i)]));
    }
    return v;
}

u64 crc64(std::string_view data) {
    constexpr u64 kPoly = 0xad93d23594c935a9ULL;
    u64 crc = 0;
    for (unsigned char c : data) {
        for (unsigned mask = 0x01; mask & 0xff; mask <<= 1) {
            u64 bit = crc & 0x8000000000000000ULL;
            if ((c & mask) != 0) bit = !bit;
            crc <<= 1;
            if (bit != 0) crc ^= kPoly;
        }
    }

    u64 reflected = 0;
    for (int i = 0; i < 64; ++i) {
        reflected = (reflected << 1) | (crc & 1ULL);
        crc >>= 1;
    }
    return reflected;
}

}  // namespace archdesign
