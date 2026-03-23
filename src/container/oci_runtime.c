#include "lc_layer.h"
#include "kernel/klog.h"
#include "../lib/string.h"

#define LC_OCI_MAX_BUNDLES 16

typedef struct {
    uint64_t     ctr_id;
    char         image[LC_IMAGE_NAME_LEN];
    char         lower[128];
    char         upper[128];
    char         work[128];
    char         rootfs[128];
    bool         used;
} lc_oci_bundle_t;

static lc_oci_bundle_t g_bundles[LC_OCI_MAX_BUNDLES];

static lc_oci_bundle_t *oci_alloc_slot(void)
{
    for (int i = 0; i < LC_OCI_MAX_BUNDLES; i++) {
        if (!g_bundles[i].used) {
            memset(&g_bundles[i], 0, sizeof(g_bundles[i]));
            g_bundles[i].used = true;
            return &g_bundles[i];
        }
    }
    return NULL;
}

void lc_oci_register_bundle(uint64_t ctr_id, const char *image,
                            const char *lower_root)
{
    lc_oci_bundle_t *b = oci_alloc_slot();
    if (!b)
        return;

    b->ctr_id = ctr_id;
    if (image) {
        strncpy(b->image, image, sizeof(b->image) - 1);
        b->image[sizeof(b->image) - 1] = '\0';
    }
    if (lower_root) {
        strncpy(b->lower, lower_root, sizeof(b->lower) - 1);
        b->lower[sizeof(b->lower) - 1] = '\0';
    }
    snprintf(b->upper, sizeof(b->upper), "/lc/overlay/%llu/upper",
             (unsigned long long)ctr_id);
    snprintf(b->work, sizeof(b->work), "/lc/overlay/%llu/work",
             (unsigned long long)ctr_id);
    snprintf(b->rootfs, sizeof(b->rootfs), "/lc/overlay/%llu/merged",
             (unsigned long long)ctr_id);

    klog("lc: OCI bundle ctr=%llu image=%s lower=%s merged=%s\n",
         (unsigned long long)ctr_id, b->image, b->lower, b->rootfs);
}

void lc_oci_init(void)
{
    memset(g_bundles, 0, sizeof(g_bundles));
    klog("lc: OCI runtime scaffold (overlay lower/upper/work per container)\n");
}
