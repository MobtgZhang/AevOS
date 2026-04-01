# AevOS-Evo 总体架构（L0–L4）

本文档采用与 `ideas/ideas2.md` **一致的层次编号**：**L0–L4** 共五层；其中 **L2** 为「AI 基础设施层」，内含 **LLM Runtime、LC 容器兼容层、HMS 存储核心** 三柱并列；**L3** 为「Agent 层」，内含 **Agent Runtime** 与 **Self-Evolution Plane** 两列。下文将 `ideas/` 中的设计与仓库源码对齐，并标明实现与路线图。

---

## 1. 分层总览（官方示意图）

```
┌─────────────────────────────────────────────────────────────────┐
│  L4  AevOS Shell  （帧缓冲 UI / CLI / WebSocket）               │
├─────────────────────────────────────────────────────────────────┤
│  L3  Agent Layer                                                │
│  ┌──────────────────────────┬────────────────────────────────┐  │
│  │  Agent Runtime           │  Self-Evolution Plane          │  │
│  │  EventLog（append-only） │  Planner（ReAct / ToT）        │  │
│  │  Mailbox MPMC            │  Corrector（差分回滚）          │  │
│  │  四态工具调度             │  Verifier（LTL/CTL 增量验证）  │  │
│  │  Cancel 广播              │  Evolver（RL + 差分证明复用）  │  │
│  └──────────────────────────┴────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────────┤
│  L2  AI Infrastructure Layer                                    │
│  ┌─────────────────┬──────────────────┬────────────────────┐   │
│  │  LLM Runtime    │  LC 容器兼容层    │  HMS 存储核心       │   │
│  │  本地 GGUF      │  OCI 容器运行时   │  History  B⁺树     │   │
│  │  Q4/Q8 + SIMD   │  Linux 子系统    │  Memory   HNSW     │   │
│  │  在线 API 兼容  │  syscall 转译     │  Skill    注册表   │   │
│  │  工具调用路由    │  Skill 沙箱 IFC  │  L1/L2/L3 缓存     │   │
│  └─────────────────┴──────────────────┴────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│  L1  Micro-kernel                                               │
│       PMM / VMM / Slab │ 协程四态调度 │ VFS │ NVMe DMA          │
│       HAL（x86_64 / AArch64 / RISC-V64 / LoongArch64）         │
├─────────────────────────────────────────────────────────────────┤
│  L0  UEFI Boot Loader  aevos_boot.efi  （目标 <2s 到推理就绪）  │
└─────────────────────────────────────────────────────────────────┘
```

**命名约定**：

- **OS 分层**仅使用 **L0–L4**。  
- 图中 HMS 侧的「多级语义缓存」在工程文档里记为 **C1 / C2 / C3**（或 tier-1/2/3），对应实现 `hms_cache` 的三档表；**勿与 L1 微内核混淆**。

---

## 2. 为什么这样分层

1. **策略与机制分离**：Planner、Skill 生成等与页表、调度、驱动解耦；**LC** 作为形式化上的**信息流边界**——若可证「不可穿越 LC」，则可将部分安全性质从 **L3 Verifier** 组合归约到 **L1** 不变量（差分证明复用）。  
2. **双引擎 LLM**（`ideas/ideas2.md`）：本地 GGUF 为**反射弧**，在线 API 为**皮层**；二者同属 **L2 的 LLM 柱**，经 `llm_sys_*` 统一暴露。  
3. **L3 双列**：**Agent Runtime** 管执行与可观测性（EventLog、Mailbox、四态工具、Cancel）；**Self-Evolution** 管计划、纠错、验证与策略更新——二者同层并列，便于调度与共享 HMS/LLM。

---

## 3. 全局数据流（自顶向下）

1. **L4 Shell** 采集输入，交给 **L3 Agent Runtime**（及将来与 Self-Evolution 协作）。  
2. **L3** 写 **EventLog**、用 **Mailbox** 通信、驱动 **四态工具**；Planner/Corrector 等从 **L3 右列** 读写轨迹与策略。  
3. **L2 HMS 柱** 装配 History / Memory / Skill，命中 **hms_cache（C1–C3）**。  
4. **L2 LLM 柱** 通过 `llm_sys_infer` 等生成 token；`prefer_remote` 时优先远程。  
5. **L2 LC 柱** 对 Skill ELF、OCI 容器做沙箱与 ABI 转译，再接通受控的 LLM syscall 或驱动能力（Skill 数据仍来自 HMS）。  
6. **L1** 提供 PMM/VMM/协程/VFS/驱动；**L0** 提供 `boot_info` 与可选 `boot.json`。

