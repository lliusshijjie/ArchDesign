---
day: "12"
title: "MVCC：快照读与版本链"
tool: "PostgreSQL / InnoDB"
source:
  - "postgres/src/backend/access/heap/heapam_visibility.c"
  - "postgres/src/backend/storage/ipc/procarray.c"
  - "mysql-server/storage/innobase/read/read0read.cc"
  - "mysql-server/storage/innobase/row/row0vers.cc"
  - "mysql-server/storage/innobase/trx/trx0purge.cc"
patterns:
  - "多版本"
  - "快照"
  - "可见性判断"
  - "版本链/undo"
  - "purge/VACUUM"
  - "写冲突"
prerequisites:
  - "Day 09：WAL"
  - "Day 10：Buffer Pool"
  - "Day 11：B+ 树页面"
estimated_minutes: 120
---

# Day 12 · MVCC：快照读与版本链

> **一句话结论**：MVCC 不是“给每行加一把锁”，而是**不覆盖旧值，保留多个版本，让每个事务按自己的快照挑一个能看的版本**。读不阻塞写，写不阻塞读；代价是旧版本要清理，长事务会把清理卡住。

---

## 0. 本次问题

1. 两个人同时读写同一行，为什么读的人不用等写的人？
2. “快照”到底是什么？是复制一份数据吗？
3. 一行数据怎么同时存在多个版本？
4. 数据库怎么判断“这个版本对我可见”？
5. 旧版本什么时候能删？为什么长事务会导致表膨胀？
6. PostgreSQL 和 InnoDB 的实现有什么不同？
7. 快照隔离能防住所有并发问题吗？
8. 自己实现 MVCC，最容易在哪个条件上写错？

---

## 0.5 先建立直觉

### 生活类比：论文的“修改历史”

你在写一篇论文：

```text
v1: "结论：A 是对的"
v2: "结论：A 可能对"
v3: "结论：A 是错的"
```

如果每次修改都**覆盖**旧内容，那么：

- 别人正在看 v1，你突然改成 v3，他会看到前后不一致；
- 你想回滚，旧内容已经没了。

更自然的做法是：

- 每次修改都**新增一个版本**；
- 每个读者开始阅读时，**拍一张照**，记住“我只看这个时间点之前的版本”；
- 读者继续看自己的旧版本，你继续写新版本，互不打扰。

数据库的 MVCC 就是这个思路：

```text
不覆盖旧值 -> 追加新版本 -> 每个事务拿一个快照 -> 按快照挑版本
```

### 只需要记住三个词

| 词 | 人话 |
|---|---|
| 版本 | 这一行在某个时刻的值 |
| 事务 ID | 谁改的/谁删的 |
| 快照 | 我读的时候，哪些事务还没结束 |

**第一遍可以先跳过**：PostgreSQL 的 `xip`、InnoDB 的 `read view` 字段、undo 链指针、purge 的具体算法。

---

## 1. 没有 MVCC 会怎样？

假设只有一把锁：

```text
T1: BEGIN; UPDATE user SET balance=80 WHERE id=1;  -- 持锁
T2: SELECT balance FROM user WHERE id=1;           -- 等锁
T1: COMMIT;                                        -- 释放锁
T2: 才拿到 80
```

问题：

- 读要等写，写要等读；
- 长事务会阻塞所有人；
- 读写混合业务的延迟很差。

另一种做法是“读写都加锁”，但读多写少的系统会被写锁拖死。

MVCC 的目标：

```text
读 -> 读一个旧快照，不等写
写 -> 写一个新版本，不覆盖读的人正在看的数据
```

这就是“读不阻塞写，写不阻塞读”。

---

## 2. 一个贯穿全文的例子

```text
初始：k = v1

T1（写）：把 k 改成 v2
T2（读）：在 T1 提交前开始读
T3（读）：在 T1 提交后开始读
```

期望：

- T2 看到 v1（它的快照在 T1 提交之前）；
- T3 看到 v2（它的快照在 T1 提交之后）；
- T1 不需要等 T2 读完才能提交。

数据可能是：

```text
版本 1: xmin=T1_old, xmax=T1, value=v1
版本 2: xmin=T1,     xmax=0,  value=v2
```

- `xmin`：创建这个版本的事务；
- `xmax`：删除/更新这个版本的事务；
- 0 表示还没被删除。

T2 的快照会记住“T1 还没提交”，所以：

