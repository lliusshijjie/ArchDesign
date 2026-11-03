# 阶段实验阅读指南

建议分两次完成：第一次追通正常路径，第二次做故障实验并填写报告。

## 第一轮：从调用者追到结果

| 顺序 | 源码入口 | 阅读任务 | 读完应能回答 |
|---|---|---|---|
| 1 | [lab.hpp](code/lab.hpp) | 区分六个容量 / 预算，画出 Snapshot 计数关系 | 哪个上限限制物理在途工作？ |
| 2 | [normal()](code/scenarios.cpp) | 找出 sink、结果收集器、Pipeline 的构造与析构顺序 | 谁的生命周期必须长于 Pipeline？ |
| 3 | [offer / dispatch_inputs / admit](code/lab.cpp) | 从请求进入一直追到 pool.execute；圈出每个拒绝分支 | 提交成功为什么不一定被执行？ |
| 4 | [make_job / JobDeleter / PooledBytes](code/lab.cpp) | 标出 Job、payload、shared_ptr 控制块的不同分配来源 | 哪些对象实际使用了 Day 05？ |
| 5 | [perform / collect_completions](code/lab.cpp) | 跟踪 shared_ptr 随完成事件交接；回到 Day 06 的 push/pop | owner 读取 checksum 前靠什么看到 worker 的写入？ |
| 6 | [respond / observe_deadlines](code/lab.cpp) | 标出普通 bool 与 atomic 的线程归属 | 为什么 responded 不需要原子操作？ |
| 7 | [shutdown](code/lab.cpp) | 按步骤画出 join、close 和销毁顺序 | 为什么 ring 不能提前 close？ |

画图时给每个函数标记执行线程：`owner`、`pool worker`、`timer worker` 或 `logger worker`。遇到共享字段时，写清它靠线程归属、锁、原子还是 release/acquire 保证正确。

## 第二轮：用故障检查假设

### 检查点 A：过载

先比较 `ingress()` 和 `overload()`。前者在 pump 前制造入站积压；后者先确认 worker 进入闸门，再制造线程池排队。

在纸上填入：

```text
overload 放开闸门之前：
running = __
worker queued = __
admitted = __
pool rejected = __
owner 是否执行过业务函数 = __
```

解释为什么改成 CallerRuns 后，拒绝数可能减少，但主循环的响应能力也会变化。

### 检查点 B：超时

在 `deadline()` 中分别看三个时刻：

1. 收到 TimedOut：用户结果已结束，但 `in_flight=1`、`retired=0`。
2. 新请求到达：配额仍被占用，应得到 AdmissionRejected。
3. 闸门打开：worker 完成后回收旧任务，`late_completed=1`，旧请求没有第二次终态。

画出“响应生命周期”和“执行生命周期”两条时间线，标出 payload 的最后一次访问发生在哪条线上。

### 检查点 C：慢日志

找到 `slow_log()` 中 sink 闸门释放的位置。业务是否在释放**之前**已经排空，必须通过那个时刻的断言证明。只看最终全部完成，无法区分“异步隔离有效”和“先等慢日志、之后才完成”。

### 检查点 D：异常清理

列出以下路径还持有哪些资源：

- Job 构造时 payload 分配失败；
- timer 注册失败；
- pool.execute 抛 RejectedExecutionError；
- 业务抛异常；
- 结果回调抛异常；
- 场景断言抛异常。

至少解释 JobDeleter、weak_ptr、ReleaseOnExit 各负责其中哪一段。

## 修改前的预测表

| 只修改一个变量 | 预测的指标变化 | 实测 | 解释差异 |
|---|---|---|---|
| completion_budget: 16 → 1 | | | |
| worker_queue: 1 → 2 | | | |
| max_in_flight: 1 → 2 | | | |
| payload: 4096 → 4097 | | | |
| 慢日志场景 DropNew → Block | | | |

Block 对照可能阻塞主循环。请先设计由独立控制线程按时释放 sink 的方法，并保留 CTest 外部超时；仅由被阻塞的 owner 打开闸门，会让实验自身死锁。

## 完成标准

- 能画出一个请求的所有权转移，并指出每次释放由谁执行。
- 能解释 offered、admitted、终态和 retired 的不同口径。
- 在至少两个故障场景中，先预测、再记录真实结果。
- CTest 通过后，仍能说清测试没有覆盖的网络、磁盘、硬件和永久阻塞边界。
- 从前六课各写一条可迁移技巧，以及一条采用它的前提条件。
