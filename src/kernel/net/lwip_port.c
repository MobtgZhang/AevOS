#include "lwip_port.h"
#include "../drivers/pci.h"
#include "../drivers/virtio_net.h"
#include "../drivers/timer.h"
#include <aevos/config.h>
#include <kernel/mm/slab.h>
#include <kernel/klog.h>
#include <kernel/arch/arch.h>
#include <kernel/arch/io.h>
#include <lib/string.h>

/* ── Global state ─────────────────────────────────────────────────── */

static net_interface_t *g_iface;
static arp_entry_t      arp_table[ARP_TABLE_SIZE];
static udp_socket_t     udp_sockets[UDP_MAX_SOCKETS];
static tcp_socket_t     tcp_sockets[TCP_MAX_SOCKETS];
static spinlock_t       net_lock = SPINLOCK_INIT;

/* ── Byte-order helpers (network = big-endian) ────────────────────── */

static inline uint16_t htons(uint16_t v) { return (v >> 8) | (v << 8); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v)
{
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

/* ── Checksum ─────────────────────────────────────────────────────── */

static uint16_t ip_checksum(const void *data, size_t len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len == 1)
        sum += *(const uint8_t *)p;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── Network interface ────────────────────────────────────────────── */

int net_init(void)
{
    memset(arp_table, 0, sizeof(arp_table));
    memset(udp_sockets, 0, sizeof(udp_sockets));
    memset(tcp_sockets, 0, sizeof(tcp_sockets));
    g_iface = NULL;
    klog("net: stack initialized\n");
    return 0;
}

void net_poll(void)
{
    virtio_net_poll();
}

void net_set_interface(net_interface_t *iface)
{
    g_iface = iface;
    if (iface)
        klog("net: interface up, ip=%u.%u.%u.%u\n",
             iface->ip & 0xFF, (iface->ip >> 8) & 0xFF,
             (iface->ip >> 16) & 0xFF, (iface->ip >> 24) & 0xFF);
}

/* ── Ethernet ─────────────────────────────────────────────────────── */

int eth_send(const uint8_t *dst_mac, uint16_t ethertype,
             const void *payload, size_t len)
{
    if (!g_iface || !g_iface->send_raw || !g_iface->is_up)
        return -EIO;
    if (len > ETH_MTU)
        return -EINVAL;

    uint8_t frame[ETH_FRAME_MAX];
    eth_header_t *hdr = (eth_header_t *)frame;
    memcpy(hdr->dst, dst_mac, ETH_ADDR_LEN);
    memcpy(hdr->src, g_iface->mac, ETH_ADDR_LEN);
    hdr->ethertype = htons(ethertype);
    memcpy(frame + sizeof(eth_header_t), payload, len);

    size_t total = sizeof(eth_header_t) + len;
    if (total < 60) total = 60; /* pad to minimum Ethernet frame */

    return (g_iface->send_raw(frame, total) > 0) ? 0 : -EIO;
}

int eth_receive(void *buf, size_t max_len)
{
    if (!g_iface || !g_iface->recv_raw)
        return -EIO;
    ssize_t n = g_iface->recv_raw(buf, max_len);
    if (n == 0)
        return 0;
    if (n < 0)
        return (int)n;
    return (int)n;
}

/* ── ARP ──────────────────────────────────────────────────────────── */

static void arp_table_add(uint32_t ip, const uint8_t *mac)
{
    /* update existing */
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            memcpy(arp_table[i].mac, mac, ETH_ADDR_LEN);
            return;
        }
    }
    /* add new */
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) {
            arp_table[i].ip    = ip;
            arp_table[i].valid = true;
            memcpy(arp_table[i].mac, mac, ETH_ADDR_LEN);
            return;
        }
    }
    /* table full: overwrite oldest (slot 0 for simplicity) */
    arp_table[0].ip    = ip;
    arp_table[0].valid = true;
    memcpy(arp_table[0].mac, mac, ETH_ADDR_LEN);
}

const uint8_t *arp_lookup(uint32_t ip)
{
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip)
            return arp_table[i].mac;
    }
    return NULL;
}

int arp_request(uint32_t target_ip)
{
    if (!g_iface) return -EIO;

    arp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.hw_type    = htons(1);        /* Ethernet */
    pkt.proto_type = htons(0x0800);   /* IPv4 */
    pkt.hw_len     = ETH_ADDR_LEN;
    pkt.proto_len  = 4;
    pkt.opcode     = htons(ARP_OP_REQUEST);
    memcpy(pkt.sender_mac, g_iface->mac, ETH_ADDR_LEN);
    pkt.sender_ip  = g_iface->ip;
    memset(pkt.target_mac, 0xFF, ETH_ADDR_LEN);
    pkt.target_ip  = target_ip;

    uint8_t broadcast[ETH_ADDR_LEN];
    memset(broadcast, 0xFF, ETH_ADDR_LEN);
    return eth_send(broadcast, ETH_TYPE_ARP, &pkt, sizeof(pkt));
}

int arp_reply(const uint8_t *target_mac, uint32_t target_ip)
{
    if (!g_iface) return -EIO;

    arp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.hw_type    = htons(1);
    pkt.proto_type = htons(0x0800);
    pkt.hw_len     = ETH_ADDR_LEN;
    pkt.proto_len  = 4;
    pkt.opcode     = htons(ARP_OP_REPLY);
    memcpy(pkt.sender_mac, g_iface->mac, ETH_ADDR_LEN);
    pkt.sender_ip  = g_iface->ip;
    memcpy(pkt.target_mac, target_mac, ETH_ADDR_LEN);
    pkt.target_ip  = target_ip;

    return eth_send(target_mac, ETH_TYPE_ARP, &pkt, sizeof(pkt));
}

int arp_process(const void *frame, size_t len)
{
    if (!g_iface)
        return -EIO;
    if (len < sizeof(eth_header_t) + sizeof(arp_packet_t))
        return -EINVAL;

    const arp_packet_t *pkt = (const arp_packet_t *)
        ((const uint8_t *)frame + sizeof(eth_header_t));

    arp_table_add(pkt->sender_ip, pkt->sender_mac);

    uint16_t op = ntohs(pkt->opcode);
    if (op == ARP_OP_REQUEST && pkt->target_ip == g_iface->ip)
        return arp_reply(pkt->sender_mac, pkt->sender_ip);

    return 0;
}

/* ── IPv4 ─────────────────────────────────────────────────────────── */

static uint16_t ip_id_counter = 1;

