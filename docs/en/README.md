# AevOS Documentation (English)

**AevOS** is a bare-metal OS for on-device agents: UEFI boot, micro-kernel, embedded GGUF inference, and a **Cursor-like** dark framebuffer shell. The UI stack uses an internal **Wayland-style** compositor protocol (`aevos_wl_*`)—same concepts as Wayland, not wire-compatible.

---

## Architecture (L0–L4)

Aligned with `ideas/ideas2.md`: **L2** is the AI infrastructure layer (**LLM / LC / HMS** pillars); **L3** is the agent layer (**agent runtime** + **self-evolution** columns); **L4** is the shell.

| OS layer | Contents |
|----------|----------|
| **L4** | Shell: sidebar, chat (streaming API), terminal, `ws_bridge` stub |
| **L3** | Agent layer: left—`EventLog`, `mailbox`, four tool states, `scheduler_cancel_*`; right—`src/evolution/` (Planner/Corrector/Verifier/Evolver scaffold) |
| **L2** | AI infrastructure: **LLM** (`llm_syscall`, `llm_api_client`) · **LC** (`src/container/`) · **HMS** (history/memory/skill, `hms_cache` tiers C1–C3) |
| **L1** | Micro-kernel: PMM/VMM, coroutines, VFS + **`procfs`** + **`devfs`**, **virtio-net** |
| **L0** | UEFI: parses **`EFI\AevOS\boot.json`** when present |

---

## Build & run

**`ARCH`**: `x86_64`, `aarch64`, `riscv64`, `loongarch64`.

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

Topic guides are **in-depth** (layer structure, algorithms, formal/roadmap notes from `ideas/`, and source pointers): [architecture.md](architecture.md), [hms.md](hms.md), [container.md](container.md), [evolution.md](evolution.md), [llm-syscall.md](llm-syscall.md).

- 中文: [简体中文文档](../zh/README.md)
