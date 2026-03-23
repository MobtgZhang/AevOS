# LC — container compatibility layer

`src/container/`:

- `sandbox.c` — skill / ELF sandbox (TinyCC + syscall allowlist — TBD)
- `ifc.c` — information-flow control
- `linux_subsys.c` — Linux ABI translation shim
- `oci_runtime.c` — OCI / overlayfs scaffold

Entry point: `lc_layer_init()` from `kernel/main.c`.
