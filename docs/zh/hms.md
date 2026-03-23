# HMS 存储核心（路线图）

- **History**：当前为环形缓冲 + LZ4；**WAL 脚手架**见 `history_wal.c`；B+ 双键索引为后续工作。
- **Memory**：HNSW + `kmalloc` 节点；注释标明未来 L3 可接 mmap/SQLite。
- **Skill**：`skill_elf.c` 预留 ELF 热加载。
- **语义缓存**：`hms_cache.c` 实现 L1 CLOCK 风格驱逐；L2/L3 为冷数据与持久化扩展点。
