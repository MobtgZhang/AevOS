#include "aevos_db.h"
#include "kernel/mm/slab.h"
#include "kernel/klog.h"
#include "kernel/drivers/timer.h"
#include "lib/string.h"

/*
 * AevOS Database — in-memory fallback with conversation-tree support.
 *
 * Each message has a parent_id, forming a tree per session.
 * Walking from any leaf to the root yields one linear conversation path.
 */

#define DB_MAX_SESSIONS    64
#define DB_MAX_MESSAGES    4096
#define DB_MAX_MEM_ENTRIES 8192
#define DB_MAX_SKILLS      256

typedef struct {
    uint64_t id;
    uint64_t session_id;
    uint64_t parent_id;       /* 0 = root */
    uint64_t timestamp;
    uint8_t  role;
    uint32_t token_count;
    char    *text;
    uint32_t text_len;
    uint32_t child_count;
    uint32_t depth;
} imsg_t;

struct aevos_db {
    db_session_t   sessions[DB_MAX_SESSIONS];
    uint32_t       session_count;
    uint64_t       next_session_id;

    imsg_t         messages[DB_MAX_MESSAGES];
    uint32_t       message_count;
    uint64_t       next_msg_id;

    db_mem_entry_t mem_entries[DB_MAX_MEM_ENTRIES];
    uint32_t       mem_count;

    db_skill_t     skills[DB_MAX_SKILLS];
    uint32_t       skill_count;

    spinlock_t     lock;
};

/* ── Helpers ───────────────────────────────────────────── */

static imsg_t *find_msg_by_id(aevos_db_t *db, uint64_t msg_id)
{
    for (uint32_t i = 0; i < db->message_count; i++) {
        if (db->messages[i].id == msg_id)
            return &db->messages[i];
    }
    return NULL;
}

static void fill_db_message(db_message_t *out, const imsg_t *m)
{
    out->id          = m->id;
    out->session_id  = m->session_id;
    out->parent_id   = m->parent_id;
    out->timestamp   = m->timestamp;
    out->role        = m->role;
    out->token_count = m->token_count;
    out->text_len    = m->text_len;
    out->child_count = m->child_count;
    out->depth       = m->depth;

    out->text = (char *)kmalloc(m->text_len + 1);
    if (out->text)
        memcpy(out->text, m->text, m->text_len + 1);
}

/* ── Lifecycle ─────────────────────────────────────────── */

int aevos_db_open(aevos_db_t **db, const char *path)
{
    (void)path;
    if (!db) return -EINVAL;

    aevos_db_t *d = (aevos_db_t *)kcalloc(1, sizeof(aevos_db_t));
    if (!d) return -ENOMEM;

    d->next_session_id = 1;
    d->next_msg_id     = 1;
    d->lock            = SPINLOCK_INIT;

    *db = d;
    klog("[db] database opened (in-memory, tree-history)\n");
    return 0;
}

void aevos_db_close(aevos_db_t *db)
{
    if (!db) return;

    for (uint32_t i = 0; i < db->message_count; i++) {
        if (db->messages[i].text)
            kfree(db->messages[i].text);
    }
    for (uint32_t i = 0; i < db->mem_count; i++) {
        if (db->mem_entries[i].content)
            kfree(db->mem_entries[i].content);
        if (db->mem_entries[i].embedding)
            kfree(db->mem_entries[i].embedding);
    }
    for (uint32_t i = 0; i < db->skill_count; i++) {
        if (db->skills[i].elf_blob)
            kfree(db->skills[i].elf_blob);
    }

    kfree(db);
    klog("[db] database closed\n");
}

/* ── Session management ────────────────────────────────── */

