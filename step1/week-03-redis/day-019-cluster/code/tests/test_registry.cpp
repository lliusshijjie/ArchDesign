#include "test_support.hpp"

namespace archdesign::test {

const std::vector<TestCase>& test_cases() {
    static const std::vector<TestCase> cases = {
        {"slot_hash", test_slot_hash},
        {"slot_map", test_slot_map},
        {"moved", test_moved},
        {"client_cache", test_client_cache},
        {"crossslot", test_crossslot},
        {"clusterdown", test_clusterdown},
        {"ask_migration", test_ask_migration},
        {"tryagain", test_tryagain},
        {"ask_target_requires_asking", test_ask_target_requires_asking},
        {"gossip_quorum", test_gossip_quorum},
        {"failover", test_failover},
        {"failover_no_quorum", test_failover_no_quorum},
        {"epoch_vote_once", test_epoch_vote_once},
        {"replica_data", test_replica_data},
    };
    return cases;
}

}  // namespace archdesign::test
