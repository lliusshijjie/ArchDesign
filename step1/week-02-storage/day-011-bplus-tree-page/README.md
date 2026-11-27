---
day: "11"
title: "B+ 树页面：索引的物理结构"
tool: "InnoDB index page / slotted page"
source:
  - "mysql-server/storage/innobase/page/page0page.cc"
  - "mysql-server/storage/innobase/page/page0cur.cc"
  - "mysql-server/storage/innobase/rem/rem0rec.cc"
  - "mysql-server/storage/innobase/btr/btr0btr.cc"
patterns:
  - "slotted page"
  - "page directory"
  - "infimum/supremum"
  - "free space/compaction"
  - "page split"
  - "latch coupling"
prerequisites:
  - "Day 10：Buffer Pool"
  - "B+ 树基础"
  - "二进制布局/字节序基础"
estimated_minutes: 120
---

# Day 11 · B+ 树页面：索引的物理结构

> **一句话结论**：B+ 树查询快，不只因为“树矮”，还因为每个页内部是**紧凑、有序、可二分、可局部修改**的物理结构。页不是 `std::map`，而是一块要自己管理空闲空间、记录头、slot 目录和分裂的字节数组。

---

## 0. 本次问题

1. 一个 16 KiB 的 InnoDB page 里到底放了什么？
2. 变长记录怎么放？为什么需要 slot 目录？
3. Infimum/Supremum 是什么？为什么每页都有？
4. Page Directory 为什么是“稀疏”的？怎么二分？
5. 删除记录后，空间为什么不立刻回收？什么时候 compact？
6. 页分裂时，记录、slot、兄弟指针、父节点分别怎么变？
7. 为什么 B+ 树页需要 latch？latch coupling 解决什么？
8. 自己设计一个 slotted page，最容易在哪些边界出错？

---

## 1. 设计背景：页是磁盘和内存的交换单位

B+ 树的节点就是一个**页**。数据库以页为单位：

- 从磁盘读入 Buffer Pool；
- 在内存中查找/修改；
- 脏页写回；
- 崩溃后用 redo/undo 恢复。

页的设计目标：

| 目标 | 手段 |
|---|---|
| 高扇出、树矮 | 紧凑记录、尽量少元数据 |
| 点查快 | 页内有序 + 二分/目录 |
| 范围扫描快 | 记录按 key 顺序、叶子页链表 |
| 变长记录 | slot 目录 + 记录头 |
| 局部更新 | 空闲空间、删除标记、compact |
| 并发 | page latch、锁耦合 |
| 崩溃恢复 | LSN、checksum、redo/undo |

如果页只是一个 `std::vector<Record>`，插入中间记录要移动大量数据，删除会留下洞，磁盘布局也无法直接映射。数据库需要更底层的布局。

---

## 2. InnoDB 页的总体布局

一个 InnoDB 索引页（简化）：

```text
+----------------------------+ 0
| FIL Header                  |  page type, prev/next, LSN, checksum...
+----------------------------+
| Page Header                 |  n_recs, heap_top, free, garbage, level...
+----------------------------+
| Infimum                     |  伪记录，小于所有用户 key
| Supremum                    |  伪记录，大于所有用户 key
+----------------------------+
| User Records                |  按 key 顺序存放的记录
| ...                         |
+----------------------------+
| Free Space                  |
+----------------------------+
| Page Directory              |  稀疏 slot，指向记录
+----------------------------+
| FIL Trailer                 |  checksum/旧 LSN
+----------------------------+ 16 KiB
```

关键区域：

- **FIL Header/Trailer**：页号、页类型、前后页、LSN、checksum；
- **Page Header**：记录数、heap top、free list、garbage、页层级、索引 id、段信息；
- **Infimum/Supremum**：两条伪记录，作为页内链表的头尾哨兵；
- **User Records**：真正索引记录，按 key 顺序；
- **Free Space**：未使用连续空间；
- **Page Directory**：稀疏 slot 数组，指向记录，支持页内二分。

### 2.1 记录不是定长的

InnoDB 记录是变长的：

- 变长字段长度列表；
- NULL bitmap；
- 记录头（info bits、n_owned、heap_no、record type、next offset）；
- 聚簇索引记录含 `DB_TRX_ID`、`DB_ROLL_PTR`；
- 二级索引记录含主键列；
- 行格式（REDUNDANT/COMPACT/DYNAMIC/COMPRESSED）不同。

所以不能简单地 `records[i] = record`，需要：

```text
记录头 + 变长字段 + 定长字段 + 主键/事务字段
```

记录头里还有 `next_record` 偏移，把所有记录串成**页内单向链表**（按 key 顺序）。

