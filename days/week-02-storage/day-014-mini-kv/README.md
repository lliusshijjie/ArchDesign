---
day: "14"
title: "周复盘：Mini KV Engine"
tool: "W2 存储引擎复盘"
source:
  - "本课 code/mini_kv.hpp"
  - "Day 08–13 的 WAL / LSM / Buffer Pool / B+ 树 / MVCC / ARIES"
patterns:
  - "组件边界"
  - "WAL + MemTable + SST"
  - "恢复与截断"
  - "compaction"
  - "故障注入"
prerequisites:
  - "Day 08–13"
estimated_minutes: 120
---

# Day 14 · 周复盘：Mini KV Engine

> **一句话结论**：这一周我们学了六个零件，今天不学新零件，而是把它们装成一台能跑、能坏、能恢复的小机器；重点不是代码量，而是**每个组件的责任边界和不变量**。

---

## 0. 本周回顾

W2 的六个核心单元：

| Day | 主题 | 一句话 | 在 Mini KV 里的位置 |
|---|---|---|---|
| 08 | LSM-tree | 追加写 + 不可变 SST + compaction | MemTable / SST / compaction |
| 09 | WAL / group commit | 先写日志，再改数据；批量 fsync | WAL |
| 10 | Buffer Pool | 页缓存、pin、脏页、淘汰 | 本复盘未集成（见扩展） |
| 11 | B+ 树页面 | slotted page、slot 目录、分裂 | 本复盘未集成（见扩展） |
| 12 | MVCC | 多版本 + 快照可见性 | 本复盘未集成（见扩展） |
| 13 | ARIES | redo/undo/checkpoint | 本复盘只用到 WAL 恢复思想 |

今天的目标：

```text
把 WAL + MemTable + SST + compaction 组成一个最小 KV，
然后用“崩溃/部分写/校验和/重开”验证它的不变量。
```

---

## 0.5 先建立直觉

### 一个仓库的流水线

```text
收件（put）
  -> 先在登记簿写一笔（WAL）
  -> 把包裹放到分拣台（MemTable）
  -> 分拣台满了，封成一箱档案（SST）
  -> 箱子太多，定期合并（compaction）
```

- **WAL**：入库登记簿。先登记，再动货；断电后照登记簿补。
- **MemTable**：分拣台。速度快、容量小、易失。
- **SST**：封好的档案箱。一旦封箱就不再改，只能新写一箱。
- **compaction**：把多个小箱子合成大箱子，顺便丢掉作废标签。
- **tombstone**：一张“此货已作废”的标签，不能立刻扔掉，否则老箱子里的货会“复活”。

**今天只需要记住：**

1. 先登记，再动货。
2. 箱子封好就不改。
3. 作废标签要等合并时才能丢。
4. 崩溃后从登记簿补，补完再继续。

---

## 1. 一条写入的完整路径

```text
put("k", "v")
  -> WAL append（带校验和）
  -> MemTable["k"] = "v"
  -> MemTable 达到阈值？
       -> 写 SST
       -> 清空 MemTable
       -> 重置 WAL
       -> SST 数量达到阈值？
            -> compaction
```

每一步的责任：

| 步骤 | 负责组件 | 不变量 |
|---|---|---|
| 追加 WAL | `Wal` | 返回前记录已 flush 到文件 |
| 写 MemTable | `MemTable` | 写入顺序与 WAL 一致 |
| 写 SST | `SSTable` | 文件内按 key 排序 |
| 重置 WAL | `Wal` | 只能在 SST 写成功后 |
| compaction | `MiniKV` | 保留每个 key 最新版本 |

### 最容易错的地方

- 先写 MemTable 再写 WAL -> 崩溃丢数据；
- SST 没写完就重置 WAL -> 崩溃丢数据；
- compaction 先删旧 SST 再写新 SST -> 崩溃丢数据；
- tombstone 过早删除 -> 旧数据复活；
- 恢复时把损坏的尾记录当成有效记录 -> 读到垃圾。

