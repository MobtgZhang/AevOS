# LLM Syscall 规约

头文件：`include/aevos/llm_syscall.h`  
实现：`llm/llm_syscall.c`

- `llm_sys_infer` — 统一推理（`prefer_remote` 为真时先尝试 `llm_api_client`）
- `llm_sys_tokenize` — 分词
- `llm_sys_tool_dispatch` — 工具路由字符串（占位）

Agent 主路径已通过 `llm_sys_infer` 调用本地模型。