int ip_send(uint32_t dst_ip, uint8_t protocol,
            const void *payload, size_t len)
{
    if (!g_iface || len > ETH_MTU - sizeof(ipv4_header_t))
        return -EINVAL;

    /* resolve next-hop MAC */
    uint32_t next_hop = dst_ip;
    if ((dst_ip & g_iface->netmask) != (g_iface->ip & g_iface->netmask))
        next_hop = g_iface->gateway;

    const uint8_t *dst_mac = arp_lookup(next_hop);
    if (!dst_mac) {
        arp_request(next_hop);
        return -EIO;
    }

    uint8_t pkt[ETH_MTU];
    ipv4_header_t *ip = (ipv4_header_t *)pkt;
    memset(ip, 0, sizeof(*ip));
    ip->ver_ihl      = 0x45; /* IPv4, 5 words (20 bytes) */
    ip->total_length  = htons((uint16_t)(sizeof(ipv4_header_t) + len));
    ip->identification= htons(ip_id_counter++);
    ip->ttl           = 64;
    ip->protocol      = protocol;
    ip->src_ip        = g_iface->ip;
    ip->dst_ip        = dst_ip;
    ip->checksum      = 0;
    ip->checksum      = ip_checksum(ip, sizeof(ipv4_header_t));

    memcpy(pkt + sizeof(ipv4_header_t), payload, len);
    return eth_send(dst_mac, ETH_TYPE_IPV4, pkt, sizeof(ipv4_header_t) + len);
}

/* Forward declarations for protocol handlers */
static void udp_process(uint32_t src_ip, const void *data, size_t len);
static void tcp_process(uint32_t src_ip, const void *data, size_t len);

/* One-shot ping waiter (terminal / single-threaded poll path) */
static struct {
    bool     active;
    uint16_t id;
    uint16_t seq;
    uint32_t target_ip;
    bool     replied;
    uint64_t start_tick;
} ping_wait;

static uint16_t ping_seq_global;

static struct {
    bool        active;
    uint16_t    id;
    uint16_t    seq;
    ipv6_addr_t target;
    bool        replied;
    uint64_t    start_tick;
} ping6_wait;

static uint16_t ping6_seq_g;

int net_parse_ipv4(const char *s, uint32_t *ip_out)
{
    if (!s || !ip_out)
        return -EINVAL;

    while (*s == ' ' || *s == '\t')
        s++;

    unsigned oct[4];
    for (int oi = 0; oi < 4; oi++) {
        unsigned n = 0;
        int      nd = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10u + (unsigned)(*s - '0');
            if (n > 255u)
                return -EINVAL;
            s++;
            nd++;
        }
        if (nd == 0)
            return -EINVAL;
        oct[oi] = n;
        if (oi < 3) {
            if (*s != '.')
                return -EINVAL;
            s++;
        }
    }
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s != '\0')
        return -EINVAL;

    *ip_out = IP4((uint8_t)oct[0], (uint8_t)oct[1],
                  (uint8_t)oct[2], (uint8_t)oct[3]);
    return 0;
}

static uint32_t ip_next_hop(uint32_t dst_ip)
{
    if (!g_iface)
        return dst_ip;
    if ((dst_ip & g_iface->netmask) != (g_iface->ip & g_iface->netmask))
        return g_iface->gateway;
    return dst_ip;
}

static int icmp_wait_arp(uint32_t hop, uint64_t deadline_tick)
{
    arp_request(hop);
    while ((int64_t)(deadline_tick - timer_get_ticks()) > 0) {
        if (arp_lookup(hop))
            return 0;
        net_poll();
        arch_spin_hint();
    }
    return -ETIMEDOUT;
}

static void icmp_process(uint32_t src_ip, const void *data, size_t len)
{
    if (len < 8 || !g_iface)
        return;

    const uint8_t *p = (const uint8_t *)data;
    uint8_t        type = p[0];

    if (type == ICMP_ECHOREPLY) {
        if (!ping_wait.active)
            return;
        uint16_t rid  = (uint16_t)(((uint16_t)p[4] << 8) | p[5]);
        uint16_t rseq = (uint16_t)(((uint16_t)p[6] << 8) | p[7]);
        if (src_ip == ping_wait.target_ip &&
            rid == ping_wait.id && rseq == ping_wait.seq)
            ping_wait.replied = true;
        return;
    }

    if (type == ICMP_ECHO) {
        uint8_t reply[512];
        if (len > sizeof(reply))
            len = sizeof(reply);
        reply[0] = ICMP_ECHOREPLY;
        reply[1] = 0;
        reply[2] = 0;
        reply[3] = 0;
        memcpy(reply + 4, p + 4, len - 4);
        uint16_t csum = ip_checksum(reply, len);
        memcpy(reply + 2, &csum, sizeof(csum));
        (void)ip_send(src_ip, IP_PROTO_ICMP, reply, len);
    }
}

int icmp_ping(uint32_t dst_ip, uint32_t timeout_ms, uint32_t *rtt_ms_out)
{
    if (!rtt_ms_out)
        return -EINVAL;
    if (!g_iface || !g_iface->is_up)
        return -EIO;

    uint32_t hop = ip_next_hop(dst_ip);
    uint64_t arp_deadline = timer_get_ticks() +
        (uint64_t)2000 * (uint64_t)TIMER_FREQ_HZ / 1000ull;
    if (icmp_wait_arp(hop, arp_deadline) < 0)
        return -ETIMEDOUT;

    if (ping_wait.active || ping6_wait.active)
        return -EBUSY;

    enum { icmp_payload = 32 };
    uint8_t           icmp[8 + icmp_payload];
    const uint16_t    id  = 0xAEA1;
    const uint16_t    seq = ++ping_seq_global;

    memset(icmp, 0, sizeof(icmp));
    icmp[0] = ICMP_ECHO;
    icmp[1] = 0;
    icmp_echo_header_t *eh = (icmp_echo_header_t *)icmp;
    eh->checksum    = 0;
    eh->identifier  = htons(id);
    eh->sequence    = htons(seq);
    for (int i = 0; i < icmp_payload; i++)
        icmp[8 + i] = (uint8_t)i;
    eh->checksum = ip_checksum(icmp, sizeof(icmp));

    ping_wait.active     = true;
    ping_wait.id         = id;
    ping_wait.seq        = seq;
    ping_wait.target_ip  = dst_ip;
    ping_wait.replied    = false;
    ping_wait.start_tick = timer_get_ticks();

    int snd = ip_send(dst_ip, IP_PROTO_ICMP, icmp, sizeof(icmp));
    if (snd < 0) {
        net_poll();
        snd = ip_send(dst_ip, IP_PROTO_ICMP, icmp, sizeof(icmp));
    }
    if (snd < 0) {
        ping_wait.active = false;
        return -EIO;
    }

    uint64_t ping_deadline = timer_get_ticks() +
        (uint64_t)timeout_ms * (uint64_t)TIMER_FREQ_HZ / 1000ull;
    if (ping_deadline < ping_wait.start_tick)
        ping_deadline = ping_wait.start_tick;

    while ((int64_t)(ping_deadline - timer_get_ticks()) > 0) {
        if (ping_wait.replied) {
            uint64_t dt = timer_get_ticks() - ping_wait.start_tick;
            *rtt_ms_out =
                (uint32_t)(dt * 1000ull / (uint64_t)TIMER_FREQ_HZ);
            ping_wait.active = false;
            return 0;
        }
        net_poll();
        arch_spin_hint();
    }

    ping_wait.active = false;
    return -ETIMEDOUT;
}

