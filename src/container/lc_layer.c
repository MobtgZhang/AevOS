#include "lc_layer.h"
#include "kernel/klog.h"

void lc_sandbox_init(void);
void lc_ifc_init(void);
void lc_linux_subsys_init(void);
void lc_oci_init(void);

void lc_layer_init(void)
{
    lc_sandbox_init();
    lc_ifc_init();
    lc_linux_subsys_init();
    lc_oci_init();
    klog("lc: Docker-compat CLI backend + OCI + Linux syscall shim + IFC ready\n");
}
