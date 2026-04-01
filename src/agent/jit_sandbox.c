#include "jit_sandbox.h"
#include "kernel/klog.h"

void aevos_jit_sandbox_compile_enter(const char *phase_tag)
{
    klog("jit_sandbox: compile phase '%s' (isolation: WASM-capable stub)\n",
         phase_tag ? phase_tag : "?");
}
