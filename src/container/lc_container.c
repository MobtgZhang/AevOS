#include "lc_layer.h"
#include <aevos/config.h>
#include "kernel/klog.h"
#include "linux/linux_abi.h"
#include "../lib/string.h"

static lc_container_slot_t g_slots[LC_MAX_CONTAINERS];
static uint64_t             g_next_id = 1;

static lc_container_slot_t *slot_alloc(void)
{
    for (int i = 0; i < LC_MAX_CONTAINERS; i++) {
        if (g_slots[i].id == 0)
            return &g_slots[i];
    }
    return NULL;
}

static void slot_free(lc_container_slot_t *s)
{
    memset(s, 0, sizeof(*s));
}

static lc_container_slot_t *find_slot(const char *q)
{
    if (!q || !*q)
        return NULL;

    lc_container_slot_t *prefix = NULL;
    int prefix_hits = 0;

    for (int i = 0; i < LC_MAX_CONTAINERS; i++) {
        lc_container_slot_t *s = &g_slots[i];
        if (s->id == 0)
            continue;

        if (strcmp(s->id_str, q) == 0 || strcmp(s->name, q) == 0)
            return s;

        size_t ql = strlen(q);
        if (ql >= 4 && strncmp(s->id_str, q, ql) == 0) {
            prefix = s;
            prefix_hits++;
        }
    }

    if (prefix_hits == 1)
        return prefix;
    return NULL;
}

static void lower_path_for_image(const char *image, char *out, size_t cap)
{
    /* Synthetic OCI layout — real pulls would populate /lc/images/<digest>/rootfs */
    snprintf(out, cap, "/lc/images/%s/rootfs", image);
}

int lc_docker_version(char *out, size_t cap)
{
    if (!out || cap == 0)
        return -1;
    snprintf(out, cap,
             "Client: AevOS-LC 0.1 (docker CLI subset)\n"
             "Server: AevOS OCI shim / Linux ABI bridge\n"
             " API version: 1.41 (compatible surface)\n"
             " OS/Arch: AevOS / multi-arch kernel + x86_64 syscall table\n");
    return 0;
}

int lc_docker_info(char *out, size_t cap)
{
    if (!out || cap == 0)
        return -1;
    snprintf(out, cap,
             "Containers: running (LC slots=%d)\n"
             " LC: OCI bundle + overlayfs semantics (shim)\n"
             " Kernel heap: %llu MiB\n"
             " IFC: container→LLM default deny\n"
             " Sandbox: skill vs docker syscall profiles active\n",
             LC_MAX_CONTAINERS,
             (unsigned long long)(KERNEL_HEAP_SIZE / (1024 * 1024)));
    return 0;
}

int lc_docker_ps(lc_container_slot_t *out, int max_slots, int *n_out)
{
    if (!n_out)
        return -1;
    *n_out = 0;
    if (!out || max_slots <= 0)
        return -1;

    int w = 0;
    for (int i = 0; i < LC_MAX_CONTAINERS && w < max_slots; i++) {
        if (g_slots[i].id == 0)
            continue;
        out[w++] = g_slots[i];
    }
    *n_out = w;
    return 0;
}

int lc_docker_images(char *out, size_t cap)
{
    if (!out || cap == 0)
        return -1;

    static const char *catalog[] = {
        "busybox    latest    ca1f010a0a01   LC catalog   1.2 MiB",
        "alpine     latest    c1a4a0b0c2d3   LC catalog   3.4 MiB",
        "hello-world latest   feb5d9fea6a5   LC catalog   16 KiB",
    };

    size_t pos = 0;
    int n = snprintf(out + pos, cap - pos,
                     "REPOSITORY   TAG       IMAGE ID      SOURCE      SIZE\n");
    if (n > 0)
        pos += (size_t)n;

    for (size_t i = 0; i < ARRAY_SIZE(catalog); i++) {
        n = snprintf(out + pos, cap - pos, "%s\n", catalog[i]);
        if (n < 0 || (size_t)n >= cap - pos)
            break;
        pos += (size_t)n;
    }

    for (int i = 0; i < LC_MAX_CONTAINERS; i++) {
        if (g_slots[i].id == 0)
            continue;
        n = snprintf(out + pos, cap - pos,
                     "%-12s (in-use)  slot %-6llu  container   —\n",
                     g_slots[i].image,
                     (unsigned long long)g_slots[i].id);
        if (n < 0 || (size_t)n >= cap - pos)
            break;
        pos += (size_t)n;
    }

    return 0;
}

int lc_docker_run(const char *image, const char *cmdline_rest,
                  char *msg_out, size_t msg_cap)
{
    if (!image || !*image || !msg_out || msg_cap == 0)
        return -1;

    if (!lc_ifc_flow_allowed(LC_IFC_DOM_CONTAINER, LC_IFC_DOM_HOST,
                             LC_IFC_OP_READ | LC_IFC_OP_WRITE)) {
        snprintf(msg_out, msg_cap,
                 "LC: IFC denied container→host I/O; start aborted");
        return -1;
    }

    lc_container_slot_t *s = slot_alloc();
    if (!s) {
        snprintf(msg_out, msg_cap, "LC: no free container slots (max=%d)",
                 LC_MAX_CONTAINERS);
        return -1;
    }

    s->id = g_next_id++;
    snprintf(s->id_str, sizeof(s->id_str), "aevos_lc_%llu",
             (unsigned long long)s->id);
    snprintf(s->name, sizeof(s->name), "focused_%llu",
             (unsigned long long)s->id);
    strncpy(s->image, image, sizeof(s->image) - 1);
    s->image[sizeof(s->image) - 1] = '\0';
    s->state = LC_CTR_RUNNING;

    char lower[160];
    lower_path_for_image(image, lower, sizeof(lower));
    lc_oci_register_bundle(s->id, image, lower);

    klog("lc: OCI slot %llu linux shim read→%s domain=%d clone→domain=%d\n",
         (unsigned long long)s->id,
         linux_abi_x64_syscall_name(0),
         (int)linux_syscall_dispatch_domain(0),
         (int)linux_syscall_dispatch_domain(56));

    const char *cmd = (cmdline_rest && *cmdline_rest) ? cmdline_rest : "(default)";
    snprintf(msg_out, msg_cap,
             "LC: started %s as %s [%s]\n"
             "    image=%s cmd=%s\n"
             "    OCI: bundle registered; exec is shim-only on bare-metal AevOS.",
             s->name, s->id_str,
             s->state == LC_CTR_RUNNING ? "running" : "?",
             image, cmd);
    return 0;
}

int lc_docker_stop(const char *id_or_name, char *err, size_t errcap)
{
    lc_container_slot_t *s = find_slot(id_or_name);
    if (!s) {
        snprintf(err, errcap, "Error: no such container: %s", id_or_name);
        return -1;
    }
    if (s->state == LC_CTR_STOPPED) {
        snprintf(err, errcap, "Container %s already stopped", id_or_name);
        return -1;
    }
    s->state = LC_CTR_STOPPED;
    snprintf(err, errcap, "Stopped %s", s->id_str);
    return 0;
}

int lc_docker_rm(const char *id_or_name, char *err, size_t errcap)
{
    lc_container_slot_t *s = find_slot(id_or_name);
    if (!s) {
        snprintf(err, errcap, "Error: no such container: %s", id_or_name);
        return -1;
    }
    if (s->state != LC_CTR_STOPPED) {
        snprintf(err, errcap,
                 "Error: container %s is running — `docker stop` first",
                 id_or_name);
        return -1;
    }
    snprintf(err, errcap, "Removed %s", s->id_str);
    slot_free(s);
    return 0;
}
