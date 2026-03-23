# L2 LLM pillar — LLM syscall surface

The **LLM runtime pillar** inside **L2 AI infrastructure** exposes a **syscall-style** façade (`include/aevos/llm_syscall.h`, `llm/llm_syscall.c`) unifying **on-device GGUF** and an **OpenAI-compatible remote client**. `ideas/ideas2.md` calls this **dual-engine**: local **reflex**, remote **cortex**; callers pick policy via flags.

---

## 1. APIs and semantics

### `llm_sys_infer`

```c
int llm_sys_infer(struct llm_ctx *local_ctx, const llm_infer_request_t *req,
                  char *output, size_t out_size);
```

**Fields**:

- `req->prompt`  
- `req->prefer_remote`: if true, try **remote first**; if the return is **not** `-ENOTSUP`, use it; if remote returns `-ENOTSUP` (unsupported / net down), **fall back** to local.  
- `req->temperature` / `top_p`: consumed when the backend supports them.

**Control flow**:

1. Validate `req`, `output`, `out_size`.  
2. If `prefer_remote`: `llm_remote_infer`; if `rc == 0` or `rc != -ENOTSUP`, return `rc`.  
3. Else require valid `local_ctx` + `prompt` and `llm_is_local_loaded`; call `llm_infer`.

**Design intent**: remote failures should not be silently ignored; `-ENOTSUP` means “try local”.

### `llm_sys_tokenize`

```c
int llm_sys_tokenize(struct llm_ctx *local_ctx, const llm_tokenize_request_t *req);
```

Local tokenizer only; returns `-ENOTSUP` if no model loaded.

### `llm_sys_tool_dispatch`

```c
int llm_sys_tool_dispatch(struct llm_ctx *local_ctx, const llm_tool_dispatch_t *req,
                          char *route_buf, size_t route_max);
```

1. `llm_tool_route_compute(local_ctx, tool_name, args_json, &rr)`  
2. Format `rr` into `line`  
3. `snprintf(route_buf, route_max, "tool=%s %s", tool_name, line)`

This is the unified **tool routing** exit—static tables, heuristics, or future small models. Maps to **`Handler.dispatch({tool_name})`** in `ideas`.

---

## 2. Coroutines and streaming UI

- **Streaming**: the decode loop can `coro_yield` per token/chunk; **L4 Shell** appends incrementally.  
- **Four tool states**: when a tool waits on another LLM turn, `AGENT_TOOL_WAIT_LLM` cooperates with the scheduler instead of spinning.

---

## 3. Formal contracts (roadmap, per `ideas`)

Incremental engineering contracts:

- **Pre**: e.g. loaded `local_ctx`, bounded `prompt`, verified weights.  
- **Post**: NUL-terminated output, length ≤ `out_size-1`, no disallowed prefixes (with IFC).  
- Full **Hoare triples** `{P} llm_sys_infer {Q}` need a pinned `llm_infer` spec and memory model—**L3 Verifier** + **L2 LLM** research boundary.

---

## 4. Boundaries with LC and HMS

- Generated **skill code** should not call `llm_infer` directly; the **LC pillar** should gate syscalls (roadmap).  
- The **HMS pillar** assembles compressed context before inference; results feed History/Memory.

---

## 5. Source index

| Piece | Path |
|-------|------|
| Header | `src/include/aevos/llm_syscall.h` |
| Façade | `src/llm/llm_syscall.c` |
| Local runtime | `src/llm/llm_runtime.c` |
| Remote client | `src/llm/llm_api_client.c` |
| Tool router | `src/llm/llm_tool_router.c` |

---

## Further reading

- [Architecture](architecture.md) · [HMS](hms.md) · [LC layer](container.md)  
- 简体中文：[llm-syscall.md](../zh/llm-syscall.md)
