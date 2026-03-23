#include "lc_layer.h"
#include "kernel/klog.h"
#include "../lib/string.h"

/*
 * Static lattice: rows = source domain, columns = sink domain.
 * Cell = bitmask of LC_IFC_OP_* permitted without a runtime grant.
 */
static uint32_t g_ifc_policy[LC_IFC_DOM_COUNT][LC_IFC_DOM_COUNT];

#define IFC_ALL (LC_IFC_OP_READ | LC_IFC_OP_WRITE | LC_IFC_OP_EXEC)

static void ifc_set(lc_ifc_domain_t a, lc_ifc_domain_t b, uint32_t mask)
{
    g_ifc_policy[a][b] = mask;
}

void lc_ifc_init(void)
{
    for (unsigned i = 0; i < LC_IFC_DOM_COUNT; i++) {
        for (unsigned j = 0; j < LC_IFC_DOM_COUNT; j++)
            g_ifc_policy[i][j] = 0;
    }

    /* Host orchestrator may touch every subsystem. */
    for (unsigned j = 0; j < LC_IFC_DOM_COUNT; j++)
        ifc_set(LC_IFC_DOM_HOST, j, IFC_ALL);

    /* Containers: virtual Linux ABI may read/write host VFS shim, not exec host. */
    ifc_set(LC_IFC_DOM_CONTAINER, LC_IFC_DOM_HOST,
            LC_IFC_OP_READ | LC_IFC_OP_WRITE);
    ifc_set(LC_IFC_DOM_CONTAINER, LC_IFC_DOM_HMS, LC_IFC_OP_READ);
    /* Block container → LLM direct channel (prompt injection). */
    ifc_set(LC_IFC_DOM_CONTAINER, LC_IFC_DOM_LLM, 0);

    /* Skills: tight boundary — may feed LLM (read) and use HMS, not spawn containers. */
    ifc_set(LC_IFC_DOM_SKILL, LC_IFC_DOM_LLM, LC_IFC_OP_READ);
    ifc_set(LC_IFC_DOM_SKILL, LC_IFC_DOM_HMS,
            LC_IFC_OP_READ | LC_IFC_OP_WRITE);
    ifc_set(LC_IFC_DOM_SKILL, LC_IFC_DOM_HOST, 0);

    /* LLM runtime reads HMS context, writes only back to host/agent path. */
    ifc_set(LC_IFC_DOM_LLM, LC_IFC_DOM_HMS, LC_IFC_OP_READ);
    ifc_set(LC_IFC_DOM_LLM, LC_IFC_DOM_HOST, LC_IFC_OP_WRITE);

    /* HMS is storage plane — skills/containers read metadata; writes from host/agent. */
    ifc_set(LC_IFC_DOM_HMS, LC_IFC_DOM_HOST, LC_IFC_OP_READ | LC_IFC_OP_WRITE);
    ifc_set(LC_IFC_DOM_HMS, LC_IFC_DOM_SKILL, LC_IFC_OP_READ);

    klog("lc: IFC lattice init (container↛LLM default deny)\n");
}

bool lc_ifc_flow_allowed(lc_ifc_domain_t from, lc_ifc_domain_t to, uint32_t ops)
{
    if (from >= LC_IFC_DOM_COUNT || to >= LC_IFC_DOM_COUNT)
        return false;
    uint32_t cell = g_ifc_policy[from][to];
    return (cell & ops) == ops;
}

void lc_ifc_grant(lc_ifc_domain_t from, lc_ifc_domain_t to, uint32_t ops)
{
    if (from >= LC_IFC_DOM_COUNT || to >= LC_IFC_DOM_COUNT)
        return;
    g_ifc_policy[from][to] |= ops;
}

void lc_ifc_revoke(lc_ifc_domain_t from, lc_ifc_domain_t to, uint32_t ops)
{
    if (from >= LC_IFC_DOM_COUNT || to >= LC_IFC_DOM_COUNT)
        return;
    g_ifc_policy[from][to] &= ~ops;
}
