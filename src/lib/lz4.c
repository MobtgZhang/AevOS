#include "lz4.h"
#include "string.h"

/*
 * LZ4 block format (simplified):
 *   Each sequence consists of a token byte, optional literal-length extension,
 *   literal bytes, a 16-bit match offset (little-endian), and optional
 *   match-length extension.
 *
 *   token = (literal_length << 4) | match_length
 *   - If literal_length == 15, subsequent bytes add to it until byte < 255
 *   - match_length has +4 bias (min match = 4)
 *   - If match_length == 15+4, subsequent bytes add to it until byte < 255
 *   - Last sequence has no match (only literals, offset omitted)
 */

#define LZ4_HASH_LOG      12
#define LZ4_HASH_SIZE      (1 << LZ4_HASH_LOG)
#define LZ4_MIN_MATCH      4
#define LZ4_MAX_INPUT_SIZE 0x7E000000
#define LZ4_SKIP_TRIGGER   6

static uint32_t lz4_hash(uint32_t val)
{
    return (val * 2654435761U) >> (32 - LZ4_HASH_LOG);
}

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0]       | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t read16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static size_t lz4_count(const uint8_t *a, const uint8_t *b, const uint8_t *limit)
{
    const uint8_t *start = a;
    while (a < limit && *a == *b) {
        a++;
        b++;
    }
    return (size_t)(a - start);
}

ssize_t lz4_compress(const void *src, size_t src_size,
                     void *dst, size_t dst_capacity)
{
    if (!src || !dst || src_size == 0)
        return -1;
    if (src_size > LZ4_MAX_INPUT_SIZE)
        return -1;

    const uint8_t *ip     = (const uint8_t *)src;
    const uint8_t *iend   = ip + src_size;
    const uint8_t *ilimit = iend - LZ4_MIN_MATCH;

    uint8_t *op    = (uint8_t *)dst;
    uint8_t *oend  = op + dst_capacity;

    /* hash table mapping hash → position in source */
    uint16_t htable[LZ4_HASH_SIZE];
    memset(htable, 0, sizeof(htable));

    const uint8_t *anchor = ip;
    ip++; /* first byte never matched */

    while (ip < ilimit) {
        /* find a match */
        uint32_t h = lz4_hash(read32(ip));
        const uint8_t *ref = (const uint8_t *)src + htable[h];
        htable[h] = (uint16_t)(ip - (const uint8_t *)src);

        if (ref < (const uint8_t *)src || ip - ref > 65535 ||
            read32(ref) != read32(ip)) {
            ip++;
            continue;
        }

        /* emit literals from anchor to ip */
        size_t lit_len = (size_t)(ip - anchor);
        size_t match_len = LZ4_MIN_MATCH + lz4_count(ip + LZ4_MIN_MATCH,
                                                       ref + LZ4_MIN_MATCH,
                                                       iend);

        /* make sure we don't overwrite output */
        size_t worst = 1 + (lit_len / 255) + lit_len + 2 + (match_len / 255) + 1;
        if (op + worst > oend) return -1;

        /* token */
        uint8_t *token = op++;
        size_t lit_hdr = (lit_len >= 15) ? 15 : lit_len;
        size_t ml_hdr  = (match_len - LZ4_MIN_MATCH >= 15) ? 15 : match_len - LZ4_MIN_MATCH;
        *token = (uint8_t)((lit_hdr << 4) | ml_hdr);

        /* extended literal length */
        if (lit_len >= 15) {
            size_t rem = lit_len - 15;
            while (rem >= 255) { *op++ = 255; rem -= 255; }
            *op++ = (uint8_t)rem;
        }

        /* literal bytes */
        memcpy(op, anchor, lit_len);
        op += lit_len;

        /* match offset (little-endian 16-bit) */
        write16_le(op, (uint16_t)(ip - ref));
        op += 2;

        /* extended match length */
        if (match_len - LZ4_MIN_MATCH >= 15) {
            size_t rem = match_len - LZ4_MIN_MATCH - 15;
            while (rem >= 255) { *op++ = 255; rem -= 255; }
            *op++ = (uint8_t)rem;
        }

        ip += match_len;
        anchor = ip;

        /* update hash for the position right after match start */
        if (ip < ilimit) {
            htable[lz4_hash(read32(ip))] = (uint16_t)(ip - (const uint8_t *)src);
        }
    }

    /* emit last literals (no match part) */
    size_t last_lit = (size_t)(iend - anchor);
    size_t worst_last = 1 + (last_lit / 255) + last_lit;
    if (op + worst_last > oend) return -1;

    uint8_t *token = op++;
    size_t lit_hdr = (last_lit >= 15) ? 15 : last_lit;
    *token = (uint8_t)(lit_hdr << 4);

    if (last_lit >= 15) {
        size_t rem = last_lit - 15;
        while (rem >= 255) { *op++ = 255; rem -= 255; }
        *op++ = (uint8_t)rem;
    }

    memcpy(op, anchor, last_lit);
    op += last_lit;

    return (ssize_t)(op - (uint8_t *)dst);
}

ssize_t lz4_decompress(const void *src, size_t src_size,
                       void *dst, size_t dst_capacity)
{
    if (!src || !dst)
        return -1;

    const uint8_t *ip   = (const uint8_t *)src;
    const uint8_t *iend = ip + src_size;

    uint8_t *op   = (uint8_t *)dst;
    uint8_t *oend = op + dst_capacity;

    while (ip < iend) {
        /* read token */
        uint8_t tok = *ip++;
        size_t lit_len   = tok >> 4;
        size_t match_len = tok & 0x0F;

        /* extended literal length */
        if (lit_len == 15) {
            while (ip < iend) {
                uint8_t b = *ip++;
                lit_len += b;
                if (b < 255) break;
            }
        }

        /* copy literals */
        if (ip + lit_len > iend || op + lit_len > oend)
            return -1;
        memcpy(op, ip, lit_len);
        ip += lit_len;
        op += lit_len;

        /* last sequence has no match */
        if (ip >= iend)
            break;

        /* read match offset */
        if (ip + 2 > iend) return -1;
        uint16_t offset = read16_le(ip);
        ip += 2;
        if (offset == 0) return -1;

        /* extended match length */
        match_len += LZ4_MIN_MATCH;
        if ((tok & 0x0F) == 15) {
            while (ip < iend) {
                uint8_t b = *ip++;
                match_len += b;
                if (b < 255) break;
            }
        }

        /* copy match (may overlap, so byte-by-byte when needed) */
        uint8_t *match_src = op - offset;
        if (match_src < (uint8_t *)dst) return -1;
        if (op + match_len > oend) return -1;

        if (offset >= match_len) {
            memcpy(op, match_src, match_len);
            op += match_len;
        } else {
            /* overlapping copy */
            for (size_t i = 0; i < match_len; i++)
                op[i] = match_src[i];
            op += match_len;
        }
    }

    return (ssize_t)(op - (uint8_t *)dst);
}