uint32_t net_ipv4_dns_server(void)
{
    if (!g_iface)
        return 0;
    if (g_iface->dns4)
        return g_iface->dns4;
    return g_iface->gateway;
}

int net_wait_arp_ipv4(uint32_t ipv4, uint32_t timeout_ms)
{
    if (!g_iface)
        return -EIO;
    uint32_t hop = ip_next_hop(ipv4);
    uint64_t deadline = timer_get_ticks() +
        (uint64_t)timeout_ms * (uint64_t)TIMER_FREQ_HZ / 1000ull;
    arp_request(hop);
    return icmp_wait_arp(hop, deadline);
}

void net_iface_ipv6_linklocal_from_mac(net_interface_t *iface)
{
    if (!iface)
        return;
    memset(&iface->ipv6_link_local, 0, sizeof(iface->ipv6_link_local));
    iface->ipv6_link_local.b[0] = 0xfe;
    iface->ipv6_link_local.b[1] = 0x80;
    uint8_t *p = iface->ipv6_link_local.b + 8;
    p[0]       = (uint8_t)(iface->mac[0] ^ 0x02);
    p[1]       = iface->mac[1];
    p[2]       = iface->mac[2];
    p[3]       = 0xff;
    p[4]       = 0xfe;
    p[5]       = iface->mac[3];
    p[6]       = iface->mac[4];
    p[7]       = iface->mac[5];
}

static int xdigit(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + c - 'a';
    if (c >= 'A' && c <= 'F')
        return 10 + c - 'A';
    return -1;
}

int net_parse_ipv6(const char *s, ipv6_addr_t *out)
{
    if (!s || !out)
        return -EINVAL;
    memset(out->b, 0, 16);

    char  tmp[128];
    size_t ti = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    while (*s && *s != '%' && ti + 1 < sizeof(tmp))
        tmp[ti++] = *s++;
    tmp[ti] = '\0';
    if (ti == 0)
        return -EINVAL;

    /* ::1 or :: */
    if (tmp[0] == ':' && tmp[1] == ':' && tmp[2] == '\0') {
        return 0;
    }
    if (tmp[0] == ':' && tmp[1] == ':' && tmp[2] == '1' && tmp[3] == '\0') {
        out->b[15] = 1;
        return 0;
    }

    /* Embedded IPv4: ... : a.b.c.d */
    const char *last_colon = NULL;
    for (const char *p = tmp; *p; p++)
        if (*p == ':')
            last_colon = p;
    int dots = 0;
    if (last_colon) {
        for (const char *p = last_colon + 1; *p; p++)
            if (*p == '.')
                dots++;
    }
    if (dots == 3 && last_colon) {
        uint32_t v4;
        if (net_parse_ipv4(last_colon + 1, &v4) != 0)
            return -EINVAL;
        char left[96];
        size_t ll = (size_t)(last_colon - tmp);
        if (ll >= sizeof(left))
            return -EINVAL;
        memcpy(left, tmp, ll);
        left[ll] = '\0';
        uint16_t words[8];
        memset(words, 0, sizeof(words));
        const char *p = left;
        if (*p == ':')
            p++;
        unsigned wi = 0;
        while (*p && wi < 8) {
            if (*p == ':') {
                p++;
                continue;
            }
            unsigned v = 0;
            int      nd = 0;
            while (*p && *p != ':') {
                int xd = xdigit((unsigned char)*p);
                if (xd < 0)
                    return -EINVAL;
                v = (v << 4) | (unsigned)xd;
                if (v > 0xFFFFu)
                    return -EINVAL;
                p++;
                nd++;
            }
            if (nd == 0)
                return -EINVAL;
            words[wi++] = (uint16_t)v;
            if (*p == ':')
                p++;
        }
        for (unsigned i = 0; i < 8; i++) {
            out->b[i * 2]     = (uint8_t)(words[i] >> 8);
            out->b[i * 2 + 1] = (uint8_t)words[i];
        }
        out->b[12] = (uint8_t)(v4 & 0xFF);
        out->b[13] = (uint8_t)((v4 >> 8) & 0xFF);
        out->b[14] = (uint8_t)((v4 >> 16) & 0xFF);
        out->b[15] = (uint8_t)((v4 >> 24) & 0xFF);
        return 0;
    }

    /* Generic: groups separated by :, optional :: */
    uint16_t words[8];
    memset(words, 0, sizeof(words));
    const char *p   = tmp;
    unsigned  wi    = 0;
    int       dbl   = -1;

    if (p[0] == ':' && p[1] == ':') {
        dbl = 0;
        p += 2;
    }

    while (*p) {
        if (p[0] == ':' && p[1] == ':') {
            if (dbl >= 0)
                return -EINVAL;
            dbl = (int)wi;
            p += 2;
            continue;
        }
        if (*p == ':')
            return -EINVAL;
        unsigned v = 0;
        int      nd = 0;
        while (*p && *p != ':') {
            int xd = xdigit((unsigned char)*p);
            if (xd < 0)
                return -EINVAL;
            v = (v << 4) | (unsigned)xd;
            if (v > 0xFFFFu)
                return -EINVAL;
            p++;
            nd++;
        }
        if (nd == 0)
            return -EINVAL;
        if (wi >= 8)
            return -EINVAL;
        words[wi++] = (uint16_t)v;
        if (*p == ':')
            p++;
    }

    unsigned used = wi;
    if (dbl < 0) {
        if (used != 8)
            return -EINVAL;
    } else {
        unsigned ins = 8 - used;
        if (ins + used > 8)
            return -EINVAL;
        uint16_t w2[8];
        memset(w2, 0, sizeof(w2));
        unsigned i = 0, o = 0;
        while (i < (unsigned)dbl && o < 8)
            w2[o++] = words[i++];
        o += ins;
        while (i < used && o < 8)
            w2[o++] = words[i++];
        memcpy(words, w2, sizeof(words));
    }

    for (unsigned i = 0; i < 8; i++) {
        out->b[i * 2]     = (uint8_t)(words[i] >> 8);
        out->b[i * 2 + 1] = (uint8_t)words[i];
    }
    return 0;
}