### 2.2 Infimum/Supremum

每页都有两条伪记录：

```text
Infimum  < 所有用户 key < Supremum
```

作用：

- 页内链表永远有头有尾，插入/删除不需要特殊处理空链表；
- Page Directory 的边界哨兵；
- 记录遍历、锁、可见性判断都有统一边界。

这是“用哨兵消除边界分支”的经典技巧。

---

## 3. Slotted Page：变长记录的标准解法

> 本课代码实现的是 slotted page 的核心，不是 InnoDB 的完整页格式。

### 3.1 布局

```text
+----------------------------+ 0
| Header                     |
+----------------------------+ free_offset
| record 0 | record 1 | ...  |  （记录向高地址增长）
|          free space        |
+----------------------------+ slot area
| slot n-1 | ... | slot 1 | slot 0 |  （slot 从页尾向低地址增长）
+----------------------------+ 4096
```

- 记录从 header 后向高地址增长；
- slot 从页尾向低地址增长；
- 中间是连续 free space；
- slot 存 `(offset, length)`，不是记录本身；
- slot 按 key 排序，记录本身可以物理无序（本课简化；InnoDB 记录物理上按 key 顺序）。

### 3.2 为什么用 slot 目录？

如果记录直接按物理顺序存放：

- 中间插入要移动后面所有记录；
- 变长记录移动成本高；
- 删除后留下洞，遍历要跳过；
- 页分裂要重新计算所有偏移。

用 slot：

- 记录可以原地写；
- 排序只维护 slot 数组；
- 二分查 slot，再读记录；
- 记录移动（compact）时只更新 slot offset；
- 删除只删 slot，记录变成“洞”。

代价：

- 每个 slot 4 B 元数据；
- 记录物理无序时，范围扫描要按 slot 顺序跳读，cache 局部性差；
- compact 需要重写页。

### 3.3 页内二分

slot 按 key 排序，所以可以：

```text
lo=0, hi=n
while lo < hi:
    mid = (lo+hi)/2
    if key_at(mid) < target: lo=mid+1
    else: hi=mid
```

找到第一个 `>= target` 的 slot，再比较是否相等。

InnoDB 的 Page Directory 更复杂：

- 不是每个记录一个 slot，而是**稀疏**目录，每个 slot 管 4–8 条记录；
- slot 指向该组最后一条记录；
- 记录头的 `n_owned` 表示该组记录数；
- 二分先找 slot 组，再在组内沿 `next_record` 链表线性找。

为什么稀疏？

- 减少目录内存；
- 插入/删除时目录调整次数少；
- 组内记录数少，线性扫描可接受。

---

## 4. 插入、删除与空间回收

### 4.1 插入

```text
1. 按 key 找到 slot 插入位置
2. 在 free_offset 处写记录
3. 在 slot 数组中间插入新 slot
4. free_offset += record_size
5. free_space -= record_size + slot_size
```

关键边界：

- free space 是否足够（记录 + slot）；
- 记录区是否与 slot 区重叠；
- 重复 key（B+ 树叶子 key 唯一；真实二级索引可带主键）；
- slot 数组中间插入要移动 slot，不是移动记录；
- 页满时返回失败，由上层触发 split。

### 4.2 删除

```text
1. 找到 key 对应 slot
2. 删除 slot（后面的 slot 上移）
3. 记录数据变成洞
4. free_space += slot_size
5. fragmentation += record_size
```

为什么不在删除时立刻移动记录？

- 移动记录要更新所有 slot offset；
- 删除可能很频繁；
- 事务可能还需要旧版本（MVCC/undo）；
- 延迟回收可以摊薄成本。

代价：

- 页内碎片；
- 可用连续空间变小；
- 插入可能失败，即使总空闲足够。

### 4.3 compact

当碎片过多或插入失败时，重建页：

```text
读出所有有效记录 -> 重新按 key 写入新页 -> 替换原页
```

本课 `compact()` 就是这么做的。生产实现会更精细：

- 只移动洞后面的记录；
- 更新 slot；
- 保留记录头/LSN/事务信息；
- 与 latch/redo 协同。

### 4.4 页满与 fill factor

B+ 树页不会等到 100% 才分裂。常见策略：

- 预留空间，避免每次插入都 split；
- 顺序插入时按“右倾斜”分裂，减少左页空洞；
- 随机插入时 50/50 或按访问方向；
- 监控 fill factor，避免页太满/太稀。

InnoDB 有 `PAGE_DIRECTION`/`PAGE_N_DIRECTION` 等启发式，根据插入方向选择 split point；不是简单平均分。

