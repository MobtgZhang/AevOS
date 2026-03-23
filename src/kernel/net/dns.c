/*
 * Minimal DNS client (UDP/53): A (1), AAAA (28), CNAME following.
 */

#include "dns.h"
#include "../drivers/timer.h"
#include <aevos/config.h>
#include <kernel/arch/arch.h>
#include <kernel/klog.h>
#include <lib/string.h>

#define DNS_PORT       53
#define DNS_TYPE_A     1
#define DNS_TYPE_CNAME 5
#define DNS_TYPE_AAAA  28
#define DNS_CLASS_IN   1

static uint16_t dns_read_u16(const uint8_t *p, size_t o)
{
    return (uint16_t)(((uint16_t)p[o] << 8) | p[o + 1]);
}

static int dns_encode_name(uint8_t *out, size_t cap, const char *name)
{
    size_t o = 0;
    while (*name) {
        const char *dot = name;
        while (*dot && *dot != '.')
            dot++;
        size_t lab = (size_t)(dot - name);
        if (lab == 0 || lab > 63 || o + 1 + lab >= cap)
            return -1;
        out[o++] = (uint8_t)lab;
        memcpy(out + o, name, lab);
        o += lab;
        name = (*dot == '.') ? dot + 1 : dot;
    }
    if (o + 1 >= cap)
        return -1;
    out[o++] = 0;
    return (int)o;
}

static int dns_name_unpack(char *dst, size_t dcap, const uint8_t *pkt, size_t pktlen,
                           size_t *ioff, unsigned depth)
{
    if (depth > 12)
        return -1;
    size_t o = 0;
    int    jumped = 0;
    size_t back   = 0;

    for (;;) {
        if (*ioff >= pktlen)
            return -1;
        uint8_t lab = pkt[*ioff];
        if (lab == 0) {
            (*ioff)++;
            if (jumped)
                *ioff = back;
            break;
        }
        if ((lab & 0xC0) == 0xC0) {
            if (*ioff + 1 >= pktlen)
                return -1;
            uint16_t ptr = (uint16_t)(((uint16_t)(lab & 0x3Fu) << 8) | pkt[*ioff + 1]);
            (*ioff) += 2;
            if (!jumped) {
                back   = *ioff;
                jumped = 1;
            }
            *ioff = ptr;
            if (*ioff >= pktlen)
                return -1;
            continue;
        }
        (*ioff)++;
        if (*ioff + lab > pktlen || o + (size_t)lab + 2 >= dcap)
            return -1;
        if (o)
            dst[o++] = '.';
        memcpy(dst + o, pkt + *ioff, lab);
        o += lab;
        *ioff += lab;
    }
    dst[o] = '\0';
    return 0;
}

static int dns_skip_question(const uint8_t *pkt, size_t len, size_t *off)
{
    for (;;) {
        if (*off >= len)
            return -1;
        uint8_t l = pkt[*off];
        if (l == 0) {
            (*off)++;
            break;
        }
        if ((l & 0xC0) == 0xC0) {
            *off += 2;
            break;
        }
        *off += 1u + (size_t)l;
    }
    if (*off + 4 > len)
        return -1;
    *off += 4;
    return 0;
}

static int dns_read_rr(const uint8_t *pkt, size_t len, size_t *off,
                       uint16_t *rtype, uint16_t *rdlen, size_t *rdata_off)
{
    for (;;) {
        if (*off >= len)
            return -1;
        uint8_t l = pkt[*off];
        if (l == 0) {
            (*off)++;
            break;
        }
        if ((l & 0xC0) == 0xC0) {
            *off += 2;
            break;
        }
        *off += 1u + (size_t)l;
    }
    if (*off + 10 > len)
        return -1;
    *rtype = (uint16_t)((uint16_t)pkt[*off] << 8 | pkt[*off + 1]);
    *off += 2;
    *off += 2; /* class */
    *off += 4; /* ttl */
    *rdlen = (uint16_t)((uint16_t)pkt[*off] << 8 | pkt[*off + 1]);
    *off += 2;
    *rdata_off = *off;
    *off += *rdlen;
    if (*off > len)
        return -1;
    return 0;
}

typedef struct {
    bool     waiting;
    uint16_t tid;
    bool     got;
    size_t   len;
    uint8_t  buf[1536];
} dns_rx_t;

static dns_rx_t dns_rx;

static void dns_udp_handler(uint32_t src_ip, uint16_t src_port,
                            const void *data, size_t len, void *userdata)
{
    (void)src_ip;
    (void)src_port;
    (void)userdata;
    if (!dns_rx.waiting || len < 12 || len > sizeof(dns_rx.buf))
        return;
    const uint8_t *p = (const uint8_t *)data;
    if (dns_read_u16(p, 0) != dns_rx.tid)
        return;
    memcpy(dns_rx.buf, data, len);
    dns_rx.len = len;
    dns_rx.got = true;
}