static void hex2(char *p, uint8_t v)
{
    static const char xd[] = "0123456789abcdef";
    p[0]                   = xd[v >> 4];
    p[1]                   = xd[v & 0x0F];
}

void net_format_ipv6(const ipv6_addr_t *a, char *buf, size_t cap)
{
    if (!a || !buf || cap < 40) {
        if (buf && cap)
            buf[0] = '\0';
        return;
    }
    char *w = buf;
    for (int i = 0; i < 16; i += 2) {
        if (i)
            *w++ = ':';
        hex2(w, a->b[i]);
        w += 2;
        hex2(w, a->b[i + 1]);
        w += 2;
    }
    *w = '\0';
}

/* ── IPv6 / ICMPv6 / NDP (subset) ─────────────────────────────────── */

#define IP6_NXT_ICMP6 58

#define ICMP6_ECHO_REQ 128
#define ICMP6_ECHO_REP 129
#define ICMP6_NS       135
#define ICMP6_NA       136

#define ND_OPT_SRCLLA 1
#define ND_OPT_TGTLLA 2

#define ND_CACHE 24

static struct {
    ipv6_addr_t ip;
    uint8_t     mac[ETH_ADDR_LEN];
    bool        valid;
} nd_tbl[ND_CACHE];

static void nd_cache_add(const ipv6_addr_t *ip, const uint8_t *mac)
{
    for (int i = 0; i < ND_CACHE; i++) {
        if (nd_tbl[i].valid && memcmp(nd_tbl[i].ip.b, ip->b, 16) == 0) {
            memcpy(nd_tbl[i].mac, mac, ETH_ADDR_LEN);
            return;
        }
    }
    for (int i = 0; i < ND_CACHE; i++) {
        if (!nd_tbl[i].valid) {
            nd_tbl[i].ip = *ip;
            memcpy(nd_tbl[i].mac, mac, ETH_ADDR_LEN);
            nd_tbl[i].valid = true;
            return;
        }
    }
    nd_tbl[0].ip = *ip;
    memcpy(nd_tbl[0].mac, mac, ETH_ADDR_LEN);
    nd_tbl[0].valid = true;
}

static const uint8_t *nd_lookup(const ipv6_addr_t *ip)
{
    for (int i = 0; i < ND_CACHE; i++) {
        if (nd_tbl[i].valid && memcmp(nd_tbl[i].ip.b, ip->b, 16) == 0)
            return nd_tbl[i].mac;
    }
    return NULL;
}

static bool ipv6_is_loopback(const ipv6_addr_t *a)
{
    for (int i = 0; i < 15; i++)
        if (a->b[i])
            return false;
    return a->b[15] == 1;
}

static bool ipv6_is_linklocal(const ipv6_addr_t *a)
{
    return a->b[0] == 0xfe && (a->b[1] & 0xc0) == 0x80;
}

static void ipv6_solicited_mcast(const ipv6_addr_t *tgt, ipv6_addr_t *mc)
{
    memset(mc->b, 0, 16);
    mc->b[0]  = 0xff;
    mc->b[1]  = 0x02;
    mc->b[11] = 1;
    mc->b[12] = 0xff;
    mc->b[13] = tgt->b[13];
    mc->b[14] = tgt->b[14];
    mc->b[15] = tgt->b[15];
}

static void ipv6_mcast_mac(const ipv6_addr_t *ip, uint8_t *mac)
{
    mac[0] = 0x33;
    mac[1] = 0x33;
    mac[2] = ip->b[12];
    mac[3] = ip->b[13];
    mac[4] = ip->b[14];
    mac[5] = ip->b[15];
}

static bool ipv6_solicited_for_us(const ipv6_addr_t *d)
{
    const ipv6_addr_t *me = &g_iface->ipv6_link_local;
    if (d->b[0] != 0xff || d->b[1] != 0x02)
        return false;
    if (d->b[11] != 1 || d->b[12] != 0xff)
        return false;
    return d->b[13] == me->b[13] && d->b[14] == me->b[14] &&
           d->b[15] == me->b[15];
}

static bool ipv6_dst_for_us(const ipv6_addr_t *d)
{
    if (memcmp(d->b, g_iface->ipv6_link_local.b, 16) == 0)
        return true;
    return ipv6_solicited_for_us(d);
}

static uint16_t icmp6_csum(const ipv6_addr_t *src, const ipv6_addr_t *dst,
                           uint8_t nxt, const uint8_t *icmp, size_t icmplen)
{
    uint8_t scratch[40 + 256];
    if (icmplen > sizeof(scratch) - 40)
        return 0;
    memcpy(scratch, src->b, 16);
    memcpy(scratch + 16, dst->b, 16);
    uint32_t lbe = htonl((uint32_t)icmplen);
    memcpy(scratch + 32, &lbe, 4);
    scratch[36] = scratch[37] = scratch[38] = 0;
    scratch[39] = nxt;
    memcpy(scratch + 40, icmp, icmplen);
    if (icmplen >= 4) {
        scratch[42] = 0;
        scratch[43] = 0;
    }
    return ip_checksum(scratch, 40 + icmplen);
}

static int ipv6_send_eth(const uint8_t *dst_mac, const ipv6_addr_t *src,
                         const ipv6_addr_t *dst, uint8_t nxt,
                         const void *payload, size_t plen)
{
    if (!g_iface || plen + 40 > ETH_MTU)
        return -EINVAL;
    uint8_t pkt[ETH_MTU];
    memset(pkt, 0, 40);
    pkt[0] = 0x60;
    uint16_t plbe = htons((uint16_t)plen);
    memcpy(pkt + 4, &plbe, 2);
    pkt[6]  = nxt;
    pkt[7]  = (nxt == IP6_NXT_ICMP6 && plen >= 4 &&
               (payload && ((const uint8_t *)payload)[0] >= ICMP6_NS)) ?
                  (uint8_t)255 :
                  (uint8_t)64;
    memcpy(pkt + 8, src->b, 16);
    memcpy(pkt + 24, dst->b, 16);
    memcpy(pkt + 40, payload, plen);
    return eth_send(dst_mac, ETH_TYPE_IPV6, pkt, 40 + plen);
}

