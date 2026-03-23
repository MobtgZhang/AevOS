# LLM syscall surface

Header: `include/aevos/llm_syscall.h`  
Implementation: `llm/llm_syscall.c`

- `llm_sys_infer` — unified inference (`prefer_remote` tries `llm_api_client` first)
- `llm_sys_tokenize` — tokenization
- `llm_sys_tool_dispatch` — tool routing string (stub)

The agent pipeline calls `llm_sys_infer` for local GGUF today.
