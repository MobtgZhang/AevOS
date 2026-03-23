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
- Topic pages: [architecture](docs/en/architecture.md) · [HMS](docs/en/hms.md) · [LC layer](docs/en/container.md) · [evolution](docs/en/evolution.md) · [LLM syscall](docs/en/llm-syscall.md) (and `docs/zh/` counterparts)

## Architecture (AevOS-Evo roadmap)

```
┌────────────────────────────────────────────────────────────┐
│  L6 — Shell (Cursor-like UI, Wayland-style compositor API) │
├────────────────────────────────────────────────────────────┤
│  L5 — Self-Evolution (Planner / Corrector / Verifier / Evolver) │
├────────────────────────────────────────────────────────────┤
│  L4 — Agent Runtime (EventLog, mailbox, tool states, cancel) │
├────────────────────────────────────────────────────────────┤
│  L3 — HMS (History / Memory / Skills + L1–L3 semantic cache) │
├────────────────────────────────────────────────────────────┤
│  LC — Container layer (sandbox, OCI/Linux ABI — in progress) │
├────────────────────────────────────────────────────────────┤
│  L2 — LLM Runtime (local GGUF + optional remote API syscall) │
├────────────────────────────────────────────────────────────┤
│  L1 — Micro-kernel (PMM, VMM, coroutines, VFS, net, virtio-net) │
├────────────────────────────────────────────────────────────┤
│  L0 — UEFI boot (boot.json, kernel.elf, GOP)                 │
└────────────────────────────────────────────────────────────┘
```

**L0** — UEFI bootloader reads `\EFI\AevOS\boot.json` when present (falls back to
defaults), loads `kernel.elf`, initializes GOP, passes `boot_info_t` to the kernel.

**L1** — C17 micro-kernel: PMM/VMM/slab, cooperative coroutines, drivers (NVMe,
AHCI on x86_64, virtio-GPU, virtio-net, HID, …), VFS with `/proc` and `/dev`, and
an in-kernel IPv4 stack.

**L2** — GGUF inference plus `llm_sys_*` syscall-style API; remote OpenAI-compatible
path is stubbed until HTTP/TLS on top of the stack is complete.

**L3** — Agent HMS: ring-buffer history (B+/WAL index planned), HNSW memory,
skills; `hms_cache` provides an L1 CLOCK-style hot cache.

**LC** — Container compatibility (skill sandbox, IFC, Linux syscall shim, OCI):
scaffold modules under `src/container/`.

**L4** — Append-only `EventLog`, MPMC `mailbox`, Neoclaw-style tool states, scheduler
cancel broadcast hook.

**L5** — Evolution plane scaffold under `src/evolution/`.

**L6** — Framebuffer shell with streaming chat helpers and internal Wayland-like
protocol (`aevos_wl_*`) for compositor-style layout.

## Directory Layout

```
src/
├── boot/               UEFI bootloader
├── kernel/             Micro-kernel (arch, mm, sched, drivers, fs, net)
├── agent/              Agent core, eventlog, mailbox, hms_cache, history_wal, …
├── llm/                LLM runtime, llm_syscall, llm_api_client (remote stub)
├── db/                 aevos_db (in-memory; optional SQLite via third_party)
├── container/          LC layer (sandbox, ifc, linux_subsys, oci)
├── evolution/          L5 scaffold (planner, corrector, verifier, evolver)
├── ui/                 Shell, terminal, chat, ws_bridge stub, wl protocol
├── posix/, lib/, tools/, include/aevos/
third_party/sqlite3/    Drop sqlite amalgamation here when enabling on-disk DB
```

## Prerequisites

- **GCC cross-compiler**: e.g. `x86_64-elf-gcc` (see `Makefile` fallbacks)
- **GNU-EFI**: UEFI bootloader
- **mtools**: FAT32 images
- **QEMU + firmware** (OVMF / AAVMF / EDK2 per architecture)

### Install on Ubuntu/Debian

```bash
sudo apt install gcc gnu-efi mtools qemu-system-x86 ovmf
```

## Building

```bash
make
make ARCH=aarch64
make ARCH=riscv64
make ARCH=loongarch64
make ARCH=mips64el
make boot / kernel / image / tools
make info
```

## Running

```bash
make run
```

QEMU is started with a **virtio-net-pci** NIC on `user` networking where the
platform supports PCI (see `Makefile`). **mips64el** uses direct `-kernel` boot
without UEFI by default.

## Boot Configuration

`boot.json` on the ESP at `\EFI\AevOS\boot.json` (see example in
[docs/zh/README.md](docs/zh/README.md)).

## Version History

| Version | Milestone |
|---------|-----------|
| v0.1.0+ | Evo roadmap: virtio-net, procfs/devfs, EventLog, llm_syscall, LC/L5 scaffolds |
| v0.1.0  | Baseline headers, slab 64 KiB, klog tags |

## License

LGPL-2.1.
