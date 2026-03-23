#include "lwip_port.h"
#include "../drivers/pci.h"
#include "../drivers/virtio_net.h"
#include <aevos/config.h>
#include <kernel/mm/slab.h>
#include <kernel/klog.h>
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

int ip_receive(const void *frame, size_t len)
{
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
