# AevOS Documentation (English)

**AevOS** is a bare-metal OS for on-device agents: UEFI boot, micro-kernel, embedded GGUF inference, and a **Cursor-like** dark framebuffer shell. The UI stack uses an internal **Wayland-style** compositor protocol (`aevos_wl_*`)—same concepts as Wayland, not wire-compatible.

---

## Architecture (L0–L6 + LC)

| Layer | Contents |
|-------|----------|
| **L6** | Shell: sidebar, chat (streaming API), terminal, `ws_bridge` stub |
| **L5** | Self-evolution: `src/evolution/` (scaffold) |
| **L4** | Agent runtime: `EventLog`, `mailbox`, four tool states, cancel broadcast |
| **L3** | HMS: history / memory / skills + `hms_cache` (L1 CLOCK-style) |
| **LC** | Container layer: `src/container/` (sandbox, IFC, Linux shim, OCI scaffold) |
| **L2** | LLM: `llm_syscall` façade; remote OpenAI path stubbed in `llm_api_client.c` |
| **L1** | Micro-kernel: PMM/VMM, coroutines, VFS + **`procfs`** + **`devfs`**, **virtio-net** |
| **L0** | UEFI: parses **`EFI\AevOS\boot.json`** when present |

---

## Build & run

**`ARCH`**: `x86_64`, `aarch64`, `riscv64`, `loongarch64`, **`mips64el`** (direct `-kernel`).

```bash
make
make run
```

QEMU is invoked with **virtio-net-pci** on **user** networking where applicable.

---

## `boot.json`

Place at `\EFI\AevOS\boot.json` on the ESP. The **bootloader parses** this file; defaults apply if missing.

---

## Database

Default: **in-memory** `aevos_db`. For SQLite, see **`third_party/sqlite3/README.md`**.

---

## Further reading

- [architecture.md](architecture.md) · [hms.md](hms.md) · [container.md](container.md) · [evolution.md](evolution.md) · [llm-syscall.md](llm-syscall.md)
- 中文: [简体中文文档](../zh/README.md)
