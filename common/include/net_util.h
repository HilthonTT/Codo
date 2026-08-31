#ifndef NET_UTIL_H
#define NET_UTIL_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

// Address helpers shared by the server and the balancer. Everything that used
// to assume struct sockaddr_in goes through these instead, so both binaries
// accept IPv4 and IPv6 clients without duplicating the family switch.

// Longest text form we ever produce: an IPv6 address, or an IPv4-mapped one
// ("::ffff:192.0.2.1"), plus the NUL.
#define NET_ADDR_STRLEN INET6_ADDRSTRLEN

// Render `ss` as a bare address (no port, no brackets) into out. An
// IPv4-mapped IPv6 address is unwrapped to its dotted-quad form, so a client
// reaching a dual-stack listener over IPv4 logs and rate-limits identically to
// one reaching an IPv4-only listener. Writes "-" and returns -1 if the family
// is unknown; out is always NUL-terminated.
int net_addr_str(const struct sockaddr_storage *ss, char *out, size_t out_size);

// Stable key for the raw address bytes, for hash tables that bucket by client
// (rate limiting, ip-hash backend selection). IPv4 and the IPv4-mapped form of
// the same address hash alike, so a dual-stack listener does not hand one
// client two buckets. Port is deliberately excluded.
uint64_t net_addr_hash(const struct sockaddr_storage *ss);

// Compare two addresses by address bytes only (ignoring port), with the same
// IPv4/IPv4-mapped equivalence as net_addr_hash. Returns true when equal.
bool net_addr_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b);

// Create a listening socket bound to `port` on all interfaces. Prefers a
// dual-stack AF_INET6 socket (IPV6_V6ONLY off, so IPv4 clients arrive as
// IPv4-mapped addresses) and falls back to AF_INET where IPv6 is unavailable.
// Applies SO_REUSEADDR and listens with `backlog`. Returns the fd, or -1.
int net_listen_any(uint16_t port, int backlog);

// Resolve host:port for an outbound connection, filling `out`. Accepts IPv4
// literals, IPv6 literals, and hostnames. Returns 0 on success.
int net_resolve(const char *host, uint16_t port, struct sockaddr_storage *out,
                socklen_t *out_len);

// Byte length of the sockaddr for its family (what connect/bind expect).
socklen_t net_addr_len(const struct sockaddr_storage *ss);

// Effective IP version: 4, 6, or 0 if unknown. An IPv4-mapped IPv6 address
// reports 4, matching how net_addr_str renders it.
int net_addr_version(const struct sockaddr_storage *ss);

// Port in host byte order, or 0 if the family is unknown.
uint16_t net_addr_port(const struct sockaddr_storage *ss);

// Build a sockaddr from a numeric address string and port. Returns 0 on
// success. Used to reconstruct the peer from a PROXY protocol header.
int net_addr_from_str(const char *addr, uint16_t port, struct sockaddr_storage *out);

#endif