int aevos_db_session_create(aevos_db_t *db, const char *title,
                            uint64_t *out_session_id)
{
    if (!db || !title) return -EINVAL;

    spin_lock(&db->lock);
    if (db->session_count >= DB_MAX_SESSIONS) {
        spin_unlock(&db->lock);
        return -ENOSPC;
    }

    db_session_t *s = &db->sessions[db->session_count];
    s->session_id      = db->next_session_id++;
    s->created_at      = timer_get_ms();
    s->updated_at      = s->created_at;
    s->message_count   = 0;
    s->active_leaf_id  = 0;
    strncpy(s->title, title, sizeof(s->title) - 1);
    s->title[sizeof(s->title) - 1] = '\0';

    db->session_count++;
    if (out_session_id) *out_session_id = s->session_id;

    spin_unlock(&db->lock);
    return 0;
}

int aevos_db_session_list(aevos_db_t *db, db_session_t *out,
                          uint32_t max_count, uint32_t *out_count)
{
    if (!db || !out) return -EINVAL;

    spin_lock(&db->lock);
    uint32_t n = MIN(db->session_count, max_count);

    for (uint32_t i = 0; i < n; i++)
        out[i] = db->sessions[i];

    /* Sort by updated_at descending (newest first) */
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = i + 1; j < n; j++) {
            if (out[j].updated_at > out[i].updated_at) {
                db_session_t tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }

    if (out_count) *out_count = n;
    spin_unlock(&db->lock);
    return 0;
}

int aevos_db_session_delete(aevos_db_t *db, uint64_t session_id)
{
    if (!db) return -EINVAL;
    spin_lock(&db->lock);

    uint32_t w = 0;
    for (uint32_t r = 0; r < db->message_count; r++) {
        if (db->messages[r].session_id == session_id) {
            if (db->messages[r].text) kfree(db->messages[r].text);
            continue;
        }
        if (w != r) db->messages[w] = db->messages[r];
        w++;
    }
    db->message_count = w;

    for (uint32_t i = 0; i < db->session_count; i++) {
        if (db->sessions[i].session_id == session_id) {
            for (uint32_t j = i; j < db->session_count - 1; j++)
                db->sessions[j] = db->sessions[j + 1];
            db->session_count--;
            break;
        }
    }

    spin_unlock(&db->lock);
    return 0;
}

int aevos_db_session_set_active_leaf(aevos_db_t *db, uint64_t session_id,
                                      uint64_t leaf_msg_id)
{
    if (!db) return -EINVAL;
    spin_lock(&db->lock);

    for (uint32_t i = 0; i < db->session_count; i++) {
        if (db->sessions[i].session_id == session_id) {
            db->sessions[i].active_leaf_id = leaf_msg_id;
            spin_unlock(&db->lock);
            return 0;
        }
    }

    spin_unlock(&db->lock);
    return -ENOENT;
}

/* ── History (conversation tree) ───────────────────────── */

int aevos_db_history_push(aevos_db_t *db, uint64_t session_id,
                          uint64_t parent_id, uint8_t role,
                          const char *text, uint32_t token_count,
                          uint64_t *out_msg_id)
{
    if (!db || !text) return -EINVAL;
    spin_lock(&db->lock);

    if (db->message_count >= DB_MAX_MESSAGES) {
        spin_unlock(&db->lock);
        return -ENOSPC;
    }

    /* Compute depth and increment parent's child_count */
    uint32_t depth = 0;
    if (parent_id != 0) {
        imsg_t *parent = find_msg_by_id(db, parent_id);
        if (parent) {
            depth = parent->depth + 1;
            parent->child_count++;
        }
    }

    uint32_t len = (uint32_t)strlen(text);
    char *copy = (char *)kmalloc(len + 1);
    if (!copy) { spin_unlock(&db->lock); return -ENOMEM; }
    memcpy(copy, text, len + 1);

    imsg_t *m = &db->messages[db->message_count];
    m->id          = db->next_msg_id++;
    m->session_id  = session_id;
    m->parent_id   = parent_id;
    m->timestamp   = timer_get_ms();
    m->role        = role;
    m->token_count = token_count;
    m->text        = copy;
    m->text_len    = len;
    m->child_count = 0;
    m->depth       = depth;
    db->message_count++;

    /* Update session */
    for (uint32_t i = 0; i < db->session_count; i++) {
        if (db->sessions[i].session_id == session_id) {
            db->sessions[i].updated_at = m->timestamp;
            db->sessions[i].message_count++;
            db->sessions[i].active_leaf_id = m->id;
            break;
        }
    }

    if (out_msg_id) *out_msg_id = m->id;
    spin_unlock(&db->lock);
    return 0;
}

