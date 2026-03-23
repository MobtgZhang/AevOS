#pragma once

#include <aevos/types.h>

/* ── Docker-compatible LC backend (OCI lifecycle + syscall/IFC shim) ── */

#define LC_MAX_CONTAINERS   16
#define LC_IMAGE_NAME_LEN  96
#define LC_CTR_NAME_LEN    64
#define LC_ID_STR_LEN      24

typedef enum {
    LC_CTR_CREATED = 0,
    LC_CTR_RUNNING,
    LC_CTR_STOPPED,
} lc_ctr_state_t;

typedef struct {
    uint64_t       id;
    char           id_str[LC_ID_STR_LEN];
    char           name[LC_CTR_NAME_LEN];
    char           image[LC_IMAGE_NAME_LEN];
    lc_ctr_state_t state;
} lc_container_slot_t;

void lc_layer_init(void);

/* OCI bundle / overlay record (one per active container). */
void lc_oci_register_bundle(uint64_t ctr_id, const char *image,
                            const char *lower_root);

int lc_docker_version(char *out, size_t cap);
int lc_docker_info(char *out, size_t cap);
int lc_docker_ps(lc_container_slot_t *out, int max_slots, int *n_out);
int lc_docker_images(char *out, size_t cap);
/*
 * Simulated `docker run`: allocates an OCI bundle slot, applies Docker-profile
 * sandbox + IFC checks (container → host I/O). cmdline_rest may be empty.
 * On success writes a one-line summary into msg_out.
 */
int lc_docker_run(const char *image, const char *cmdline_rest,
                  char *msg_out, size_t msg_cap);
int lc_docker_stop(const char *id_or_name, char *err, size_t errcap);
int lc_docker_rm(const char *id_or_name, char *err, size_t errcap);

/* ── Information-flow control (Skill / Container / LLM boundaries) ── */

typedef enum {
    LC_IFC_DOM_HOST = 0,
    LC_IFC_DOM_CONTAINER,
    LC_IFC_DOM_SKILL,
    LC_IFC_DOM_LLM,
    LC_IFC_DOM_HMS,
    LC_IFC_DOM_COUNT
} lc_ifc_domain_t;

#define LC_IFC_OP_READ  1u
#define LC_IFC_OP_WRITE 2u
#define LC_IFC_OP_EXEC  4u

bool lc_ifc_flow_allowed(lc_ifc_domain_t from, lc_ifc_domain_t to, uint32_t ops);
/* Register a runtime exception (e.g. verifier-approved Skill → LLM). */
void lc_ifc_grant(lc_ifc_domain_t from, lc_ifc_domain_t to, uint32_t ops);
void lc_ifc_revoke(lc_ifc_domain_t from, lc_ifc_domain_t to, uint32_t ops);

/* ── Linux x86_64 syscall viewing + sandbox policy (per profile) ── */

typedef enum {
    LC_SB_PROFILE_SKILL = 0,
    LC_SB_PROFILE_DOCKER,
    LC_SB_PROFILE_COUNT
} lc_sb_profile_t;

bool lc_sandbox_syscall_allow(lc_sb_profile_t prof, uint32_t linux_nr);
const char *lc_linux_x64_syscall_name(uint32_t nr);

/*
 * Translate a Linux syscall number for host-side tooling / future musl payloads.
 * Returns 0 if allowed under profile after IFC hop from `from_dom`, else -1.
 */
int lc_linux_syscall_gate(lc_sb_profile_t prof, lc_ifc_domain_t from_dom,
                          uint32_t linux_nr);