---

## 5. 页分裂：B+ 树最危险的局部操作

当页满时：

```text
1. 分配新页
2. 选择 split point
3. 把一部分记录移到新页
4. 维护叶子页 next/prev 链表
5. 把 separator key + 新页号插入父节点
6. 父节点可能也满 -> 递归分裂
7. 根分裂 -> 树高 +1
```

示意：

```text
before:
  parent: [ ... 20 ... ]
  leaf:   [10 20 30 40 50 60]  (full)

after split:
  parent: [ ... 20 ... 45 ... ]
  leaf L: [10 20 30]
  leaf R: [40 50 60]  (separator 40 或 45，取决于策略)
```

关键点：

- **叶子页链表**：范围扫描依赖 `next/prev`，分裂必须维护；
- **父节点分隔键**：右页最小 key 通常作为 separator；
- **父节点更新**：需要 latch/coupling，避免并发读看到“记录消失”；
- **根分裂**：创建新根，树高增加；
- **崩溃恢复**：split 是结构化修改（SMO），需要 redo/undo 或 InnoDB 的 mini-transaction 保护。

本课 `split()` 只做叶子页左右分裂和 sibling 指针，不实现父节点/根/内部页；文章会明确这个边界。

---

## 6. 并发：page latch 与 latch coupling

B+ 树页会被多个线程并发访问。需要区分：

| 机制 | 保护什么 |
|---|---|
| page latch | 页内容（读/写） |
| index lock | 索引结构/SMO 的更高层协调 |
| transaction lock | 行/记录的逻辑锁 |
| pin | 页不能被 Buffer Pool 淘汰 |

常见优化是 **latch coupling/crabbing**：

```text
持有父页 latch -> 获取子页 latch -> 释放父页 latch
```

这样遍历时最多持有少量 latch，避免整棵树锁住。

页分裂时：

- 需要持有相关页的 latch；
- 更新父节点；
- 维护 sibling 指针；
- 不能让读者看到“记录暂时消失”；
- 可能需要重启/重试 B+ 树游标。

InnoDB 用 mini-transaction（mtr）把一组页修改和 redo 绑定，保证崩溃恢复；这是比“加一把大锁”复杂得多的工程。

---

## 7. C++20 最小实现

完整代码在 [`code/`](code/)：

- [`slotted_page.hpp`](code/slotted_page.hpp)：4 KiB slotted page；
- [`main.cpp`](code/main.cpp)：5 个行为测试；
- [`CMakeLists.txt`](code/CMakeLists.txt)：构建配置。

### 7.1 布局映射

| InnoDB | 本实现 | 说明 |
|---|---|---|
| 16 KiB page | 4 KiB | 教学缩小 |
| FIL/Page Header | `Header` | magic、页号、slot 数、free space、fragmentation、links |
| Infimum/Supremum | 未实现 | 文章讲概念 |
| 变长记录 | `key_len + value_len + key + value` | 简化记录 |
| 记录头/next_record | 未实现 | slot 数组代替页内链表 |
| Page Directory | 每个记录一个 slot | 稀疏目录简化为全目录 |
| 页分裂 | `split()` | 只做叶子层 |
| latch/mtr | 无 | 单线程 |

### 7.2 Header

```cpp
struct Header {
    std::uint32_t magic;
    std::uint16_t page_type;
    std::uint16_t level;
    std::uint16_t n_slots;
    std::uint16_t free_offset;
    std::uint16_t free_space;
    std::uint16_t fragmentation;
    std::uint64_t page_id;
    std::uint64_t next_page;
    std::uint64_t prev_page;
    std::uint64_t lsn;
};
```

字段含义：

- `free_offset`：下一条记录写在哪里；
- `free_space`：连续空闲字节；
- `fragmentation`：删除留下的洞；
- `next/prev`：叶子页链表；
- `lsn`：预留给 WAL/恢复（本实现未用）。

### 7.3 slot

```cpp
struct Slot {
    std::uint16_t offset;
    std::uint16_t length;
};
```

- 每个 slot 4 B；
- slot 数组从页尾向低地址增长；
- slot 按 key 排序；
- 插入/删除只移动 slot，不移动记录。

### 7.4 插入

```cpp
const std::size_t pos = lower_bound(key);
if (pos < size() && key_at(pos) == key) return false;

const std::uint16_t offset = header.free_offset;
write_record(offset, key, value);
insert_slot(pos, offset, record_size);

header.free_offset += record_size;
header.free_space -= record_size + sizeof(Slot);
```

注意：

- 先检查 free space；
- 记录区不能与 slot 区重叠；
- slot 中间插入要移动 slot；
- 记录可以物理无序，slot 有序。

