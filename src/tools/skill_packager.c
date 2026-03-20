/*
 * skill_packager — Package a C source file into an AevOS .skill module.
 *
 * Usage: skill_packager -i skill_source.c -o skill_name.skill -n "Name" -d "Description"
 *
 * Host Linux tool — uses standard C library.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>

#define SKILL_MAGIC       0x534B494C4C000000ULL  /* "SKILL\0\0\0" */
#define SKILL_VERSION     1
#define SKILL_NAME_MAX    64
#define SKILL_DESC_MAX    256
#define SKILL_SIG_MAX     128

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t flags;
    char     name[SKILL_NAME_MAX];
    char     description[SKILL_DESC_MAX];
    char     signature[SKILL_SIG_MAX];
    uint32_t source_size;
    uint32_t compiled_size;
    uint64_t checksum;
    uint8_t  reserved[56];
} __attribute__((packed)) skill_header_t;

_Static_assert(sizeof(skill_header_t) == 512 + 64,
               "skill_header_t must be 576 bytes");

/* ── Utilities ── */

static uint64_t fnv1a_hash(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t hash = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

static char *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s': %s\n",
                path, strerror(errno));
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 16 * 1024 * 1024) {
        fprintf(stderr, "Error: file '%s' has invalid size %ld\n",
                path, size);
        fclose(f);
        return NULL;
    }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fprintf(stderr, "Error: out of memory\n");
        fclose(f);
        return NULL;
    }

    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "Error: reading '%s'\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }

    buf[size] = '\0';
    *out_size = (size_t)size;
    fclose(f);
    return buf;
}

/*
 * Verify the source contains a valid skill entry point.
 * Looks for: int skill_fn_name(const char *input, char *output, int max_out)
 * or the typedef: typedef int (*skill_fn_t)(...)
 */
static bool verify_skill_signature(const char *source, char *sig_out, int sig_max)
{
    const char *patterns[] = {
        "skill_fn_t",
        "int skill_",
        "int (*skill_fn_t)",
        NULL
    };

    for (int i = 0; patterns[i]; i++) {
        const char *found = strstr(source, patterns[i]);
        if (found) {
            const char *line_start = found;
            while (line_start > source && *(line_start - 1) != '\n')
                line_start--;

            const char *line_end = found;
            while (*line_end && *line_end != '\n' && *line_end != '{')
                line_end++;

            int len = (int)(line_end - line_start);
            if (len >= sig_max) len = sig_max - 1;
            memcpy(sig_out, line_start, (size_t)len);
            sig_out[len] = '\0';

            /* Clean up whitespace */
            char *p = sig_out;
            while (*p) {
                if (*p == '\r' || *p == '\t') *p = ' ';
                p++;
            }
            return true;
        }
    }

    return false;
}

/* ── TinyCC Header Stubs ── */

static const char *tcc_header_stub =
    "/* AevOS TinyCC runtime header stub */\n"
    "typedef unsigned long size_t;\n"
    "typedef long ssize_t;\n"
    "typedef int bool;\n"
    "#define true 1\n"
    "#define false 0\n"
    "#define NULL ((void*)0)\n"
    "void *memset(void *s, int c, size_t n);\n"
    "void *memcpy(void *d, const void *s, size_t n);\n"
    "size_t strlen(const char *s);\n"
    "char *strcpy(char *d, const char *s);\n"
    "char *strncpy(char *d, const char *s, size_t n);\n"
    "int strcmp(const char *a, const char *b);\n"
    "int strncmp(const char *a, const char *b, size_t n);\n"
    "char *strstr(const char *h, const char *n);\n"
    "int snprintf(char *buf, size_t max, const char *fmt, ...);\n"
    "\n"
    "/* AevOS skill API */\n"
    "typedef int (*skill_fn_t)(const char *input, char *output, int max_out);\n"
    "\n";

