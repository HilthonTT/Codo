#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "net_util.h"

// True when `a6` is an IPv4-mapped IPv6 address (::ffff:a.b.c.d). Such an
// address reaches a dual-stack listener from an ordinary IPv4 peer, and must
// be treated as that IPv4 address everywhere -- otherwise the same client gets
// one identity on an IPv4-only build and a different one on a dual-stack build.
static bool is_v4_mapped(const struct in6_addr *a6)
{
  return IN6_IS_ADDR_V4MAPPED(a6);
}

// Address bytes to hash/compare/print, with IPv4-mapped addresses unwrapped to
// their 4 embedded bytes. Returns the length, or 0 for an unknown family.
static size_t addr_bytes(const struct sockaddr_storage *ss, const uint8_t **out)
{
  if (!ss)
  {
    return 0;
  }
  if (ss->ss_family == AF_INET)
  {
    const struct sockaddr_in *v4 = (const struct sockaddr_in *)ss;
    *out = (const uint8_t *)&v4->sin_addr;
    return 4;
  }
  if (ss->ss_family == AF_INET6)
  {
    const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)ss;
    if (is_v4_mapped(&v6->sin6_addr))
    {
      *out = ((const uint8_t *)&v6->sin6_addr) + 12; // the embedded IPv4 bytes
      return 4;
    }
    *out = (const uint8_t *)&v6->sin6_addr;
    return 16;
  }
  return 0;
}

int net_addr_str(const struct sockaddr_storage *ss, char *out, size_t out_size)
{
  if (!out || out_size == 0)
  {
    return -1;
  }
  if (ss && ss->ss_family == AF_INET)
  {
    const struct sockaddr_in *v4 = (const struct sockaddr_in *)ss;
    if (inet_ntop(AF_INET, &v4->sin_addr, out, (socklen_t)out_size))
    {
      return 0;
    }
  }
  else if (ss && ss->ss_family == AF_INET6)
  {
    const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)ss;
    if (is_v4_mapped(&v6->sin6_addr))
    {
      struct in_addr v4;
      memcpy(&v4, ((const char *)&v6->sin6_addr) + 12, sizeof(v4));
      if (inet_ntop(AF_INET, &v4, out, (socklen_t)out_size))
      {
        return 0;
      }
    }
    else if (inet_ntop(AF_INET6, &v6->sin6_addr, out, (socklen_t)out_size))
    {
      return 0;
    }
  }

  snprintf(out, out_size, "-");
  return -1;
}

uint64_t net_addr_hash(const struct sockaddr_storage *ss)
{
  const uint8_t *bytes = NULL;
  size_t n = addr_bytes(ss, &bytes);

  // FNV-1a, 64-bit. The family is not folded in: an IPv4 address and its
  // IPv4-mapped form already reduce to the same 4 bytes above, which is the
  // point.
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++)
  {
    h ^= bytes[i];
    h *= 1099511628211ULL;
  }
  return h;
}

bool net_addr_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
  const uint8_t *ba = NULL, *bb = NULL;
  size_t na = addr_bytes(a, &ba);
  size_t nb = addr_bytes(b, &bb);
  return na > 0 && na == nb && memcmp(ba, bb, na) == 0;
}

socklen_t net_addr_len(const struct sockaddr_storage *ss)
{
  if (ss && ss->ss_family == AF_INET)
  {
    return sizeof(struct sockaddr_in);
  }
  if (ss && ss->ss_family == AF_INET6)
  {
    return sizeof(struct sockaddr_in6);
  }
  return 0;
}

int net_listen_any(uint16_t port, int backlog)
{
  // Try dual-stack first: one AF_INET6 socket with IPV6_V6ONLY cleared serves
  // both families, and IPv4 peers show up as ::ffff:a.b.c.d (which the helpers
  // above unwrap). Some hosts disable IPv6 entirely, so fall back to AF_INET
  // rather than failing to start.
  int fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd >= 0)
  {
    int off = 0;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) == 0)
    {
      struct sockaddr_in6 addr6;
      memset(&addr6, 0, sizeof(addr6));
      addr6.sin6_family = AF_INET6;
      addr6.sin6_addr = in6addr_any;
      addr6.sin6_port = htons(port);
      if (bind(fd, (struct sockaddr *)&addr6, sizeof(addr6)) == 0 &&
          listen(fd, backlog) == 0)
      {
        return fd;
      }
    }
    close(fd);
  }

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
  {
    return -1;
  }
  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  struct sockaddr_in addr4;
  memset(&addr4, 0, sizeof(addr4));
  addr4.sin_family = AF_INET;
  addr4.sin_addr.s_addr = INADDR_ANY;
  addr4.sin_port = htons(port);
  if (bind(fd, (struct sockaddr *)&addr4, sizeof(addr4)) < 0 ||
      listen(fd, backlog) < 0)
  {
    close(fd);
    return -1;
  }
  return fd;
}

int net_resolve(const char *host, uint16_t port, struct sockaddr_storage *out,
                socklen_t *out_len)
{
  if (!host || !out)
  {
    return -1;
  }

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC; // IPv4 or IPv6, whichever the name resolves to
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res = NULL;
  if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
  {
    return -1;
  }

  memset(out, 0, sizeof(*out));
  memcpy(out, res->ai_addr, res->ai_addrlen);
  if (out_len)
  {
    *out_len = res->ai_addrlen;
  }
  freeaddrinfo(res);
  return 0;
}

int net_addr_version(const struct sockaddr_storage *ss)
{
  const uint8_t *bytes = NULL;
  size_t n = addr_bytes(ss, &bytes);
  return n == 4 ? 4 : (n == 16 ? 6 : 0);
}

uint16_t net_addr_port(const struct sockaddr_storage *ss)
{
  if (ss && ss->ss_family == AF_INET)
  {
    return ntohs(((const struct sockaddr_in *)ss)->sin_port);
  }
  if (ss && ss->ss_family == AF_INET6)
  {
    return ntohs(((const struct sockaddr_in6 *)ss)->sin6_port);
  }
  return 0;
}

int net_addr_from_str(const char *addr, uint16_t port, struct sockaddr_storage *out)
{
  if (!addr || !out)
  {
    return -1;
  }
  memset(out, 0, sizeof(*out));

  struct in_addr v4;
  if (inet_pton(AF_INET, addr, &v4) == 1)
  {
    struct sockaddr_in *sa = (struct sockaddr_in *)out;
    sa->sin_family = AF_INET;
    sa->sin_addr = v4;
    sa->sin_port = htons(port);
    return 0;
  }

  struct in6_addr v6;
  if (inet_pton(AF_INET6, addr, &v6) == 1)
  {
    struct sockaddr_in6 *sa = (struct sockaddr_in6 *)out;
    sa->sin6_family = AF_INET6;
    sa->sin6_addr = v6;
    sa->sin6_port = htons(port);
    return 0;
  }

  return -1;
}