---

## 2. 一条读取路径

```text
get("k")
  -> MemTable 有？返回（包括 tombstone）
  -> 从新到旧查 SST
       -> 二分找到 key
       -> 第一个命中决定结果
  -> 都没有 -> 不存在
```

为什么从新到旧？

- 新 SST 的版本更新；
- 第一个命中的版本就是最新可见版本；
- tombstone 也是“命中”，表示这个 key 被删除。

---

## 3. 崩溃恢复路径

```text
重启
  -> 加载所有 SST
  -> 读 WAL
       -> 逐条校验
       -> 校验失败/不完整 -> 截断到上一条有效记录
       -> 重放到 MemTable
  -> 打开 WAL 继续追加
```

三个关键动作：

1. **校验和**：发现尾部损坏；
2. **截断**：把坏尾切掉，避免后续追加继续污染；
3. **重放**：把有效记录恢复到 MemTable。

### 为什么“截断”很重要？

假设 WAL 末尾有 5 个字节的垃圾：

```text
[有效记录 1][有效记录 2][垃圾 5B]
```

如果只忽略垃圾、不截断，下一次 append 会写在垃圾后面：

```text
[有效记录 1][有效记录 2][垃圾 5B][新记录]
```

下次恢复时，垃圾会被当成新记录的头，继续解析出错误长度，后面的新记录也可能读不出来。

所以恢复时要：

```cpp
if (valid_bytes < file_size) {
    fs::resize_file(path_, valid_bytes);
}
```

---

## 4. 代码结构

完整代码在 [`code/`](code/)：

- [`mini_kv.hpp`](code/mini_kv.hpp)：WAL + MemTable + SST + compaction；
- [`main.cpp`](code/main.cpp)：6 个集成测试；
- [`CMakeLists.txt`](code/CMakeLists.txt)：构建配置。

### 4.1 组件

```text
MiniKV
├── Wal          // append / recover / reset
├── MemTable     // std::map<string, optional<string>>
├── SSTable      // vector<KVEntry> + binary file
└── compaction   // full merge
```

`MiniKV` 自己不实现 WAL/SST 的字节格式，只负责组合和顺序。这是“组件边界”的第一步。

### 4.2 WAL 记录

```text
key_size(4) | value_size(4) | tombstone(1) | checksum(4) | key | value
```

- 校验和用 FNV-1a；
- 记录头固定 13 B；
- 恢复时逐条读，失败就停；
- 有效字节数用于截断。

### 4.3 WAL 恢复

```cpp
const std::uint32_t actual = fnv1a(tombstone != 0, key, value);
if (actual != checksum) {
    break;  // 尾部损坏
}

memtable[key] = tombstone ? std::nullopt
                          : std::optional<std::string>(std::move(value));
++report.records;
valid_bytes += 13 + key_size + value_size;
```

最后：

```cpp
if (valid_bytes < file_size) {
    report.truncated_bytes = file_size - valid_bytes;
    fs::resize_file(path_, valid_bytes);
}
```

### 4.4 flush

```cpp
void flush() {
    if (memtable_.empty()) return;

    auto sst = write_sst(memtable_);
    sstables_.push_back(std::move(sst));
    memtable_.clear();
    wal_.reset();
    ++stats_.flushes;

    if (sstables_.size() >= options_.compaction_trigger) {
        compact();
    }
}
```

顺序：**写 SST -> 清 MemTable -> 重置 WAL**。

生产还要：fsync SST、写 Manifest、原子切换，再删旧 WAL。

### 4.5 compaction

```cpp
MemTable merged;
for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
    for (const auto& entry : it->entries) {
        if (merged.find(entry.key) != merged.end()) continue;
        merged.emplace(entry.key,
                       entry.tombstone ? std::nullopt
                                       : std::optional(entry.value));
    }
}
```

- 从新到旧，保留第一次出现的版本；
- 全量合并，所以可以丢 tombstone；
- 写新 SST，再删旧 SST。

