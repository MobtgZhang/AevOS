# AevOS-Evo architecture

Layers **L0–L6** plus **LC** (container compatibility), aligned with `ideas/`.

- **Data flow**: Shell → Agent (`EventLog`) → HMS / LLM syscall → optional LC sandbox → micro-kernel drivers.
- **Dual LLM**: local GGUF via `llm_sys_infer`; remote OpenAI-compatible path is stubbed in `llm_api_client.c` until HTTP/TLS is implemented.

See the root `README.md` diagram and `src/` layout.