int aevos_db_history_get(aevos_db_t *db, uint64_t session_id,
                         db_message_t *out, uint32_t max_count,
                         uint32_t *out_count)
{
    if (!db || !out) return -EINVAL;
    spin_lock(&db->lock);

    uint32_t found = 0;
    for (uint32_t i = 0; i < db->message_count && found < max_count; i++) {
        imsg_t *m = &db->messages[i];
        if (m->session_id != session_id) continue;
        fill_db_message(&out[found], m);
        found++;
    }

    if (out_count) *out_count = found;
    spin_unlock(&db->lock);
    return 0;
}

/*
 * Walk from leaf_id up to root, collecting the linear path.
 * Result is ordered root→leaf (chronological).
 */
int aevos_db_history_get_branch(aevos_db_t *db, uint64_t leaf_id,
                                db_message_t *out, uint32_t max_count,
                                uint32_t *out_count)
{
    if (!db || !out) return -EINVAL;
    spin_lock(&db->lock);

    /* Trace from leaf to root, collecting into a temp stack */
    uint64_t stack[512];
    uint32_t depth = 0;
    uint64_t cur = leaf_id;

    while (cur != 0 && depth < 512) {
        stack[depth++] = cur;
        imsg_t *m = find_msg_by_id(db, cur);
        if (!m) break;
        cur = m->parent_id;
    }

    /* Reverse into output (root first) */
    uint32_t n = MIN(depth, max_count);
    for (uint32_t i = 0; i < n; i++) {
        imsg_t *m = find_msg_by_id(db, stack[depth - 1 - i]);
        if (m) fill_db_message(&out[i], m);
    }

    if (out_count) *out_count = n;
    spin_unlock(&db->lock);
    return 0;
}

int aevos_db_history_get_children(aevos_db_t *db, uint64_t parent_id,
                                  db_message_t *out, uint32_t max_count,
                                  uint32_t *out_count)
{
    if (!db || !out) return -EINVAL;
    spin_lock(&db->lock);

    uint32_t found = 0;
    for (uint32_t i = 0; i < db->message_count && found < max_count; i++) {
        imsg_t *m = &db->messages[i];
        if (m->parent_id != parent_id) continue;
        fill_db_message(&out[found], m);
        found++;
    }

    if (out_count) *out_count = found;
    spin_unlock(&db->lock);
    return 0;
}

int aevos_db_history_search(aevos_db_t *db, uint64_t session_id,
                            const char *keyword,
                            db_message_t *out, uint32_t max_count,
                            uint32_t *out_count)
{
    if (!db || !keyword || !out) return -EINVAL;
    spin_lock(&db->lock);

    uint32_t found = 0;
    for (uint32_t i = 0; i < db->message_count && found < max_count; i++) {
        imsg_t *m = &db->messages[i];
        if (session_id != 0 && m->session_id != session_id) continue;
        if (!m->text || !strstr(m->text, keyword)) continue;
        fill_db_message(&out[found], m);
        found++;
    }

    if (out_count) *out_count = found;
    spin_unlock(&db->lock);
    return 0;
}

int aevos_db_history_clear(aevos_db_t *db, uint64_t session_id)
{
    if (!db) return -EINVAL;
    spin_lock(&db->lock);

    uint32_t w = 0;
    for (uint32_t r = 0; r < db->message_count; r++) {
        if (db->messages[r].session_id == session_id) {
            if (db->messages[r].text) kfree(db->messages[r].text);
            continue;
        }
        if (w != r) db->messages[w] = db->messages[r];
        w++;
    }
    db->message_count = w;

    for (uint32_t i = 0; i < db->session_count; i++) {
        if (db->sessions[i].session_id == session_id) {
            db->sessions[i].message_count = 0;
            db->sessions[i].active_leaf_id = 0;
            break;
        }
    }

    spin_unlock(&db->lock);
    return 0;
}

