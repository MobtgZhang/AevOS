# L3 agent layer — self-evolution column (design deep dive)

In the canonical stack (`ideas/ideas2.md`), the **Self-Evolution Plane** is a **column inside L3** alongside **Agent Runtime**: it sits **below L4 Shell** and **above L2 AI infrastructure**, closing the loop **plan → act → correct → verify → update policy**, with a roadmap toward **formal assurance**. Today `src/evolution/` only has **init stubs**; this page specifies **target algorithms and data shapes**, then maps them to **EventLog / History / L2 LC** already in the tree.

---

## 1. Modules and roles

| Module | Intended role | Typical I/O |
|--------|---------------|-------------|
| **Planner** | Decompose goals into executable subtasks | Intent + HMS digest → subgoal sequence or search tree |
| **Corrector** | **Diff rollback** + repair on violations/failures | EventLog slice + failure class → rollback point + patch |
| **Verifier** | **Incremental** **LTL/CTL** (or related) model checking | Transition system (explicit or abstracted from logs) + spec → pass/counterexample |
| **Evolver** | Update policies (e.g. **RL**) and reuse **diff proofs** | Reward/counterexample + params → new policy + (goal) proof delta |

Entry point: `evolution_plane_init()` (currently chains per-module `*_init` and logs).

---

## 2. Planner: ReAct and ToT

### 2.1 ReAct (reason + act)

**Idea**: alternate **Thought** and **Action** until a final answer.

**Skeleton**:

1. Initialize context \(c_0\) (HMS compressed blocks, system prompt).  
2. Until stop: LLM emits thought + planned action; if tool call, execute via **L2** and the **L3 agent-runtime** path, append **EventLog**, fold into \(c_{i+1}\); if final, break.

**Mapping**: the main agent loop is already “LLM → tools”; Planner can add explicit **goal stacks** and **retry templates**.

### 2.2 ToT (tree of thoughts)

**Idea**: maintain **multiple candidate thoughts** as a tree; expand/prune/backtrack using scores.

**Skeleton**:

1. Root = problem.  
2. **Expand** each leaf into \(k\) children (branch cap \(B\)).  
3. **Evaluate** nodes (heuristic or smaller LLM).  
4. **Prune** to top-\(m\); stop on depth/token budget.  
5. **Select** best leaf and trace actions.

**Complexity**: roughly \(O(B^d)\) LLM calls; must cooperate with **Cancel** and **Mailbox** for mid-flight abort.

---

## 3. Corrector: diff rollback and constraints

### 3.1 Differential rollback

**Goal**: avoid replaying all history; undo only the bad suffix.

**History hook**: `history_truncate_keep(history_t *h, uint32_t keep_count)` keeps the oldest `keep_count` logical entries—fits “rollback before checkpoint”.

**Conceptual steps**:

1. Verifier or runtime detects violation.  
2. Find last good **seq** on **EventLog** (or planner checkpoint).  
3. Truncate history and undo external side effects on Memory/Skill (needs **compensating actions** per skill—mostly future work).  
4. Append **EVLOG_CORRECT_ROLLBACK** for Evolver.

### 3.2 Constraint classes

- **Static**: syscall allowlists, memory regions, LC policy (see [container.md](container.md)).  
- **Dynamic**: temporal specs like “never load unverified ELF”—checked by Verifier.

---

## 4. Verifier: incremental LTL/CTL (target)

### 4.1 Where the model comes from

- **Explicit** finite transition system.  
- **Implicit** abstraction from **EventLog** + agent FSM, refined on counterexamples (CEGAR-style loop).

### 4.2 Incrementality (“diff proof reuse” in `ideas`)

When moving \(S \to S'\) with small \(\Delta\):

- **Full**: re-check property \(\phi\) on \(S'\).  
- **Incremental**: reuse certificates from \(S\) and only re-analyze the affected subgraph.

Typical techniques: incremental **IC3/PDR**, incremental **k-induction**, or simulation-based monotonicity—implementation-specific.

### 4.3 LC as a reduction anchor

If **no escape from LC** is proved for skills/containers, many properties compose from **LC interfaces** plus **L1 invariants** instead of whole-program verification per skill.

---

## 5. Evolver: RL + proof reuse

### 5.1 RL view

- **State**: HMS summary, recent EventLog window, resource use.  
- **Actions**: planner hyperparameters (temperature, ToT width), tool biases, remote/local LLM choice.  
- **Reward**: success − latency − large penalty on verifier failure.

### 5.2 Proof reuse

If a policy change only affects **which** tool is chosen inside an unchanged **LC capability set**, safety proofs should ideally stay stable while performance analysis updates—analogous to incremental verification.

---

## 6. Observation hooks in the L3 agent-runtime column

- **EventLog** already reserves `EVLOG_PLAN_STEP`, `EVLOG_CORRECT_ROLLBACK`, `EVLOG_VERIFY`, `EVLOG_EVOLVER`.  
- **agent_t.evolution** can host a per-agent evolution state machine (see source for fields).

---

## 7. Implementation status

| Piece | Status |
|-------|--------|
| `planner.c` / `corrector.c` / `verifier.c` / `evolver.c` | Init stubs with klog banners |
| Closed loop to `llm_sys_*` / HMS / LC | Not wired |

Suggested order: **EventLog subscription + Corrector + `history_truncate_keep`**, then a tiny verifier (enumerated FSM properties), then incremental proofs and RL.

---

## Further reading

- [Architecture](architecture.md) · [HMS (L2 pillar)](hms.md) · [LC (L2 pillar)](container.md)  
- 简体中文：[evolution.md](../zh/evolution.md)
