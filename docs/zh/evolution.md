# L3 Agent Layer — Self-Evolution 子平面（设计透彻说明）

在官方分层（`ideas/ideas2.md`）中，**Self-Evolution Plane** 与 **Agent Runtime** 同属 **L3 Agent Layer**（并列两列）：Self-Evolution 在 **L4 Shell** 之下、**L2 AI Infrastructure** 之上，负责对「计划—执行—纠错—验证—策略更新」闭环提供**算法与（目标中的）形式化支撑**。当前仓库中 `src/evolution/` 仅有 **初始化桩**；下文写清**目标算法与数据结构**，并说明与 **EventLog / History / L2 LC** 的衔接。

---

## 1. 模块划分与职责

| 模块 | 职责（目标） | 典型输入/输出 |
|------|----------------|---------------|
| **Planner** | 把高层目标分解为可执行子任务树 | 用户意图、HMS 摘要 → 子目标序列或搜索树 |
| **Corrector** | 在约束被违反或执行失败时做**差分回滚**与修复 | EventLog 片段、失败类型 → 回滚点 + 补丁动作 |
| **Verifier** | 对计划或系统迁移做 **LTL/CTL** 等性质的**增量模型检验** | 状态迁移图（或日志归纳的抽象模型）、规约 → 通过/反例轨迹 |
| **Evolver** | 在验证反馈下更新策略（如 **RL**），并复用**差分证明** | 奖励/反例、策略参数 → 新策略 +（目标）证明增量 |

统一入口：`evolution_plane_init()`（当前仅调用各子模块的 `*_init` 并打日志）。

---

## 2. Planner：从 ReAct 到 ToT

### 2.1 ReAct（推理—行动交错）

**思想**：每一步交替进行 **Thought（推理）** 与 **Action（工具调用）**，直到得到最终答案。

**算法骨架**（概念）：

1. 初始化上下文 \(c_0\)（含 HMS 压缩块、系统提示）。
2. 循环直到终止条件：  
   - 调用 LLM 生成「思考 + 计划动作」；  
   - 若动作为工具调用，经 **L2**（LLM/工具链）与 **L3 左列**执行路径配合，结果写 **EventLog** 并拼回 \(c_{i+1}\)；  
   - 若为最终回复，跳出。

**与 AevOS 的落点**：`agent_process_input` 主路径已具备「LLM → 工具」循环的雏形；Planner 可把「一步贪心」扩展为「显式目标栈」与「可重试策略模板」。

### 2.2 ToT（Tree of Thought）

**思想**：在每一步维护**多候选思路**（树节点），用价值估计或 LLM 打分做 **扩展 / 剪枝 / 回溯**。

**算法骨架**：

1. 根节点 = 问题陈述。  
2. **扩展**：对每个叶节点，生成 \(k\) 个子思路（宽度限制 \(B\)）。  
3. **评估**：对每个节点打分（启发式或小型 LLM）。  
4. **剪枝**：保留 top-\(m\) 节点；深度或 token 预算耗尽则停止。  
5. **选择**：从最优叶回溯得到行动序列。

**复杂度**：时间近似 \(O(B^d)\) 乘每次 LLM 调用；工程上需与 **Cancel**、**Mailbox** 协作以便中途终止。

---

## 3. Corrector：差分回滚与约束检测

### 3.1 差分回滚（differential rollback）

**动机**：全量重放历史成本高；只希望撤销「最近一段无效迁移」。

**与 History 的衔接**：`history_truncate_keep(history_t *h, uint32_t keep_count)` 保留最旧的 `keep_count` 条逻辑条目、丢弃较新部分——语义上适合「回滚到校验点之前」。

**算法步骤（概念）**：

1. Verifier 或运行时检测到违反（例如工具返回错误码、IFC 拒绝）。  
2. 在 **EventLog** 上定位**最后一个已知良好 seq**（或 Planner 提交的 checkpoint）。  
3. 调用 History 截断 +（若存在）撤销对 Memory/Skill 状态的外部副作用（需每个 Skill 登记 **compensating action**，尚未全量实现）。  
4. 将 **EVLOG_CORRECT_ROLLBACK** 写入 EventLog，供 Evolver 学习。

### 3.2 约束检测

约束可分类为：

- **静态**：syscall 白名单、内存区域、LC 策略（与 [container.md](container.md) 一致）。  
- **动态**：LTL 式「永远不在未验证状态下加载 ELF」等，由 Verifier 检查。

---

## 4. Verifier：增量 LTL/CTL 检验（目标）

### 4.1 模型从哪里来

- **显式**：有限状态迁移系统（结点数可控）。  
- **隐式**：从 **EventLog** 与 **Agent 状态机** 抽象出有限抽象模型（CEGAR 类循环：反例 → 精化抽象）。

### 4.2 增量性（与 `ideas` 中「差分证明复用」一致）

当系统从状态 \(S\) 经小补丁到 \(S'\)：

- **全量**：对 \(S'\) 从头检验性质 \(\phi\)。  
- **增量**：若 \(\phi\) 在 \(S\) 上已证，且变更集 \(\Delta\) 仅影响局部迁移，则尝试**重用**证明片段，只重算受影响子图上的子目标。

**算法层面**常见技巧：增量 **IC3/PDR**、增量 **k-induction**、或基于 **模拟关系** 的保形扩展。具体选择属实现阶段决策。

### 4.3 L2 LC 柱作为归约锚点

若证明 **Skill/容器不可穿越 L2 的 LC 柱**，则 **L3 Verifier** 可将大量性质归约到 **LC 接口** 与 **L1 不变量** 的组合，而不必对每个 Skill 体做全程序验证。

---

## 5. Evolver：RL + Proof Reuse

### 5.1 RL 侧（概念）

- **状态**：HMS 摘要、最近 EventLog 窗口、资源占用。  
- **动作**：Planner 超参（温度、ToT 宽度）、工具选择偏置、是否请求远程 LLM。  
- **奖励**：任务成功 − 延迟 − 违规惩罚（Verifier 失败为大负奖励）。

### 5.2 Proof Reuse

当策略更新仅改变「选哪个工具」而不改变 **LC 允许的能力集**，理想情况下**安全证明**不变，仅性能相关部分重分析——这与 Verifier 的增量检验同构。

---

## 6. 与 L3 左列（Agent Runtime）的观测接口

- **EventLog**：`EVLOG_PLAN_STEP`、`EVLOG_CORRECT_ROLLBACK`、`EVLOG_VERIFY`、`EVLOG_EVOLVER` 等类型已预留，便于将来把 **Self-Evolution 子平面**的决策与结果**结构化写入**。  
- **agent_t.evolution**：`evolution_state_t` 可承载 per-agent 进化状态机（与具体字段以源码为准）。

---

## 7. 当前实现状态

| 组件 | 状态 |
|------|------|
| `planner.c` / `corrector.c` / `verifier.c` / `evolver.c` | 初始化日志桩 |
| 与 `llm_sys_*` / HMS / LC 的闭环 | 未接线 |

实现时可优先：**EventLog 订阅 + Corrector 与 `history_truncate_keep` 联调**，再接入轻量 Verifier（例如仅检查状态机枚举性质），最后扩展增量证明与 RL。

---

## 延伸阅读

- [总体架构](architecture.md) · [HMS](hms.md) · [LC](container.md)  
- English: [evolution.md](../en/evolution.md)
