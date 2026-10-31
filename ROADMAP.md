# 60 次课 / 约 120 天路线图

> 4 个阶段 ×（14 个核心单元 + 1 个阶段工作坊）= 60 次课。
>
> **每两天一次，19:00（Asia/Shanghai）推进一个单元，约 120 天。**
>
> 表中的“源码坐标”是**阅读入口**，不是要求通读整个项目。每次只追一条关键路径。

## 使用说明

- 状态：`✅` 已完成，`—` 待学习。
- **语言偏好**：C++20 主讲，Rust 对照，Java 只作官方实现/概念参考；代码练习默认 C++，不要求 Java 实现。
- 顺序有依赖：W1–W2 是底座，W3–W4 是中间件，W5–W8 是分布式与工程化。可以按兴趣微调，但建议不要跳过 W1 的线程池、W2 的 WAL 和 W5 的 Raft。
- 核心周的第 7 个单元不引入新工具，而是把前 6 个单元的模式组合成一个能运行的小系统。
- 每 14 个核心单元后插入一个**阶段工作坊**（A/B/C/D），用来压测、故障注入和迁移，不占用新的 Day 编号。
- 如果某次只有 45 分钟：读“背景/架构/技巧”，跑代码，做自测；把练习留到下一次之前。

---

## W1 · 执行与并发底座

**本周问题**：任务从提交到执行，中间要经过哪些资源边界？队列、线程、时钟、日志、内存分别如何反噬系统？

| Day | 主题 | 案例 / 源码坐标 | 可复用模式 | 状态 |
|---|---|---|---|---|
| 01 | [线程池：把并发关进笼子](days/day-001-thread-pool/README.md) | OpenJDK `ThreadPoolExecutor`：`ctl`、`execute`、`runWorker`、`getTask`、`tryTerminate` | 状态与计数打包、准入控制、拒绝策略、两阶段关闭；延伸：单队列 vs 工作窃取 | ✅ |
| 02 | [Redis 事件循环：单线程为什么能快](days/day-002-redis-event-loop/README.md) | `ae.c` / `ae_epoll.c` / `networking.c`：`aeProcessEvents`、`beforeSleep`、文件事件与时间事件 | Reactor、水平触发、beforeSleep 延迟写、时间事件 | ✅ |
| 03 | [时间轮：海量定时器为什么能做到 O(1)](days/day-003-timing-wheel/README.md) | Netty `HashedWheelTimer`；Kafka `SystemTimer` / `TimingWheel` | 分桶、rounds、层级时间轮、原子取消 | ✅ |
| 04 | [异步日志：别让磁盘拖慢业务线程](days/day-004-async-logging/README.md) | Log4j2 `AsyncLogger` + LMAX Disruptor；spdlog `async_logger` | 有界队列、溢出策略、批量消费、flush/shutdown | ✅ |
| 05 | 内存分配器：malloc 为什么会成为瓶颈 | jemalloc：size class、arena、thread cache；tcmalloc 对照 | 尺寸分级、线程缓存、slab、减少锁竞争 | — |
| 06 | 无锁环形队列：Disruptor 的机械同情 | LMAX Disruptor：`RingBuffer`、sequence、gating、cache line padding | 单写者、序列栅栏、内存序、批处理、背压 | — |
| 07 | **周复盘**：有界、可观测的执行器 | 组合 Day 01/03/04/06；压测 + 故障注入 | 把“池”抽象成资源边界；指标与关闭语义 | — |

## W2 · 存储引擎与数据库内核

**本周问题**：一条写入如何变得持久？查询如何找到数据？并发事务如何互不干扰？崩溃后如何恢复？

| Day | 主题 | 案例 / 源码坐标 | 可复用模式 | 状态 |
|---|---|---|---|---|
| 08 | LSM-tree：用顺序写换随机写 | LevelDB / RocksDB：WAL → MemTable → Immutable → SSTable；compaction | 追加写、不可变结构、后台合并、读写放大权衡 | — |
| 09 | WAL 与 group commit：fsync 的批量艺术 | PostgreSQL WAL（LSN、segment、checkpoint）；etcd WAL | 预写日志、批量 fsync、校验和、幂等恢复 | — |
| 10 | Buffer Pool：数据库的页缓存 | InnoDB buffer pool：LRU young/old、midpoint insertion、flush list、doublewrite | 页缓存、写回、淘汰策略、二次写 | — |
| 11 | B+ 树页面：索引的物理结构 | InnoDB page：File Header、Page Directory、Infimum/Supremum、split | 槽页（slotted page）、有序数组 + 目录、锁耦合 | — |
| 12 | MVCC：快照读与版本链 | PostgreSQL `xmin/xmax` + snapshot；InnoDB `DB_TRX_ID/DB_ROLL_PTR` + undo + read view | 版本链、快照隔离、GC/purge、可见性判断 | — |
| 13 | ARIES：崩溃恢复的教科书 | ARIES 论文；InnoDB redo/undo、LSN、CLR、fuzzy checkpoint | steal/no-force、生理日志、redo 幂等、undo 补偿 | — |
| 14 | **周复盘**：Mini KV Engine | WAL + MemTable + SST + 恢复 + 简单 compaction | 把存储引擎拆成可替换组件 | — |

