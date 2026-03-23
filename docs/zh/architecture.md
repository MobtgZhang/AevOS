# AevOS-Evo 架构

分层与 `ideas/` 中描述一致：**L0–L6** 与 **LC** 容器兼容层。

- **数据流**：Shell → Agent（EventLog）→ HMS / LLM syscall →（可选）LC 沙箱 → 微内核驱动。
- **双引擎 LLM**：本地 GGUF 经 `llm_sys_infer`；远程 API 在 `llm_api_client.c` 中预留（需网络栈上构建 HTTP/TLS）。

详见根目录 `README.md` ASCII 图与 `src/` 各子目录。
