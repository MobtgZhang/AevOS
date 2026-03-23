# LC 容器兼容层

`src/container/`：

- `sandbox.c` — Skill / ELF 沙箱（TinyCC + syscall 白名单 — 待实现）
- `ifc.c` — 信息流控制
- `linux_subsys.c` — Linux ABI 转译
- `oci_runtime.c` — OCI / overlayfs 语义

入口：`lc_layer_init()`（由 `kernel/main.c` 调用）。
