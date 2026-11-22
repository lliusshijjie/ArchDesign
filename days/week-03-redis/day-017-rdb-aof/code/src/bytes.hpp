// bytes.hpp
//
// 字节序工具与 Redis 的 CRC64。
//
// 为什么单独一个文件：RDB 的魔数、长度、TTL、checksum 全是字节级约定，
// 把它们和 PageStore / AOF 混在一起，会让“持久化格式”这件事更难看清。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "common.hpp"

namespace archdesign {

void put_u32_be(std::string& out, u32 v);
void put_u64_le(std::string& out, u64 v);
void put_u64_be(std::string& out, u64 v);

u32 read_u32_be(std::string_view data, std::size_t pos);
u64 read_u64_le(std::string_view data, std::size_t pos);
u64 read_u64_be(std::string_view data, std::size_t pos);

// Redis 使用 Jones 多项式 0xad93d23594c935a9：先 MSB-first 逐位处理，
// 最后把 64 位结果反转。已知向量：crc64("123456789") == 0xe9c6d914c4b8d9ca。
u64 crc64(std::string_view data);

}  // namespace archdesign