- v1 的 `xmax=T1`，但 T1 对 T2 不可见 -> v1 仍然可见；
- v2 的 `xmin=T1`，T1 对 T2 不可见 -> v2 不可见。

T3 的快照在 T1 提交之后：

- v1 的 `xmax=T1`，T1 已提交且可见 -> v1 被删；
- v2 的 `xmin=T1`，可见 -> 返回 v2。

---

## 3. 三个核心对象

### 3.1 版本（Version）

```cpp
struct Version {
    TxId xmin;       // 谁创建
    TxId xmax;       // 谁删除/更新（0 = 没删）
    std::string value;
    bool tombstone;  // 删除标记
};
```

一行不再是“一个值”，而是一条版本链：

```text
v1 (xmin=10, xmax=20) -> v2 (xmin=20, xmax=0)
```

新版本在链尾，旧版本在链头。

### 3.2 快照（Snapshot）

快照不是复制数据，而是记录：

```text
snapshot.xmax   = 快照开始时“下一个事务 id”
snapshot.active = 快照开始时还活着的事务 id 集合
```

直觉：

- 事务 id 比 `xmax` 小的，可能是“过去”的；
- 但在 `active` 里的，说明快照开始时还没结束，不能算“过去”；
- 比 `xmax` 大的，是快照之后才开始的，不能看。

### 3.3 事务状态

每个事务有三种状态：

```text
Active（进行中）
Committed（已提交）
Aborted（已回滚）
```

可见性判断不只看 id，还要看状态：

- 创建者未提交/回滚 -> 版本不可见；
- 删除者未提交/回滚 -> 版本仍然可见。

---

## 4. 可见性规则：用“人话”写一遍

对版本 `V` 和读事务 `T`：

### 4.1 创建者可见吗？

```text
如果 V.xmin == T.id：
    可见（自己的写自己看得见）

否则如果 V.xmin 已提交
     且 V.xmin < T.snapshot.xmax
     且 V.xmin 不在 T.snapshot.active：
    可见

否则：
    不可见
```

### 4.2 删除者生效吗？

```text
如果 V.xmax == 0：
    没被删 -> 可见

如果 V.xmax == T.id：
    自己删的 -> 不可见

如果 V.xmax 未提交/回滚：
    删除无效 -> 可见

如果 V.xmax >= T.snapshot.xmax
   或 V.xmax 在 T.snapshot.active：
    删除发生在快照之后 -> 可见

否则：
    已删除 -> 不可见
```

### 4.3 最终判断

```text
可见 = 创建者可见 && 删除者未生效
```

读的时候从新版本往旧版本找，**第一个可见的版本**决定结果：

- 是 tombstone -> 返回“不存在”；
- 是普通值 -> 返回该值；
- 都没有 -> 返回“不存在”。

> 这就是为什么删除不能直接删数据：删掉旧版本，老快照就没得看了。

---

## 5. PostgreSQL 怎么做