### 7.5 删除与 compact

```cpp
const Slot slot = read_slot(pos);
erase_slot(pos);
header.n_slots--;
header.free_space += sizeof(Slot);
header.fragmentation += slot.length;
```

- 记录变成洞；
- slot 目录回收 4 B；
- 碎片累计；
- 需要时 `compact()` 重建页。

### 7.6 分裂

```cpp
auto all = entries();
const std::size_t mid = all.size() / 2;
const std::string separator = all[mid].first;

SlottedPage left(old_id, level);
left.set_next_page(new_id);
...
SlottedPage right(new_id, level);
right.set_prev_page(old_id);
right.set_next_page(old_next);
...
```

本实现是 50/50 分裂；InnoDB 会根据插入方向/访问模式选择 split point，并更新父节点。这里只维护叶子链表，父节点/内部页留给后续。

### 7.7 测试

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
# 或
./build/slotted_page_demo
```

Windows（PowerShell）：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++
cmake --build build
.\build\slotted_page_demo.exe
```

实测输出：

```text
[  OK  ] insert_find_scan
[  OK  ] delete_and_compact
[  OK  ] serialize_roundtrip
[  OK  ] split
[  OK  ] variable_length_records

All 5 tests passed.
```

> 验证环境：Windows 11 + MinGW-w64 g++ 15.2 + CMake 4.2 + Ninja；`-std=c++20 -Wall -Wextra -Wpedantic -Werror` 编译通过，重复运行 20 次无失败。

### 7.8 生产化缺口

- 没有 Infimum/Supremum、记录头、next_record 链表；
- 没有稀疏 Page Directory 和 `n_owned`；
- 没有变长字段长度列表/NULL bitmap/行格式；
- 没有聚簇索引/二级索引/主键字段；
- 没有 page latch、latch coupling、mtr；
- 没有父节点/内部页/根分裂；
- 没有崩溃恢复/checksum/压缩；
- 没有自适应 split point/fill factor；
- 没有 purge/MVCC 旧版本；
- 单线程，无并发测试。

---

## 8. 如何借鉴到自己的项目

### 8.1 模式卡片

```text
模式名称：Slotted Page + Sparse Directory
解决什么问题：在固定大小页内存储变长记录，支持有序查找、局部插入/删除和页分裂。
核心机制：
  1. header + 记录区 + slot 区；
  2. 记录从低地址增长，slot 从高地址增长；
  3. slot 按 key 排序，支持二分；
  4. 删除留洞，compact 回收；
  5. 页分裂 + sibling/父节点；
  6. latch coupling 保护并发。
代价/限制：
  - 元数据/碎片；
  - compact/split 复杂；
  - 并发/恢复协议；
  - 记录物理无序时局部性差。
不适用场景：
  - 定长小记录、简单数组；
  - 纯内存、无持久化；
  - 不需要范围查询的场景。
一个最小例子：本课的 SlottedPage。
```

### 8.2 接口清单

```text
[ ] 页大小/记录格式/字节序是否固定？
[ ] 变长记录如何编码长度/NULL？
[ ] slot 目录是否按 key 排序？二分是否正确？
[ ] free space 与 slot 区是否会重叠？
[ ] 删除后碎片如何统计/回收？
[ ] 页满时 split point 如何选？
[ ] sibling/父节点/根分裂是否原子？
[ ] 并发读者会不会看到中间状态？
[ ] 崩溃恢复是否有 redo/undo/checksum？
[ ] 是否有 fill factor/页利用率指标？
```

### 8.3 迁移步骤

1. **先定记录格式**：长度、NULL、版本、事务字段。
2. **先做单页**：插入/查找/删除/compact，跑边界测试。
3. **再做分裂**：sibling、separator、父节点。
4. **再做并发**：page latch、latch coupling、重试。
5. **最后做恢复**：mtr/redo/checksum。
6. **做模糊测试**：随机插入/删除/分裂/compact，校验不变量。

---

## 9. 练习

**基础**

1. 给 `SlottedPage` 增加 `scan(start, end)`，按 slot 顺序返回范围。
2. 实现稀疏 Page Directory：每 4 条记录一个 slot，记录 `n_owned`。
3. 给页加 checksum，检测字节损坏。

**进阶**

1. 实现 Infimum/Supremum 和记录 `next_record` 链表，比较遍历性能。
2. 实现自适应 split point：顺序插入时右倾斜，随机插入时 50/50。
3. 加 page latch（读写锁），实现 latch coupling 的查找。

**挑战**

