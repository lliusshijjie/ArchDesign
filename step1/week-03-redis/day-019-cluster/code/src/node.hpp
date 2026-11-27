// node.hpp
//
// 一个节点的最小状态：角色、master、epoch、数据、故障举报。
// 真正的 cluster bus 消息在文章里讲；代码只保留路由/故障转移需要的字段。

#pragma once

#include <map>
#include <set>
#include <string>

#include "common.hpp"

namespace archdesign {

struct Node {
    NodeId id;
    NodeId master_id;  // 空 = 自己就是 master
    bool master = false;
    bool failed = false;
    Epoch epoch = 0;
    std::map<std::string, std::string> data;
    std::set<NodeId> failure_reports;
};

}  // namespace archdesign
