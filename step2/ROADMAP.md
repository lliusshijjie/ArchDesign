# Step 2 · 49 天中间件源码路线图

每天 19:00（Asia/Shanghai）推进一个单元。每周一个项目，先读 `00-primer.md`，最后一天用小系统或一组实验复盘。`✅` 表示课文和实验已验证交付，`—` 表示待交付。

| 周 | 项目 | 周目录 | 核心问题 |
|---|---|---|---|
| 01 | [Qdrant](https://github.com/qdrant/qdrant) · Rust | [week-01-qdrant](week-01-qdrant/00-primer.md) | 如何共同控制向量检索的召回率、查询延迟和过滤范围？ |
| 02 | [NGINX](https://github.com/nginx/nginx) · C | `week-02-nginx` | 长时间运行的代理怎样管理请求、连接和配置？ |
| 03 | [gRPC](https://github.com/grpc/grpc) · C++ | `week-03-grpc` | 如何把跨进程消息交换封装成有生命周期的调用？ |
| 04 | [RabbitMQ](https://github.com/rabbitmq/rabbitmq-server) · Erlang | `week-04-rabbitmq` | 发布、存储和消费分别承诺了什么？ |
| 05 | [Envoy](https://github.com/envoyproxy/envoy) · C++ | `week-05-envoy` | 控制面的配置如何安全地改变数据面的转发行为？ |
| 06 | [Apache Flink](https://github.com/apache/flink) · Java | `week-06-flink` | 无界数据上的计算如何管理时间、状态和恢复？ |
| 07 | [Fast DDS](https://github.com/eProsima/Fast-DDS) · C++ | `week-07-fast-dds` | 去中心化的实时通信如何发现对端并约束投递成本？ |

目录名按 `day-NNN-<主题路径>` 创建，按表格顺序取第一个 `—`。当周首课连同周导读交付。

## W1 · Qdrant

| Day | 专题 | 主题路径 | 实验或验收问题 | 状态 |
|---|---|---|---|---|
| 01 | [精确 Top-K 与 Recall@K](week-01-qdrant/day-001-exact-top-k/README.md) | `exact-top-k` | 用精确搜索作基线，验证漏掉最近点时的召回率 | ✅ |
| 02 | HNSW 的层级和图搜索 | `hnsw-graph-search` | 在小图上逐步记录入口、候选队列和访问集合 | — |
| 03 | HNSW 参数与查询预算 | `hnsw-search-budget` | 固定图和查询，扫 `hnsw_ef`，对照召回率与距离计算次数 | — |
| 04 | Payload 过滤与搜索策略 | `payload-filtering` | 改变过滤选择性，对照先筛后扫和图搜索的候选范围 | — |
| 05 | 量化与重排 | `quantization-rescore` | 对照量化前后排序，衡量重排能找回多少近邻 | — |
| 06 | Segment 与索引生命周期 | `segment-lifecycle` | 对照追加、删除、重建期间读者所见的数据 | — |
| 07 | **周复盘**：可测量的向量搜索 | `vector-search-lab` | 组合精确基线、图索引和过滤，注入过滤过窄与搜索预算不足 | — |

## W2 · NGINX

| Day | 专题 | 主题路径 | 实验或验收问题 | 状态 |
|---|---|---|---|---|
| 08 | Master、worker 与配置重载 | `graceful-reload` | 同时观察慢请求、新请求和两代 worker | — |
| 09 | HTTP phase 与请求恢复 | `http-phase-engine` | 画出 handler 让出执行和异步恢复的位置 | — |
| 10 | 请求内存池与清理 | `request-memory-pool` | 跟踪短生命周期对象和外部资源的清理顺序 | — |
| 11 | Upstream 连接和代理状态机 | `upstream-proxy` | 连接拒绝与读取超时时，分别在哪层结束请求 | — |
| 12 | 缓冲、临时文件与慢客户端 | `proxy-buffering` | 固定上游响应，改变下游读取速度和缓冲上限 | — |
| 13 | 响应过滤器链 | `response-filter-chain` | 比较两种过滤顺序对响应头和数据块的影响 | — |
| 14 | **周复盘**：可重载反向代理 | `proxy-lifecycle-lab` | 注入错误配置和慢客户端，验证旧连接与新请求的边界 | — |

## W3 · gRPC

| Day | 专题 | 主题路径 | 实验或验收问题 | 状态 |
|---|---|---|---|---|
| 15 | Proto 接口与消息成帧 | `proto-and-framing` | 对照消息、长度前缀和 HTTP/2 DATA 帧的边界 | — |
| 16 | 双向流与 callback reactor | `bidi-reactor` | 从 `StartRead` 追到 `OnDone` 并标记消息所有权 | — |
| 17 | 半关闭与最终状态 | `half-close-and-status` | 客户端停止写入后仍读取响应，并检查最终状态 | — |
| 18 | Deadline 与取消传播 | `deadline-cancellation` | 注入慢下游，观察时间预算和清理发生的时点 | — |
| 19 | HTTP/2 流量控制 | `stream-flow-control` | 放慢读端，观察写入进度及缓冲限制 | — |
| 20 | 重试与请求提交边界 | `retry-commit-boundary` | 对照收到响应头前后失败时的重试行为 | — |
| 21 | **周复盘**：有预算的双向 RPC | `streaming-rpc-lab` | 组合半关闭、流控和 deadline，注入取消与慢读 | — |

## W4 · RabbitMQ

| Day | 专题 | 主题路径 | 实验或验收问题 | 状态 |
|---|---|---|---|---|
| 22 | Exchange、binding 与队列路由 | `exchange-routing` | 对照 direct 和 topic 的命中与未路由消息 | — |
| 23 | Publisher confirm | `publisher-confirms` | 记录发布、接收确认和连接中断时的事件顺序 | — |
| 24 | Consumer ACK 与重投 | `consumer-ack-redelivery` | 在处理结果已写入、ACK 尚未发送时断开消费者 | — |
| 25 | Prefetch 与未确认额度 | `prefetch-and-backpressure` | 改变 prefetch，观察慢消费者的在途上限和分发 | — |
| 26 | Quorum queue 与副本确认 | `quorum-queue` | 对照 leader 丢失和丢失多数派的行为 | — |
| 27 | 死信与投递次数限制 | `dead-lettering` | 隔离反复失败的消息，验证正常消息仍可推进 | — |
| 28 | **周复盘**：可恢复的消息处理链路 | `reliable-delivery-lab` | 同时注入发布端失联和消费端崩溃，校验幂等 | — |

## W5 · Envoy

| Day | 专题 | 主题路径 | 实验或验收问题 | 状态 |
|---|---|---|---|---|
| 29 | Listener、Route、Cluster 与 Endpoint | `proxy-resource-model` | 用一条 HTTP 请求串起四类配置资源 | — |
| 30 | xDS 版本、nonce 与 ACK/NACK | `xds-ack-nack` | 顺序发布有效、无效配置，检查代理保留的路由 | — |
| 31 | 资源依赖与 warming | `resource-warming` | 延后发送依赖资源，观察新配置何时接流量 | — |
| 32 | Endpoint 更新与负载均衡 | `endpoint-update` | 移除一个端点，观察新连接和已有请求 | — |
| 33 | 异常实例摘除 | `outlier-detection` | 注入后端错误，观察探测窗口、摘除与恢复 | — |
| 34 | 代理过载保护 | `overload-manager` | 改变资源压力，验证拒绝的位置和观测信号 | — |
| 35 | **周复盘**：动态路由与配置回退 | `dynamic-routing-lab` | 注入坏配置和后端故障，验证更新隔离与恢复 | — |

## W6 · Apache Flink

| Day | 专题 | 主题路径 | 实验或验收问题 | 状态 |
|---|---|---|---|---|
| 36 | 流图、分区与算子链 | `stream-graph` | 跟踪相同 key 的事件落到哪个子任务 | — |
| 37 | 事件时间与 watermark | `event-time-watermark` | 依次输入乱序、迟到事件，判断窗口何时输出 | — |
| 38 | Keyed State 与定时器 | `keyed-state-timers` | 比较两个 key 之间的状态隔离及定时器触发 | — |
| 39 | 多输入 Barrier 与 checkpoint | `checkpoint-barriers` | 让一路输入迟到，观察对齐条件和状态切面 | — |
| 40 | Unaligned checkpoint 与背压 | `unaligned-checkpoint` | 对照对齐耗时与保存的在途数据量 | — |
| 41 | 恢复与端到端处理语义 | `recovery-and-sink` | 从快照恢复，区分算子状态与外部副作用 | — |
| 42 | **周复盘**：可恢复的窗口聚合 | `stateful-window-lab` | 组合乱序、状态与快照，注入慢输入和进程退出 | — |

## W7 · Fast DDS

| Day | 专题 | 主题路径 | 实验或验收问题 | 状态 |
|---|---|---|---|---|
| 43 | DDS 实体、Topic 与发现 | `dds-discovery` | 分别从发布端、订阅端启动，记录匹配条件 | — |
| 44 | QoS 兼容与可靠性 | `qos-reliability` | 对照可靠与尽力交付，观察不兼容配置 | — |
| 45 | Writer history 与 Reader 缓存 | `dds-history` | 改变历史深度并减慢订阅端，检查保留的数据 | — |
| 46 | Shared Memory 与 Data-sharing | `shared-memory-delivery` | 画出同机传输的样本路径，记录每次拷贝发生处 | — |
| 47 | Loaned samples 与归还 | `loaned-samples` | 跟踪缓冲区生命周期，实验延迟归还的影响 | — |
| 48 | Deadline 与 Liveliness | `deadline-liveliness` | 停止发布与终止进程，观察两种状态通知 | — |
| 49 | **周复盘**：本机实时数据通道 | `realtime-data-lab` | 组合发现、QoS 与共享数据，注入慢读和发布端退出 | — |