/* ── Main ── */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -i <source.c> -o <output.skill> "
        "-n \"Skill Name\" -d \"Description\"\n"
        "\n"
        "Options:\n"
        "  -i  Input C source file\n"
        "  -o  Output .skill module file\n"
        "  -n  Skill display name\n"
        "  -d  Skill description\n"
        "\n", prog);
}

int main(int argc, char *argv[])
{
    const char *input_path  = NULL;
    const char *output_path = NULL;
    const char *skill_name  = NULL;
    const char *skill_desc  = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
            input_path = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output_path = argv[++i];
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            skill_name = argv[++i];
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            skill_desc = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    if (!input_path || !output_path || !skill_name || !skill_desc) {
        usage(argv[0]);
        return 1;
    }

    printf("=== AevOS Skill Packager ===\n");
    printf("Input:  %s\n", input_path);
    printf("Output: %s\n", output_path);
    printf("Name:   %s\n", skill_name);
    printf("Desc:   %s\n", skill_desc);

    /* Read source file */
    size_t source_size = 0;
    char *source = read_file(input_path, &source_size);
    if (!source)
        return 1;

    printf("\nSource size: %zu bytes\n", source_size);

    /* Verify skill signature */
    char signature[SKILL_SIG_MAX];
    memset(signature, 0, sizeof(signature));

    if (!verify_skill_signature(source, signature, SKILL_SIG_MAX)) {
        fprintf(stderr,
            "Warning: no skill_fn_t signature found in source.\n"
            "         The skill must define a function matching:\n"
            "         int skill_name(const char *input, "
            "char *output, int max_out)\n");
        strncpy(signature, "(unverified)", SKILL_SIG_MAX - 1);
    } else {
        printf("Signature:  %s\n", signature);
    }

    /* Build the combined source: TCC header stubs + user source */
    size_t stub_len = strlen(tcc_header_stub);
    size_t combined_size = stub_len + source_size;
    char *combined = (char *)malloc(combined_size + 1);
    if (!combined) {
        fprintf(stderr, "Error: out of memory\n");
        free(source);
        return 1;
    }
    memcpy(combined, tcc_header_stub, stub_len);
    memcpy(combined + stub_len, source, source_size);
    combined[combined_size] = '\0';

    /* Build the .skill file: header + combined source */
    skill_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic   = SKILL_MAGIC;
    hdr.version = SKILL_VERSION;
    hdr.flags   = 0;

    strncpy(hdr.name, skill_name, SKILL_NAME_MAX - 1);
    hdr.name[SKILL_NAME_MAX - 1] = '\0';

    strncpy(hdr.description, skill_desc, SKILL_DESC_MAX - 1);
    hdr.description[SKILL_DESC_MAX - 1] = '\0';

    strncpy(hdr.signature, signature, SKILL_SIG_MAX - 1);
    hdr.signature[SKILL_SIG_MAX - 1] = '\0';

    hdr.source_size   = (uint32_t)combined_size;
    hdr.compiled_size = 0;
    hdr.checksum      = fnv1a_hash(combined, combined_size);

    /* Write output */
    FILE *out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "Error: cannot create '%s': %s\n",
                output_path, strerror(errno));
        free(combined);
        free(source);
        return 1;
    }

    if (fwrite(&hdr, sizeof(hdr), 1, out) != 1) {
        fprintf(stderr, "Error: writing header\n");
        fclose(out);
        free(combined);
        free(source);
        return 1;
    }

    if (fwrite(combined, 1, combined_size, out) != combined_size) {
        fprintf(stderr, "Error: writing source data\n");
        fclose(out);
        free(combined);
        free(source);
        return 1;
    }

    fclose(out);
    free(combined);
    free(source);

    size_t total_size = sizeof(hdr) + combined_size;
    printf("\nSkill module written: %zu bytes\n", total_size);
    printf("  Header:   %zu bytes\n", sizeof(hdr));
    printf("  Source:    %zu bytes (includes TCC stubs)\n", combined_size);
    printf("  Checksum:  0x%016lx\n", (unsigned long)hdr.checksum);
    printf("\nDone! Skill '%s' packaged successfully.\n", skill_name);

    return 0;
}
