# LC 容器兼容层（Docker / Linux 子系统 / Skill 沙箱）

在官方分层（`ideas/ideas2.md`）中，**LC（容器兼容层）** 是 **L2 AI Infrastructure Layer** 的**中间柱**，与 **LLM Runtime**、**HMS** 并列——不是「插在两个 OS 层之间的额外层号」。本文件说明：**三柱为何同处 L2**、**LC 内部子系统结构与算法**、**与 L3 Verifier 的归约关系**、以及 `src/container/` 的**当前落点**。

---

## 1. 位置语义（L2 三柱之一）

1. **Skill 执行链**：**L2 HMS 柱**负责存储与注册 Skill；动态可执行体（如 TinyCC → ELF）须先经 **LC 柱**的沙箱（内存边界、syscall 白名单、IFC），再调用 **L2 LLM 柱**的 `llm_sys_*` 或经 L1 驱动暴露的受控能力。  
2. **Docker 类负载**：OCI 需要 **overlayfs、rootfs、namespace、cgroup**；不宜全部塞进 **L1** 微内核。LC 通过 **Linux ABI 转译** 与 **procfs/devfs 视图** 吸收复杂度。  
3. **验证归约**：若可证明 **LC 边界不可穿越**，则 **L3 Verifier** 可将安全性质**组合**到 **L1** 不变量上，实现**差分证明复用**，而不必对每个 Skill 做全程序验证。

---

## 2. 子系统 A：OCI 容器运行时（`oci_runtime.c`）

**目标**：兼容 **Docker 镜像布局**与**分层 rootfs**（overlay 语义），在裸机上提供 **runc 级**但裁剪过的运行时。

**结构要点**：

- **镜像**：manifest + layer tarball → 解压到只读 lower；upper/work 可写层记录差异。  
- **挂载**：合并视图作为容器的 `/`；与 VFS 的挂载表对接。  
- **生命周期**：create → start → stop → delete；状态机与 **cgroup 资源映射**（CPU/内存限额映射到 AevOS 调度与 PMM 策略）。

**算法侧**：overlay 解析为 **copy-on-write** 路径选择：读失败则沿层栈向下找；写时复制到 upper。与块设备缓存交互需注意 **页对齐与刷盘顺序**。

**现状**：脚手架为主，接口与数据结构随实现演进。

---

## 3. 子系统 B：Linux 子系统（`linux_subsys.c`）

**目标**：类似 **WSL2 的用户态视角**——在 AevOS 上运行**静态链接 musl** 等 Linux ELF，通过 **syscall 号转译** 映射到 AevOS 原生 syscall 或 VFS。

**结构要点**：

- **转译表**：`linux_nr → aevos_handler`，未实现返回 `-ENOSYS` 或兼容层模拟。  
- **虚拟 FS**：`/proc`、`/dev` 由内核 **procfs/devfs** 提供；LC 负责**进程命名空间内**的路径解析与视图裁剪。  
- **信号/线程**：可简化为单线程进程模型先行，逐步补齐。

**算法侧**：热路径是 **dispatch + 参数封送**；需避免双重拷贝（可用 `iovec` 式接口映射到 POSIX 层）。

---

## 4. 子系统 C：Skill 沙箱（`sandbox.c`）与 IFC（`ifc.c`）

### 4.1 Skill 沙箱

**目标**：在 **用户态或受控特权级** 加载 `skill_elf` 产出的 ELF，限制：

- **代码/数据段**固定范围，**禁止 W^X 违反**（视平台能力）。  
- **syscall 白名单**：仅允许文件读、已注册 HMS 调用、经代理的 `llm_sys_infer` 等。  
- **配额**：CPU 时间片、分配上限由调度与 slab/PMM 策略强制执行。

**算法骨架**：

1. 加载 ELF，解析 PT_LOAD，建立合法 VA 区间集合 \(R\)。  
2. 安装 **trampoline**：所有 syscall 进入 LC 校验，校验失败 → `kill` + EventLog。  
3. 动态链接若不存在，则仅支持 **静态 PIC** 或内核提供的最小 GOT。

### 4.2 IFC（信息流控制）

**目标**：缓解 **提示注入** 与 **跨 Agent 数据泄露**——给对象打 **标签**（如 {public, user_secret, model_output}），规定 **允许流** 的格或策略。

**算法侧（概念）**：

- 每个内存页、文件句柄、Mailbox 消息带 **label**。  
- 在 syscall 边界执行 **流约束**：\(L_{src} \leq L_{dst}\)（格上序）才允许写。  
- 与 **Verifier** 结合：策略可写成「不存在从 `user_secret` 到 `network` 的流」。

---

## 5. 与调度域的关系（`ideas` 原话摘要）

**Docker 容器**可作为「**重量级 Skill**」与 **AevOS 原生轻量协程**共存于同一调度域：容器内进程映射到 **POSIX 进程 + 阻塞点**，原生 Agent 仍用 **四态工具 + coro_yield**。LC 负责把两类世界**隔离在各自策略包**内。

---

## 6. 初始化与调用图

`lc_layer_init()`（`lc_layer.c`）顺序：

1. `lc_sandbox_init`  
2. `lc_ifc_init`  
3. `lc_linux_subsys_init`  
4. `lc_oci_init`  

由 `kernel_main` 在 `agent_system_init` 与默认 Agent 创建之后调用（与 `evolution_plane_init` 相邻），保证 **VFS/POSIX/网络** 已就绪，便于转译层挂接。

---

## 7. 实现状态与路线图

| 文件 | 当前角色 |
|------|----------|
| `sandbox.c` | Skill / ELF 隔离、TinyCC 管线 — **TBD** |
| `ifc.c` | 标签与流策略 — **TBD / 部分桩** |
| `linux_subsys.c` | Linux ABI shim — **渐进** |
| `oci_runtime.c` | OCI / overlay — **脚手架** |
| `lc_container.c` | 容器相关聚合（若存在 CLI 后端等） |

优先建议：**ELF 加载 + syscall 拦截 + 白名单** 最小闭环，再叠 **IFC 格**，最后扩展 **OCI 完整生命周期**。

---

## 延伸阅读

- [总体架构](architecture.md) · [L3 Self-Evolution](evolution.md) · [LLM Syscall（L2 柱）](llm-syscall.md)  
- English: [container.md](../en/container.md)
