#include "test_support.hpp"

namespace archdesign::test {

const std::vector<TestCase>& test_cases() {
    static const std::vector<TestCase> cases = {
        {"full_sync", test_full_sync},
        {"incremental", test_incremental},
        {"full_sync_tail", test_full_sync_tail},
        {"full_sync_ttl", test_full_sync_ttl},
        {"partial_resync", test_partial_resync},
        {"backlog_overflow", test_backlog_overflow},
        {"psync_boundary", test_psync_boundary},
        {"wrong_replid", test_wrong_replid},
        {"wait_acks", test_wait_acks},
        {"wait_timeout", test_wait_timeout},
        {"failover_replid2", test_failover_replid2},
        {"master_driven_expire", test_master_driven_expire},
        {"backlog_wrap", test_backlog_wrap},
        {"command_stream", test_command_stream},
    };
    return cases;
}

}  // namespace archdesign::test