1. 实现一个最小 B+ 树：叶子页 + 内部页 + 根分裂 + 范围扫描。
2. 加 mtr/redo：页修改与 WAL 绑定，模拟崩溃恢复。
3. 对比“每记录一个 slot”和“稀疏目录”在插入/查找/空间上的差异。

---

## 10. 自测题

<details>
<summary>点击展开答案</summary>

**1. 为什么需要 slotted page？**

记录是变长的，不能简单用固定数组下标。slot 目录保存每条记录的 offset/length，让记录可以原地写、排序只维护 slot、删除只删 slot，并支持页内二分。

**2. 为什么 slot 按 key 排序，而记录可以物理无序？**

排序只需要 slot 的顺序；记录可以留在原地，避免移动变长数据。查找时先二分 slot，再按 offset 读记录。代价是范围扫描的物理局部性较差，compact 可以改善。

**3. Infimum/Supremum 的作用？**

它们是页内伪记录，作为链表头尾哨兵，保证插入/删除/遍历/Page Directory 边界统一，减少空链表和边界分支。

**4. 删除记录后为什么不立即移动数据？**

移动记录要更新所有 slot offset，成本高；删除可能频繁；MVCC/undo 可能还需要旧版本。先留洞、累计 fragmentation，必要时 compact。

**5. Page Directory 为什么是稀疏的？**

每个 slot 管一组记录（4–8 条），减少目录内存和插入/删除时的目录调整；二分先找组，再组内沿 next_record 线性找。全目录更简单但元数据更多。

**6. 页分裂时哪些东西必须更新？**

新页分配、记录/slot 迁移、split point、叶子 sibling 的 next/prev、父节点 separator/子页指针；根分裂还要创建新根并增加树高。并发下需要 latch/mtr 保证原子性。

**7. latch coupling 解决什么？**

遍历 B+ 树时，如果一直持有所有祖先页 latch，会阻塞其他线程。latch coupling 在拿到子页 latch 后释放父页 latch，把锁持有范围限制在局部；分裂等 SMO 需要更复杂的协调/重试。

**8. 为什么页不能等到 100% 满才分裂？**

插入需要预留空间；太满会导致每次插入都 split、页利用率抖动；顺序插入还可能让左页几乎空。fill factor/split point 需要根据访问模式调整。

</details>

---

## 11. 模式卡片

```text
模式名称：变长记录的页内布局
解决什么问题：固定页内高效存储/查找/修改变长记录。
核心机制：
  1. header；
  2. 记录区向上、slot 区向下；
  3. slot 排序 + 二分；
  4. 删除留洞 + compact；
  5. 分裂 + sibling/父节点；
  6. latch/mtr。
代价/限制：
  - 碎片/compact；
  - 分裂/并发/恢复复杂；
  - 物理局部性权衡。
不适用场景：
  - 定长简单数组；
  - 纯内存无范围查询；
  - 不需要崩溃恢复。
一个最小例子：本课的 SlottedPage。
```

---

## 12. 下次预习

下一次进入 **MVCC：快照读与版本链**。带着三个问题读：

1. PostgreSQL 的 `xmin/xmax` 快照如何判断可见性？
2. InnoDB 的 `DB_TRX_ID/DB_ROLL_PTR` + undo + read view 如何构造旧版本？
3. 旧版本什么时候可以清理？长事务为什么会让 purge 落后？

阅读入口：PostgreSQL `heapam_visibility.c`、`procarray.c`；InnoDB `read0read.cc`、`trx0trx.cc`、`row0vers.cc`、`purge`。

---

## 参考资料

- MySQL, [`page0page.cc`](https://github.com/mysql/mysql-server/blob/trunk/storage/innobase/page/page0page.cc)：页头、记录、Page Directory。
- MySQL, [`page0cur.cc`](https://github.com/mysql/mysql-server/blob/trunk/storage/innobase/page/page0cur.cc)：页内游标/插入/删除。
- MySQL, [`rem0rec.cc`](https://github.com/mysql/mysql-server/blob/trunk/storage/innobase/rem/rem0rec.cc)：记录格式/变长字段。
- MySQL, [`btr0btr.cc`](https://github.com/mysql/mysql-server/blob/trunk/storage/innobase/btr/btr0btr.cc)：B+ 树页分裂/合并。
- MySQL Reference Manual, [InnoDB Page Structure](https://dev.mysql.com/doc/refman/8.0/en/innodb-page-structure.html)。
- C++ reference：[`std::string_view`](https://en.cppreference.com/w/cpp/string/basic_string_view)、[`std::memcpy`](https://en.cppreference.com/w/cpp/string/byte/memcpy)。
