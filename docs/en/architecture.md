# AevOS-Evo architecture (L0–L4)

This document uses the **same numbering as `ideas/ideas2.md`**: **five OS layers L0–L4**. **L2** is the **AI infrastructure layer** with **three pillars**—**LLM runtime**, **LC (container compatibility)**, and **HMS storage**. **L3** is the **agent layer** with **two columns**—**agent runtime** and **self-evolution plane**. Below we map `ideas/` to the tree and mark shipped vs roadmap code.

---

## 1. Layer stack (canonical diagram)

```
┌─────────────────────────────────────────────────────────────────┐
│  L4  AevOS Shell  (framebuffer UI / CLI / WebSocket)            │
├─────────────────────────────────────────────────────────────────┤
│  L3  Agent Layer                                                │
│  ┌──────────────────────────┬────────────────────────────────┐  │
│  │  Agent Runtime           │  Self-Evolution Plane          │  │
│  │  EventLog (append-only)  │  Planner (ReAct / ToT)         │  │
│  │  Mailbox MPMC            │  Corrector (diff rollback)     │  │
│  │  four-state tools        │  Verifier (LTL/CTL incremental)│  │
│  │  Cancel broadcast        │  Evolver (RL + proof reuse)    │  │
│  └──────────────────────────┴────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────────┤
│  L2  AI Infrastructure Layer                                    │
│  ┌─────────────────┬──────────────────┬────────────────────┐   │
│  │  LLM Runtime    │  LC layer        │  HMS core          │   │
│  │  local GGUF     │  OCI runtime     │  History B+-tree   │   │
│  │  Q4/Q8 + SIMD   │  Linux persona   │  Memory HNSW       │   │
│  │  remote API     │  syscall trans.  │  Skill registry    │   │
│  │  tool routing   │  skill sandbox   │  L1/L2/L3 cache    │   │
│  │                 │  IFC             │  (HMS tiers)       │   │
│  └─────────────────┴──────────────────┴────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│  L1  Micro-kernel                                               │
│       PMM / VMM / Slab │ coroutine scheduler │ VFS │ NVMe DMA   │
│       HAL (x86_64 / AArch64 / RISC-V64 / LoongArch64)          │
├─────────────────────────────────────────────────────────────────┤
│  L0  UEFI Boot Loader  aevos_boot.efi  (goal: infer-ready <2s)  │
└─────────────────────────────────────────────────────────────────┘
```

**Naming**:

- **OS layers** are **L0–L4** only.  
- HMS **semantic cache** tiers in code (`hms_cache`) are called **C1 / C2 / C3** here so they are not confused with **L1 micro-kernel**.

---

## 2. Why this shape

1. **Policy vs mechanism**: planners and generated skills stay separate from paging/scheduling/drivers. **LC** is an **information-flow boundary** for formal work—if LC is non-escapable, **L3 Verifier** can compose properties down to **L1** invariants (diff proof reuse).  
2. **Dual-engine LLM** (`ideas/ideas2.md`): local GGUF vs remote API—both live in the **L2 LLM pillar** behind `llm_sys_*`.  
3. **L3 split columns**: **Agent runtime** owns execution and observability; **self-evolution** owns plan/correct/verify/learn—same layer, shared HMS/LLM access.

---

## 3. Global data flow (top down)

1. **L4 Shell** gathers input for **L3 agent runtime** (and future evolution hooks).  
2. **L3** writes **EventLog**, uses **Mailbox**, drives **four tool states**; evolution modules (right column) consume/produce structured traces.  
3. **L2 HMS** assembles History/Memory/Skill with **hms_cache (C1–C3)**.  
4. **L2 LLM** runs `llm_sys_infer` etc.; remote first when `prefer_remote` and supported.  
5. **L2 LC** sandboxes ELF skills and OCI workloads, then allows controlled LLM syscalls or drivers (skill *bytes* still come from HMS).  
6. **L1** provides mm/sched/VFS/drivers; **L0** hands off **boot_info** / `boot.json`.

`kernel_main`: hardware + MM → drivers + fb → VFS/POSIX/scheduler → **L2 LLM** → **L3** `agent_system_init` + **L3 evolution** `evolution_plane_init` + **L2 LC** `lc_layer_init` → **L4** shell coroutine → `scheduler_run()`.

---

## 4. Layer details

### L4 — AevOS Shell

Cursor-like framebuffer UI; Wayland-**like** compositor (`aevos_wl_*`); streaming chat, terminal, sidebar; future WebSocket. **No direct GGUF parsing**—calls go through **L3** into **L2**.

---

### L3 — Agent layer

#### Agent runtime (left column)

**agent_t**: history/memory/skills/hms_cache, llm pointer, `agent_state_t`, orthogonal **agent_tool_state_t**.

- **EventLog**: append-only ring (user, LLM, tools, cancel, mailbox, plan/verify/evolve hooks).  
- **Mailbox**: fixed ring; `to_agent == 0` = broadcast.  
- **Cancel**: integrates with **scheduler_cancel_***.

#### Self-evolution plane (right column)

Target: **Planner**, **Corrector**, **Verifier**, **Evolver**. Today: **stubs** in `src/evolution/`—see [evolution.md](evolution.md). Shares **EventLog** and **History** with the left column (e.g. `history_truncate_keep` for rollback).

---

### L2 — AI infrastructure (three pillars)

- **LLM pillar**: GGUF, SIMD, `llm_sys_*`, tool router—[llm-syscall.md](llm-syscall.md).  
- **LC pillar**: OCI, Linux ABI shim, skill sandbox, IFC—[container.md](container.md). **Peer** to LLM and HMS, not a separate “OS layer” between L3 and L2.  
- **HMS pillar**: History, Memory, Skill, **hms_cache C1–C3**—[hms.md](hms.md).

---

### L1 — Micro-kernel

PMM, VMM, slab, coroutine states, VFS (procfs/devfs), storage, net, multi-arch HAL. **L2–L4** use drivers and mm abstractions, not raw MMIO from agent logic.

---

### L0 — UEFI boot

`aevos_boot.efi`, **boot_info**, GOP, optional NVMe preload, Secure Boot, `EFI\AevOS\boot.json`.

---

## 5. Innovation map vs code

| Theme | Where |
|-------|-------|
| L3 self-evolution | `src/evolution/` + [evolution.md](evolution.md) |
| L2 LC / OCI / Linux / sandbox | `src/container/` + `src/linux/` + [container.md](container.md) |
| L2 HMS + cache C1–C3 | `agent/hms_cache.c` + [hms.md](hms.md) |
| L3 four-state tools + cancel | `agent_core.h`, `coroutine.h`, `scheduler_cancel_*` |
| L2 dual-engine LLM | `llm_syscall.c`, `llm_api_client` + [llm-syscall.md](llm-syscall.md) |

---

## 6. Further reading

- [HMS (L2 pillar)](hms.md) · [LC (L2 pillar)](container.md) · [Self-evolution (L3 column)](evolution.md) · [LLM syscall (L2 pillar)](llm-syscall.md)  
- 简体中文：[architecture.md](../zh/architecture.md)
