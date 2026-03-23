#pragma once

#include <aevos/types.h>
#include "memory.h"
#include "history.h"
#include "skill.h"
#include "hms_cache.h"

/*
 * HMS 存储平面编排：Memory 走 L1→L2→L3→HNSW（外存类比），
 * History 由 B+ 树按 seq 索引，Skill 经注册表按名解析。
 * 生成带 [M:…] / [H:…] / @skill#id 的压缩引用块以节省上下文 tokens。
 */
void hms_text_to_pseudo_embed(const char *text, int8_t *out, uint32_t dim);

int hms_memory_retrieve_tiered(memory_engine_t *mem, hms_cache_t *cache,
                               const int8_t *qemb, uint32_t top_k,
                               mem_result_t *results, hms_mem_tier_t *resolved_tier);

size_t hms_append_compressed_block(memory_engine_t *mem, history_t *hist,
                                   skill_engine_t *skills, hms_cache_t *cache,
                                   const char *user_text,
                                   char *out, size_t out_max);