## W3 · Redis 专题

**本周问题**：一个单线程内存数据库，如何同时做到低延迟、可持久化、可复制、可水平扩展？

| Day | 主题 | 案例 / 源码坐标 | 可复用模式 | 状态 |
|---|---|---|---|---|
| 15 | 渐进式 rehash：大字典不能一次搬完 | Redis `dict.c`：`dictRehash`、`rehashidx`、双表 | 增量迁移、双结构并存、摊还成本 | — |
| 16 | 过期删除与内存淘汰 | `expire.c`、`evict.c`：惰性删除、active expire、LRU/LFU 采样、eviction pool | 惰性清理、采样近似、策略分离、内存水位 | — |
| 17 | RDB 与 AOF：快照、COW 与日志重写 | `rdb.c`、`aof.c`：fork + COW、`fsync` 策略、AOF rewrite | 快照、写时复制、日志压缩、持久性取舍 | — |
| 18 | 主从复制：PSYNC、backlog 与断点续传 | `replication.c`：repl offset、backlog ring、full resync、WAIT | 复制日志、环形缓冲、可恢复同步、延迟监控 | — |
| 19 | Redis Cluster：槽、重定向与去中心化故障转移 | `cluster.c`：16384 slots、MOVED/ASK、gossip、failover、quorum | 分片、重定向、成员协议、脑裂边界 | — |
| 20 | Streams：消费者组与 PEL | `t_stream.c`：consumer group、PEL、XACK、XAUTOCLAIM | 日志 + 游标、未确认消息、消息认领 | — |
| 21 | **周复盘**：缓存架构的五个坑 | cache-aside、穿透/击穿/雪崩/热 key/一致性 | singleflight、逻辑过期、降级、容量规划 | — |

## W4 · 消息队列

**本周问题**：消息如何被高效存储、批量发送、可靠消费？“恰好一次”到底承诺了什么？

| Day | 主题 | 案例 / 源码坐标 | 可复用模式 | 状态 |
|---|---|---|---|---|
| 22 | Kafka 日志存储：分段、稀疏索引与零拷贝 | `Log` / `LogSegment` / `OffsetIndex`；`sendfile` | 追加日志、稀疏索引、OS page cache、零拷贝 | — |
| 23 | Kafka Producer：批量、重试与幂等 | `RecordAccumulator`、`Sender`、PID/sequence、`acks`、`max.in.flight` | 批处理、幂等生产者、在途上限、背压 | — |
| 24 | Kafka Consumer Group：重平衡与位移 | `GroupCoordinator`、`JoinGroup/SyncGroup`、generation、cooperative sticky | 成员协议、代际 fencing、协作式重平衡 | — |
| 25 | RocketMQ：CommitLog + ConsumeQueue | `CommitLog`、`ConsumeQueue`、`IndexFile`、事务消息、延迟消息 | 统一提交日志、异步分发、事务半消息 | — |
| 26 | Pulsar：存算分离与 BookKeeper | Broker / BookKeeper ledger / segment / tiered storage | 存算分离、分段存储、多租户、分层存储 | — |
| 27 | 可靠投递：至少一次、幂等与 Outbox | 幂等键、去重表、transactional outbox、CDC、Saga | 幂等消费者、事务外盒、补偿、去重窗口 | — |
| 28 | **周复盘**：Mini Broker | append-only segment + offset + consumer + ack | 把“日志”抽象成消息系统 | — |

## W5 · 协调、RPC 与韧性

**本周问题**：多个节点如何达成一致？请求如何找到服务？过载时如何优雅地失败？