/* ── Memory store ──────────────────────────────────────── */

int aevos_db_memory_store(aevos_db_t *db, const db_mem_entry_t *entry)
{
    if (!db || !entry) return -EINVAL;
    spin_lock(&db->lock);

    if (db->mem_count >= DB_MAX_MEM_ENTRIES) {
        spin_unlock(&db->lock);
        return -ENOSPC;
    }

    db_mem_entry_t *e = &db->mem_entries[db->mem_count];
    *e = *entry;

    if (entry->content && entry->content_len > 0) {
        e->content = (uint8_t *)kmalloc(entry->content_len);
        if (e->content)
            memcpy(e->content, entry->content, entry->content_len);
    }
    if (entry->embedding && entry->embed_dim > 0) {
        e->embedding = (int8_t *)kmalloc(entry->embed_dim);
        if (e->embedding)
            memcpy(e->embedding, entry->embedding, entry->embed_dim);
    }

    db->mem_count++;
    spin_unlock(&db->lock);
    return 0;
}

int aevos_db_memory_get(aevos_db_t *db, uint64_t id, db_mem_entry_t *out)
{
    if (!db || !out) return -EINVAL;
    spin_lock(&db->lock);

    for (uint32_t i = 0; i < db->mem_count; i++) {
        if (db->mem_entries[i].id == id) {
            *out = db->mem_entries[i];
            out->content   = NULL;
            out->embedding = NULL;

            db_mem_entry_t *e = &db->mem_entries[i];
            if (e->content && e->content_len > 0) {
                out->content = (uint8_t *)kmalloc(e->content_len);
                if (out->content)
                    memcpy(out->content, e->content, e->content_len);
            }
            if (e->embedding && e->embed_dim > 0) {
                out->embedding = (int8_t *)kmalloc(e->embed_dim);
                if (out->embedding)
                    memcpy(out->embedding, e->embedding, e->embed_dim);
            }

            spin_unlock(&db->lock);
            return 0;
        }
    }
    spin_unlock(&db->lock);
    return -ENOENT;
}

int aevos_db_memory_update_importance(aevos_db_t *db, uint64_t id,
                                      float new_importance)
{
    if (!db) return -EINVAL;
    spin_lock(&db->lock);

    for (uint32_t i = 0; i < db->mem_count; i++) {
        if (db->mem_entries[i].id == id) {
            db->mem_entries[i].importance = new_importance;
            spin_unlock(&db->lock);
            return 0;
        }
    }
    spin_unlock(&db->lock);
    return -ENOENT;
}

int aevos_db_memory_delete(aevos_db_t *db, uint64_t id)
{
    if (!db) return -EINVAL;
    spin_lock(&db->lock);

    for (uint32_t i = 0; i < db->mem_count; i++) {
        if (db->mem_entries[i].id == id) {
            if (db->mem_entries[i].content)
                kfree(db->mem_entries[i].content);
            if (db->mem_entries[i].embedding)
                kfree(db->mem_entries[i].embedding);
            for (uint32_t j = i; j < db->mem_count - 1; j++)
                db->mem_entries[j] = db->mem_entries[j + 1];
            db->mem_count--;
            spin_unlock(&db->lock);
            return 0;
        }
    }
    spin_unlock(&db->lock);
    return -ENOENT;
}

uint32_t aevos_db_memory_count(aevos_db_t *db)
{
    return db ? db->mem_count : 0;
}

/* ── Skill store ───────────────────────────────────────── */