static int dns_exchange(uint16_t qtype, const char *name, uint32_t timeout_ms)
{
    uint32_t dns_ip = net_ipv4_dns_server();
    if (!dns_ip)
        return -EIO;

    if (net_wait_arp_ipv4(dns_ip, 2000) < 0)
        return -ETIMEDOUT;

    uint16_t sport =
        (uint16_t)(42000u + (uint16_t)(timer_get_ticks() & 0x1FFFu));
    if (udp_bind(sport, dns_udp_handler, NULL) < 0)
        return -ENOMEM;

    uint8_t qbuf[512];
    dns_rx.tid = (uint16_t)(timer_get_ticks() ^ (timer_get_ticks() >> 16));
    if (dns_rx.tid == 0)
        dns_rx.tid = 0xACE1u;

    qbuf[0] = (uint8_t)(dns_rx.tid >> 8);
    qbuf[1] = (uint8_t)dns_rx.tid;
    qbuf[2] = 0x01; /* flags: RD */
    qbuf[3] = 0;
    qbuf[4] = 0;
    qbuf[5] = 1; /* QDCOUNT */
    qbuf[6] = 0;
    qbuf[7] = 0; /* ANCOUNT */
    qbuf[8] = 0;
    qbuf[9] = 0;
    qbuf[10] = 0;
    qbuf[11] = 0;

    int nl = dns_encode_name(qbuf + 12, sizeof(qbuf) - 12 - 4, name);
    if (nl < 0) {
        udp_unbind(sport);
        return -EINVAL;
    }

    size_t qo = 12u + (size_t)nl;
    qbuf[qo++] = (uint8_t)(qtype >> 8);
    qbuf[qo++] = (uint8_t)qtype;
    qbuf[qo++] = 0;
    qbuf[qo++] = (uint8_t)DNS_CLASS_IN;

    dns_rx.waiting = true;
    dns_rx.got     = false;
    dns_rx.len     = 0;

    if (udp_send(dns_ip, DNS_PORT, sport, qbuf, qo) < 0) {
        udp_unbind(sport);
        return -EIO;
    }

    uint64_t deadline =
        timer_get_ticks() + (uint64_t)timeout_ms * (uint64_t)TIMER_FREQ_HZ / 1000ull;
    while ((int64_t)(deadline - timer_get_ticks()) > 0) {
        if (dns_rx.got)
            break;
        net_poll();
        arch_spin_hint();
    }

    udp_unbind(sport);
    dns_rx.waiting = false;

    if (!dns_rx.got)
        return -ETIMEDOUT;
    return 0;
}

static int dns_parse_response(uint16_t want_type, uint32_t *a_out, ipv6_addr_t *aaaa_out,
                              char *cname_out, size_t cname_cap)
{
    (void)want_type;
    if (dns_rx.len < 12)
        return -EIO;

    uint16_t f       = dns_read_u16(dns_rx.buf, 2);
    uint8_t  rcode   = (uint8_t)(f & 0x0Fu);
    if (rcode == 3)
        return -ENOTFOUND;
    if (rcode != 0)
        return -EIO;

    uint16_t an = dns_read_u16(dns_rx.buf, 6);
    size_t   off = 12;

    if (dns_skip_question(dns_rx.buf, dns_rx.len, &off) < 0)
        return -EIO;

    cname_out[0] = '\0';

    for (uint16_t i = 0; i < an; i++) {
        uint16_t rtype, rdlen;
        size_t   rdoff;
        if (dns_read_rr(dns_rx.buf, dns_rx.len, &off, &rtype, &rdlen, &rdoff) < 0)
            return -EIO;

        if (rtype == DNS_TYPE_A && rdlen == 4 && a_out) {
            memcpy(a_out, dns_rx.buf + rdoff, 4);
            return 1; /* got A */
        }
        if (rtype == DNS_TYPE_AAAA && rdlen == 16 && aaaa_out) {
            memcpy(aaaa_out->b, dns_rx.buf + rdoff, 16);
            return 2; /* got AAAA */
        }
        if (rtype == DNS_TYPE_CNAME && cname_out && cname_cap > 0) {
            size_t co = rdoff;
            if (dns_name_unpack(cname_out, cname_cap, dns_rx.buf, dns_rx.len, &co, 0) == 0)
                (void)0;
        }
    }

    if (cname_out[0])
        return 0; /* follow CNAME */
    return -ENOTFOUND;
}

int dns_resolve_a(const char *name, uint32_t *ipv4_out, uint32_t timeout_ms)
{
    if (!name || !ipv4_out || !name[0])
        return -EINVAL;

    char n[256];
    strncpy(n, name, sizeof(n) - 1);
    n[sizeof(n) - 1] = '\0';

    for (int hop = 0; hop < 8; hop++) {
        int rc = dns_exchange(DNS_TYPE_A, n, timeout_ms);
        if (rc < 0)
            return rc;

        char     cname[256];
        uint32_t a = 0;
        int      pr =
            dns_parse_response(DNS_TYPE_A, &a, NULL, cname, sizeof(cname));
        if (pr == 1) {
            *ipv4_out = a;
            klog("dns: %s -> %u.%u.%u.%u\n", n,
                 a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, (a >> 24) & 0xFF);
            return 0;
        }
        if (pr == 0 && cname[0]) {
            strncpy(n, cname, sizeof(n) - 1);
            n[sizeof(n) - 1] = '\0';
            continue;
        }
        return pr < 0 ? pr : -ENOTFOUND;
    }
    return -ENOTFOUND;
}

int dns_resolve_aaaa(const char *name, ipv6_addr_t *ipv6_out, uint32_t timeout_ms)
{
    if (!name || !ipv6_out || !name[0])
        return -EINVAL;

    char n[256];
    strncpy(n, name, sizeof(n) - 1);
    n[sizeof(n) - 1] = '\0';

    for (int hop = 0; hop < 8; hop++) {
        int rc = dns_exchange(DNS_TYPE_AAAA, n, timeout_ms);
        if (rc < 0)
            return rc;

        char        cname[256];
        ipv6_addr_t a6;
        memset(&a6, 0, sizeof(a6));
        int pr = dns_parse_response(DNS_TYPE_AAAA, NULL, &a6, cname, sizeof(cname));
        if (pr == 2) {
            *ipv6_out = a6;
            return 0;
        }
        if (pr == 0 && cname[0]) {
            strncpy(n, cname, sizeof(n) - 1);
            n[sizeof(n) - 1] = '\0';
            continue;
        }
        return pr < 0 ? pr : -ENOTFOUND;
    }
    return -ENOTFOUND;
}
