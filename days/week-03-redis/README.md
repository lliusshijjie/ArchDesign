# Week 03 · Redis 专题

Day 15–21。先读 **[Week 03 入门：Redis 专题](00-primer.md)**，再进入各天课程。

本组主题：单线程事件模型、渐进式 rehash、过期/淘汰、持久化、复制、Cluster、Streams。

## 已完成

- [Day 15 · 渐进式 rehash：大字典不能一次搬完](day-015-incremental-rehash/README.md)
- [Day 16 · 过期删除与内存淘汰：内存满了，谁该走？](day-016-expire-evict/README.md)（惰性删除 + 抽样回收 + 淘汰池；C++20，14 个 CTest 用例）
- [Day 17 · RDB 与 AOF：快照、COW 与日志重写](day-017-rdb-aof/README.md)（RDB + fork/COW + AOF rewrite；C++20，18 个 CTest 用例）
