// rdb.hpp
//
// RDB 编解码。文件布局与真实 Redis 同构，但只支持 string：
//
//   "REDIS0011"
//   AUX <redis-ver> <7.2.0>          (0xFA)
//   AUX <archdesign> <day17>
//   SELECTDB 0                        (0xFE)
//   RESIZEDB <keys> <expires>         (0xFB)
//   [EXPIRETIME_MS <ms>] TYPE_STRING KEY VALUE  (0xFC, 0x00)
//   ...
//   EOF                               (0xFF)
//   CRC64                             (8 bytes, little endian；关闭校验时写 0)

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "common.hpp"
#include "page_store.hpp"

namespace archdesign {

struct RdbOptions {
    bool checksum = true;
    std::string redis_version = "7.2.0";
};

struct RdbLoadOptions {
    bool verify_checksum = true;
    // 独立 RDB 启动时跳过已过期键；AOF 的 RDB preamble 不跳过，
    // 因为后面的 AOF tail 仍可能引用/删除这些键。
    bool skip_expired = true;
    int max_version = 11;
};

struct RdbDecodeResult {
    std::size_t consumed = 0;
    std::size_t loaded = 0;
    std::size_t skipped_expired = 0;
    u64 checksum = 0;
};

std::string rdb_encode(const Snapshot& snapshot, const RdbOptions& options = {});

// 从 data 的开头解析一个 RDB；consumed 可能小于 data.size()，
// 这样 AOF 的 RDB preamble 可以把剩余字节继续当 RESP 解析。
RdbDecodeResult rdb_decode(std::string_view data, PageStore& store, i64 now_ms,
                           const RdbLoadOptions& options = {});

}  // namespace archdesign
