# AevOS — Autonomous Evolving OS v0.1.0

AevOS is a bare-metal operating system designed to run AI agents natively,
without Linux or any other host OS. It boots via UEFI, manages its own memory
and scheduling, embeds an LLM inference runtime (based on llama.cpp concepts),
and provides a Cursor-style shell UI — all from power-on in under 2 seconds.

## Screenshot

The framebuffer shell (dark theme, sidebar, AI chat, terminal, and status bar):

![AevOS shell UI](asserts/screenshots.png)

## Documentation

- [English](docs/en/README.md) — features, build, run, and boot configuration
- [简体中文](docs/zh/README.md) — 同上（中文）

## Architecture

```
┌──────────────────────────────────────────────────────┐
│  Layer 4 — AevOS Shell (Cursor-style framebuffer UI) │
├──────────────────────────────────────────────────────┤
│  Layer 3 — AI Agent Core (History / Memory / Skills) │
├──────────────────────────────────────────────────────┤
│  Layer 2 — LLM Runtime (GGUF, Q4/Q8, SIMD inference)│
├──────────────────────────────────────────────────────┤
│  Layer 1 — Micro-kernel (PMM, VMM, coroutines, VFS) │
├──────────────────────────────────────────────────────┤
│  Layer 0 — UEFI Boot Loader (aevos_boot.efi)        │
└──────────────────────────────────────────────────────┘
```

**Layer 0** — UEFI bootloader reads `boot.json`, loads `kernel.elf`, initializes
the GOP framebuffer, and jumps to the kernel.

**Layer 1** — Freestanding C17 micro-kernel. Physical/virtual memory management
with 4-level page tables, coroutine-based cooperative scheduler, NVMe/HID/GPU
drivers, a simple VFS, and network stack.

**Layer 2** — LLM inference engine running quantized GGUF models. Used by
the Agent layer for system-call–level LLM inference. Supports Q4_K_M and Q8
quantization, AVX2 SIMD acceleration, and streaming token output.

**Layer 3** — The AI Agent core. Each agent has a conversation history (multiple
sessions sorted by time, backed by SQLite3), a vector memory engine (HNSW index
for semantic recall), and a skill engine that can generate, compile, and
hot-load new C functions at runtime.

**Layer 4** — Framebuffer-based UI with a dark theme. Sidebar file browser,
AI chat panel with streaming output, built-in terminal, mouse cursor support
(movement, left/right click), keyboard driver, and status bar.

## Directory Layout

```
src/
├── boot/               UEFI bootloader (aevos_boot.efi)
│   ├── efi_main.c      EFI entry point
│   ├── efi_types.h     UEFI type definitions
│   └── linker_boot.lds Bootloader linker script
├── kernel/             Micro-kernel
│   ├── main.c          kernel_main() entry
│   ├── arch/           Multi-arch support (x86_64, aarch64, riscv64, loongarch64)
│   ├── mm/             PMM, VMM, slab allocator
│   ├── sched/          Coroutine scheduler, context switch
│   ├── drivers/        NVMe, GPU framebuffer, HID (keyboard+mouse), audio, serial, timer, PCI
│   ├── fs/             VFS, AevOSFS
│   └── net/            Network stack
├── agent/              AI Agent Core
├── llm/                LLM inference runtime
├── db/                 Database layer (SQLite3-backed History / Memory / Skills)
├── ui/                 Framebuffer UI shell
├── lib/                Freestanding libc subset
├── tools/              Host utilities (mkfs_aevos, skill_packager)
├── include/aevos/      Shared headers
│   ├── types.h         Primitive types
│   ├── boot_info.h     Boot information structures
│   └── config.h        Compile-time constants
└── build/              Build artifacts

third_party/
└── sqlite3/            SQLite3 amalgamation (git submodule)
```

## Prerequisites

- **GCC cross-compiler**: `x86_64-elf-gcc` (or host `gcc` as fallback)
- **GNU-EFI**: UEFI development headers and libraries
- **mtools**: For building FAT32 disk images
- **QEMU + OVMF**: For testing in a virtual machine

### Install on Ubuntu/Debian

```bash
sudo apt install gcc gnu-efi mtools qemu-system-x86 ovmf
```

## Building

```bash
# Build everything (bootloader + kernel + disk image)
make

# Build for a specific architecture
make ARCH=aarch64
make ARCH=riscv64
make ARCH=loongarch64

# Build individual components
make boot       # UEFI bootloader only
make kernel     # Kernel only
make tools      # Host tools only
make image      # Disk image

# Show build info
make info
```

## Running

```bash
# Launch in QEMU with UEFI firmware (current ARCH, default x86_64)
make run
```

This starts QEMU with OVMF (x86_64) or EDK2 pflash (LoongArch), 4 GB RAM, and the
generated disk image. Serial output is on stdio.

**LoongArch 64** needs a LoongArch cross GCC (`loongarch64-linux-gnu-gcc` or
similar). Firmware defaults to `AEVOS_LOONGARCH_FW` (see `Makefile`), e.g.
`~/Firmware/LoongArchVirtMachine/QEMU_EFI.fd` and `QEMU_VARS.fd`. Override:

```bash
AEVOS_LOONGARCH_FW=/path/to/LoongArchVirtMachine make ARCH=loongarch64 run
```

## Boot Configuration

Place a `boot.json` file in the EFI system partition at `\EFI\AevOS\boot.json`:

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

If the file is missing, sensible defaults are used.

## Version History

| Version | Milestone                                                  |
|---------|------------------------------------------------------------|
| v0.1.0  | Release baseline: versioned headers, slab up to 64 KiB (UI coroutine stacks), log tags |
| v0.1    | Manual skill registration, LLM tool calls                  |
| v0.2    | Semi-auto skill extraction from conversation history       |
| v0.3    | Full auto: LLM generates C → TinyCC → sandbox → deploy    |
| v0.4    | Skill evaluation & retirement (success rate < 60% → regen) |
| v0.5    | Mouse cursor support, enhanced drivers, multi-arch builds  |

## License

LGPL-2.1 license.
