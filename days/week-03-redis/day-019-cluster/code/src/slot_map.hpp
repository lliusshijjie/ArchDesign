// slot_map.hpp
//
// 16384 个槽 -> 节点 ID 的映射。真实 Redis 用 bitmap/数组维护；
// 这里用定长 vector，便于把“槽是否已分配、归谁”直接画出来。

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common.hpp"

namespace archdesign {

class SlotMap {
public:
    explicit SlotMap(std::size_t slot_count = kClusterSlots);

    void assign(Slot slot, NodeId owner);
    void assign_range(Slot begin, Slot end, NodeId owner);  // inclusive
    const NodeId& owner(Slot slot) const;
    bool assigned(Slot slot) const;

    std::size_t assigned_count() const;
    bool all_assigned() const;
    std::size_t slot_count() const;

private:
    std::vector<NodeId> owners_;
};

}  // namespace archdesign