### 4.6 close 与析构

```cpp
~MiniKV() {
    wal_.close();  // 不 flush，模拟进程退出
}

void close() {
    flush();
    wal_.close();
    closed_ = true;
}
```

- 显式 `close()`：flush + 重置 WAL；
- 析构：只关 WAL，靠 WAL 恢复；
- 这是为了让测试能模拟“进程崩溃”。

---

## 5. 测试与故障注入

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
# 或
./build/mini_kv_demo
```

Windows（PowerShell）：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++
cmake --build build
.\build\mini_kv_demo.exe
```

实测输出：

```text
[  OK  ] basic_and_reopen
[  OK  ] wal_recovery_without_flush
[  OK  ] partial_tail_is_truncated
[  OK  ] checksum_corruption_truncates_tail
[  OK  ] flush_and_compaction
[  OK  ] tombstone_persistence

All 6 tests passed.
```

> 验证环境：Windows 11 + MinGW-w64 g++ 15.2 + CMake 4.2 + Ninja；`-std=c++20 -Wall -Wextra -Wpedantic -Werror` 编译通过，重复运行 10 次无失败。

### 故障注入矩阵

| 注入点 | 期望行为 |
|---|---|
| WAL 尾部部分写 | 截断坏尾，保留前缀 |
| WAL 校验和错误 | 截断到上一条有效记录 |
| flush 前崩溃 | WAL 重放恢复 |
| flush 后、重置 WAL 前崩溃 | WAL 重放重复记录，幂等覆盖 |
| compaction 中崩溃 | 需要 Manifest/原子切换；本实现没有 |
| 删除后崩溃 | tombstone 持久化，旧值不复活 |

---

## 6. 本复盘没有集成的部分

### 6.1 Buffer Pool

本课的 SST 直接整个读入内存，没有 page cache、pin、LRU、脏页。

如果要集成：

```text
SST block -> BufferPool page -> pin -> 读/写 -> unpin
```

### 6.2 B+ 树页面

本课的索引是 `std::map` + 排序 vector；没有页内 slot、分裂、latch。

如果要集成：

```text
B+ 树页 = 一个小型 slotted page
```

### 6.3 MVCC

本课只有“最新版本”，没有快照读取。

如果要集成：

```text
每个 key 的版本链 + 事务快照 + 可见性 + purge
```

### 6.4 ARIES

本课只有“WAL + 全量重放”，没有：

- pageLSN / recLSN；
- redo/undo 阶段；
- CLR；
- checkpoint；
- 事务回滚。

这也是为什么 Day 13 的 ARIES 是“单课”，不是 Mini KV 的一部分：它是一套恢复协议，不是某个单独组件。

---

## 7. 组件可替换的“缝”

一个可维护的存储引擎，应该允许替换：

| 组件 | 当前实现 | 可替换为 |
|---|---|---|
| Log | 单文件 WAL + 校验和 | group commit WAL / 多 segment |
| MemTable | `std::map` | skiplist / arena / 并发跳表 |
| SST | 全量排序 vector | block + index + Bloom + 压缩 |
| Compaction | 全量合并 | leveled / tiered / universal |
| Manifest | 目录扫描 | VersionSet / Manifest |
| Cache | 无 | Buffer Pool |
| 事务 | 无 | MVCC + undo |

**判断一个组件是否“可替换”：**

- 它的输入/输出是否清晰？
- 它的失败是否会污染其他组件？
- 它是否依赖具体文件格式？
- 它是否有独立测试？

---

## 8. 本周自评清单

能用自己的话回答，算过关：

- [ ] 为什么 LSM 先写 WAL，再写 MemTable？
- [ ] 为什么 SST 一旦写出就不可变？
- [ ] tombstone 什么时候能删？
- [ ] Buffer Pool 的 pin 和 latch 有什么区别？
- [ ] B+ 树页为什么要 slot 目录？
- [ ] MVCC 的快照为什么不复制数据？
- [ ] ARIES 的 redo 和 undo 分别解决什么？
- [ ] 崩溃恢复为什么需要 checkpoint？
- [ ] 全量 compaction 和分层 compaction 的取舍？
- [ ] 你的 Mini KV 在哪个崩溃点会丢数据？

