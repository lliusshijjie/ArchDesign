// client.hpp
//
// Cluster 客户端的最小行为：缓存 slot -> 节点；遇到 MOVED 更新缓存并重试；
// 遇到 ASK 只对下一次请求发 ASKING，不更新缓存（迁移完成后才更新）。

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cluster.hpp"
#include "common.hpp"

namespace archdesign {

class ClusterClient {
public:
    explicit ClusterClient(Cluster& cluster);

    Reply execute(const Command& command);

    // 测试/诊断用：模拟客户端从 CLUSTER SLOTS 提前学到一个（可能过期的）路由。
    void learn_slot(Slot slot, NodeId node);

    std::size_t moved_count() const;
    std::size_t ask_count() const;

private:
    NodeId route_slot(Slot slot);
    Cluster& cluster_;
    std::vector<NodeId> cache_;
    std::size_t moved_ = 0;
    std::size_t ask_ = 0;
};

}  // namespace archdesign
