# AevOS 文档（简体中文）

**AevOS** 是裸机自主智能体操作系统：UEFI 启动、微内核、内嵌 GGUF 推理、类 **Cursor** 的深色帧缓冲 Shell。桌面协议侧采用内部 **Wayland 风格** 合成与 `aevos_wl_*` 消息（与上游 Wayland  wire format 不同，但概念对齐）。

---

## 架构（L0–L6 + LC）

| 层 | 内容 |
|----|------|
| **L6** | Shell：侧栏、聊天（流式 `chat_view_append_stream_chunk`）、终端、`ws_bridge` 占位 |
| **L5** | 自进化平面：`src/evolution/`（Planner/Corrector/Verifier/Evolver 脚手架） |
| **L4** | Agent 运行时：`EventLog`、`mailbox`、四态 `tool_state`、`scheduler_cancel_*` |
| **L3** | HMS：History / Memory / Skill + `hms_cache`（L1 CLOCK 风格） |
| **LC** | 容器兼容层：`src/container/`（沙箱/IFC/Linux 子系统/OCI 脚手架） |
| **L2** | LLM：`llm_syscall` 统一入口；远程 OpenAI 兼容路径见 `llm_api_client.c`（待 TCP/HTTP） |
| **L1** | 微内核：PMM/VMM、协程、VFS（含 **`/proc` `procfs`**、**`/dev` `devfs`**）、**virtio-net** |
| **L0** | UEFI：读取 **`EFI\AevOS\boot.json`** 覆盖默认启动配置 |

---

## 构建与运行

支持的 **`ARCH`**：`x86_64`、`aarch64`、`riscv64`、`loongarch64`、**`mips64el`**（`mips64el` 为 `-kernel` 直启路径）。

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

- [架构说明](architecture.md) · [HMS](hms.md) · [LC 层](container.md) · [自进化](evolution.md) · [LLM Syscall](llm-syscall.md)
- English: [Documentation in English](../en/README.md)
