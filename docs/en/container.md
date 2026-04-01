# LC — container compatibility layer (Docker / Linux personality / skill sandbox)

In the canonical stack (`ideas/ideas2.md`), **LC** is the **middle pillar** of **L2 AI infrastructure**, **peer** to **LLM runtime** and **HMS**—not a separate “layer number” wedged between L3 and L2. This page explains **why LC shares L2**, its **internal subsystems**, the **formal reduction** to **L3 Verifier** + **L1**, and what exists under `src/container/` today.

---

## 1. Placement semantics (L2 pillar)

1. **Skill path**: the **HMS pillar** stores/registers skills; dynamic ELF must pass the **LC pillar** (bounds, syscall allowlists, **IFC**) before calling the **LLM pillar**’s `llm_sys_*` or controlled drivers.  
2. **Docker-class workloads**: OCI needs **overlayfs, rootfs, namespaces, cgroups**—too heavy for the slim **L1** core. LC uses **Linux ABI translation** and **procfs/devfs** views.  
3. **Verification reduction**: if LC is **non-escapable**, **L3 Verifier** can **compose** properties down to **L1** invariants (**diff proof reuse**), avoiding per-skill whole-program proofs.

---

## 2. Subsystem A — OCI runtime (`oci_runtime.c`)

**Goal**: Docker-ish images with **layered rootfs** (overlay semantics) and a trimmed **runc-like** lifecycle on bare metal.

**Structure**:

- **Image**: manifest + layer tarballs → read-only lowers; upper/work for writes.  
- **Mount**: merged view as container `/`; integrates with the kernel VFS mount table.  
- **Lifecycle**: create/start/stop/delete with **cgroup mapping** to scheduler and PMM policies.

**Algorithms**: overlay resolves reads by **walking the layer stack**; writes are **copy-on-write** into upper. Watch page alignment and ordering with block caches.

**Status**: scaffold-level today.

---

## 3. Subsystem B — Linux compat (`src/linux/`, migrated from `linux_subsys.c`)

**Boundary**: **`src/container/`** keeps OCI lifecycle, syscall allowlists, and IFC. **`src/linux/`** holds **Linux ABI metadata** (x86_64 syscall name table, `linux_syscall_dispatch_*` domain routing), in a **FreeBSD linuxulator-style** split.

**Goal**: run Linux ELFs (e.g. **static musl**) by **translating syscall numbers** to AevOS POSIX/VFS or user-space services (Cap-IPC roadmap).

**Structure**:

- **Dispatch table**: `linux_nr → dispatch_domain → stub/service`; uncovered → `LINUX_DISPATCH_ENOSYS`.  
- **Virtual FS**: `/proc`, `/dev` from **procfs/devfs**; LC trims per-process namespaces.  
- **OCI hook**: a successful `docker run` slot logs **read/clone** syscall → domain mapping (see `lc_container.c`).

**Algorithms**: hot path is **dispatch + marshalling**; avoid double copies (`iovec`-style mapping into the POSIX layer).

---

## 4. Subsystem C — skill sandbox (`sandbox.c`) + IFC (`ifc.c`)

### 4.1 Sandbox

**Goal**: load `skill_elf` ELFs in a **restricted mode**:

- Fixed **text/data** ranges; **W^X** when the platform allows.  
- **Syscall allowlist** (files, HMS APIs, proxied `llm_sys_infer`, …).  
- **Quotas**: time slices and allocation caps via scheduler + mm policies.

**Skeleton**:

1. Map ELF PT_LOAD segments → allowed VA set \(R\).  
2. Install **trampolines** so every syscall hits LC policy; violations → terminate + **EventLog**.  
3. Prefer **static PIC** if dynamic linking is absent.

### 4.2 IFC (information-flow control)

**Goal**: mitigate **prompt injection** and **cross-agent leaks**—label objects `{public, user_secret, model_output, …}` and enforce a **lattice** (or policy language) on flows.

**Concept**:

- Pages, fds, mailbox messages carry **labels**.  
- At syscall boundaries enforce \(L_{src} \leq L_{dst}\).  
- **Verifier** can state “no flow from `user_secret` to `network`”.

---

## 5. Scheduling domain (`ideas` summary)

Treat **Docker** as a **heavy skill** coexisting with **lightweight coroutines**: container processes map to **POSIX processes + blocking**; native agents keep **four tool states + coro_yield**. LC keeps each world inside its **policy bundle**.

---

## 6. Init order

`lc_layer_init()` (`lc_layer.c`) calls:

1. `lc_sandbox_init`  
2. `lc_ifc_init`  
3. `linux_compat_init` (`src/linux/`)  
4. `lc_oci_init`  

Invoked from `kernel_main` next to `evolution_plane_init`, after **VFS/POSIX/net** are up.

---

## 7. Status and roadmap

| File | Role today |
|------|------------|
| `sandbox.c` | Skill/ELF isolation, TinyCC path — **TBD** |
| `ifc.c` | Labels/policies — **TBD / partial stubs** |
| `src/linux/*.c` | Linux ABI table + dispatch domains — **incremental** |
| `oci_runtime.c` | OCI/overlay — **scaffold** |
| `lc_container.c` | Aggregated container/CLI backend pieces |

Suggested order: **ELF load + syscall trap + allowlist** minimal loop, then **IFC lattice**, then full **OCI lifecycle**.

---

## Further reading

- [Architecture](architecture.md) · [L3 self-evolution](evolution.md) · [LLM syscall (L2 pillar)](llm-syscall.md)  
- 简体中文：[container.md](../zh/container.md)
