---
day: "03"
title: "时间轮：海量定时器为什么能做到 O(1)"
tool: "Netty HashedWheelTimer / Kafka TimingWheel"
source:
  - "netty/common/src/main/java/io/netty/util/HashedWheelTimer.java"
  - "kafka/clients/src/main/java/org/apache/kafka/common/utils/SystemTimer.java"
  - "kafka/clients/src/main/java/org/apache/kafka/common/utils/TimingWheel.java"
patterns:
  - "分桶"
  - "哈希时间轮"
  - "层级时间轮"
  - "惰性取消"
  - "批量推进"
prerequisites:
  - "Day 02：事件循环与最近 timer"
  - "C++ 基础：thread / mutex / condition_variable / list"
estimated_minutes: 120
---

# Day 03 · 时间轮：海量定时器为什么能做到 O(1)

> **一句话结论**：时间轮把“按到期时间排序”换成“按到期时间分桶”。插入和取消不再需要维护全局有序结构，只需要算一个 bucket 下标；代价是**时间精度被 tick 量化**，并且要接受“同一时刻批量处理一桶任务”。

---

## 0. 本次问题

1. 最小堆做定时器已经是 O(log N)，为什么还需要时间轮？
2. Netty 的 `HashedWheelTimer` 里 `tick`、`wheelSize`、`rounds` 分别解决什么问题？
3. 延迟超过一整轮怎么办？为什么不能简单地对 wheelSize 取模？
4. 取消一个定时器，需要从桶的链表里摘除吗？
5. Kafka 的层级时间轮和 Netty 的单层时间轮，差异在哪里？
6. 时间轮的 O(1) 是“端到端 O(1)”吗？它换来了什么代价？

这次读完后，你应该能对一个“百万连接、每连接一个 60 秒空闲超时”的场景，估算：

- 最小堆需要多少次比较；
- 时间轮需要多少内存和每 tick 的工作量；
- tick 设成 10 ms、100 ms、1 s 分别意味着什么；
- 什么时候应该用时间轮，什么时候老老实实用堆。

---

## 1. 设计背景：定时器为什么会成为瓶颈

### 1.1 最小堆的代价

最直观的定时器实现是最小堆：

```text
add(deadline)   -> O(log N)
cancel(task)    -> O(log N)（需要索引/惰性删除）
next_deadline() -> O(1)
pop_expired()   -> O(log N)
```

在定时器数量不大时，它几乎完美：精确、通用、实现简单。很多语言标准库、事件循环、RPC 框架都用堆/有序结构。

但当 N 变成百万级，问题开始出现：

- 每次插入/删除都要比较和调整堆，cache 不友好；
- 每个任务通常要分配一个节点，内存开销大；
- 大量任务在同一时间附近到期，堆会集中弹出；
- 取消需要额外的索引或惰性删除，垃圾会堆积；
- 多线程提交时，堆需要锁保护。

典型场景：

| 场景 | N | 特点 |
|---|---|---|
| 连接空闲超时 | 10 万–100 万 | 延迟固定（如 60 s），大量连接 |
| RPC 请求超时 | 每秒数万–数十万 | 延迟短、变化多 |
| 消息队列延迟投递 | 百万级 | 延迟范围大（秒–天） |
| 重试退避 | 数十万 | 延迟指数增长 |
| 定时任务调度 | 数百–数千 | 数量少，但要求日历语义 |

对于“连接空闲超时”这类场景，N 很大、延迟比较固定、精度要求通常是百毫秒级——这正是时间轮的甜点区。

### 1.2 时间轮的核心思想

不要维护“全局按 deadline 排序”，而是维护一组**桶（bucket）**：

```text
wheel = [bucket0, bucket1, bucket2, ..., bucketN-1]
tick  = 时间轮每走一步的时长
```

每个桶代表一个时间片。任务按 deadline 放到对应的桶里；时间轮每 tick 前进一格，处理当前桶里的任务。

```text
tick=0     tick=1     tick=2     tick=3
  |          |          |          |
[bucket0] [bucket1] [bucket2] [bucket3] ...
```

插入时只需要：

```text
bucket = (current_tick + delay_ticks) & (wheel_size - 1)
```

取模用位运算，O(1)。取消只需要把任务标记为 canceled，等桶被处理时跳过，O(1)。

### 1.3 约束表

