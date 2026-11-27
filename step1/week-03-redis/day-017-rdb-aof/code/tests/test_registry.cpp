#include "test_support.hpp"

namespace archdesign::test {

const std::vector<TestCase>& test_cases() {
    static const std::vector<TestCase> cases = {
        {"page_store_cow", test_page_store_cow},
        {"rdb_roundtrip", test_rdb_roundtrip},
        {"rdb_ttl", test_rdb_ttl},
        {"rdb_checksum", test_rdb_checksum_and_truncation},
        {"rdb_bgsave", test_rdb_bgsave_and_atomic_replace},
        {"aof_resp", test_aof_resp},
        {"aof_always", test_aof_always},
        {"aof_everysec", test_aof_everysec},
        {"aof_no", test_aof_no},
        {"aof_truncated", test_aof_truncated_tail},
        {"aof_corrupt", test_aof_corrupt},
        {"aof_rewrite", test_aof_rewrite},
        {"rewrite_crash", test_aof_rewrite_crash_before_commit},
        {"rewrite_preamble", test_aof_rewrite_with_rdb_preamble},
        {"recovery_precedence", test_recovery_precedence},
        {"auto_rewrite", test_auto_rewrite},
        {"cow_cost", test_cow_cost},
        {"model", test_model},
    };
    return cases;
}

}  // namespace archdesign::test
