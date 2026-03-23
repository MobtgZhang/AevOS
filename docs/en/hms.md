# HMS storage core (roadmap)

- **History**: ring buffer + LZ4 today; **WAL scaffold** in `history_wal.c`; B+ dual-key index is future work.
- **Memory**: HNSW with slab-backed nodes via `kmalloc`; comments note optional L3 mmap/SQLite.
- **Skills**: `skill_elf.c` reserves ELF hot-load.
- **Semantic cache**: `hms_cache.c` implements an L1 CLOCK-style tier; L2/L3 are extension points.
