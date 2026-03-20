#pragma once

#include <aevos/types.h>

/*
 * AevOS Database Layer — persistent storage for History, Memory, and Skills.
 *
 * History uses a conversation-tree model (like OpenAI's chat UI):
 * each message has a parent_id, allowing branching when the user
 * edits a previous message.  A "path" through the tree represents
 * one linear conversation.
 */

/* ── Opaque handle ──────────────────────────────────────── */

typedef struct aevos_db aevos_db_t;

/* ── Conversation session ───────────────────────────────── */

typedef struct {
    uint64_t session_id;
    uint64_t created_at;
    uint64_t updated_at;
    char     title[128];
    uint32_t message_count;
    uint64_t active_leaf_id;  /* current "head" of the active branch */
} db_session_t;

/* ── History message row (tree node) ────────────────────── */

typedef struct {
    uint64_t id;
    uint64_t session_id;
    uint64_t parent_id;       /* 0 = root (no parent) */
    uint64_t timestamp;
    uint8_t  role;            /* HIST_ROLE_USER / ASSISTANT / SYSTEM / TOOL */
    uint32_t token_count;
    char    *text;            /* caller must free via kfree() */
    uint32_t text_len;
    uint32_t child_count;     /* number of direct children */
    uint32_t depth;           /* depth in the tree (root = 0) */
} db_message_t;

/* ── Memory entry row ───────────────────────────────────── */

typedef struct {
    uint64_t id;
    float    importance;
    uint64_t last_access;
    uint32_t access_count;
    uint8_t *content;
    uint32_t content_len;
    int8_t  *embedding;
    uint32_t embed_dim;
} db_mem_entry_t;

/* ── Skill row ──────────────────────────────────────────── */

typedef struct {
    uint64_t id;
    char     name[64];
    char     description[256];
    char     signature[128];
    float    success_rate;
    uint32_t call_count;
    uint32_t fail_count;
    uint64_t created_at;
    uint64_t last_used;
    uint32_t version;
    bool     active;
    uint8_t *elf_blob;
    uint32_t elf_blob_len;
} db_skill_t;

/* ── Lifecycle ──────────────────────────────────────────── */

int  aevos_db_open(aevos_db_t **db, const char *path);
void aevos_db_close(aevos_db_t *db);

/* ── Session management ─────────────────────────────────── */

int aevos_db_session_create(aevos_db_t *db, const char *title,
                            uint64_t *out_session_id);
int aevos_db_session_list(aevos_db_t *db, db_session_t *out,
                          uint32_t max_count, uint32_t *out_count);
int aevos_db_session_delete(aevos_db_t *db, uint64_t session_id);
int aevos_db_session_set_active_leaf(aevos_db_t *db, uint64_t session_id,
                                      uint64_t leaf_msg_id);

/* ── History (conversation tree) ────────────────────────── */

int aevos_db_history_push(aevos_db_t *db, uint64_t session_id,
                          uint64_t parent_id, uint8_t role,
                          const char *text, uint32_t token_count,
                          uint64_t *out_msg_id);
int aevos_db_history_get(aevos_db_t *db, uint64_t session_id,
                         db_message_t *out, uint32_t max_count,
                         uint32_t *out_count);
int aevos_db_history_get_branch(aevos_db_t *db, uint64_t leaf_id,
                                db_message_t *out, uint32_t max_count,
                                uint32_t *out_count);
int aevos_db_history_get_children(aevos_db_t *db, uint64_t parent_id,
                                  db_message_t *out, uint32_t max_count,
                                  uint32_t *out_count);
int aevos_db_history_search(aevos_db_t *db, uint64_t session_id,
                            const char *keyword,
                            db_message_t *out, uint32_t max_count,
                            uint32_t *out_count);
int aevos_db_history_clear(aevos_db_t *db, uint64_t session_id);

/* ── Memory store ───────────────────────────────────────── */

int      aevos_db_memory_store(aevos_db_t *db, const db_mem_entry_t *entry);
int      aevos_db_memory_get(aevos_db_t *db, uint64_t id,
                             db_mem_entry_t *out);
int      aevos_db_memory_update_importance(aevos_db_t *db, uint64_t id,
                                           float new_importance);
int      aevos_db_memory_delete(aevos_db_t *db, uint64_t id);
uint32_t aevos_db_memory_count(aevos_db_t *db);

/* ── Skill store ────────────────────────────────────────── */

int  aevos_db_skill_save(aevos_db_t *db, const db_skill_t *skill);
int  aevos_db_skill_load(aevos_db_t *db, const char *name,
                         db_skill_t *out);
int  aevos_db_skill_list(aevos_db_t *db, db_skill_t *out,
                         uint32_t max_count, uint32_t *out_count);
int  aevos_db_skill_delete(aevos_db_t *db, uint64_t skill_id);

/* ── Utility ────────────────────────────────────────────── */

void aevos_db_free_message(db_message_t *msg);
void aevos_db_free_mem_entry(db_mem_entry_t *entry);
void aevos_db_free_skill(db_skill_t *skill);
