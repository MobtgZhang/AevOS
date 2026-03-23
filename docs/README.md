# AevOS documentation

- **English:** [docs/en/README.md](en/README.md)
- **简体中文:** [docs/zh/README.md](zh/README.md)

## Topic guides

Each topic file explains **layer structure, algorithms, and boundaries** (including roadmap items from `ideas/`) and points to **source files** where relevant. **OS layers are L0–L4** per `ideas/ideas2.md` (L2 = LLM + LC + HMS; L3 = agent runtime + self-evolution; L4 = shell). `ideas/` is design reference; you may remove it locally after reading—canonical long-form text lives here.

| Topic | English | 中文 |
|-------|---------|------|
| Architecture | [architecture.md](en/architecture.md) | [architecture.md](zh/architecture.md) |
| HMS storage | [hms.md](en/hms.md) | [hms.md](zh/hms.md) |
| LC container layer | [container.md](en/container.md) | [container.md](zh/container.md) |
| Evolution plane | [evolution.md](en/evolution.md) | [evolution.md](zh/evolution.md) |
| LLM syscall | [llm-syscall.md](en/llm-syscall.md) | [llm-syscall.md](zh/llm-syscall.md) |

The repository root [README.md](../README.md) is the primary clone-and-build entry point.