| 约束 | 具体表现 | 对设计的影响 |
|---|---|---|
| 定时器数量 | 百万级 | 不能用 O(N) 的链表查找 |
| 精度 | 连接超时通常 100 ms 级 | tick 可以粗一点 |
| 延迟范围 | 秒到天 | 单层轮需要 rounds/overflow |
| 内存 | 每连接一个 timer | 节点要小；空桶成本可控 |
| 取消 | 连接关闭、请求完成 | 取消要 O(1)，不能全局查找 |
| 线程模型 | 大量提交者 + 一个推进者 | 提交队列/锁要低竞争 |
| 回调 | 到期任务可能很多 | 批量执行，避免阻塞推进 |

---

## 2. 单层哈希时间轮

### 2.1 三个参数

```text
tickDuration：每格代表多长时间
wheelSize：桶的数量，必须是 2 的幂
mask：wheelSize - 1
```

总时间跨度：

```text
span = tickDuration × wheelSize
```

例如：

```text
tick = 100 ms
wheelSize = 512
span = 51.2 s
```

如果所有延迟都小于 51.2 s，单层轮就够了。超过 span 的延迟，需要：

- **rounds**：在桶里记录还要转几圈（Netty 的做法）；
- **层级时间轮**：用多级轮扩大范围（Kafka 的做法）。

### 2.2 插入：算 bucket 和 rounds

假设当前 tick 是 `current`，延迟 `delay`：

```text
delay_ticks = ceil(delay / tickDuration)
target_tick = current + delay_ticks
bucket = target_tick & mask
```

如果 `target_tick` 落在当前轮之后，但 bucket 下标和当前轮某个桶相同，就需要 `rounds` 告诉推进线程“这次只是路过，不要执行”。

一个常见公式（Netty 风格）：

```text
remainingRounds = (target_tick - current_tick) / wheelSize
```

这个公式的边界取决于 `target_tick` 和“当前 bucket 是否已经处理”的定义。用“下一次访问该 bucket 的 tick”来推导更直观：

```text
start = current_tick + 1
first_visit = start + (bucket - (start & mask) + wheelSize) % wheelSize
rounds = (target_tick - first_visit) / wheelSize
```

含义：

- `first_visit`：从下一个 tick 开始，第一次走到这个 bucket 的 tick；
- `rounds`：第一次访问之后，还要跳过多少个完整轮才到期。

因为 `target_tick - first_visit` 一定是 `wheelSize` 的整数倍，这个式子可以化简为：

```text
ticks = target_tick - current_tick
rounds = (ticks - 1) / wheelSize
```

本课实现用这个更简单的等价式；`first_visit` 保留在这里作为理解工具。

例子：`tick=10 ms`，`wheelSize=8`，当前 tick=0：

| 延迟 | target tick | bucket | first visit | rounds | 实际触发 |
|---|---|---|---|---|---|
| 20 ms | 2 | 2 | 2 | 0 | tick 2 |
| 100 ms | 10 | 2 | 2 | 1 | tick 10 |
| 180 ms | 18 | 2 | 2 | 2 | tick 18 |

三个任务都落在 bucket 2：

- tick 2：20 ms 任务 rounds=0，执行；100 ms 任务 rounds 从 1 减到 0，保留；180 ms 任务 rounds 从 2 减到 1，保留；
- tick 10：100 ms 任务执行；
- tick 18：180 ms 任务执行。

这就是“单层轮 + rounds”的全部魔法。

### 2.3 推进：每 tick 处理一个桶

```text
while running:
    sleep_until(next_tick)
    current_tick += 1
    bucket = wheel[current_tick & mask]
    for task in bucket:
        if canceled: remove
        else if rounds > 0: rounds -= 1
        else if deadline <= current_tick: expire
    next_tick += tick
```

复杂度：

- 插入：O(1)（计算 bucket + 链表尾插）；
- 取消：O(1)（原子标记，惰性删除）；
- 每 tick：O(当前桶任务数)，空桶 O(1)；
- 内存：O(wheelSize + 任务数)。

注意：**O(1) 是插入/取消的摊还成本，不是端到端延迟**。任务仍然要等到 tick 边界，且同一桶里任务多了，处理一桶可能是 O(k)。

### 2.4 时间轮的“分辨率税”

tick 是精度下限。延迟 1 ms 的任务，在 tick=100 ms 的轮里可能要等 100 ms；延迟 150 ms 的任务，可能落在第 2 个 tick，实际 200 ms 才触发。

所以选择 tick 时要问：

