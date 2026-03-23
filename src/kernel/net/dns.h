#pragma once

#include "lwip_port.h"

/* UDP DNS (A / AAAA), follows CNAME a few hops. Polls net internally. */
int dns_resolve_a(const char *name, uint32_t *ipv4_out, uint32_t timeout_ms);
int dns_resolve_aaaa(const char *name, ipv6_addr_t *ipv6_out, uint32_t timeout_ms);