| Day | 主题 | 案例 / 源码坐标 | 可复用模式 | 状态 |
|---|---|---|---|---|
| 29 | Raft 选举与日志复制 | etcd `raft`：term、RequestVote、AppendEntries、commit index | Quorum、任期 fencing、复制日志、状态机 | — |
| 30 | Raft 读、快照与成员变更 | ReadIndex、lease read、InstallSnapshot、joint consensus | 线性一致读、快照、联合共识、learner | — |
| 31 | ZooKeeper / ZAB：会话、临时节点与 Watch | `Zab`、session、ephemeral/sequential、one-shot watch | 会话租约、有序广播、一次性通知、临时节点 | — |
| 32 | 服务发现与负载均衡 | gRPC resolver / `pick_first` / `round_robin`；xDS；outlier detection | 控制面/数据面、客户端负载均衡、健康检查、局部性 | — |
| 33 | 限流：令牌桶、漏桶与分布式配额 | Redis + Lua 令牌桶；滑动窗口；配额服务 | 准入控制、突发额度、分布式计数、公平性 | — |
| 34 | 韧性模式：超时、重试、熔断、舱壁 | Hystrix/Resilience4j 思想；gRPC deadline；重试抖动 | 快速失败、隔离、预算、退避、避免重试风暴 | — |
| 35 | **周复盘**：韧性网关 | 限流 + 熔断 + 超时 + 重试 + 降级组合 | 失败模式优先的架构设计 | — |

## W6 · 调度与运行时

**本周问题**：线程、协程、Pod、定时任务分别如何被调度？分布式环境下如何避免重复执行？

| Day | 主题 | 案例 / 源码坐标 | 可复用模式 | 状态 |
|---|---|---|---|---|
| 36 | Go GMP：M:N 调度与 work stealing | `runtime/proc.go`：G/M/P、run queue、netpoller、sysmon、抢占 | M:N 调度、工作窃取、协作+抢占、自旋线程 | — |
| 37 | Tokio：异步运行时与取消安全 | Tokio scheduler、reactor、timer、blocking pool、`select!` | 异步运行时、取消安全、阻塞隔离、结构化并发 | — |
| 38 | Kubernetes Scheduler：过滤、打分与乐观并发 | `Schedule` cycle、Filter/Score plugins、informer cache、preemption | 两阶段调度、缓存、乐观更新、抢占 | — |
| 39 | 定时任务：Cron、misfire 与分布式调度 | Quartz misfire；K8s CronJob；分布式调度器 | 错过触发策略、幂等、租约、补跑 | — |
| 40 | 分布式锁：锁、租约与 fencing token | Redis `SET NX PX`、Redlock 批评；etcd lease/transaction | 租约、fencing、共识、GC 停顿边界 | — |
| 41 | 连接池：HikariCP 的快路径 | `ConcurrentBag`、proxy connection、leak detection、validation | 稀缺资源池、借用/归还、健康检查、公平性 | — |
| 42 | **周复盘**：带租约的分布式任务调度器 | 租约 + fencing + 幂等 + 重试 | 把“调度”做成可恢复状态机 | — |

## W7 · 可观测性与扩展性

**本周问题**：系统上线后，如何知道它正在发生什么？过载时如何不雪崩？如何安全地关闭和扩展？

| Day | 主题 | 案例 / 源码坐标 | 可复用模式 | 状态 |
|---|---|---|---|---|
| 43 | 结构化日志：事件、上下文与采样 | Log4j2/zerolog/OTel logs；MDC/context；采样与脱敏 | 结构化事件、上下文传播、采样、PII 治理 | — |
| 44 | Metrics：Prometheus 与直方图 | pull 模型、counter/gauge/histogram、quantile、cardinality、exemplar | 维度指标、聚合、基数控制、RED/USE | — |
| 45 | Tracing：OpenTelemetry 与上下文传播 | OTel API/SDK、span、W3C `traceparent`、collector、tail sampling | 分布式上下文、span 树、采样、开销控制 | — |
| 46 | 背压与过载保护 | 有界队列、Reactive Streams、load shedding、adaptive concurrency | 反馈控制、丢弃策略、Little's Law、排队延迟 | — |
| 47 | 优雅关闭：从 SIGTERM 到连接排空 | K8s termination、readiness、connection draining、in-flight deadline | 两阶段关闭、就绪/存活分离、截止时间 | — |
| 48 | 插件架构：过滤器、拦截器与 SPI | Envoy HTTP filter chain、gRPC interceptor、Java SPI、WASM | 管道、扩展点、顺序、隔离、版本兼容 | — |
| 49 | **周复盘**：可观测、可扩展的服务骨架 | 日志 + 指标 + 追踪 + 背压 + 关闭 + 插件 | 用“可运行骨架”串起工程化能力 | — |

## W8 · 架构综合与落地

**本周问题**：这些模式如何进入真实项目？如何测试、度量、分片、隔离，并最终组合成自己的基础设施？