- 业务能接受多大的提前/延迟误差？
- tick 越小，轮转越快，空桶扫描和线程唤醒越频繁；
- tick 越大，精度越差，但 CPU 开销越低；
- 大量任务集中在同一 tick 时，会造成“批处理尖峰”。

时间轮不是高精度定时器。高精度、少量定时器，堆或 `timerfd` 更合适。

---

## 3. Netty `HashedWheelTimer` 的关键设计

> 阅读入口：[`HashedWheelTimer.java`](https://github.com/netty/netty/blob/4.1/common/src/main/java/io/netty/util/HashedWheelTimer.java)。

Netty 的时间轮服务于大量 Channel 的超时（空闲、读、写、连接），所以它的设计重点不是“单次定时精度”，而是**海量 timer 的吞吐、取消和内存**。

### 3.1 结构

```text
HashedWheelTimer
├── wheel: HashedWheelBucket[wheelSize]
├── tickDuration, wheelSize, mask
├── workerThread
├── timeouts: MPSC queue（新任务先入队）
├── pendingTimeouts: AtomicLong
└── maxPendingTimeouts
```

每个 `HashedWheelBucket` 是一条 `HashedWheelTimeout` 双向链表。每个 timeout 记录：

- `deadline`：绝对纳秒时间；
- `remainingRounds`：还要转几圈；
- `state`：INIT / CANCELLED / EXPIRED；
- `bucket`、`next`、`prev`。

### 3.2 新任务先进入 MPSC 队列

`newTimeout()` 不直接把任务塞进 bucket，而是先放进一个 MPSC（多生产者单消费者）队列：

```text
业务线程 newTimeout -> MPSC queue -> worker 每 tick 批量转移到 bucket
```

为什么？

- 业务线程可能很多，直接改 bucket 链表需要锁；
- MPSC 队列把“提交”和“推进”解耦，减少锁竞争；
- worker 可以批量转移，摊薄同步成本；
- 转移时可以统一计算 `remainingRounds`。

Netty 每 tick 最多转移一批（代码里限制 100000 个），避免一次提交风暴把 tick 拖长。这是“批处理 + 背压”的典型组合。

### 3.3 `remainingRounds` 与惰性取消

worker 推进到某个 bucket 时：

```text
for timeout in bucket:
    if canceled:
        remove
    else if remainingRounds > 0:
        remainingRounds -= 1
    else:
        remove and expire
```

取消不立即从链表中摘除，而是把状态置为 `CANCELLED`；等 bucket 被访问时再清理。这样：

- 取消是 O(1)，不需要知道任务在哪个 bucket；
- 代价是取消的任务会占用内存，直到对应 tick 被处理；
- 如果 tick 很大、任务延迟很长，取消垃圾可能停留很久。

Netty 还提供 `pendingTimeouts` 和 `maxPendingTimeouts`：

- `pendingTimeouts` 统计未完成 timeout；
- 超过 `maxPendingTimeouts` 时，`newTimeout` 直接拒绝（`RejectedExecutionException`）。

这是时间轮的内存背压：**不能无限接收定时器**。

### 3.4 启动、停止与线程模型

- `HashedWheelTimer` 默认在第一次 `newTimeout` 时启动 worker；
- `start()` 会等待 worker 设置 `startTime`，保证 deadline 基准一致；
- worker 是一个单独的线程，按 tick 睡眠、推进、执行到期任务；
- `stop()` 停止 worker，并返回尚未执行的 timeout 列表；
- 任务回调在 worker 线程执行；如果回调阻塞，整个时间轮都会延迟。

因此，Netty 的时间轮适合“短小的超时回调”（关闭连接、触发事件），不适合在回调里做重活。重活应该扔给业务线程池。

### 3.5 默认参数与边界

Netty 默认：

```text
tickDuration = 100 ms
ticksPerWheel = 512
span = 51.2 s
maxPendingTimeouts = -1（不限制）
```

这意味着：

- 延迟小于 100 ms 的任务，精度会被拉到 100 ms 级；
- 超过 51.2 s 的延迟靠 `remainingRounds` 跨轮；
- 如果任务量很大，建议显式设置 `maxPendingTimeouts`，防止 OOM；
- 对高精度场景，应该用更小的 tick 或换用堆/`timerfd`。

---

## 4. Kafka 的层级时间轮

> 阅读入口：[`SystemTimer.java`](https://github.com/apache/kafka/blob/trunk/clients/src/main/java/org/apache/kafka/common/utils/SystemTimer.java)、[`TimingWheel.java`](https://github.com/apache/kafka/blob/trunk/clients/src/main/java/org/apache/kafka/common/utils/TimingWheel.java)。

Netty 用单层轮 + `remainingRounds` 处理超长延迟。Kafka 面对的是另一个量级：

- 生产/消费请求的延迟投递；
- 请求超时；
- purgatory 中大量等待条件满足的请求；
- 延迟范围从毫秒到天。

单层轮跨轮会导致同一个长延迟任务反复被访问，Kafka 选择**层级时间轮（Hierarchical Timing Wheel）**。

### 4.1 多级轮的结构

```text
Level 0: tick=1ms,   wheelSize=20  -> 覆盖 20ms
Level 1: tick=20ms,  wheelSize=20  -> 覆盖 400ms
Level 2: tick=400ms, wheelSize=20  -> 覆盖 8s
Level 3: tick=8s,    wheelSize=20  -> 覆盖 160s
...
```

每一级的 bucket 数量相同，但 tick 逐级放大。任务先放入能覆盖它的最低层级；当高层轮推进时，把任务“降级”到低层轮，直到落到最底层。

Kafka 的 `TimingWheel` 用 `overflowWheel` 表示下一级：

```text
TimingWheel
├── tickMs
├── wheelSize
├── startMs
├── buckets: TimerTaskList[]
└── overflowWheel: TimingWheel
```

### 4.2 `DelayQueue` 驱动推进

Kafka 不只靠一个线程睡 tick，而是用 `DelayQueue<TimerTaskList>`：

- 每个 bucket 有一个到期时间；
- 到期 bucket 被放入 `DelayQueue`；
- `Reaper` 线程从 `DelayQueue` 取出到期 bucket；
- 推进时钟，处理 bucket 里的任务；
- 到期任务交给执行器/回调。

这样做的收益：

- 不需要每 tick 唤醒空轮；
- 只有真正有任务的 bucket 到期才唤醒；
- 高层轮降级和底层轮推进可以统一处理。

代价：

- 结构更复杂；
- `DelayQueue` 本身有锁/堆开销；
- 需要处理任务取消、bucket 过期、层级降级等状态。

### 4.3 为什么 Kafka 不用 Netty 式 rounds？

Kafka 的延迟范围很大，如果只用单层轮 + rounds：

- 一个延迟 1 天的任务会跨很多轮，每次经过 bucket 都要检查/递减；
- 大量长延迟任务会反复占用 bucket 扫描；
- 需要很大的 wheel 或很小的 tick，内存/精度取舍困难。

层级轮把“长时间范围”分摊到多级，长延迟任务大部分时间待在高层轮，只有接近到期才降到底层。**这是用空间和层级换“长延迟任务不反复扫描”。**

### 4.4 Netty vs Kafka

| 维度 | Netty `HashedWheelTimer` | Kafka `SystemTimer/TimingWheel` |
|---|---|---|
| 结构 | 单层轮 + rounds | 层级轮 + overflow |
| 长延迟 | 跨轮递减 | 降级到低层轮 |
| 推进 | worker 睡 tick | `DelayQueue` 驱动 |
| 适用 | 大量短中期连接超时 | 大量请求超时/延迟投递，范围大 |
| 精度 | tick 决定 | 最底层 tick 决定 |
| 复杂度 | 较低 | 较高 |
| 任务执行 | worker 线程 | Reaper/执行器（视实现） |

---

## 5. 其他定时器结构对比

| 结构 | 插入 | 取消 | 取最近到期 | 适用 |
|---|---|---|---|---|
| 有序链表 | O(N) | O(1)（有节点） | O(1) | 定时器极少 |
| 最小堆 | O(log N) | O(log N)/惰性 | O(1) | 通用、数量中等 |
| 红黑树/`multimap` | O(log N) | O(log N) | O(1) | 需要有序遍历 |
| 单层时间轮 | O(1) | O(1) | O(1) per tick | 大量、延迟范围有限 |
| 层级时间轮 | O(1) | O(1) | O(1) per bucket | 大量、延迟范围大 |
| `timerfd` | 内核管理 | 内核管理 | `poll` 可读 | 少量高精度、Linux |
| 时间轮 + 堆 | 混合 | 混合 | 混合 | 需要兼顾精度和吞吐 |

几个容易误解的点：

- **时间轮不是“更精确”**：它通常更粗，靠 tick 量化。
- **O(1) 不是免费**：空桶扫描、批量执行、取消垃圾、内存都是成本。
- **时间轮不持久化**：进程崩溃，内存里的 timer 全丢；需要持久化的延迟任务要用 MQ/DB。
- **时间轮不解决分布式**：多实例各自有轮，重复执行需要租约/幂等/fencing。

---

## 6. C++20 最小实现

完整代码在 [`code/`](code/)：

- [`hashed_wheel_timer.hpp`](code/hashed_wheel_timer.hpp)：单层哈希时间轮；
- [`main.cpp`](code/main.cpp)：6 个行为测试；
- [`CMakeLists.txt`](code/CMakeLists.txt)：构建配置。

### 6.1 设计映射

| Netty | 本实现 | 说明 |
|---|---|---|
| `HashedWheelTimer` | `HashedWheelTimer` | 单 worker 线程推进 |
| `HashedWheelBucket` | `std::list<shared_ptr<Timeout>>` | 每个 bucket 一条链表 |
| `HashedWheelTimeout` | `Timeout` | deadline、rounds、state |
| `remainingRounds` | `rounds` | 从首次访问推导，边界更直观 |
| `CANCELLED` | `State::Canceled` | 原子状态，惰性删除 |
| `pendingTimeouts` | `scheduled/executed/canceled` | 统计 + `max_pending` 限制 |
| `MPSC queue` | 直接加锁入 bucket | 教学简化；生产可换 MPSC |

### 6.2 插入：bucket 与 rounds

```cpp
const auto rounded = std::chrono::ceil<Duration>(delay);
const auto ticks = std::max<std::uint64_t>(
    1, static_cast<std::uint64_t>(rounded / tick_));

const std::uint64_t target = current_tick_ + ticks;
const std::uint64_t wheel_size = wheel_.size();
const std::uint64_t bucket =
    static_cast<std::uint64_t>(target & mask_);

// 目标 bucket 每隔 wheelSize 个 tick 被访问一次；在到达 target 之前，
// 一共有 floor((ticks-1)/wheelSize) 次“提前访问”。
const std::uint64_t rounds = (ticks - 1) / wheel_size;
```

关键点：

- `delay=0` 也至少等一个 tick，避免“刚插入就在当前桶里被处理”的竞态；
- 目标 bucket 每隔 `wheelSize` 个 tick 被访问一次；
- `rounds` 表示在真正到期前还要跳过几轮；
- 这个式子和前面用 `first_visit` 推导的公式等价，但少一次取模/分支；
- 位运算 `& mask` 要求 wheelSize 是 2 的幂。

### 6.3 推进：先收集，后执行

```cpp
++current_tick_;
auto& bucket = wheel_[current_tick_ & mask_];

std::vector<std::shared_ptr<Timeout>> due;
for (auto it = bucket.begin(); it != bucket.end();) {
    auto timeout = *it;

    if (timeout->state.load() == Timeout::State::Canceled) {
        it = bucket.erase(it);
        continue;
    }
    if (timeout->rounds > 0) {
        --timeout->rounds;
        ++it;
        continue;
    }
    if (timeout->deadline_tick <= current_tick_) {
        due.push_back(timeout);
        it = bucket.erase(it);
    } else {
        ++it;
    }
}

// 解锁后执行回调，避免回调里 new_timeout/cancel 时死锁
lock.unlock();
for (auto& timeout : due) { ... }
lock.lock();
```

为什么不在持锁时执行回调？

- 回调可能调用 `new_timeout` 或 `cancel`，需要同一把锁；
- 回调可能很慢，持锁会阻塞所有提交者；
- 先把到期任务收集出来，再统一执行，临界区更短。

### 6.4 取消：状态机而不是布尔值

一个看似简单的取消逻辑：

```cpp
if (!timeout->canceled.exchange(true)) {
    ++canceled_;
    return true;
}
```

会有竞态：

- worker 已经判定“未取消”，正要执行；
- 取消线程把 `canceled` 设为 true；
- 结果任务仍然执行，但取消计数也增加了；
- `executed + canceled` 可能大于 `scheduled`。

本实现用四态：

```text
Pending -> Canceled        （取消成功）
Pending -> Executing -> Done（开始执行并完成）
```

```cpp
// cancel
auto expected = Timeout::State::Pending;
if (timeout->state.compare_exchange_strong(expected,
                                           Timeout::State::Canceled)) {
    ++canceled_;
    return true;
}
return false;

// worker
auto expected = Timeout::State::Pending;
if (!timeout->state.compare_exchange_strong(expected,
                                            Timeout::State::Executing)) {
    continue;  // 已被取消
}
task();
timeout->state.store(Timeout::State::Done);
++executed_;
```

这样：

- 取消和执行只有一个能赢；
- 取消后到达执行点的任务会被跳过；
- 统计不会重复计数；
- 取消仍然是 O(1) 原子操作。

### 6.5 不补跑错过的 tick

worker 每次执行完回调后：

```cpp
next_tick_ += tick_;
if (next_tick_ <= now) {
    next_tick_ = now + tick_;
}
```

如果回调执行了 500 ms，而 tick 是 10 ms，不补跑 50 个 tick，而是从当前时间重新开始。

为什么？

- 补跑会在一次循环里处理大量过期 bucket，造成“定时器风暴”；
- 对连接超时、心跳这类任务，“晚一点”通常比“瞬间爆发”更安全；
- 代价是 timer 会向后漂移，不能用于严格周期调度。

需要严格周期/补偿的场景，应该记录绝对时间、计算错过的周期，并显式决定“补跑/跳过/合并”。

### 6.6 测试

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
# 或
./build/timing_wheel_demo
```

Windows（PowerShell）：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++
cmake --build build
.\build\timing_wheel_demo.exe
```

实测输出：

```text
[  OK  ] one_shot
[  OK  ] order_and_rounds
[  OK  ] cancel
[  OK  ] callback_exception_isolation
[  OK  ] pending_limit
[  OK  ] stop_rejects_new_timeouts

All 6 tests passed.
```

> 验证环境：Windows 11 + MinGW-w64 g++ 15.2 + CMake 4.2 + Ninja；`-std=c++20 -Wall -Wextra -Wpedantic -Werror` 编译通过。

### 6.7 生产化缺口

- **单层轮**：超长延迟靠 rounds，任务多时不如层级轮；
- **直接加锁入桶**：业务线程多时，锁竞争高于 Netty 的 MPSC 队列；
- **无持久化**：进程崩溃丢 timer；
- **无分布式协调**：多实例会各自触发，需要幂等/租约/fencing；
- **回调异常只计数**：生产要把异常和任务上下文一起记录；
- **停止语义**：`stop()` 不执行未到期任务，也不返回列表；并且不能在 timer 回调里调用（会 `join` 自己，示例直接抛 `logic_error`）；
- **时钟**：使用 `steady_clock`，适合测量间隔，但不提供日历时间；
- **精度**：tick 量化，不保证毫秒级精确；
- **背压**：只有 `max_pending`，没有按优先级/租户隔离；
- **可观测性**：只有计数，没有 bucket 分布、延迟分布、空转率。

---

## 7. 反例与边界

| 看起来对的用法 | 实际问题 | 正确做法 |
|---|---|---|
| 用 1 ms tick 支持百万连接 | 每秒 1000 次唤醒/扫描，CPU 空转 | 按业务精度选 tick（如 100 ms） |
| 所有延迟都塞进单层轮 | 长延迟任务反复跨轮扫描 | 层级轮或分离长/短延迟轮 |
| 取消时立刻从链表摘除 | 需要全局定位，破坏 O(1) | 原子标记 + 惰性删除 |
| 回调里做数据库/网络调用 | 阻塞整个时间轮 | 回调只投递到业务线程池 |
| 不限制 pending 数量 | 定时器无限增长，OOM | `maxPendingTimeouts`/配额 |
| 依赖时间轮做精确 cron | tick 量化 + 漂移，错过触发 | 专用调度器 + 补偿策略 |
| 多实例各自定时 | 重复执行 | 分布式锁/租约/幂等 |
| 认为 O(1) 插入等于 O(1) 延迟 | 实际要等 bucket 轮转 | 明确精度和尾延迟 |
| 停止时直接销毁 | 未处理任务丢失且无记录 | 返回/记录待处理任务，优雅关闭 |
| 时间轮里执行长任务 | 后续所有 timer 延迟 | 短回调 + 独立执行器 |

### 压力下会发生什么

- **同一 tick 大量到期**：一桶任务集中执行，worker 被占满，后续 tick 漂移；需要批处理/限流/分片。
- **大量取消**：取消任务仍占 bucket 内存，直到对应 tick；长延迟 + 高取消率会堆积。
- **tick 太细**：空转和唤醒成本高；tick 太粗：精度差、批量尖峰。
- **回调阻塞**：所有 timer 延迟；时间轮不适合做业务执行器。
- **时钟回拨**：`steady_clock` 不受墙钟影响，但业务语义（如“每天 9 点”）需要墙钟和时区处理。
- **进程重启**：内存 timer 全丢；需要持久化延迟任务（MQ/DB + 恢复扫描）。

---

## 8. 如何借鉴到自己的项目

### 8.1 模式卡片

```text
模式名称：哈希时间轮（Hashed Timing Wheel）
解决什么问题：海量定时器的插入、取消和到期处理，避免全局有序结构的 O(log N) 成本。
核心机制：
  1. tick + wheelSize 分桶；
  2. deadline -> bucket，rounds 处理跨轮；
  3. 单推进线程按 tick 扫描当前桶；
  4. 取消用原子状态 + 惰性删除；
  5. pending 上限形成背压。
代价/限制：
  - 精度被 tick 量化；
  - 同一 tick 任务会批量执行；
  - 长延迟需要 rounds/层级轮；
  - 回调阻塞会拖慢整个轮；
  - 内存 timer 不持久化。
不适用场景：
  - 少量高精度定时器（堆/timerfd 更合适）；
  - 日历级 cron（需要时区/补偿/持久化）；
  - 回调本身很重（应投递到执行器）。
一个最小例子：本课的 HashedWheelTimer。
```

### 8.2 接口清单

```text
[ ] tick 的依据是什么？业务能接受多大误差？
[ ] wheelSize 是否 2 的幂？span 是否覆盖大多数延迟？
[ ] 超长延迟用 rounds 还是层级轮？
[ ] 取消是否 O(1)？取消垃圾何时清理？
[ ] pending 是否有上限？超限是拒绝还是降级？
[ ] 回调是否只做短工作？慢任务去哪里？
[ ] 推进线程阻塞/漂移如何监控？
[ ] 进程重启后，timer 是否需要恢复？
[ ] 多实例部署，如何避免重复触发？
[ ] 是否监控 bucket 分布、到期延迟、空转率？
```

### 8.3 迁移步骤

1. **先统计**：timer 数量、延迟分布、取消率、tick 误差、回调耗时。
2. **判断是否值得**：定时器少/精度高，继续用堆；百万级、精度百毫秒，考虑时间轮。
3. **先做单层轮**：固定 tick，限制 pending，短回调。
4. **再加 rounds/层级**：按延迟范围决定；不要一上来就写多级轮。
5. **做压力测试**：同 tick 大量到期、取消风暴、长回调、时钟漂移。
6. **补持久化/幂等**：跨进程的延迟任务不能只靠内存轮。

---

## 9. 练习

**基础**

1. 给 `HashedWheelTimer` 增加 `pending()` 的精确快照（当前是 `scheduled - executed - canceled`）。
2. 把 `std::list` 换成侵入式链表，比较内存和缓存局部性。
3. 增加 bucket 分布指标：每个 tick 处理了多少任务、空桶比例。

**进阶**

1. 实现一个简单的**层级时间轮**：两级轮，第一级 tick=10 ms/64 桶，第二级 tick=640 ms/64 桶；长延迟任务从高级降到低级。
2. 把 `new_timeout` 的提交改成 MPSC 队列，worker 每 tick 批量转移，比较锁竞争。
3. 增加 `stop()` 返回未执行任务列表，并支持 finalizer。

**挑战**

1. 用时间轮实现“百万连接空闲超时”：每个连接有最后活跃时间，超时后续期/取消；测量内存、tick 延迟和取消垃圾。
2. 实现一个持久化延迟队列：时间轮 + WAL/MQ，进程重启后恢复未到期任务；处理重复触发和幂等。
3. 对比 `std::priority_queue`、单层轮、层级轮在 1 万/100 万 timer 下的插入、取消、触发延迟和内存。

---

## 10. 自测题

<details>
<summary>点击展开答案</summary>

**1. 时间轮为什么能做到 O(1) 插入？**

插入时只需要根据 deadline 算出目标 tick 和 bucket 下标（位运算），然后把任务挂到该 bucket 的链表尾部；不需要维护全局有序结构。取消也用原子状态标记，不需要定位链表节点。

**2. `rounds` 解决什么问题？**

单层轮的总跨度是 `tick × wheelSize`。超过这个跨度的任务会在未来某一轮再次经过同一个 bucket；`rounds` 记录还要跳过几轮，避免提前执行。每次经过 bucket 时递减，减到 0 才真正到期。

**3. 为什么取消不立即从 bucket 链表摘除？**

立即摘除需要知道任务在哪个 bucket、哪个链表节点，通常要额外索引或加锁查找。惰性取消只做一次原子状态变更，等 bucket 被扫描时再清理，保证 O(1)。代价是取消的任务会占内存，直到对应 tick。

**4. 时间轮的 O(1) 是端到端 O(1) 吗？**

不是。插入/取消是 O(1)，但任务要等 tick 边界；一个 bucket 里的任务多时，处理是 O(k)；空桶推进是 O(1) 但仍有唤醒成本；长延迟跨轮会反复访问。它是“摊还的插入/取消成本”，不是“任意精度的常数延迟”。

**5. Netty 和 Kafka 的时间轮有什么核心差异？**

Netty 是单层轮 + `remainingRounds` 处理跨轮，worker 按 tick 推进；Kafka 是层级时间轮 + `DelayQueue` 驱动，长延迟任务在高层轮，接近到期时降到底层轮。前者结构简单，后者适合更大延迟范围和更多长延迟任务。

**6. 为什么时间轮回调不能做重活？**

回调在推进线程执行；回调阻塞会延迟当前 tick 的后续任务和后续 tick，导致所有 timer 漂移。应该把重活投递到业务线程池，回调只做状态推进/投递。

**7. 进程崩溃后时间轮里的任务会怎样？**

全部丢失。时间轮是内存结构；需要跨重启的延迟任务必须持久化到 WAL/MQ/DB，并在启动时恢复/重建。多实例还要处理重复触发和幂等。

**8. 如何选择 tick 和 wheelSize？**

从业务精度和延迟分布出发：tick 决定误差下限，wheelSize 决定单层跨度。先覆盖大多数延迟，长尾用 rounds/层级轮；再通过压测看空转、批量尖峰和内存。不要为了“更精确”把 tick 设得极小。

</details>

---

## 11. 模式卡片

```text
模式名称：分桶 + 惰性取消的定时器
解决什么问题：海量 timer 的 O(1) 插入/取消，避免全局堆的日志级比较。
核心机制：
  1. tick 量化时间；
  2. deadline -> bucket；
  3. rounds 跨轮；
  4. 原子状态 + 惰性删除；
  5. 单线程推进 + 批量回调。
代价/限制：
  - 精度受 tick 限制；
  - 同 tick 批量执行；
  - 长延迟需要额外结构；
  - 回调阻塞全局。
不适用场景：
  - 少量高精度 timer；
  - 日历调度/持久化任务；
  - 长耗时回调。
一个最小例子：本课的 HashedWheelTimer。
```

---

## 12. 下次预习

下一次进入 **异步日志**。带着三个问题读：

1. 日志写入为什么不能直接在业务线程做？
2. Log4j2 的 AsyncLogger 如何用 Disruptor 把日志生产者和消费者解耦？
3. 队列满时，日志应该丢弃、阻塞还是降级？停机时如何保证已排队日志落盘？

阅读入口：Log4j2 `AsyncLogger` / `AsyncLoggerContextSelector`、LMAX Disruptor、spdlog `async_logger`。

---

## 参考资料

- Netty, [`HashedWheelTimer.java`](https://github.com/netty/netty/blob/4.1/common/src/main/java/io/netty/util/HashedWheelTimer.java)：单层轮、MPSC 队列、`remainingRounds`、取消状态。
- Kafka, [`SystemTimer.java`](https://github.com/apache/kafka/blob/trunk/clients/src/main/java/org/apache/kafka/common/utils/SystemTimer.java)：层级时间轮 + `DelayQueue` + Reaper。
- Kafka, [`TimingWheel.java`](https://github.com/apache/kafka/blob/trunk/clients/src/main/java/org/apache/kafka/common/utils/TimingWheel.java)：overflow wheel、bucket 推进。
- Kafka, [`TimerTaskList.java`](https://github.com/apache/kafka/blob/trunk/clients/src/main/java/org/apache/kafka/common/utils/TimerTaskList.java)：bucket 链表与过期时间。
- Linux man page, [`timerfd_create(2)`](https://man7.org/linux/man-pages/man2/timerfd_create.2.html)：内核定时器与 fd 就绪模型。
- C++ reference：[`std::chrono::ceil`](https://en.cppreference.com/w/cpp/chrono/duration/ceil)、[`std::condition_variable`](https://en.cppreference.com/w/cpp/thread/condition_variable)。
- Netty 文档/源码中的 `IdleStateHandler`、`ReadTimeoutHandler`：时间轮在连接超时中的典型用法。