> 阅读入口：[`heapam_visibility.c`](https://github.com/postgres/postgres/blob/master/src/backend/access/heap/heapam_visibility.c)、[`procarray.c`](https://github.com/postgres/postgres/blob/master/src/backend/storage/ipc/procarray.c)。

PostgreSQL 把版本直接放在**堆表**里：

```text
tuple header:
  xmin  -- 创建事务
  xmax  -- 删除/更新事务
  ctid  -- 物理位置
  ...
```

- 更新 = 插入一条新 tuple，旧 tuple 的 `xmax` 设为当前事务；
- 旧 tuple 不立刻删除，留在堆里；
- 读取时根据快照判断 `xmin/xmax` 是否可见；
- 清理由 `VACUUM` 做，不是事务提交时立刻做。

快照大致包含：

```text
xmin  -- 最老的活跃事务
xmax  -- 第一个未分配的事务 id
xip   -- 快照时活跃的事务 id 列表
```

直觉：

- `xmin` 之前的事务都结束了；
- `xmax` 之后的事务都快照之后才开始；
- `xip` 是“中间还没结束”的例外。

### 5.1 为什么会有表膨胀？

旧 tuple 还在堆里，只是“对某些快照不可见”。如果长事务一直不结束：

- 它的快照还需要旧版本；
- `VACUUM` 不能清理这些旧 tuple；
- 表越来越大 -> 膨胀（bloat）。

### 5.2 PostgreSQL 的 MVCC 特点

- 旧版本在堆表里，不是 undo log；
- 更新可能产生新 tuple（HOT 更新可复用页内空间）；
- `VACUUM` 负责回收；
- 长事务/长快照会阻塞清理；
- 默认 `READ COMMITTED`，每条语句一个快照；`REPEATABLE READ` 整个事务一个快照。

---

## 6. InnoDB 怎么做

> 阅读入口：[`read0read.cc`](https://github.com/mysql/mysql-server/blob/trunk/storage/innobase/read/read0read.cc)、[`row0vers.cc`](https://github.com/mysql/mysql-server/blob/trunk/storage/innobase/row/row0vers.cc)、[`trx0purge.cc`](https://github.com/mysql/mysql-server/blob/trunk/storage/innobase/trx/trx0purge.cc)。

InnoDB 的聚簇索引记录里有两个重要字段：

```text
DB_TRX_ID    -- 最近修改这条记录的事务 id
DB_ROLL_PTR  -- 指向 undo log 里的旧版本
```

- 更新记录时，旧值写进 undo log；
- 记录本身指向 undo 版本；
- 读取时沿 `DB_ROLL_PTR` 找回旧版本；
- 清理由 purge 线程做。

### 6.1 Read View

InnoDB 的 read view 记录：

```text
m_ids           -- 活跃事务 id 列表
m_up_limit_id   -- 最小活跃事务 id（低水位）
m_low_limit_id  -- 下一个事务 id（高水位）
```

可见性直觉：

- 创建者事务 id < 低水位：已结束，可见；
- 创建者事务 id >= 高水位：快照之后才开始，不可见；
- 在 `m_ids` 里：快照时还活着，不可见。

和 PostgreSQL 的 `xmin/xmax/xip` 是同一类思想，只是字段名和存储位置不同。

### 6.2 undo 链

```text
当前记录 (trx_id=30)
   -> undo: 旧版本 (trx_id=20)
       -> undo: 更旧版本 (trx_id=10)
```

读快照时，如果当前记录不可见，就沿 undo 链找旧版本，直到找到可见版本。

### 6.3 purge

purge 线程清理：

- 不再被任何 read view 需要的 undo；
- 删除标记（delete-marked）且没有更老版本需要的记录；
- 二级索引里的旧版本。

长事务会卡住 purge：

```text
长事务的快照还需要旧版本
-> purge 不能推进
-> undo 表空间/历史版本膨胀
```

### 6.4 当前读 vs 快照读

InnoDB 里：

- 普通 `SELECT`：快照读（consistent read），走 MVCC；
- `SELECT ... FOR UPDATE` / `LOCK IN SHARE MODE`：当前读，加锁，读最新版本；
- `UPDATE`/`DELETE`：当前读 + 锁。

所以“MVCC 让读写不阻塞”只对普通快照读成立；加锁读仍然会阻塞。

---

## 7. PostgreSQL vs InnoDB

| 维度 | PostgreSQL | InnoDB |
|---|---|---|
| 旧版本放哪 | 堆表里的旧 tuple | undo log |
| 删除/更新 | 新 tuple + 旧 tuple `xmax` | 记录 `DB_TRX_ID` + `DB_ROLL_PTR` |
| 快照字段 | `xmin/xmax/xip` | `m_ids/m_up_limit_id/m_low_limit_id` |
| 清理 | `VACUUM` | purge 线程 |
| 膨胀位置 | 表/索引 | undo 表空间、历史版本 |
| 默认隔离级别 | READ COMMITTED | REPEATABLE READ |
| 长事务影响 | 表膨胀、VACUUM 卡住 | undo 膨胀、purge 卡住 |

共同点：

- 都不覆盖正在被读的旧版本；
- 都用“事务 id + 快照”做可见性；
- 都需要清理旧版本；
- 长事务都会让清理落后。

---

## 8. C++20 最小实现

完整代码在 [`code/`](code/)：

- [`mvcc_store.hpp`](code/mvcc_store.hpp)：版本链 + 快照 + 可见性 + purge；
- [`main.cpp`](code/main.cpp)：6 个行为测试；
- [`CMakeLists.txt`](code/CMakeLists.txt)：构建配置。

> 建议先读本文第 0–4 节，再看代码；不要一上来抠 `visible()`。

### 8.1 数据结构

```cpp
struct Version {
    TxId xmin = 0;
    TxId xmax = 0;
    std::string value;
    bool tombstone = false;
};

struct Snapshot {
    TxId xmax = 0;
    std::unordered_set<TxId> active;
};

std::map<std::string, std::vector<Version>> data_;
```

- `data_` 里每个 key 一条版本链；
- 新版本追加到链尾；
- 读取从链尾往链头找。

### 8.2 begin：拍照

```cpp
TxId begin() {
    const TxId id = next_txid_++;
    Snapshot snapshot;
    snapshot.xmax = id;
    snapshot.active = active_;

    transactions_.emplace(
        id, Transaction{id, std::move(snapshot), Status::Active});
    active_.insert(id);
    return id;
}
```

注意顺序：

1. 先分配 id；
2. 快照的 `xmax = id`；
3. 快照的 `active` 是“加自己之前”的活跃集合；
4. 最后才把自己加入活跃集合。

### 8.3 put：追加版本 + 标记旧版本

```cpp
void put(TxId txid, const std::string& key, std::string value) {
    auto& versions = data_[key];
    check_write_conflict(versions, txid, tx);

    if (!versions.empty()) {
        auto& latest = versions.back();
        if (latest.xmax == 0 || latest.xmax == txid) {
            latest.xmax = txid;
        }
    }
    versions.push_back(Version{txid, 0, std::move(value), false});
}
```

- 旧版本 `xmax = 当前事务`；
- 新版本 `xmin = 当前事务`；
- 不覆盖旧值。

### 8.4 get：从新到旧找可见版本

```cpp
for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
    if (visible(*it, tx)) {
        if (it->tombstone) return std::nullopt;
        return it->value;
    }
}
return std::nullopt;
```

### 8.5 可见性

```cpp
bool visible(const Version& version, const Transaction& tx) const {
    // 创建者可见？
    if (version.xmin != tx.id) {
        if (status_of(version.xmin) != Status::Committed) return false;
        if (version.xmin >= tx.snapshot.xmax) return false;
        if (tx.snapshot.active.count(version.xmin)) return false;
    }

    // 删除者生效？
    if (version.xmax == 0) return true;
    if (version.xmax == tx.id) return false;
    if (status_of(version.xmax) != Status::Committed) return true;
    if (version.xmax >= tx.snapshot.xmax) return true;
    if (tx.snapshot.active.count(version.xmax)) return true;

    return false;
}
```

这段代码是本课最值得逐行理解的部分。建议用第 2 节的例子手工走一遍。

### 8.6 回滚

```cpp
void abort(TxId txid) {
    tx.status = Status::Aborted;
    active_.erase(txid);
}
```

本实现不真正删除版本：

- 创建者状态是 Aborted -> 版本不可见；
- 删除者状态是 Aborted -> 旧版本重新可见。

这就是“逻辑回滚”。

### 8.7 purge

```cpp
std::size_t purge() {
    if (!active_.empty()) return 0;  // 保守：有快照就不清理
    // 移除 aborted 版本；
    // 每个 key 只保留最新已提交版本；
    // 如果最新是 tombstone，整个 key 可以删。
}
```

这是教学简化。生产 purge 要算“最老活跃快照”的水位：

```text
horizon = 所有活跃事务快照中最早还需要的位置
只清理 horizon 之前的旧版本
```

### 8.8 测试

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
# 或
./build/mvcc_demo
```

Windows（PowerShell）：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++
cmake --build build
.\build\mvcc_demo.exe
```

实测输出：

```text
[  OK  ] snapshot_isolation
[  OK  ] own_write_and_rollback
[  OK  ] aborted_update_restores_old_value
[  OK  ] delete_visibility
[  OK  ] write_conflict
[  OK  ] purge_horizon

All 6 tests passed.
```

> 验证环境：Windows 11 + MinGW-w64 g++ 15.2 + CMake 4.2 + Ninja；`-std=c++20 -Wall -Wextra -Wpedantic -Werror` 编译通过，重复运行 20 次无失败。

### 8.9 生产化缺口

- 没有 WAL/redo/undo 的物理恢复；
- 没有真正的回滚链，只靠状态判断；
- 没有行锁/间隙锁/可串行化；
- 没有索引和范围扫描的可见性；
- 没有长事务 horizon/增量 purge；
- 没有多线程 latch；
- 没有 `READ COMMITTED` / `REPEATABLE READ` 的切换；
- 没有写偏斜检测。

---

## 9. 反例与边界

| 看起来对的用法 | 实际问题 | 正确做法 |
|---|---|---|
| 修改时直接覆盖旧值 | 老快照读不到旧版本 | 追加版本/undo |
| 删除时直接物理删除 | 老快照读不到、回滚困难 | tombstone/delete-mark |
| 只比事务 id，不看 active | 快照开始时还活着的事务被误判 | 快照保存 active 集合 |
| 只比创建者，不看删除者 | 已删除版本仍然可见 | 创建者 + 删除者都判断 |
| 提交后立刻清所有旧版本 | 活跃快照被破坏 | 按最老快照水位清理 |
| 长事务一直不提交 | purge/VACUUM 卡住，膨胀 | 监控长事务，设置上限 |
| 以为 MVCC = 可串行化 | 写偏斜仍可能发生 | 需要串行化/显式锁 |
| 加锁读也用快照 | 读到旧值，业务错 | 当前读/锁定读 |

### 压力下会发生什么

- **长事务**：旧版本无法清理，undo/表膨胀，purge 落后。
- **高并发写同一行**：写冲突/锁等待，重试或串行化。
- **快照很久不释放**：复制槽/逻辑解码/长查询也可能卡住清理。
- **大量删除**：tombstone/delete-mark 堆积，清理跟不上。
- **写偏斜**：两个事务各读一部分、各写另一部分，快照隔离允许，但业务约束被破坏。

---

## 10. 快照隔离不是“万能的”

经典写偏斜（write skew）：

```text
规则：至少一名医生值班。

T1: 查当前有 2 名医生值班 -> 我下班
T2: 查当前有 2 名医生值班 -> 我下班
T1/T2 都提交 -> 现在 0 名医生
```

两个事务读到的是同一个旧快照，各自认为“还有别人值班”，于是都更新。快照隔离允许这个结果；要防住它，需要：

- 可串行化隔离级别；
- 谓词锁/间隙锁；
- 物化冲突/显式锁；
- 业务层约束。

**记住：MVCC 解决“读写不互相阻塞”，不自动解决“所有并发正确性”。**

---

## 11. 如何借鉴到自己的项目

### 11.1 模式卡片

```text
模式名称：MVCC（多版本 + 快照可见性）
解决什么问题：让读不阻塞写、写不阻塞读，并给每个事务一致快照。
核心机制：
  1. 版本链（xmin/xmax 或 trx_id/roll_ptr）；
  2. 事务状态；
  3. 快照（xmax + active）；
  4. 可见性判断；
  5. purge/VACUUM。
代价/限制：
  - 旧版本占空间；
  - 长事务卡清理；
  - 写冲突/写偏斜；
  - 实现复杂。
不适用场景：
  - 纯内存、无并发；
  - 强串行化且冲突极高的场景（可能锁更好）；
  - 无法承受历史版本存储成本。
一个最小例子：本课的 MVCCStore。
```

### 11.2 接口清单

```text
[ ] 版本里有没有创建者/删除者？
[ ] 快照是否保存 active 集合？
[ ] 创建者/删除者都判断了吗？
[ ] 自己的写/删是否特殊处理？
[ ] 回滚后旧版本是否能恢复可见？
[ ] tombstone 什么时候能删？
[ ] purge 的水位怎么算？
[ ] 长事务/长快照如何监控？
[ ] 加锁读是否绕过 MVCC？
[ ] 快照隔离够不够，是否需要串行化？
```

### 11.3 迁移步骤

1. **先明确隔离级别**：RC、RR、Serializable。
2. **先做版本链 + 快照读**，再考虑写冲突。
3. **先做单线程正确性**：用测试覆盖快照、回滚、删除。
4. **再做 purge**：从“无活跃事务才清理”开始，逐步做水位。
5. **最后做并发/锁/恢复**：行锁、间隙锁、WAL/undo。
6. **压测长事务/大量删除/写冲突**。

---

## 12. 练习

**基础**

1. 给 `MVCCStore` 增加 `read_committed` 模式：每条 `get` 重新拍快照。
2. 打印每个 key 的版本链（xmin/xmax/value），手工验证可见性。
3. 增加“事务开始时间”和“最老活跃事务”统计。

**进阶**

1. 实现按最老快照水位的增量 purge，而不是“没有活跃事务才清理”。
2. 实现写冲突重试/等待队列；比较 first-writer-wins 和 wait-die。
3. 给版本链加范围扫描/索引可见性。

**挑战**

1. 实现快照隔离 + 写偏斜检测（SSI 的简化版）。
2. 对比 PostgreSQL 堆表旧版本和 InnoDB undo 链的读放大/空间放大。
3. 注入长事务，观察 purge 落后和空间膨胀。

---

## 13. 自测题

<details>
<summary>点击展开答案</summary>

**1. MVCC 为什么能让读不阻塞写？**

读事务按自己的快照读旧版本；写事务追加新版本，不覆盖读事务正在看的版本。两者操作不同版本，不需要互相等待。

**2. 快照是复制一份数据吗？**

通常不是。快照只记录事务 id 水位和活跃事务集合；读时用这些信息判断哪个版本可见。数据版本仍然共享。

**3. `xmin` 和 `xmax` 分别是什么？**

`xmin` 是创建版本的事务；`xmax` 是删除/更新版本的事务。0 表示还没被删除。判断可见性时两者都要看。

**4. 为什么删除不能直接物理删除？**

老快照可能还需要看到删除前的版本；回滚也需要恢复。所以先做 tombstone/delete-mark，等没有快照需要时再 purge。

**5. 长事务为什么会导致表膨胀/undo 膨胀？**

长事务的快照还需要旧版本，purge/VACUUM 不能清理这些版本；旧版本越积越多，表和 undo 表空间膨胀。

**6. PostgreSQL 和 InnoDB 的旧版本分别放在哪？**

PostgreSQL 旧 tuple 留在堆表里，由 VACUUM 清理；InnoDB 旧版本在 undo log 里，由 purge 线程清理。

**7. 快照隔离能防住写偏斜吗？**

不能。写偏斜是两个事务读同一快照、各写不同行，快照隔离允许这种交错；需要可串行化/谓词锁/显式约束。

**8. 为什么 `SELECT ... FOR UPDATE` 不走普通快照读？**

它是当前读，要读最新版本并加锁，防止其他事务修改；普通快照读才走 MVCC 旧版本。

</details>

---

## 14. 模式卡片

```text
模式名称：快照隔离 + 版本链
解决什么问题：读写并发、事务一致快照。
核心机制：
  1. 追加版本，不覆盖；
  2. xmin/xmax（或 trx_id/roll_ptr）；
  3. snapshot.xmax + active；
  4. 可见性判断；
  5. purge/VACUUM。
代价/限制：
  - 空间膨胀；
  - 长事务卡清理；
  - 写偏斜；
  - 实现/调优复杂。
不适用场景：
  - 无并发；
  - 强串行化且冲突极高；
  - 无法承受旧版本存储。
一个最小例子：本课的 MVCCStore。
```

---

## 15. 下次预习

下一次进入 **ARIES：崩溃恢复的教科书**。带着三个问题读：

1. `pageLSN`、`recLSN`、`flushedLSN` 分别在恢复的哪一步用？
2. 为什么 redo 必须幂等？CLR 解决什么？
3. steal/no-force 和 checkpoint 如何影响恢复时间？

阅读入口：ARIES 论文；InnoDB `log0log.cc`、`log0recv.cc`、`trx0rec.cc`；PostgreSQL `xlogrecovery.c`。

---

## 参考资料

- PostgreSQL, [`heapam_visibility.c`](https://github.com/postgres/postgres/blob/master/src/backend/access/heap/heapam_visibility.c)：元组可见性。
- PostgreSQL, [`procarray.c`](https://github.com/postgres/postgres/blob/master/src/backend/storage/ipc/procarray.c)：快照/活跃事务。
- MySQL, [`read0read.cc`](https://github.com/mysql/mysql-server/blob/trunk/storage/innobase/read/read0read.cc)：read view。
- MySQL, [`row0vers.cc`](https://github.com/mysql/mysql-server/blob/trunk/storage/innobase/row/row0vers.cc)：undo 版本链。
- MySQL, [`trx0purge.cc`](https://github.com/mysql/mysql-server/blob/trunk/storage/innobase/trx/trx0purge.cc)：purge。
- PostgreSQL Wiki, [MVCC](https://wiki.postgresql.org/wiki/MVCC)。
- MySQL Reference Manual, [InnoDB Multi-Versioning](https://dev.mysql.com/doc/refman/8.0/en/innodb-multi-versioning.html)。
