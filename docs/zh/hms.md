# HMS 存储核心（History · Memory · Skill）

HMS（History–Memory–Skill）是 **L2 AI Infrastructure Layer** 中的 **HMS 柱**（与 LLM、LC 并列），为 **L3 Agent** 提供**可检索的对话轨迹**、**向量语义记忆**与**可执行技能**。`ideas/` 架构图中将 HMS 侧多级缓存记为「L1/L2/L3 缓存」——本文称为 **C1/C2/C3 语义缓存 tier**，以免与 **OS 的 L1 微内核**混淆；另含 **WAL/B⁺ 树/持久化 HNSW** 等路线图。下文说明**数据结构、核心算法、缓存 tier 行为**与代码落点。

---

## 1. 总览与编排

`hms_core.h` 描述编排关系：

- **Memory**：检索路径设计为 **C1 → C2 → C3 → HNSW（外存类比）**（`hms_memory_retrieve_tiered`；与 `hms_cache` 三档一致）。  
- **History**：以 **B⁺ 树** 按 `seq` 索引环形缓冲中的条目。  
- **Skill**：按名解析、可热加载 ELF。  
- **伪嵌入**：`hms_text_to_pseudo_embed` 生成紧凑向量，用于无完整嵌入管线时的占位检索；`hms_append_compressed_block` 生成带 `[M:…]` / `[H:…]` / `@skill#id` 的**压缩引用块**以节省上下文 token。

---

## 2. History（对话历史）

### 2.1 结构

- **环形缓冲** `hist_entry_t ring[HIST_RING_SIZE]`：`head` / `tail` 维护逻辑队列。  
- 每条含 `seq`、时间戳、`role`（user/assistant/system/tool）、**LZ4 压缩**后的 `data` 与 `raw_len` 等（见 `history.h`）。  
- **B⁺ 树** `hist_bptree_t`：`hist_bpt_insert(seq, ring_idx)` 建立 **seq → 环上下标**；`hist_bpt_range_from_seq` 支持从某 `seq` 起批量拉取索引。

### 2.2 算法要点

| 操作 | 行为 |
|------|------|
| **push** | 写入环槽、分配新 `seq`、插入 B⁺、维护 `window_tokens` 与 `max_tokens` |
| **按序检索** | B⁺ 上界/范围 → 映射到环上连续或分段读取 |
| **关键字搜索** | 当前实现可扫描解压（规模受限时可用） |
| **truncate_keep** | 保留最旧 `keep_count` 条、丢弃较新——服务 **L3 Self-Evolution** 中 **Corrector** 的差分回滚 |
| **序列化** | 用于 checkpoint / 迁移 |

### 2.3 持久化与 WAL

`history_wal.c` 等为 **write-ahead log** 脚手架：`ideas` 目标为崩溃后仍可恢复到一致 **seq**；与 B⁺ 联合可做「叶节点存 EventLog 偏移」式二级索引（路线图）。

---

## 3. Memory（向量记忆 + HNSW）

### 3.1 条目与图索引

- **`mem_entry_t`**：`int8_t embedding[EMBED_DIM]`（量化向量）、`importance`、`last_access`、`access_count`、可变长 `content`。  
- **`hnsw_graph_t`**：`hnsw_node_t` 数组，每层最多 `HNSW_M` / 底层 `HNSW_M0` 个邻居；`entry_point`、`max_level` 维护全局入口。

### 3.2 HNSW 算法（概要）

**插入**（`hnsw_insert`）：

1. 随机层数 \(l \sim \text{exponential}\)（由 `level_mult` 控制期望）。  
2. 从顶层入口贪心下降到新层，找**最近邻集合**作为候选邻居。  
3. 在每一层连接至多 `M` 条边，必要时对邻居列表**裁剪**以保持常数度。  

**搜索**（`hnsw_search`）：

1. 从 `entry_point` 在高层贪心向查询向量靠近（余弦相似度在 `int8` 上实现）。  
2. 逐层下降，在底层用更大的 `ef`（`HNSW_EF_SEARCH`）扩展候选集，取 top-k。

**性质**：近似最近邻；`ideas` 中「召回精度下界形式证明」属研究目标。

### 3.3 遗忘（forget）

`memory_forget` 一类策略可结合 **importance** 与 **访问频率** 淘汰低价值节点，避免图无限膨胀；与 **hms_cache** 驱逐协同可避免「图在内存、内容已丢」不一致（需实现上保持一致性协议）。

### 3.4 Working set

`hashmap_t *working_mem`：`memory_working_set` / `memory_working_get` 提供会话内键值，不必入 HNSW。

---

## 4. Skill（技能）

- **注册表**与 **`skill_elf`**：预留 **ELF 热加载**、按名/ID 解析；与 **LC 沙箱** 联调后才能在受控条件下执行（见 [container.md](container.md)）。  
- `ideas`：**成功率 < 60% 触发重生**、**Hoare 逻辑验证** 等属 **L3 Evolver/Verifier** 与工具链的长期目标。

---

## 5. 语义缓存 `hms_cache`（tier C1 / C2 / C3）

### 5.1 目的

把「热 KV、温摘要、冷持久化」分层，降低 HMS 组装上下文时的延迟与内存压力（`ideas` 架构图称「L1/L2/L3 缓存」，此处记为 **C1/C2/C3**）。

### 5.2 实现要点（`hms_cache.c`）

- **C1（代码中 l1_*）**：`l1_evict` 实现 **CLOCK** 近似——环形 `hand` 扫描，`ref` 位为 0 则驱逐，否则清 0 再给一次机会。  
- **C2 / C3（代码中 l2/l3 槽位）**：`lru_evict` 选 **`lru_tag` 最小**的条目作为受害者（LRU 近似）。  
- 每层是独立的 `hms_kv_t` 表，条目含 `key`、`blob`、`ref`、`lru_tag`。

### 5.3 查找与回填（概念算法）

1. 查 **C1** 命中则返回并置 `ref=1`。  
2. 未命中则查 **C2 → C3**；命中则**晋升**到更热 tier（可选实现）。  
3. 全未命中则从 **Memory HNSW / History** 拉取，填入 **C3**→…  

当前代码以 KV 层为主，与 HNSW 的**自动回填**可在后续把 `hms_memory_retrieve_tiered` 与 cache put 串起来。

---

## 6. 与 EventLog / L3 Self-Evolution 的关系

- 重要 HMS 操作可伴随 **EventLog**（L3 左列）记录，供 **Corrector / Evolver** 使用。  
- **Verifier**（L3 右列）可规范「Skill 未验证不得标记为 active」等跨层性质。

---

## 7. 文件索引（便于跳转源码）

| 主题 | 路径 |
|------|------|
| History 环 + B⁺ | `agent/history.c`, `agent/hist_bptree.c` |
| WAL 脚手架 | `agent/history_wal.c` |
| Memory / HNSW | `agent/memory.c`, `agent/memory.h` |
| Skill / ELF | `agent/skill.c`, `agent/skill_elf.c` |
| 缓存 | `agent/hms_cache.c` |
| 编排 API | `agent/hms_core.c` |

---

## 延伸阅读

- [总体架构](architecture.md) · [L3 Self-Evolution](evolution.md) · [LC（L2 柱）](container.md)  
- English: [hms.md](../en/hms.md)