| Day | 主题 | 案例 / 源码坐标 | 可复用模式 | 状态 |
|---|---|---|---|---|
| 50 | 配置中心：版本、推送与回滚 | Apollo / Nacos：namespace、长轮询、灰度、本地缓存 | 版本化配置、watch、降级、灰度发布 | — |
| 51 | 确定性模拟测试：把分布式 bug 复现出来 | FoundationDB simulation、Jepsen、failpoint、确定性调度 | 确定性回放、故障注入、不变量检查 | — |
| 52 | 性能工程：测量、剖析与基准 | USE、perf/flamegraph、wrk/fio、coordinated omission、p99 | 先测量后优化、隔离变量、排队论 | — |
| 53 | 分片与再平衡：热点、虚拟节点与迁移 | Redis Cluster slots、Kafka partition、consistent hashing | 分区键、虚拟节点、迁移、热点拆分 | — |
| 54 | 多租户配额与隔离：拒绝吵闹邻居 | token bucket per tenant、cgroups、bulkhead、fair queuing | 隔离、配额、公平调度、资源上限 | — |
| 55 | **Capstone**：设计你自己的 Mini Infra Toolkit | 组合线程池、日志、定时器、配置、指标、关闭 | 接口设计、失败模式、演进路线 | — |
| 56 | **总复盘**：模式地图与下一步 | 48 张卡片去重、合并、形成自己的工具箱 | 从“知道”到“能选型、能落地” | — |

---

## 阶段工作坊（A–D）

> 目录与模板见 [workshops/README.md](workshops/README.md)。

工作坊不是“多学一个工具”，而是把前 14 个核心单元放进一个**可失败的实验**里。每个工作坊同样占一个两日单元，产出一篇文章 + 一个可运行 harness。

| 工作坊 | 插入位置 | 主题 | 必须完成的实验 | 产出 |
|---|---|---|---|---|
| **A** | Day 14 后 | 存储引擎压测与崩溃恢复 | 写入中途 `kill -9`；部分写/坏校验和；恢复后校验不变量 | 实验报告 + recovery harness |
| **B** | Day 28 后 | 消息系统故障注入 | 重复投递、乱序、积压、消费者崩溃；验证 Outbox 与幂等 | 故障注入脚本 + 消费端幂等测试 |
| **C** | Day 42 后 | 调度与锁的混沌实验 | 时钟偏移、网络分区、lease 过期；验证 fencing token | 混沌实验 + 调度器恢复报告 |
| **D** | Day 56 后 | 个人项目迁移工作坊 | 从 48 张卡片中选 3 个模式做 PoC；设计评审；给出 30 天落地计划 | 迁移设计文档 + PoC 代码 |

**顺序**：`Day 01–14 → A → Day 15–28 → B → Day 29–42 → C → Day 43–56 → D`。

---

## 复盘日产出的固定结构

每个核心周的第 7 个单元（07/14/21/…/56）产出一篇**综合文章 + 一个可运行的最小系统**，必须包含：

1. **概念图**：本周 6 个主题之间的关系，不是目录复述。
2. **组合实现**：至少组合 3 个本周模式，能运行、能失败、能恢复。
3. **破坏性实验**：至少 2 个（例如队列打满、进程崩溃、网络分区、时钟跳跃）。
4. **模式卡片**：每个模式一行“解决什么问题 / 代价 / 不适用场景”。
5. **迁移清单**：如果要用到自己的项目，第一步改什么、如何验证、如何回滚。

## 工作坊产出的固定结构

每个工作坊 A–D 产出一篇**实验报告 + 可运行 harness**，必须包含：

1. **假设**：这次要验证哪个设计在压力/故障下成立。
2. **注入**：具体故障、时间、频率、如何复现。
3. **观测**：恢复时间、数据丢失量、重复数、p99、错误预算。
4. **结论**：设计是否成立；不成立时，改机制还是改策略。
5. **迁移**：如果用到自己的项目，最小改动、验证方式、回滚方式。

## 进度记录

**总进度：4 / 60（核心 4/56，工作坊 0/4）**

| 阶段 | 核心单元 | 核心完成 | 工作坊 | 工作坊完成 | 最大收获（一句话） |
|---|---|---|---|---|---|
| 1 | Day 01–14 | 4 / 14 | A | 0 / 1 | 有界队列换业务延迟；flush 是屏障，不是“不会丢” |
| 2 | Day 15–28 | 0 / 14 | B | 0 / 1 | |
| 3 | Day 29–42 | 0 / 14 | C | 0 / 1 | |
| 4 | Day 43–56 | 0 / 14 | D | 0 / 1 | |
