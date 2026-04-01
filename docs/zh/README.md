# AevOS 文档（简体中文）

**AevOS** 是裸机自主智能体操作系统：UEFI 启动、微内核、内嵌 GGUF 推理、类 **Cursor** 的深色帧缓冲 Shell。桌面协议侧采用内部 **Wayland 风格** 合成与 `aevos_wl_*` 消息（与上游 Wayland  wire format 不同，但概念对齐）。

---

## 架构（L0–L4）

与 `ideas/ideas2.md` 一致：**L2** 为 AI 基础设施层，含 **LLM / LC / HMS** 三柱；**L3** 为 Agent 层，含 **Agent Runtime** 与 **Self-Evolution** 两列；**L4** 为 Shell。

| OS 层 | 内容 |
|--------|------|
| **L4** | Shell：侧栏、聊天（流式 `chat_view_append_stream_chunk`）、终端、`ws_bridge` 占位 |
| **L3** | Agent 层：左列 `EventLog`、`mailbox`、四态 `tool_state`、`scheduler_cancel_*`；右列 `src/evolution/`（Planner/Corrector/Verifier/Evolver 脚手架） |
| **L2** | AI 基础设施：**LLM**（`llm_syscall`、`llm_ipc`、`llm_api_client`）· **LC**（`src/container/` + `src/linux/`）· **HMS**（History/Memory/Skill、`hms_cache` 语义缓存 C1–C3） |
| **L1** | 微内核：PMM/VMM、协程、VFS（含 **`/proc` `procfs`**、**`/dev` `devfs`**）、**virtio-net** |
| **L0** | UEFI：读取 **`EFI\AevOS\boot.json`** 覆盖默认启动配置 |

---

## 构建与运行

支持的 **`ARCH`**：`x86_64`、`aarch64`、`riscv64`、`loongarch64`。

```bash
make
make ARCH=aarch64
make run
```

QEMU 默认附带 **virtio-net-pci** + **user** 网桥（见根目录 `Makefile`）。

依赖示例：`gcc` `gnu-efi` `mtools` `qemu-system-*` `ovmf`（按架构选固件）。

---

## `boot.json`

路径：`\EFI\AevOS\boot.json`。字段与根 `README` 示例一致；**引导器已实现解析**，缺失时使用内置默认值。

---

## 数据库

会话树默认在 **`src/db/aevos_db.c` 内存实现**。若需 SQLite，将 amalgamation 放入 **`third_party/sqlite3/`** 并阅读其中 `README.md`。

---

## 更多

各专题文档已展开为**分层结构、算法与边界**（融合 `ideas/` 中的设计与路线图），并尽量对应到 `src/` 源码路径：[架构说明](architecture.md)、[HMS](hms.md)、[LC 层](container.md)、[自进化](evolution.md)、[LLM Syscall](llm-syscall.md)。

- English: [Documentation in English](../en/README.md)