static void ndp_send_ns(const ipv6_addr_t *target)
{
    ipv6_addr_t mc;
    uint8_t     dmac[ETH_ADDR_LEN];
    ipv6_solicited_mcast(target, &mc);
    ipv6_mcast_mac(&mc, dmac);

    uint8_t icmp[32];
    memset(icmp, 0, sizeof(icmp));
    icmp[0] = ICMP6_NS;
    icmp[1] = 0;
    memcpy(icmp + 8, target->b, 16);
    icmp[24] = ND_OPT_SRCLLA;
    icmp[25] = 1;
    memcpy(icmp + 26, g_iface->mac, ETH_ADDR_LEN);

    uint16_t cs = icmp6_csum(&g_iface->ipv6_link_local, &mc, IP6_NXT_ICMP6,
                              icmp, 32);
    icmp[2] = (uint8_t)(cs & 0xFF);
    icmp[3] = (uint8_t)(cs >> 8);

    (void)ipv6_send_eth(dmac, &g_iface->ipv6_link_local, &mc, IP6_NXT_ICMP6,
                        icmp, 32);
}

static int ndp_resolve(const ipv6_addr_t *tgt, uint8_t *mac_out,
                       uint32_t timeout_ms)
{
    const uint8_t *e = nd_lookup(tgt);
    if (e) {
        memcpy(mac_out, e, ETH_ADDR_LEN);
        return 0;
    }
    ndp_send_ns(tgt);
    uint64_t deadline = timer_get_ticks() +
        (uint64_t)timeout_ms * (uint64_t)TIMER_FREQ_HZ / 1000ull;
    while ((int64_t)(deadline - timer_get_ticks()) > 0) {
        e = nd_lookup(tgt);
        if (e) {
            memcpy(mac_out, e, ETH_ADDR_LEN);
            return 0;
        }
        net_poll();
        arch_spin_hint();
    }
    return -ETIMEDOUT;
}

static void ndp_send_na(const ipv6_addr_t *dst_ip, const uint8_t *dst_mac,
                        const ipv6_addr_t *target)
{
    uint8_t icmp[32];
    memset(icmp, 0, sizeof(icmp));
    icmp[0] = ICMP6_NA;
    icmp[1] = 0;
    icmp[4] = 0x40; /* Solicited */
    memcpy(icmp + 8, target->b, 16);
    icmp[24] = ND_OPT_TGTLLA;
    icmp[25] = 1;
    memcpy(icmp + 26, g_iface->mac, ETH_ADDR_LEN);

    uint16_t cs =
        icmp6_csum(&g_iface->ipv6_link_local, dst_ip, IP6_NXT_ICMP6, icmp, 32);
    icmp[2] = (uint8_t)(cs & 0xFF);
    icmp[3] = (uint8_t)(cs >> 8);

    (void)ipv6_send_eth(dst_mac, &g_iface->ipv6_link_local, dst_ip,
                        IP6_NXT_ICMP6, icmp, 32);
}

static void icmp6_process(const ipv6_addr_t *src, const ipv6_addr_t *dst,
                          const uint8_t *pay, size_t len)
{
    (void)dst;
    if (len < 8 || !g_iface)
        return;
    uint8_t type = pay[0];

    if (type == ICMP6_ECHO_REP) {
        if (!ping6_wait.active || len < 8)
            return;
        uint16_t rid =
            (uint16_t)(((uint16_t)pay[4] << 8) | pay[5]);
        uint16_t rseq =
            (uint16_t)(((uint16_t)pay[6] << 8) | pay[7]);
        if (memcmp(src->b, ping6_wait.target.b, 16) == 0 &&
            rid == ping6_wait.id && rseq == ping6_wait.seq)
            ping6_wait.replied = true;
        return;
    }

    if (type == ICMP6_ECHO_REQ && len >= 8) {
        uint8_t  reply[128];
        size_t   rlen = (len > sizeof(reply)) ? sizeof(reply) : len;
        memcpy(reply, pay, rlen);
        reply[0] = ICMP6_ECHO_REP;
        reply[2] = reply[3] = 0;
        uint16_t cs = icmp6_csum(&g_iface->ipv6_link_local, src, IP6_NXT_ICMP6,
                                  reply, rlen);
        reply[2] = (uint8_t)(cs & 0xFF);
        reply[3] = (uint8_t)(cs >> 8);
        const uint8_t *dm = nd_lookup(src);
        uint8_t          mac_buf[ETH_ADDR_LEN];
        if (!dm) {
            if (ndp_resolve(src, mac_buf, 500) < 0)
                return;
            dm = mac_buf;
        }
        (void)ipv6_send_eth(dm, &g_iface->ipv6_link_local, src, IP6_NXT_ICMP6,
                            reply, rlen);
        return;
    }

    if (type == ICMP6_NS && len >= 24) {
        ipv6_addr_t tgt;
        memcpy(tgt.b, pay + 8, 16);
        if (memcmp(tgt.b, g_iface->ipv6_link_local.b, 16) != 0)
            return;
        const uint8_t *smac = NULL;
        uint8_t          optmac[ETH_ADDR_LEN];
        size_t           off = 24;
        while (off + 2 <= len) {
            uint8_t ot = pay[off];
            uint8_t olen = pay[off + 1];
            if (olen == 0)
                break;
            size_t ob = (size_t)olen * 8u;
            if (off + ob > len)
                break;
            if (ot == ND_OPT_SRCLLA && ob >= 8) {
                memcpy(optmac, pay + off + 2, ETH_ADDR_LEN);
                smac = optmac;
                break;
            }
            off += ob;
        }
        if (!smac)
            return;
        ndp_send_na(src, smac, &tgt);
        return;
    }

    if (type == ICMP6_NA && len >= 24) {
        ipv6_addr_t tgt;
        memcpy(tgt.b, pay + 8, 16);
        size_t off = 24;
        while (off + 2 <= len) {
            uint8_t ot = pay[off];
            uint8_t olen = pay[off + 1];
            if (olen == 0)
                break;
            size_t ob = (size_t)olen * 8u;
            if (off + ob > len)
                break;
            if (ot == ND_OPT_TGTLLA && ob >= 8) {
                nd_cache_add(&tgt, pay + off + 2);
                break;
            }
            off += ob;
        }
    }
}

static int ipv6_receive(const void *frame, size_t len)
{
    if (!g_iface || len < sizeof(eth_header_t) + 40)
        return -EINVAL;

    const uint8_t *eth = (const uint8_t *)frame;
    const uint8_t *ip6 = eth + sizeof(eth_header_t);
    if ((ip6[0] & 0xf0) != 0x60)
        return 0;

    uint16_t plen =
        (uint16_t)(((uint16_t)ip6[4] << 8) | ip6[5]);
    if (sizeof(eth_header_t) + 40u + (size_t)plen > len)
        return -EINVAL;

    ipv6_addr_t dst, src;
    memcpy(dst.b, ip6 + 24, 16);
    memcpy(src.b, ip6 + 8, 16);

    if (!ipv6_dst_for_us(&dst))
        return 0;

    uint8_t nxt = ip6[6];
    if (nxt != IP6_NXT_ICMP6)
        return 0;

    icmp6_process(&src, &dst, ip6 + 40, plen);
    return 0;
}

