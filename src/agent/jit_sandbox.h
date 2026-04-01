#pragma once

/*
 * 热路径编译（TinyCC 等）的沙箱策略入口：未来可切换为 WASM 或独立地址空间。
 */

void aevos_jit_sandbox_compile_enter(const char *phase_tag);