如果有一项答不上来，回到对应课文，不要继续加功能。

---

## 9. 练习

**基础**

1. 给 `MiniKV` 加 `scan(start, end)`，合并 MemTable + SST。
2. 给 SST 加每条记录的校验和。
3. 统计每次 compaction 的输入/输出字节，计算写放大。

**进阶**

1. 把 `compaction` 抽成 `CompactionPolicy` 接口，实现 full merge 和 size-tiered 两种。
2. 加一个 `Manifest`：记录 SST 列表和版本，启动时不再扫描目录。
3. 注入“写 SST 后、重置 WAL 前”崩溃，验证重复重放的幂等性。

**挑战**

1. 把 Day 10 的 Buffer Pool 接进来，让 SST 按 block 读取。
2. 把 Day 12 的 MVCC 接进来，支持快照读。
3. 用 Day 13 的 ARIES 思路，给 Mini KV 加 redo/undo 和 checkpoint。

---

## 10. 自测题

<details>
<summary>点击展开答案</summary>

**1. 为什么先写 WAL 再写 MemTable？**

MemTable 易失；WAL 持久化后才能保证崩溃恢复。顺序反了，崩溃时内存里的修改没有日志可重放。

**2. 为什么 SST 不可变？**

不可变让读无锁、缓存/校验/压缩简单；更新写新文件，旧文件由 compaction 替换。代价是空间放大和 compaction 重写。

**3. tombstone 什么时候能删？**

确认没有更老版本可能包含该 key、且没有快照需要它时。全量 compaction 可以删；分层 compaction 要确保 tombstone 已下沉到最老层。

**4. 为什么恢复时要截断 WAL 尾部？**

不截断的话，下一次 append 会写在坏尾后面；下次恢复会把垃圾当成记录头，导致后续有效记录也读不出来。

**5. 全量 compaction 和分层 compaction 的取舍？**

全量简单，但每次重写全部数据，写放大高；分层增量合并，写放大可控，但实现复杂，需要 Manifest/Version。

**6. 这个 Mini KV 在哪些点还可能丢数据？**

SST 写后没有 fsync、WAL reset 没有原子 rename、compaction 没有 Manifest/原子切换、没有 checkpoint；崩溃窗口仍可能丢数据。

**7. 为什么说“组件可替换”很重要？**

存储引擎的瓶颈会变化：写放大、读放大、恢复时间、并发。把 Log/MemTable/SST/Compaction 的边界划清，才能单独替换和验证。

</details>

---

## 11. 下一步：工作坊 A

W2 的核心单元到此结束。下一次不是 Day 15，而是 **工作坊 A：存储引擎压测与崩溃恢复**。

目标：

```text
给 Mini KV 注入断电/部分写/compaction 中断，
测恢复时间、数据丢失量、写放大，
然后判断：当前设计在哪个故障点不成立？
```

工作坊 A 之后，才进入 **W3 · Redis 专题**。

进入 W3 前先读 [Week 03 入门：Redis 专题](../../week-03-redis/00-primer.md)。

---

## 参考资料

- 本课代码：[`mini_kv.hpp`](code/mini_kv.hpp)
- Day 08：[LSM-tree](../day-008-lsm-tree/README.md)
- Day 09：[WAL 与 group commit](../day-009-wal-group-commit/README.md)
- Day 10：[Buffer Pool](../day-010-buffer-pool/README.md)
- Day 11：[B+ 树页面](../day-011-bplus-tree-page/README.md)
- Day 12：[MVCC](../day-012-mvcc/README.md)
- Day 13：[ARIES](../day-013-aries/README.md)
- 工作坊：[阶段工作坊 A–D](../../../workshops/README.md)