int icmp6_ping(const ipv6_addr_t *dst, uint32_t timeout_ms, uint32_t *rtt_ms_out)
{
    if (!dst || !rtt_ms_out || !g_iface || !g_iface->is_up)
        return -EINVAL;

    if (ipv6_is_loopback(dst)) {
        *rtt_ms_out = 0;
        return 0;
    }

    if (ping6_wait.active || ping_wait.active)
        return -EBUSY;

    uint8_t dmac[ETH_ADDR_LEN];
    if (ipv6_is_linklocal(dst)) {
        if (ndp_resolve(dst, dmac, 2000) < 0)
            return -ETIMEDOUT;
    } else {
        if (net_wait_arp_ipv4(g_iface->gateway, 2000) < 0)
            return -ETIMEDOUT;
        const uint8_t *gm = arp_lookup(g_iface->gateway);
        if (!gm)
            return -EIO;
        memcpy(dmac, gm, ETH_ADDR_LEN);
    }

    enum { pld = 32 };
    uint8_t icmp[8 + pld];
    memset(icmp, 0, sizeof(icmp));
    icmp[0] = ICMP6_ECHO_REQ;
    icmp[1] = 0;
    uint16_t id  = 0xAE6u;
    uint16_t seq = ++ping6_seq_g;
    icmp[4] = (uint8_t)(id >> 8);
    icmp[5] = (uint8_t)id;
    icmp[6] = (uint8_t)(seq >> 8);
    icmp[7] = (uint8_t)seq;
    for (int i = 0; i < pld; i++)
        icmp[8 + i] = (uint8_t)i;

    uint16_t cs = icmp6_csum(&g_iface->ipv6_link_local, dst, IP6_NXT_ICMP6,
                             icmp, sizeof(icmp));
    icmp[2] = (uint8_t)(cs & 0xFF);
    icmp[3] = (uint8_t)(cs >> 8);

    ping6_wait.active     = true;
    ping6_wait.id         = id;
    ping6_wait.seq        = seq;
    ping6_wait.target     = *dst;
    ping6_wait.replied    = false;
    ping6_wait.start_tick = timer_get_ticks();

    if (ipv6_send_eth(dmac, &g_iface->ipv6_link_local, dst, IP6_NXT_ICMP6,
                       icmp, sizeof(icmp)) < 0) {
        ping6_wait.active = false;
        return -EIO;
    }

    uint64_t deadline = timer_get_ticks() +
        (uint64_t)timeout_ms * (uint64_t)TIMER_FREQ_HZ / 1000ull;
    while ((int64_t)(deadline - timer_get_ticks()) > 0) {
        if (ping6_wait.replied) {
            uint64_t dt = timer_get_ticks() - ping6_wait.start_tick;
            *rtt_ms_out =
                (uint32_t)(dt * 1000ull / (uint64_t)TIMER_FREQ_HZ);
            ping6_wait.active = false;
            return 0;
        }
        net_poll();
        arch_spin_hint();
    }

    ping6_wait.active = false;
    return -ETIMEDOUT;
}

int ip_receive(const void *frame, size_t len)
{
    if (!g_iface)
        return -EINVAL;
    if (len < sizeof(eth_header_t) + sizeof(ipv4_header_t))
        return -EINVAL;

    const ipv4_header_t *ip = (const ipv4_header_t *)
        ((const uint8_t *)frame + sizeof(eth_header_t));

    uint16_t ihl  = (ip->ver_ihl & 0x0F) * 4;
    uint16_t tlen = ntohs(ip->total_length);
    if (tlen < ihl) return -EINVAL;

    const uint8_t *payload = (const uint8_t *)ip + ihl;
    size_t payload_len = tlen - ihl;

    /* only accept packets for our IP or broadcast */
    if (ip->dst_ip != g_iface->ip && ip->dst_ip != 0xFFFFFFFF)
        return 0;

    switch (ip->protocol) {
    case IP_PROTO_UDP:
        udp_process(ip->src_ip, payload, payload_len);
        break;
    case IP_PROTO_TCP:
        tcp_process(ip->src_ip, payload, payload_len);
        break;
    case IP_PROTO_ICMP:
        icmp_process(ip->src_ip, payload, payload_len);
        break;
    default:
        break;
    }
    return 0;
}

/* ── UDP ──────────────────────────────────────────────────────────── */

int udp_bind(uint16_t port, udp_recv_callback_t cb, void *userdata)
{
    spin_lock(&net_lock);
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (!udp_sockets[i].in_use) {
            udp_sockets[i].in_use     = true;
            udp_sockets[i].local_port = port;
            udp_sockets[i].callback   = cb;
            udp_sockets[i].userdata   = userdata;
            spin_unlock(&net_lock);
            return i;
        }
    }
    spin_unlock(&net_lock);
    return -ENOMEM;
}

int udp_unbind(uint16_t port)
{
    spin_lock(&net_lock);
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (udp_sockets[i].in_use && udp_sockets[i].local_port == port) {
            udp_sockets[i].in_use = false;
            spin_unlock(&net_lock);
            return 0;
        }
    }
    spin_unlock(&net_lock);
    return -ENOENT;
}

int udp_send(uint32_t dst_ip, uint16_t dst_port,
             uint16_t src_port, const void *data, size_t len)
{
    if (len > ETH_MTU - sizeof(ipv4_header_t) - sizeof(udp_header_t))
        return -EINVAL;

    uint8_t buf[ETH_MTU];
    udp_header_t *udp = (udp_header_t *)buf;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons((uint16_t)(sizeof(udp_header_t) + len));
    udp->checksum = 0; /* optional for IPv4 */

    memcpy(buf + sizeof(udp_header_t), data, len);
    return ip_send(dst_ip, IP_PROTO_UDP, buf, sizeof(udp_header_t) + len);
}

static void udp_process(uint32_t src_ip, const void *data, size_t len)
{
    if (len < sizeof(udp_header_t)) return;

    const udp_header_t *hdr = (const udp_header_t *)data;
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint16_t src_port = ntohs(hdr->src_port);

    const uint8_t *payload = (const uint8_t *)data + sizeof(udp_header_t);
    size_t payload_len = len - sizeof(udp_header_t);

    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (udp_sockets[i].in_use && udp_sockets[i].local_port == dst_port) {
            if (udp_sockets[i].callback)
                udp_sockets[i].callback(src_ip, src_port,
                                        payload, payload_len,
                                        udp_sockets[i].userdata);
            return;
        }
    }
}

/* ── TCP (simplified state machine) ───────────────────────────────── */

