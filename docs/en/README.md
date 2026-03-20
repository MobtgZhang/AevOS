# AevOS Documentation (English)

**AevOS** (Autonomous Evolving OS) is a bare-metal operating system that runs AI agents natively—without Linux or another host OS. It boots from UEFI, manages memory and scheduling, embeds an LLM inference stack (llama.cpp–style), and ships with a Cursor-inspired framebuffer shell.

---

## Highlights

| Area | What you get |
|------|----------------|
| **Boot** | UEFI loader (`aevos_boot.efi`), reads `boot.json`, loads `kernel.elf`, GOP framebuffer, handoff to kernel |
| **Kernel** | C17 micro-kernel: PMM/VMM, 4-level paging, cooperative coroutine scheduler, drivers (NVMe, GPU FB, HID, serial, timer, PCI), VFS, network stack |
| **Agent** | Per-agent history (ring buffer), vector memory (HNSW-style recall), skill engine (generate / compile / hot-load C at runtime) |
| **LLM** | GGUF models, Q4/Q8 quantization, AVX2 SIMD, streaming tokens |
| **UI** | Dark-themed framebuffer shell: sidebar file browser, AI chat with streaming, built-in terminal, mouse cursor, status bar |

Design goal: from power-on to interactive shell in **under about two seconds** (as stated in the project README).

---

## Architecture (layers)

1. **Layer 0** — UEFI bootloader  
2. **Layer 1** — Micro-kernel (memory, scheduling, drivers, VFS, net)  
3. **Layer 2** — LLM runtime (GGUF inference, system-call for Agent)  
4. **Layer 3** — AI agent core (history sorted by time, memory, skills; backed by SQLite3)  
5. **Layer 4** — Shell UI (framebuffer, mouse movement/click, keyboard driver)

See the root [README.md](../../README.md) for the ASCII diagram.

---

## Prerequisites

- **Cross GCC**: e.g. `x86_64-elf-gcc` (or host `gcc` as fallback, per Makefile)
- **GNU-EFI**: UEFI headers and libs for building the bootloader
- **mtools**: FAT32 disk image tooling
- **QEMU + firmware**: e.g. OVMF on x86_64 for UEFI guests

**Ubuntu / Debian example:**

```bash
sudo apt install gcc gnu-efi mtools qemu-system-x86 ovmf
```

**LoongArch 64**: needs a LoongArch cross toolchain; QEMU uses EDK2 pflash paths configurable via `AEVOS_LOONGARCH_FW` (see Makefile).

---

## Build

From the repository root:

```bash
# Full build: bootloader + kernel + disk image
make

# Specific architecture
make ARCH=aarch64
make ARCH=riscv64
make ARCH=loongarch64

# Components
make boot      # UEFI bootloader only
make kernel    # Kernel only
make tools     # Host tools (e.g. mkfs, skill_packager)
make image     # Disk image

make info      # Print build configuration
```

Supported `ARCH` values: `x86_64`, `aarch64`, `riscv64`, `loongarch64`.

---

## Run (QEMU)

```bash
make run
```

Uses the current `ARCH` (default `x86_64`), OVMF on x86_64 (or EDK2 pflash where applicable), **4 GB RAM**, generated disk image, and **serial on stdio**.

**LoongArch override example:**

```bash
AEVOS_LOONGARCH_FW=/path/to/LoongArchVirtMachine make ARCH=loongarch64 run
```

---

## Boot configuration (`boot.json`)

Place `boot.json` on the EFI system partition at `\EFI\AevOS\boot.json`.

Example:

```json
{
  "model_path": "/models/qwen-7b-q4.gguf",
  "n_ctx": 32768,
  "n_threads": 4,
  "use_gpu": false,
  "target_fps": 60,
  "screen_width": 1920,
  "screen_height": 1080
}
```

If the file is missing, built-in defaults apply.

---

## Repository layout (summary)

| Path | Role |
|------|------|
| `src/boot/` | UEFI bootloader |
| `src/kernel/` | Kernel, arch ports, mm, sched, drivers, fs, net |
| `src/agent/` | Agent core |
| `src/llm/` | LLM runtime |
| `src/ui/` | Framebuffer shell |
| `src/lib/` | Freestanding libc subset |
| `src/tools/` | Host utilities |
| `src/include/aevos/` | Shared headers (`config.h`, `boot_info.h`, …) |

---

## Version and license

- Version baseline: **v0.1.0** (see root README history table for roadmap).
- License: **LGPL-2.1**.

---

*Other language: [中文文档](../zh/README.md).*
