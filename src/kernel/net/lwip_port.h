#pragma once

#include <aevos/types.h>

/* ── Ethernet / MAC ───────────────────────────────────────────────── */

#define ETH_ADDR_LEN   6
#define ETH_MTU        1500
#define ETH_FRAME_MAX  (ETH_MTU + 14)

#define ETH_TYPE_ARP   0x0806
#define ETH_TYPE_IPV4  0x0800

typedef struct PACKED {
    uint8_t  dst[ETH_ADDR_LEN];
    uint8_t  src[ETH_ADDR_LEN];
    uint16_t ethertype;
} eth_header_t;

/* ── ARP ──────────────────────────────────────────────────────────── */

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2
#define ARP_TABLE_SIZE 64

typedef struct PACKED {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t opcode;
    uint8_t  sender_mac[ETH_ADDR_LEN];
    uint32_t sender_ip;
    uint8_t  target_mac[ETH_ADDR_LEN];
    uint32_t target_ip;
} arp_packet_t;

typedef struct {
    uint32_t ip;
    uint8_t  mac[ETH_ADDR_LEN];
    bool     valid;
    uint64_t timestamp;
} arp_entry_t;

/* ── IPv4 ─────────────────────────────────────────────────────────── */

#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

typedef struct PACKED {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragoff;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} ipv4_header_t;

/* ── UDP ──────────────────────────────────────────────────────────── */

typedef struct PACKED {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_header_t;

#define UDP_MAX_SOCKETS 32

typedef void (*udp_recv_callback_t)(uint32_t src_ip, uint16_t src_port,
                                    const void *data, size_t len, void *userdata);

typedef struct {
    uint16_t            local_port;
    bool                in_use;
    udp_recv_callback_t callback;
    void               *userdata;
} udp_socket_t;

/* ── TCP (simplified) ─────────────────────────────────────────────── */

typedef struct PACKED {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;   /* upper 4 bits = offset in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} tcp_header_t;

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

typedef enum {
    TCP_STATE_CLOSED,
    TCP_STATE_LISTEN,
    TCP_STATE_SYN_SENT,
    TCP_STATE_SYN_RECEIVED,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_LAST_ACK,
    TCP_STATE_TIME_WAIT
} tcp_state_t;

#define TCP_MAX_SOCKETS   16
#define TCP_RX_BUF_SIZE   8192
#define TCP_TX_BUF_SIZE   8192

typedef struct {
    bool        in_use;
    tcp_state_t state;
    uint32_t    local_ip;
    uint16_t    local_port;
    uint32_t    remote_ip;
    uint16_t    remote_port;
    uint32_t    seq;
    uint32_t    ack;
    uint8_t     rx_buf[TCP_RX_BUF_SIZE];
    size_t      rx_len;
    uint8_t     tx_buf[TCP_TX_BUF_SIZE];
    size_t      tx_len;
} tcp_socket_t;

/* ── Network interface ────────────────────────────────────────────── */

typedef ssize_t (*net_send_fn)(const void *data, size_t len);
typedef ssize_t (*net_recv_fn)(void *data, size_t max_len);

typedef struct {
    uint8_t    mac[ETH_ADDR_LEN];
    uint32_t   ip;
    uint32_t   netmask;
    uint32_t   gateway;
    bool       is_up;
    net_send_fn send_raw;
    net_recv_fn recv_raw;
} net_interface_t;

typedef struct {
    void    *data;
    size_t   length;
    uint8_t  protocol;
} net_packet_t;

/* ── IP address construction ──────────────────────────────────────── */

#define IP4(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* ── Public API ───────────────────────────────────────────────────── */

int  net_init(void);
void net_set_interface(net_interface_t *iface);

/* Ethernet */
int  eth_send(const uint8_t *dst_mac, uint16_t ethertype,
              const void *payload, size_t len);
int  eth_receive(void *buf, size_t max_len);

/* ARP */
int  arp_request(uint32_t target_ip);
int  arp_reply(const uint8_t *target_mac, uint32_t target_ip);
int  arp_process(const void *frame, size_t len);
const uint8_t *arp_lookup(uint32_t ip);

/* IPv4 */
int  ip_send(uint32_t dst_ip, uint8_t protocol,
             const void *payload, size_t len);
int  ip_receive(const void *frame, size_t len);

/* UDP */
int      udp_bind(uint16_t port, udp_recv_callback_t cb, void *userdata);
int      udp_send(uint32_t dst_ip, uint16_t dst_port,
                  uint16_t src_port, const void *data, size_t len);
int      udp_unbind(uint16_t port);

/* TCP */
int      tcp_socket_create(void);
int      tcp_connect(int sock, uint32_t ip, uint16_t port);
int      tcp_listen(int sock, uint16_t port);
ssize_t  tcp_send(int sock, const void *data, size_t len);
ssize_t  tcp_recv(int sock, void *buf, size_t max_len);
int      tcp_close(int sock);

/* DHCP */
int      dhcp_discover(void);

/* Incoming frame dispatch (called from NIC driver interrupt) */
int      net_process_frame(const void *frame, size_t len);

/* PCI detection */
int      net_detect_pci(void);