static tcp_socket_t *get_tcp_socket(int sock)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) return NULL;
    if (!tcp_sockets[sock].in_use) return NULL;
    return &tcp_sockets[sock];
}

static int tcp_send_segment(tcp_socket_t *s, uint8_t flags,
                            const void *data, size_t len)
{
    uint8_t buf[ETH_MTU];
    tcp_header_t *tcp = (tcp_header_t *)buf;
    memset(tcp, 0, sizeof(*tcp));

    tcp->src_port    = htons(s->local_port);
    tcp->dst_port    = htons(s->remote_port);
    tcp->seq_num     = htonl(s->seq);
    tcp->ack_num     = htonl(s->ack);
    tcp->data_offset = (5 << 4); /* 20 bytes, no options */
    tcp->flags       = flags;
    tcp->window      = htons(TCP_RX_BUF_SIZE);
    tcp->checksum    = 0;

    if (data && len > 0)
        memcpy(buf + sizeof(tcp_header_t), data, len);

    size_t total = sizeof(tcp_header_t) + len;
    int rc = ip_send(s->remote_ip, IP_PROTO_TCP, buf, total);

    if (flags & TCP_SYN) s->seq++;
    if (flags & TCP_FIN) s->seq++;
    if (len > 0) s->seq += (uint32_t)len;

    return rc;
}

int tcp_socket_create(void)
{
    spin_lock(&net_lock);
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!tcp_sockets[i].in_use) {
            memset(&tcp_sockets[i], 0, sizeof(tcp_socket_t));
            tcp_sockets[i].in_use = true;
            tcp_sockets[i].state  = TCP_STATE_CLOSED;
            tcp_sockets[i].seq    = 1000; /* initial sequence number */
            spin_unlock(&net_lock);
            return i;
        }
    }
    spin_unlock(&net_lock);
    return -ENOMEM;
}

int tcp_connect(int sock, uint32_t ip, uint16_t port)
{
    tcp_socket_t *s = get_tcp_socket(sock);
    if (!s || s->state != TCP_STATE_CLOSED) return -EINVAL;

    s->remote_ip   = ip;
    s->remote_port = port;
    s->local_ip    = g_iface ? g_iface->ip : 0;
    s->local_port  = 49152 + (uint16_t)(sock * 7 + 1); /* ephemeral port */
    s->state       = TCP_STATE_SYN_SENT;

    return tcp_send_segment(s, TCP_SYN, NULL, 0);
}

int tcp_listen(int sock, uint16_t port)
{
    tcp_socket_t *s = get_tcp_socket(sock);
    if (!s || s->state != TCP_STATE_CLOSED) return -EINVAL;

    s->local_port = port;
    s->local_ip   = g_iface ? g_iface->ip : 0;
    s->state      = TCP_STATE_LISTEN;
    return 0;
}

ssize_t tcp_send(int sock, const void *data, size_t len)
{
    tcp_socket_t *s = get_tcp_socket(sock);
    if (!s || s->state != TCP_STATE_ESTABLISHED) return -EINVAL;

    size_t max_payload = ETH_MTU - sizeof(ipv4_header_t) - sizeof(tcp_header_t);
    size_t sent = 0;

    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > max_payload)
            chunk = max_payload;

        int rc = tcp_send_segment(s, TCP_ACK | TCP_PSH,
                                  (const uint8_t *)data + sent, chunk);
        if (rc < 0) return (sent > 0) ? (ssize_t)sent : rc;
        sent += chunk;
    }
    return (ssize_t)sent;
}

ssize_t tcp_recv(int sock, void *buf, size_t max_len)
{
    tcp_socket_t *s = get_tcp_socket(sock);
    if (!s || s->state != TCP_STATE_ESTABLISHED) return -EINVAL;

    spin_lock(&net_lock);
    if (s->rx_len == 0) {
        spin_unlock(&net_lock);
        return 0;
    }

    size_t n = (max_len < s->rx_len) ? max_len : s->rx_len;
    memcpy(buf, s->rx_buf, n);

    /* shift remaining data */
    if (n < s->rx_len)
        memmove(s->rx_buf, s->rx_buf + n, s->rx_len - n);
    s->rx_len -= n;

    spin_unlock(&net_lock);
    return (ssize_t)n;
}

int tcp_close(int sock)
{
    tcp_socket_t *s = get_tcp_socket(sock);
    if (!s) return -EINVAL;

    if (s->state == TCP_STATE_ESTABLISHED) {
        s->state = TCP_STATE_FIN_WAIT_1;
        tcp_send_segment(s, TCP_FIN | TCP_ACK, NULL, 0);
    }

    s->in_use = false;
    s->state  = TCP_STATE_CLOSED;
    return 0;
}

static void tcp_process(uint32_t src_ip, const void *data, size_t len)
{
    if (len < sizeof(tcp_header_t)) return;

    const tcp_header_t *hdr = (const tcp_header_t *)data;
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint16_t src_port = ntohs(hdr->src_port);
    uint32_t seq      = ntohl(hdr->seq_num);
    uint8_t  flags    = hdr->flags;

    uint8_t data_off = (hdr->data_offset >> 4) * 4;
    const uint8_t *payload = (const uint8_t *)data + data_off;
    size_t payload_len = (len > data_off) ? len - data_off : 0;

    /* find matching socket */
    tcp_socket_t *s = NULL;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!tcp_sockets[i].in_use) continue;

        if (tcp_sockets[i].state == TCP_STATE_LISTEN &&
            tcp_sockets[i].local_port == dst_port) {
            s = &tcp_sockets[i];
            break;
        }
        if (tcp_sockets[i].local_port == dst_port &&
            tcp_sockets[i].remote_port == src_port &&
            tcp_sockets[i].remote_ip == src_ip) {
            s = &tcp_sockets[i];
            break;
        }
    }
    if (!s) return;

    switch (s->state) {
    case TCP_STATE_LISTEN:
        if (flags & TCP_SYN) {
            s->remote_ip   = src_ip;
            s->remote_port = src_port;
            s->ack         = seq + 1;
            s->state       = TCP_STATE_SYN_RECEIVED;
            tcp_send_segment(s, TCP_SYN | TCP_ACK, NULL, 0);
        }
        break;

    case TCP_STATE_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            s->ack   = seq + 1;
            s->state = TCP_STATE_ESTABLISHED;
            tcp_send_segment(s, TCP_ACK, NULL, 0);
            klog("tcp: connection established (port %u)\n", s->local_port);
        }
        break;

    case TCP_STATE_SYN_RECEIVED:
        if (flags & TCP_ACK) {
            s->state = TCP_STATE_ESTABLISHED;
            klog("tcp: connection established (port %u)\n", s->local_port);
        }
        break;

    case TCP_STATE_ESTABLISHED:
        if (flags & TCP_FIN) {
            s->ack = seq + 1;
            s->state = TCP_STATE_CLOSE_WAIT;
            tcp_send_segment(s, TCP_ACK, NULL, 0);
            s->state = TCP_STATE_LAST_ACK;
            tcp_send_segment(s, TCP_FIN | TCP_ACK, NULL, 0);
        } else if (payload_len > 0) {
            spin_lock(&net_lock);
            size_t space = TCP_RX_BUF_SIZE - s->rx_len;
            size_t copy = (payload_len < space) ? payload_len : space;
            if (copy > 0) {
                memcpy(s->rx_buf + s->rx_len, payload, copy);
                s->rx_len += copy;
            }
            spin_unlock(&net_lock);

            s->ack = seq + (uint32_t)payload_len;
            tcp_send_segment(s, TCP_ACK, NULL, 0);
        }
        break;

    case TCP_STATE_FIN_WAIT_1:
        if ((flags & (TCP_FIN | TCP_ACK)) == (TCP_FIN | TCP_ACK)) {
            s->ack = seq + 1;
            s->state = TCP_STATE_TIME_WAIT;
            tcp_send_segment(s, TCP_ACK, NULL, 0);
        } else if (flags & TCP_ACK) {
            s->state = TCP_STATE_FIN_WAIT_2;
        }
        break;

    case TCP_STATE_FIN_WAIT_2:
        if (flags & TCP_FIN) {
            s->ack = seq + 1;
            s->state = TCP_STATE_TIME_WAIT;
            tcp_send_segment(s, TCP_ACK, NULL, 0);
        }
        break;

    case TCP_STATE_LAST_ACK:
        if (flags & TCP_ACK) {
            s->state  = TCP_STATE_CLOSED;
            s->in_use = false;
        }
        break;

    default:
        break;
    }
}