int aevos_db_skill_save(aevos_db_t *db, const db_skill_t *skill)
{
    if (!db || !skill) return -EINVAL;
    spin_lock(&db->lock);

    for (uint32_t i = 0; i < db->skill_count; i++) {
        if (strncmp(db->skills[i].name, skill->name, 64) == 0) {
            if (db->skills[i].elf_blob) kfree(db->skills[i].elf_blob);
            db->skills[i] = *skill;
            db->skills[i].elf_blob = NULL;
            if (skill->elf_blob && skill->elf_blob_len > 0) {
                db->skills[i].elf_blob = (uint8_t *)kmalloc(skill->elf_blob_len);
                if (db->skills[i].elf_blob)
                    memcpy(db->skills[i].elf_blob, skill->elf_blob, skill->elf_blob_len);
            }
            spin_unlock(&db->lock);
            return 0;
        }
    }

    if (db->skill_count >= DB_MAX_SKILLS) {
        spin_unlock(&db->lock);
        return -ENOSPC;
    }

    db->skills[db->skill_count] = *skill;
    db->skills[db->skill_count].elf_blob = NULL;
    if (skill->elf_blob && skill->elf_blob_len > 0) {
        db->skills[db->skill_count].elf_blob =
            (uint8_t *)kmalloc(skill->elf_blob_len);
        if (db->skills[db->skill_count].elf_blob)
            memcpy(db->skills[db->skill_count].elf_blob,
                   skill->elf_blob, skill->elf_blob_len);
    }
    db->skill_count++;

    spin_unlock(&db->lock);
    return 0;
}

int aevos_db_skill_load(aevos_db_t *db, const char *name, db_skill_t *out)
{
    if (!db || !name || !out) return -EINVAL;
    spin_lock(&db->lock);

    for (uint32_t i = 0; i < db->skill_count; i++) {
        if (strncmp(db->skills[i].name, name, 64) == 0) {
            *out = db->skills[i];
            out->elf_blob = NULL;
            if (db->skills[i].elf_blob && db->skills[i].elf_blob_len > 0) {
                out->elf_blob = (uint8_t *)kmalloc(db->skills[i].elf_blob_len);
                if (out->elf_blob)
                    memcpy(out->elf_blob, db->skills[i].elf_blob,
                           db->skills[i].elf_blob_len);
            }
            spin_unlock(&db->lock);
            return 0;
        }
    }
    spin_unlock(&db->lock);
    return -ENOENT;
}

int aevos_db_skill_list(aevos_db_t *db, db_skill_t *out,
                        uint32_t max_count, uint32_t *out_count)
{
    if (!db || !out) return -EINVAL;
    spin_lock(&db->lock);

    uint32_t n = MIN(db->skill_count, max_count);
    for (uint32_t i = 0; i < n; i++) {
        out[i] = db->skills[i];
        out[i].elf_blob = NULL;
    }
    if (out_count) *out_count = n;

    spin_unlock(&db->lock);
    return 0;
}

int aevos_db_skill_delete(aevos_db_t *db, uint64_t skill_id)
{
    if (!db) return -EINVAL;
    spin_lock(&db->lock);

    for (uint32_t i = 0; i < db->skill_count; i++) {
        if (db->skills[i].id == skill_id) {
            if (db->skills[i].elf_blob) kfree(db->skills[i].elf_blob);
            for (uint32_t j = i; j < db->skill_count - 1; j++)
                db->skills[j] = db->skills[j + 1];
            db->skill_count--;
            spin_unlock(&db->lock);
            return 0;
        }
    }
    spin_unlock(&db->lock);
    return -ENOENT;
}

/* ── Free helpers ──────────────────────────────────────── */

void aevos_db_free_message(db_message_t *msg)
{
    if (msg && msg->text) {
        kfree(msg->text);
        msg->text = NULL;
    }
}

void aevos_db_free_mem_entry(db_mem_entry_t *entry)
{
    if (!entry) return;
    if (entry->content)   { kfree(entry->content);   entry->content   = NULL; }
    if (entry->embedding) { kfree(entry->embedding); entry->embedding = NULL; }
}

void aevos_db_free_skill(db_skill_t *skill)
{
    if (skill && skill->elf_blob) {
        kfree(skill->elf_blob);
        skill->elf_blob = NULL;
    }
}
