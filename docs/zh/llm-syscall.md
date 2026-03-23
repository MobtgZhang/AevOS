# L2 LLM Runtime 柱 — LLM Syscall 接口

**L2 AI Infrastructure Layer** 中的 **LLM Runtime 柱** 对 **L3/L4** 暴露 **syscall 风格** API（`include/aevos/llm_syscall.h`，实现 `llm/llm_syscall.c`），把**本地 GGUF**与**远程 OpenAI 兼容客户端**收敛到同一套函数边界。`ideas/ideas2.md` 称之为**双引擎**：本地是**反射弧**，远程是**皮层**；调用方通过标志位选择策略，而不直接依赖具体后端。

---

## 1. API 一览与语义

### `llm_sys_infer`

```c
int llm_sys_infer(struct llm_ctx *local_ctx, const llm_infer_request_t *req,
                  char *output, size_t out_size);
```

**参数**：

- `req->prompt`：输入文本。  
- `req->prefer_remote`：为真时**先**尝试远程；若返回非 `-ENOTSUP` 则采用该结果；若远程返回 `-ENOTSUP`（例如未实现或网络未就绪）则**回退**本地。  
- `req->temperature` / `top_p`：由具体后端在支持时消费。

**控制流（算法）**：

1. 校验 `req`、`output`、`out_size`。  
2. 若 `prefer_remote`：`llm_remote_infer`；若 `rc == 0` 或 `rc != -ENOTSUP`，返回 `rc`。  
3. 否则要求 `local_ctx` 与 `prompt` 合法，且 `llm_is_local_loaded`；调用 `llm_infer`。

**不变量（设计意图）**：远程路径失败不应静默吞掉错误；`-ENOTSUP` 明确表示「此路不通，可换本地」。

### `llm_sys_tokenize`

```c
int llm_sys_tokenize(struct llm_ctx *local_ctx, const llm_tokenize_request_t *req);
```

仅走**已加载的本地词表/模型**；未加载返回 `-ENOTSUP`。用于在不入推理环的情况下统计或截断 token。

### `llm_sys_tool_dispatch`

```c
int llm_sys_tool_dispatch(struct llm_ctx *local_ctx, const llm_tool_dispatch_t *req,
                          char *route_buf, size_t route_max);
```

1. 调用 `llm_tool_route_compute(local_ctx, tool_name, args_json, &rr)`。  
2. 将 `rr` 格式化为单行 `line`。  
3. `snprintf(route_buf, route_max, "tool=%s %s", tool_name, line)`。

**语义**：这是 **L2 工具路由策略** 的统一出口；具体 `compute` 可实现为静态表、启发式或将来由小型本地模型生成。与 `ideas` 中 **Handler.dispatch({tool_name})** 对应。

---

## 2. 与协程、流式 UI 的关系

- **流式 token**：推理循环可在每个 token 或每块后 `coro_yield`，**L4 Shell** 侧增量刷新（如 `chat_view_append_stream_chunk`）。  
- **四态工具**：当工具等待 LLM 再次调用时，Agent 将 `agent_tool_state_t` 置为 `AGENT_TOOL_WAIT_LLM`，与调度器协作避免忙等。

---

## 3. 形式化规约（目标，对齐 `ideas`）

工程上可逐步引入**契约**：

- **前置条件**：例如「`local_ctx` 已加载且权重校验通过」「`prompt` 长度 ≤ 某界」。  
- **后置条件**：输出缓冲区以 NUL 结尾、长度 ≤ `out_size-1`、不含未授权敏感前缀（与 IFC 协同）。  
- **Hoare 三元组** `{P} llm_sys_infer {Q}` 的真正证明需固定 `llm_infer` 实现与内存模型，属 **L3 Verifier** 与 **L2 LLM 柱**的交界研究。

---

## 4. 与 LC、HMS 的边界

- **Skill 生成的代码**不应绕过 `llm_sys_*` 直接调用 `llm_infer`，而应由 **L2 LC 柱**策略注入允许列表（路线图）。  
- **L2 HMS 柱**在调用推理前组装压缩上下文；推理结果再写回 History/Memory，形成闭环。

---

## 5. 源码路径

| 内容 | 路径 |
|------|------|
| 头文件 | `src/include/aevos/llm_syscall.h` |
|  façade | `src/llm/llm_syscall.c` |
| 本地推理 | `src/llm/llm_runtime.c` |
| 远程 stub | `src/llm/llm_api_client.c` |
| 工具路由 | `src/llm/llm_tool_router.c` |

---

## 延伸阅读

- [总体架构](architecture.md) · [HMS](hms.md) · [LC](container.md)  
- English: [llm-syscall.md](../en/llm-syscall.md)