/* ── DHCP (simplified: discover → offer → request → ack) ──────────── */

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC       0x63825363

typedef struct PACKED {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[312];
} dhcp_packet_t;

static bool dhcp_done = false;

static void dhcp_recv_handler(uint32_t src_ip, uint16_t src_port,
                               const void *data, size_t len, void *userdata)
{
    (void)src_ip;
    (void)src_port;
    (void)userdata;

    if (len < sizeof(dhcp_packet_t) - 312)
        return;

    const dhcp_packet_t *reply = (const dhcp_packet_t *)data;
    if (reply->op != 2) return; /* BOOTREPLY */

    /* extract offered IP */
    uint32_t offered_ip = reply->yiaddr;
    if (offered_ip && g_iface) {
        g_iface->ip = offered_ip;
        g_iface->is_up = true;
        dhcp_done = true;

        /* parse options for subnet mask and gateway */
        const uint8_t *opt = reply->options;
        const uint8_t *opt_end = opt + 312;
        while (opt < opt_end && *opt != 0xFF) {
            uint8_t code = *opt++;
            if (code == 0) continue; /* padding */
            if (opt >= opt_end) break;
            uint8_t olen = *opt++;
            if (opt + olen > opt_end) break;

            if (code == 1 && olen >= 4) /* subnet mask */
                memcpy(&g_iface->netmask, opt, 4);
            else if (code == 3 && olen >= 4) /* router/gateway */
                memcpy(&g_iface->gateway, opt, 4);

            opt += olen;
        }

        klog("dhcp: got ip %u.%u.%u.%u\n",
             offered_ip & 0xFF, (offered_ip >> 8) & 0xFF,
             (offered_ip >> 16) & 0xFF, (offered_ip >> 24) & 0xFF);
    }
}

int dhcp_discover(void)
{
    if (!g_iface) return -EIO;

    int rc = udp_bind(DHCP_CLIENT_PORT, dhcp_recv_handler, NULL);
    if (rc < 0) return rc;

    dhcp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.op    = 1;           /* BOOTREQUEST */
    pkt.htype = 1;           /* Ethernet */
    pkt.hlen  = ETH_ADDR_LEN;
    pkt.xid   = htonl(0x12345678);
    pkt.flags = htons(0x8000); /* broadcast */
    memcpy(pkt.chaddr, g_iface->mac, ETH_ADDR_LEN);
    pkt.magic = htonl(DHCP_MAGIC);

    /* options: DHCP Discover */
    pkt.options[0] = 53;  /* DHCP message type */
    pkt.options[1] = 1;   /* length */
    pkt.options[2] = 1;   /* DISCOVER */
    pkt.options[3] = 0xFF; /* end */

    return udp_send(0xFFFFFFFF, DHCP_SERVER_PORT, DHCP_CLIENT_PORT,
                    &pkt, sizeof(pkt));
}

/* ── Frame dispatcher ─────────────────────────────────────────────── */

int net_process_frame(const void *frame, size_t len)
{
    if (len < sizeof(eth_header_t))
        return -EINVAL;

    const eth_header_t *hdr = (const eth_header_t *)frame;
    uint16_t type = ntohs(hdr->ethertype);

    switch (type) {
    case ETH_TYPE_ARP:
        return arp_process(frame, len);
    case ETH_TYPE_IPV4:
        return ip_receive(frame, len);
    case ETH_TYPE_IPV6:
        return ipv6_receive(frame, len);
    default:
        return 0;
    }
}

/* ── PCI NIC detection (virtio-net / e1000) — uses kernel PCI enumerator ─ */

int net_detect_pci(void)
{
    klog("net: scanning PCI for network controllers...\n");

    for (uint32_t i = 0; i < pci_get_device_count(); i++) {
        pci_device_t *d = pci_get_device(i);
        if (!d)
            continue;

        if (d->vendor_id == 0x8086 &&
            (d->device_id == 0x100E || d->device_id == 0x100F ||
             d->device_id == 0x10D3 || d->device_id == 0x153A)) {
            klog("net: found Intel e1000 at PCI %u:%u.%u\n",
                 d->bus, d->device, d->function);
            return 0;
        }

        if (d->vendor_id == 0x1AF4 && d->device_id == 0x1041) {
            klog("net: found virtio-net (modern) at PCI %u:%u.%u\n",
                 d->bus, d->device, d->function);
            return 0;
        }

        if (d->vendor_id == 0x1AF4 && d->device_id >= 0x1000 &&
            d->device_id <= 0x103F) {
            uint32_t sub = pci_read_config(d->bus, d->device, d->function, 0x2C);
            if ((sub >> 16) == 1) {
                klog("net: found virtio-net (transitional) at PCI %u:%u.%u\n",
                     d->bus, d->device, d->function);
                return 0;
            }
        }
    }

    klog("net: no supported network card found\n");
    return -ENOENT;
}
