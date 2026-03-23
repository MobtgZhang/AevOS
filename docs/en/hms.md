# HMS storage core (History · Memory · Skill)

HMS is the **HMS pillar** inside **L2 AI infrastructure** (peers: LLM, LC). It backs **L3 agents** with **dialogue**, **vector memory**, and **skills**. The `ideas` diagram labels HMS cache tiers as “L1/L2/L3 cache”; here we call them **C1/C2/C3** to avoid clashing with **OS layer L1** (micro-kernel). This page covers **structures, algorithms, cache tiers**, and **source paths**.

---

## 1. Overview and orchestration

`hms_core.h` sketches orchestration:

- **Memory**: tiered lookup **C1 → C2 → C3 → HNSW** (`hms_memory_retrieve_tiered`; aligns with `hms_cache`).  
- **History**: **B⁺-tree** keyed by monotonic **`seq`** into a ring buffer.  
- **Skills**: name resolution and ELF hot load.  
- **Pseudo embeddings**: `hms_text_to_pseudo_embed` when a full embedder is absent.  
- **Compressed context blocks**: `hms_append_compressed_block` emits `[M:…]` / `[H:…]` / `@skill#id` references to save tokens.

---

## 2. History

### 2.1 Structures

- **Ring** `hist_entry_t ring[HIST_RING_SIZE]` with `head` / `tail`.  
- Fields include `seq`, timestamp, `role`, **LZ4-compressed** payload (`history.h`).  
- **B⁺-tree** `hist_bptree_t`: `hist_bpt_insert(seq, ring_idx)` maps **seq → ring index**; `hist_bpt_range_from_seq` bulk-fetches indices from a `seq` lower bound.

### 2.2 Algorithms

| Op | Behavior |
|----|----------|
| **push** | write slot, new `seq`, B⁺ insert, maintain `window_tokens` / `max_tokens` |
| **by-seq** | B⁺ range → walk ring segments |
| **keyword search** | scan/decompress when sizes are bounded |
| **truncate_keep** | keep oldest `keep_count`, drop newer—supports **L3 Self-Evolution / Corrector** rollback |
| **serialize** | checkpoint / migration |

### 2.3 WAL / persistence

`history_wal.c` scaffolds **write-ahead logging** toward crash-consistent **`seq`**; combined with B⁺, a roadmap is “leaf stores EventLog offset” style secondary indexing.

---

## 3. Memory (vectors + HNSW)

### 3.1 Entries and graph

- **`mem_entry_t`**: `int8_t embedding[EMBED_DIM]`, `importance`, access stats, variable `content`.  
- **`hnsw_graph_t`**: `hnsw_node_t` array, per-level neighbor caps `HNSW_M` / base `HNSW_M0`, `entry_point`, `max_level`.

### 3.2 HNSW (sketch)

**Insert** (`hnsw_insert`):

1. Sample layer count \(l\) (exponential via `level_mult`).  
2. Greedy descent from the top to attach the new node.  
3. Connect up to `M` edges per layer; prune neighbor lists to bounded degree.

**Search** (`hnsw_search`):

1. Greedy walk from `entry_point` using **int8 cosine similarity**.  
2. Descend layers; at the bottom expand candidates with `ef` (`HNSW_EF_SEARCH`) and take top-k.

**Note**: approximate NN; a formal recall lower bound in `ideas` is research scope.

### 3.3 Forgetting

`memory_forget` can evict low-importance / low-usage nodes; must stay consistent with **hms_cache** evictions so the graph does not point at freed payloads.

### 3.4 Working set

`hashmap_t *working_mem`: `memory_working_set` / `memory_working_get` for ephemeral session keys without indexing in HNSW.

---

## 4. Skills

- Registry plus **`skill_elf`** hooks for **ELF hot load**; execution should go through **LC sandboxing** ([container.md](container.md)).  
- `ideas`: **regenerate when success < 60%**, **Hoare-style verification**—long-term toolchain + **L3 Evolver/Verifier** goals.

---

## 5. Semantic cache `hms_cache` (tiers C1 / C2 / C3)

### 5.1 Purpose

Tier **hot KV**, **warm digests**, **cold persistence** (`ideas` diagram: “L1/L2/L3 cache”; we write **C1/C2/C3** here).

### 5.2 Implementation notes (`hms_cache.c`)

- **C1** (`l1_*`): `l1_evict` is **CLOCK-like**—scan `hand`, evict if `ref==0`, else clear `ref`.  
- **C2/C3** (`l2`/`l3` slots): `lru_evict` picks the smallest **`lru_tag`** victim (LRU-ish).  
- Each tier is an `hms_kv_t` table (`key`, `blob`, `ref`, `lru_tag`).

### 5.3 Lookup / refill (concept)

1. **C1** hit → return, set `ref`.  
2. Else **C2 → C3**; on hit optionally **promote** hotter.  
3. On full miss, pull from **HNSW / History** and insert at **C3** upward.

Today the KV tiers are primary; wiring automatic refill from `hms_memory_retrieve_tiered` is future integration work.

---

## 6. EventLog and L3 self-evolution

- Log HMS mutations for **Corrector / Evolver** (via **EventLog** in the L3 agent-runtime column).  
- **Verifier** (L3 evolution column) can state temporal policies like “no active unverified skill”.

---

## 7. Source index

| Topic | Path |
|-------|------|
| History + B⁺ | `agent/history.c`, `agent/hist_bptree.c` |
| WAL scaffold | `agent/history_wal.c` |
| Memory / HNSW | `agent/memory.c`, `agent/memory.h` |
| Skill / ELF | `agent/skill.c`, `agent/skill_elf.c` |
| Cache | `agent/hms_cache.c` |
| Orchestration | `agent/hms_core.c` |

---

## Further reading

- [Architecture](architecture.md) · [L3 self-evolution](evolution.md) · [LC (L2 pillar)](container.md)  
- 简体中文：[hms.md](../zh/hms.md)