`kernel_main` 顺序：硬件与 MM → 驱动与帧缓冲 → VFS/POSIX/调度 → **L2 LLM 初始化** → **L3** `agent_system_init` + **L3 Self-Evolution** `evolution_plane_init` + **L2 LC** `lc_layer_init` → **L4** Shell 协程 → `scheduler_run()`。

---

## 4. 分层详解

### L4 — AevOS Shell（用户交互平面）

**职责**：类 Cursor 的深色帧缓冲 UI；内部 **Wayland 风格** 合成（`aevos_wl_*`）；流式聊天、终端、侧栏；未来 WebSocket。

**边界**：**不直接解析 GGUF**；通过 **L3** 提供的 Agent/抽象调用链触及 **L2**。

---

### L3 — Agent Layer

#### 4.1 Agent Runtime（左列）

**核心**（`agent_t`）：history / memory / skills / hms_cache、llm 指针、`agent_state_t`、**四态** `agent_tool_state_t`。

- **EventLog**：append-only 环形，类型含用户输入、LLM、工具、取消、邮箱、规划/验证/进化占位。  
- **Mailbox**：定长环；`to_agent == 0` 为广播。  
- **Cancel**：与 `scheduler_cancel_*` 协作。

#### 4.2 Self-Evolution Plane（右列）

**目标模块**：Planner（ReAct/ToT）、Corrector（差分回滚）、Verifier（增量 LTL/CTL）、Evolver（RL + 证明复用）。当前 `src/evolution/` 为**初始化桩**，详见 [evolution.md](evolution.md)。

**与左列关系**：共享同一 **EventLog** 与 **History**（如 `history_truncate_keep` 服务 Corrector）；Verifier 的证明目标可锚定在 **L2 LC** 边界。

---

### L2 — AI Infrastructure Layer（三柱）

#### 4.3 LLM Runtime 柱

GGUF、SIMD、`llm_sys_*`、工具路由；双引擎语义见 [llm-syscall.md](llm-syscall.md)。

#### 4.4 LC 容器兼容层柱

OCI / Linux syscall 转译 / Skill 沙箱 / IFC；与 HMS、LLM 为**并列柱**，不是「夹在两层之间的独立 OS 层」。详见 [container.md](container.md)。

#### 4.5 HMS 存储核心柱

History（环 + B⁺）、Memory（HNSW）、Skill、**hms_cache（C1–C3）**；详见 [hms.md](hms.md)。

---

### L1 — Micro-kernel

PMM、VMM、Slab、协程四态、VFS（含 procfs/devfs）、块设备与网络等；HAL 多架构。为 **L2** 提供统一底层，**L3/L4** 不直接触碰硬件寄存器。

---

### L0 — UEFI Boot Loader

`aevos_boot.efi`：`boot_info`、GOP、可选 NVMe 预载、Secure Boot 等；`EFI\AevOS\boot.json`。

---

## 5. 创新点与代码对照

| 主题 | 落点 |
|------|------|
| L3 Self-Evolution（增量验证等） | `src/evolution/` + [evolution.md](evolution.md) |
| L2 LC + OCI/Linux/沙箱 | `src/container/` + `src/linux/` + [container.md](container.md) |
| L2 HMS + 语义缓存 C1–C3 | `agent/hms_cache.c` + [hms.md](hms.md) |
| L3 四态工具 + Cancel | `agent_core.h`、`coroutine.h`、`scheduler_cancel_*` |
| L2 双引擎 LLM | `llm_syscall.c`、`llm_api_client` + [llm-syscall.md](llm-syscall.md) |

---

## 6. 延伸阅读

- [HMS（L2 柱）](hms.md) · [LC（L2 柱）](container.md) · [Self-Evolution（L3 列）](evolution.md) · [LLM Syscall（L2 柱）](llm-syscall.md)  
- English: [architecture.md](../en/architecture.md)
