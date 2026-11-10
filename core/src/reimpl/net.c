/*
 * Real networking on host sockets.
 *
 * A guest socket is a host socket and its fd is the host fd, so read, write,
 * close, fcntl and poll (fileio.c) work on it unchanged. What differs between
 * bionic and Darwin is translated here: socket addresses (Linux sockaddrs
 * have no length byte, and AF_INET6 is 10), option levels and names, message
 * flags, ioctl numbers, struct timeval and errno values (runtime.c).
 *
 * epoll is emulated: an epoll instance is a placeholder host fd (a kqueue,
 * never used as one) with an interest list here, and epoll_wait polls the
 * listed fds. EPOLLIN, EPOLLERR and EPOLLHUP are level-triggered; with
 * EPOLLET, EPOLLOUT is reported only when the fd has become writable since
 * the last report, so an edge-triggered writer doesn't spin.
 *
 * Darwin raises SIGPIPE on a write to a closed socket; every socket gets
 * SO_NOSIGPIPE and the process ignores SIGPIPE.
 *
 * DL_NET=off keeps the game offline, as droidlift's tests expect: sockets
 * are created, but connect fails with ENETUNREACH and lookups find nothing.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../cpu.h"
#include "../guest_mem.h"
#include "reimpl.h"

/* Linux numbers. */
#define L_AF_UNIX 1
#define L_AF_INET 2
#define L_AF_INET6 10
#define L_SOCK_NONBLOCK 0x800
#define L_SOCK_CLOEXEC 0x80000
#define L_SOL_SOCKET 1
#define L_EBADF 9
#define L_EINVAL 22
#define L_ENOTTY 25
#define L_ENOSPC 28
#define L_EAFNOSUPPORT 97
#define L_ENETUNREACH 101
#define L_ERANGE 34
#define L_EAI_NONAME 8
#define L_HOST_NOT_FOUND 1

static int offline(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("DL_NET");
    v = e && strcmp(e, "off") == 0;
  }
  return v;
}

static void fail(cpu *c, int linux_err) {
  gm_w32(guest_errno_addr(), (uint32_t)linux_err);
  cpu_ret(c, (uint32_t)-1);
}

/* r (0 or a count) to the guest, or -1 with errno from the host. */
static void ret_host(cpu *c, long r) {
  if (r < 0) {
    guest_set_errno(errno);
    cpu_ret(c, (uint32_t)-1);
    return;
  }
  cpu_ret(c, (uint32_t)r);
}

static pthread_once_t sigpipe_once = PTHREAD_ONCE_INIT;
static void ignore_sigpipe(void) { signal(SIGPIPE, SIG_IGN); }

static void no_sigpipe(int fd) {
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
}

/* --- addresses --- */

static int family_to_host(uint32_t f) {
  switch (f) {
  case L_AF_UNIX: return AF_UNIX;
  case L_AF_INET: return AF_INET;
  case L_AF_INET6: return AF_INET6;
  case 0: return AF_UNSPEC;
  default: return -1;
  }
}

static uint16_t family_to_guest(int f) {
  return f == AF_INET6 ? L_AF_INET6 : f == AF_UNIX ? L_AF_UNIX : f == AF_INET ? L_AF_INET : 0;
}

/* A guest sockaddr of glen bytes as a host one; 0, or a Linux errno. */
static int sa_from_guest(uint32_t g, uint32_t glen, struct sockaddr_storage *h, socklen_t *hlen) {
  memset(h, 0, sizeof *h);
  if (!g || glen < 2) return L_EINVAL;
  const uint8_t *p = g2h(g);
  uint16_t fam;
  memcpy(&fam, p, 2);
  switch (fam) {
  case L_AF_INET: {
    if (glen < 8) return L_EINVAL;
    struct sockaddr_in *s = (struct sockaddr_in *)h;
    s->sin_len = sizeof *s;
    s->sin_family = AF_INET;
    memcpy(&s->sin_port, p + 2, 2);
    memcpy(&s->sin_addr, p + 4, 4);
    *hlen = sizeof *s;
    return 0;
  }
  case L_AF_INET6: {
    if (glen < 24) return L_EINVAL;
    struct sockaddr_in6 *s = (struct sockaddr_in6 *)h;
    s->sin6_len = sizeof *s;
    s->sin6_family = AF_INET6;
    memcpy(&s->sin6_port, p + 2, 2);
    memcpy(&s->sin6_flowinfo, p + 4, 4);
    memcpy(&s->sin6_addr, p + 8, 16);
    if (glen >= 28) memcpy(&s->sin6_scope_id, p + 24, 4);
    *hlen = sizeof *s;
    return 0;
  }
  case L_AF_UNIX: {
    struct sockaddr_un *s = (struct sockaddr_un *)h;
    s->sun_family = AF_UNIX;
    size_t n = glen - 2 < sizeof s->sun_path - 1 ? glen - 2 : sizeof s->sun_path - 1;
    memcpy(s->sun_path, p + 2, n);
    s->sun_len = (uint8_t)SUN_LEN(s);
    *hlen = s->sun_len;
    return 0;
  }
  default:
    return L_EAFNOSUPPORT;
  }
}

/* Writes a host sockaddr to the guest buffer g of *glen bytes (truncating,
 * as the kernel does) and sets *glen to the full guest size. */
static void sa_to_guest(const struct sockaddr *h, uint32_t g, uint32_t glen_addr) {
  uint8_t buf[128];
  uint32_t n = 0;
  memset(buf, 0, sizeof buf);
  uint16_t fam = family_to_guest(h->sa_family);
  memcpy(buf, &fam, 2);
  if (h->sa_family == AF_INET) {
    const struct sockaddr_in *s = (const struct sockaddr_in *)h;
    memcpy(buf + 2, &s->sin_port, 2);
    memcpy(buf + 4, &s->sin_addr, 4);
    n = 16;
  } else if (h->sa_family == AF_INET6) {
    const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)h;
    memcpy(buf + 2, &s->sin6_port, 2);
    memcpy(buf + 4, &s->sin6_flowinfo, 4);
    memcpy(buf + 8, &s->sin6_addr, 16);
    memcpy(buf + 24, &s->sin6_scope_id, 4);
    n = 28;
  } else if (h->sa_family == AF_UNIX) {
    const struct sockaddr_un *s = (const struct sockaddr_un *)h;
    size_t len = strnlen(s->sun_path, sizeof s->sun_path);
    memcpy(buf + 2, s->sun_path, len);
    n = 2 + (uint32_t)len + 1;
  } else {
    n = 2;
  }
  if (!g || !glen_addr) return;
  uint32_t room = gm_r32(glen_addr);
  memcpy(g2h(g), buf, room < n ? room : n);
  gm_w32(glen_addr, n);
}

/* --- sockets --- */

/* socket(domain, type, protocol) */
static void h_socket(cpu *c) {
  pthread_once(&sigpipe_once, ignore_sigpipe);
  int dom = family_to_host(cpu_arg(c, 0));
  uint32_t type = cpu_arg(c, 1);
  if (dom < 0 || dom == AF_UNSPEC) {
    fail(c, L_EAFNOSUPPORT);
    return;
  }
  int fd = socket(dom, (int)(type & ~(uint32_t)(L_SOCK_NONBLOCK | L_SOCK_CLOEXEC)),
                  (int)cpu_arg(c, 2));
  if (fd < 0) {
    ret_host(c, -1);
    return;
  }
  no_sigpipe(fd);
  if (type & L_SOCK_NONBLOCK) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  if (type & L_SOCK_CLOEXEC) fcntl(fd, F_SETFD, FD_CLOEXEC);
  cpu_ret(c, (uint32_t)fd);
}

/* socketpair(domain, type, protocol, int sv[2]) */
static void h_socketpair(cpu *c) {
  pthread_once(&sigpipe_once, ignore_sigpipe);
  int dom = family_to_host(cpu_arg(c, 0));
  uint32_t type = cpu_arg(c, 1);
  int sv[2];
  if (dom < 0 ||
      socketpair(dom, (int)(type & ~(uint32_t)(L_SOCK_NONBLOCK | L_SOCK_CLOEXEC)),
                 (int)cpu_arg(c, 2), sv) != 0) {
    if (dom < 0) fail(c, L_EAFNOSUPPORT);
    else ret_host(c, -1);
    return;
  }
  for (int i = 0; i < 2; i++) {
    no_sigpipe(sv[i]);
    if (type & L_SOCK_NONBLOCK) fcntl(sv[i], F_SETFL, fcntl(sv[i], F_GETFL) | O_NONBLOCK);
    gm_w32(cpu_arg(c, 3) + 4 * (uint32_t)i, (uint32_t)sv[i]);
  }
  cpu_ret(c, 0);
}

/* connect(fd, addr, len) */
static void h_connect(cpu *c) {
  struct sockaddr_storage sa;
  socklen_t len;
  int err = sa_from_guest(cpu_arg(c, 1), cpu_arg(c, 2), &sa, &len);
  if (err) {
    fail(c, err);
    return;
  }
  if (offline() && sa.ss_family != AF_UNIX) {
    fprintf(stderr, "[net] connect: offline (DL_NET=off), ENETUNREACH\n");
    fail(c, L_ENETUNREACH);
    return;
  }
  if (getenv("DL_TRACE_NET")) {
    char host[INET6_ADDRSTRLEN] = "?";
    int port = 0;
    if (sa.ss_family == AF_INET) {
      inet_ntop(AF_INET, &((struct sockaddr_in *)&sa)->sin_addr, host, sizeof host);
      port = ntohs(((struct sockaddr_in *)&sa)->sin_port);
    }
    fprintf(stderr, "[net] connect(%d, %s:%d)\n", (int)cpu_arg(c, 0), host, port);
  }
  ret_host(c, connect((int)cpu_arg(c, 0), (struct sockaddr *)&sa, len));
}

static void h_bind(cpu *c) {
  struct sockaddr_storage sa;
  socklen_t len;
  int err = sa_from_guest(cpu_arg(c, 1), cpu_arg(c, 2), &sa, &len);
  if (err) fail(c, err);
  else ret_host(c, bind((int)cpu_arg(c, 0), (struct sockaddr *)&sa, len));
}

static void h_listen(cpu *c) { ret_host(c, listen((int)cpu_arg(c, 0), (int)cpu_arg(c, 1))); }

/* accept(fd, addr, socklen_t *len) */
static void h_accept(cpu *c) {
  struct sockaddr_storage sa;
  socklen_t len = sizeof sa;
  int fd = accept((int)cpu_arg(c, 0), (struct sockaddr *)&sa, &len);
  if (fd < 0) {
    ret_host(c, -1);
    return;
  }
  no_sigpipe(fd);
  sa_to_guest((struct sockaddr *)&sa, cpu_arg(c, 1), cpu_arg(c, 2));
  cpu_ret(c, (uint32_t)fd);
}

static void h_getsockname(cpu *c) {
  struct sockaddr_storage sa;
  socklen_t len = sizeof sa;
  if (getsockname((int)cpu_arg(c, 0), (struct sockaddr *)&sa, &len) != 0) {
    ret_host(c, -1);
    return;
  }
  sa_to_guest((struct sockaddr *)&sa, cpu_arg(c, 1), cpu_arg(c, 2));
  cpu_ret(c, 0);
}

static void h_getpeername(cpu *c) {
  struct sockaddr_storage sa;
  socklen_t len = sizeof sa;
  if (getpeername((int)cpu_arg(c, 0), (struct sockaddr *)&sa, &len) != 0) {
    ret_host(c, -1);
    return;
  }
  sa_to_guest((struct sockaddr *)&sa, cpu_arg(c, 1), cpu_arg(c, 2));
  cpu_ret(c, 0);
}

/* shutdown(fd, how): SHUT_RD/WR/RDWR are 0-2 on both. */
static void h_shutdown(cpu *c) { ret_host(c, shutdown((int)cpu_arg(c, 0), (int)cpu_arg(c, 1))); }

/* Linux MSG_* flags as the host's; MSG_NOSIGNAL is SO_NOSIGPIPE here. */
static int msg_flags(uint32_t f) {
  int h = 0;
  if (f & 0x1) h |= MSG_OOB;
  if (f & 0x2) h |= MSG_PEEK;
  if (f & 0x4) h |= MSG_DONTROUTE;
  if (f & 0x40) h |= MSG_DONTWAIT;
  if (f & 0x100) h |= MSG_WAITALL;
  return h;
}

/* send(fd, buf, len, flags) */
static void h_send(cpu *c) {
  ret_host(c, send((int)cpu_arg(c, 0), g2h(cpu_arg(c, 1)), cpu_arg(c, 2), msg_flags(cpu_arg(c, 3))));
}

static void h_recv(cpu *c) {
  ret_host(c, recv((int)cpu_arg(c, 0), g2h(cpu_arg(c, 1)), cpu_arg(c, 2), msg_flags(cpu_arg(c, 3))));
}

/* sendto(fd, buf, len, flags, addr, addrlen) */
static void h_sendto(cpu *c) {
  struct sockaddr_storage sa;
  socklen_t len = 0;
  uint32_t ga = cpu_arg(c, 4);
  if (ga) {
    int err = sa_from_guest(ga, cpu_arg(c, 5), &sa, &len);
    if (err) {
      fail(c, err);
      return;
    }
  }
  ret_host(c, sendto((int)cpu_arg(c, 0), g2h(cpu_arg(c, 1)), cpu_arg(c, 2), msg_flags(cpu_arg(c, 3)),
                     ga ? (struct sockaddr *)&sa : NULL, len));
}

/* recvfrom(fd, buf, len, flags, addr, socklen_t *addrlen) */
static void h_recvfrom(cpu *c) {
  struct sockaddr_storage sa;
  socklen_t len = sizeof sa;
  memset(&sa, 0, sizeof sa);
  ssize_t r = recvfrom((int)cpu_arg(c, 0), g2h(cpu_arg(c, 1)), cpu_arg(c, 2),
                       msg_flags(cpu_arg(c, 3)), (struct sockaddr *)&sa, &len);
  if (r >= 0 && cpu_arg(c, 4) && len) sa_to_guest((struct sockaddr *)&sa, cpu_arg(c, 4), cpu_arg(c, 5));
  ret_host(c, r);
}

/* --- options --- */

enum { OPT_INT, OPT_TIMEVAL, OPT_LINGER, OPT_IGNORE };

/* Linux (level, name) as the host's, and how the value is laid out. */
static int opt_to_host(uint32_t level, uint32_t name, int *hl, int *hn) {
  if (level == L_SOL_SOCKET) {
    *hl = SOL_SOCKET;
    switch (name) {
    case 1: *hn = SO_DEBUG; return OPT_INT;
    case 2: *hn = SO_REUSEADDR; return OPT_INT;
    case 3: *hn = SO_TYPE; return OPT_INT;
    case 4: *hn = SO_ERROR; return OPT_INT;
    case 5: *hn = SO_DONTROUTE; return OPT_INT;
    case 6: *hn = SO_BROADCAST; return OPT_INT;
    case 7: *hn = SO_SNDBUF; return OPT_INT;
    case 8: *hn = SO_RCVBUF; return OPT_INT;
    case 9: *hn = SO_KEEPALIVE; return OPT_INT;
    case 10: *hn = SO_OOBINLINE; return OPT_INT;
    case 13: *hn = SO_LINGER; return OPT_LINGER;
    case 15: *hn = SO_REUSEPORT; return OPT_INT;
    case 18: *hn = SO_RCVLOWAT; return OPT_INT;
    case 19: *hn = SO_SNDLOWAT; return OPT_INT;
    case 20: *hn = SO_RCVTIMEO; return OPT_TIMEVAL;
    case 21: *hn = SO_SNDTIMEO; return OPT_TIMEVAL;
    default: return OPT_IGNORE;
    }
  }
  if (level == IPPROTO_TCP) {
    *hl = IPPROTO_TCP;
    switch (name) {
    case 1: *hn = TCP_NODELAY; return OPT_INT;
    case 2: *hn = TCP_MAXSEG; return OPT_INT;
    case 4: *hn = TCP_KEEPALIVE; return OPT_INT; /* TCP_KEEPIDLE */
    case 5: *hn = TCP_KEEPINTVL; return OPT_INT;
    case 6: *hn = TCP_KEEPCNT; return OPT_INT;
    default: return OPT_IGNORE;
    }
  }
  if (level == IPPROTO_IP) {
    *hl = IPPROTO_IP;
    switch (name) {
    case 1: *hn = IP_TOS; return OPT_INT;
    case 2: *hn = IP_TTL; return OPT_INT;
    default: return OPT_IGNORE;
    }
  }
  if (level == IPPROTO_IPV6) {
    *hl = IPPROTO_IPV6;
    if (name == 26) {
      *hn = IPV6_V6ONLY;
      return OPT_INT;
    }
  }
  return OPT_IGNORE;
}

/* setsockopt(fd, level, name, const void *val, socklen_t len) */
static void h_setsockopt(cpu *c) {
  int fd = (int)cpu_arg(c, 0);
  int hl, hn;
  uint32_t val = cpu_arg(c, 3), len = cpu_arg(c, 4);
  int kind = opt_to_host(cpu_arg(c, 1), cpu_arg(c, 2), &hl, &hn);
  if (kind == OPT_IGNORE) {
    fprintf(stderr, "[net] setsockopt(level %u, option %u) ignored\n", cpu_arg(c, 1), cpu_arg(c, 2));
    cpu_ret(c, 0);
    return;
  }
  if (!val || len < (kind == OPT_INT ? 4u : 8u)) {
    fail(c, L_EINVAL);
    return;
  }
  if (kind == OPT_INT) {
    int v = (int32_t)gm_r32(val);
    ret_host(c, setsockopt(fd, hl, hn, &v, sizeof v));
  } else if (kind == OPT_TIMEVAL) {
    struct timeval tv = {(int32_t)gm_r32(val), (int32_t)gm_r32(val + 4)};
    ret_host(c, setsockopt(fd, hl, hn, &tv, sizeof tv));
  } else {
    struct linger l = {(int32_t)gm_r32(val), (int32_t)gm_r32(val + 4)};
    ret_host(c, setsockopt(fd, hl, hn, &l, sizeof l));
  }
}

/* getsockopt(fd, level, name, void *val, socklen_t *len) */
static void h_getsockopt(cpu *c) {
  int fd = (int)cpu_arg(c, 0);
  int hl, hn;
  uint32_t val = cpu_arg(c, 3), lenp = cpu_arg(c, 4);
  int kind = opt_to_host(cpu_arg(c, 1), cpu_arg(c, 2), &hl, &hn);
  if (!val || !lenp) {
    fail(c, L_EINVAL);
    return;
  }
  if (kind == OPT_IGNORE) {
    if (gm_r32(lenp) >= 4) gm_w32(val, 0), gm_w32(lenp, 4);
    cpu_ret(c, 0);
    return;
  }
  if (kind == OPT_INT) {
    int v = 0;
    socklen_t l = sizeof v;
    if (getsockopt(fd, hl, hn, &v, &l) != 0) {
      ret_host(c, -1);
      return;
    }
    if (hn == SO_ERROR && hl == SOL_SOCKET && v) v = linux_errno(v);
    gm_w32(val, (uint32_t)v);
    gm_w32(lenp, 4);
  } else if (kind == OPT_TIMEVAL) {
    struct timeval tv;
    socklen_t l = sizeof tv;
    if (getsockopt(fd, hl, hn, &tv, &l) != 0) {
      ret_host(c, -1);
      return;
    }
    gm_w32(val, (uint32_t)tv.tv_sec);
    gm_w32(val + 4, (uint32_t)tv.tv_usec);
    gm_w32(lenp, 8);
  } else {
    struct linger lg;
    socklen_t l = sizeof lg;
    if (getsockopt(fd, hl, hn, &lg, &l) != 0) {
      ret_host(c, -1);
      return;
    }
    gm_w32(val, (uint32_t)lg.l_onoff);
    gm_w32(val + 4, (uint32_t)lg.l_linger);
    gm_w32(lenp, 8);
  }
  cpu_ret(c, 0);
}

/* ioctl(fd, request, arg): the socket requests; anything else is ENOTTY. */
static void h_ioctl(cpu *c) {
  int fd = (int)cpu_arg(c, 0);
  uint32_t req = cpu_arg(c, 1), arg = cpu_arg(c, 2);
  switch (req) {
  case 0x5421: { /* FIONBIO */
    int v = arg ? (int32_t)gm_r32(arg) : 0;
    ret_host(c, ioctl(fd, FIONBIO, &v));
    return;
  }
  case 0x541B: { /* FIONREAD */
    int v = 0;
    int r = ioctl(fd, FIONREAD, &v);
    if (r == 0 && arg) gm_w32(arg, (uint32_t)v);
    ret_host(c, r);
    return;
  }
  default:
    fprintf(stderr, "[net] ioctl(%d, 0x%x) unsupported, ENOTTY\n", fd, req);
    fail(c, L_ENOTTY);
  }
}

/* select(nfds, readfds, writefds, exceptfds, struct timeval *timeout):
 * bionic's fd_set is 1024 bits in 32-bit words, the host's layout. The
 * timeout isn't updated with the time left (Linux does; few callers rely on
 * it). */
static void h_select(cpu *c) {
  int n = (int32_t)cpu_arg(c, 0);
  if (n < 0 || n > FD_SETSIZE) {
    fail(c, L_EINVAL);
    return;
  }
  fd_set sets[3];
  fd_set *ps[3];
  for (int i = 0; i < 3; i++) {
    uint32_t g = cpu_arg(c, 1 + i);
    ps[i] = g ? &sets[i] : NULL;
    if (g) memcpy(&sets[i], g2h(g), sizeof sets[i]);
  }
  uint32_t gt = cpu_arg(c, 4);
  struct timeval tv, *ptv = NULL;
  if (gt) {
    tv.tv_sec = (int32_t)gm_r32(gt);
    tv.tv_usec = (int32_t)gm_r32(gt + 4);
    ptv = &tv;
  }
  int r = select(n, ps[0], ps[1], ps[2], ptv);
  if (r >= 0)
    for (int i = 0; i < 3; i++)
      if (ps[i]) memcpy(g2h(cpu_arg(c, 1 + i)), &sets[i], sizeof sets[i]);
  ret_host(c, r);
}

/* --- name lookup --- */

/* bionic's struct addrinfo (32 bytes): flags, family, socktype, protocol,
 * addrlen, canonname, addr, next. Each result node is one guest block holding
 * the node, its sockaddr and its canonical name, freed by freeaddrinfo. */
#define G_AI_SIZE 32

static void h_getaddrinfo(cpu *c) {
  uint32_t node = cpu_arg(c, 0), service = cpu_arg(c, 1), hints = cpu_arg(c, 2), res = cpu_arg(c, 3);
  const char *name = node ? (const char *)g2h(node) : NULL;
  if (res) gm_w32(res, 0);
  if (offline()) {
    fprintf(stderr, "[net] getaddrinfo(\"%s\"): offline (DL_NET=off), EAI_NONAME\n", name ? name : "(null)");
    cpu_ret(c, L_EAI_NONAME);
    return;
  }
  struct addrinfo h, *out = NULL;
  memset(&h, 0, sizeof h);
  if (hints) {
    h.ai_flags = (int32_t)gm_r32(hints);
    int fam = family_to_host(gm_r32(hints + 4));
    h.ai_family = fam < 0 ? AF_UNSPEC : fam;
    h.ai_socktype = (int32_t)gm_r32(hints + 8);
    h.ai_protocol = (int32_t)gm_r32(hints + 12);
  }
  int r = getaddrinfo(name, service ? (const char *)g2h(service) : NULL, hints ? &h : NULL, &out);
  if (getenv("DL_TRACE_NET"))
    fprintf(stderr, "[net] getaddrinfo(\"%s\") -> %d\n", name ? name : "(null)", r);
  if (r != 0) {
    /* EAI_* values are the BSD ones on both; EAI_SYSTEM's errno is Darwin's. */
    if (r == EAI_SYSTEM) guest_set_errno(errno);
    cpu_ret(c, (uint32_t)r);
    return;
  }
  uint32_t first = 0, prev = 0;
  for (struct addrinfo *a = out; a; a = a->ai_next) {
    if (a->ai_family != AF_INET && a->ai_family != AF_INET6) continue;
    size_t cn = a->ai_canonname ? strlen(a->ai_canonname) + 1 : 0;
    uint32_t g = guest_malloc(G_AI_SIZE + 28 + (uint32_t)cn);
    if (!g) break;
    memset(g2h(g), 0, G_AI_SIZE + 28);
    uint32_t sa = g + G_AI_SIZE, sl = 28;
    uint32_t tmp = guest_malloc(4);
    gm_w32(tmp, sl);
    sa_to_guest(a->ai_addr, sa, tmp);
    sl = gm_r32(tmp);
    guest_free(tmp);
    gm_w32(g + 0, (uint32_t)a->ai_flags);
    gm_w32(g + 4, family_to_guest(a->ai_family));
    gm_w32(g + 8, (uint32_t)a->ai_socktype);
    gm_w32(g + 12, (uint32_t)a->ai_protocol);
    gm_w32(g + 16, sl);
    if (cn) {
      memcpy(g2h(sa + 28), a->ai_canonname, cn);
      gm_w32(g + 20, sa + 28);
    }
    gm_w32(g + 24, sa);
    if (prev) gm_w32(prev + 28, g);
    else first = g;
    prev = g;
  }
  freeaddrinfo(out);
  if (!first) {
    cpu_ret(c, L_EAI_NONAME);
    return;
  }
  if (res) gm_w32(res, first);
  cpu_ret(c, 0);
}

static void h_freeaddrinfo(cpu *c) {
  for (uint32_t a = cpu_arg(c, 0); a;) {
    uint32_t next = gm_r32(a + 28);
    guest_free(a);
    a = next;
  }
}

/* IPv4 addresses of name, at most max; returns how many. */
static int resolve4(const char *name, struct in_addr *out, int max) {
  if (offline()) return 0;
  struct addrinfo h, *res = NULL;
  memset(&h, 0, sizeof h);
  h.ai_family = AF_INET;
  h.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(name, NULL, &h, &res) != 0) return 0;
  int n = 0;
  for (struct addrinfo *a = res; a && n < max; a = a->ai_next)
    out[n++] = ((struct sockaddr_in *)a->ai_addr)->sin_addr;
  freeaddrinfo(res);
  return n;
}

/* Builds a guest struct hostent (h_name, h_aliases, h_addrtype, h_length,
 * h_addr_list: 20 bytes) at ent, with its strings and lists in [buf, buf+len).
 * Returns 0, or -1 if they don't fit. */
static int fill_hostent(uint32_t ent, uint32_t buf, uint32_t len, const char *name,
                        const struct in_addr *addrs, int n) {
  uint32_t need = (uint32_t)strlen(name) + 1 + 4 /* aliases */ + 4 * ((uint32_t)n + 1) + 4 * (uint32_t)n;
  if (need + 3 > len) return -1;
  uint32_t p = (buf + 3) & ~3u;
  uint32_t list = p;
  p += 4 * ((uint32_t)n + 1);
  uint32_t aliases = p;
  gm_w32(aliases, 0);
  p += 4;
  for (int i = 0; i < n; i++) {
    memcpy(g2h(p), &addrs[i], 4);
    gm_w32(list + 4 * (uint32_t)i, p);
    p += 4;
  }
  gm_w32(list + 4 * (uint32_t)n, 0);
  strcpy(g2h(p), name);
  gm_w32(ent + 0, p);
  gm_w32(ent + 4, aliases);
  gm_w32(ent + 8, L_AF_INET);
  gm_w32(ent + 12, 4);
  gm_w32(ent + 16, list);
  return 0;
}

static void h_gethostbyname(cpu *c) {
  static uint32_t ent, buf;
  const uint32_t buf_len = 1024;
  if (!ent) ent = guest_malloc(20), buf = guest_malloc(buf_len);
  const char *name = g2h(cpu_arg(c, 0));
  struct in_addr a[16];
  int n = resolve4(name, a, 16);
  if (getenv("DL_TRACE_NET")) fprintf(stderr, "[net] gethostbyname(\"%s\") -> %d\n", name, n);
  cpu_ret(c, n && fill_hostent(ent, buf, buf_len, name, a, n) == 0 ? ent : 0);
}

/* gethostbyname_r(name, struct hostent *ret, char *buf, size_t len,
 *                 struct hostent **result, int *h_errnop) */
static void h_gethostbyname_r(cpu *c) {
  const char *name = g2h(cpu_arg(c, 0));
  uint32_t ret = cpu_arg(c, 1), buf = cpu_arg(c, 2), len = cpu_arg(c, 3);
  uint32_t result = cpu_arg(c, 4), herr = cpu_arg(c, 5);
  struct in_addr a[16];
  int n = resolve4(name, a, 16);
  if (getenv("DL_TRACE_NET")) fprintf(stderr, "[net] gethostbyname_r(\"%s\") -> %d\n", name, n);
  if (result) gm_w32(result, 0);
  if (!n) {
    if (herr) gm_w32(herr, L_HOST_NOT_FOUND);
    cpu_ret(c, 0);
    return;
  }
  if (fill_hostent(ret, buf, len, name, a, n) != 0) {
    cpu_ret(c, L_ERANGE);
    return;
  }
  if (result) gm_w32(result, ret);
  cpu_ret(c, 0);
}

static void h_gethostname(cpu *c) {
  uint32_t buf = cpu_arg(c, 0), len = cpu_arg(c, 1);
  const char *name = "localhost";
  if (len < strlen(name) + 1) {
    fail(c, 36 /* ENAMETOOLONG */);
    return;
  }
  strcpy(g2h(buf), name);
  cpu_ret(c, 0);
}

/* in_addr_t inet_addr(const char *) */
static void h_inet_addr(cpu *c) { cpu_ret(c, inet_addr(g2h(cpu_arg(c, 0)))); }

/* char *inet_ntoa(struct in_addr): the struct comes by value in r0. */
static void h_inet_ntoa(cpu *c) {
  static uint32_t buf;
  if (!buf) buf = guest_malloc(16);
  struct in_addr a;
  a.s_addr = cpu_arg(c, 0);
  strcpy(g2h(buf), inet_ntoa(a));
  cpu_ret(c, buf);
}

/* inet_ntop(af, const void *src, char *dst, socklen_t size) */
static void h_inet_ntop(cpu *c) {
  int af = family_to_host(cpu_arg(c, 0));
  if (af != AF_INET && af != AF_INET6) {
    gm_w32(guest_errno_addr(), L_EAFNOSUPPORT);
    cpu_ret(c, 0);
    return;
  }
  const char *r = inet_ntop(af, g2h(cpu_arg(c, 1)), g2h(cpu_arg(c, 2)), cpu_arg(c, 3));
  if (!r) gm_w32(guest_errno_addr(), L_ENOSPC);
  cpu_ret(c, r ? cpu_arg(c, 2) : 0);
}

/* inet_pton(af, const char *src, void *dst) */
static void h_inet_pton(cpu *c) {
  int af = family_to_host(cpu_arg(c, 0));
  if (af != AF_INET && af != AF_INET6) {
    fail(c, L_EAFNOSUPPORT);
    return;
  }
  cpu_ret(c, (uint32_t)inet_pton(af, g2h(cpu_arg(c, 1)), g2h(cpu_arg(c, 2))));
}

static void h_if_nametoindex(cpu *c) { cpu_ret(c, if_nametoindex(g2h(cpu_arg(c, 0)))); }

/* --- epoll over poll --- */

#define L_EPOLLIN 0x001u
#define L_EPOLLOUT 0x004u
#define L_EPOLLERR 0x008u
#define L_EPOLLHUP 0x010u
#define L_EPOLLRDHUP 0x2000u
#define L_EPOLLONESHOT (1u << 30)
#define L_EPOLLET (1u << 31)
#define MAX_EPOLL 16
#define MAX_WATCH 256

typedef struct {
  int fd;
  uint32_t events;
  uint64_t data;
  int was_writable; /* EPOLLET: writable at the last report */
} watch;

static struct {
  int fd; /* the placeholder host fd; 0: free slot */
  int n;
  watch w[MAX_WATCH];
} eps[MAX_EPOLL];
static pthread_mutex_t ep_lock = PTHREAD_MUTEX_INITIALIZER;

static int ep_slot(int fd) {
  for (int i = 0; i < MAX_EPOLL; i++)
    if (eps[i].fd && eps[i].fd == fd) return i;
  return -1;
}

int net_is_socket(int fd) {
  if (fd < 0) return 0;
  pthread_mutex_lock(&ep_lock);
  int r = ep_slot(fd) >= 0;
  pthread_mutex_unlock(&ep_lock);
  return r;
}

int net_close(int fd) {
  pthread_mutex_lock(&ep_lock);
  int i = ep_slot(fd);
  if (i >= 0) eps[i].fd = 0, eps[i].n = 0;
  pthread_mutex_unlock(&ep_lock);
  if (i < 0) {
    gm_w32(guest_errno_addr(), L_EBADF);
    return -1;
  }
  close(fd);
  return 0;
}

/* epoll_create(size) */
static void h_epoll_create(cpu *c) {
  int fd = kqueue();
  if (fd < 0) {
    ret_host(c, -1);
    return;
  }
  pthread_mutex_lock(&ep_lock);
  int i = 0;
  while (i < MAX_EPOLL && eps[i].fd) i++;
  if (i < MAX_EPOLL) eps[i].fd = fd, eps[i].n = 0;
  pthread_mutex_unlock(&ep_lock);
  if (i == MAX_EPOLL) {
    close(fd);
    fail(c, 24 /* EMFILE */);
    return;
  }
  cpu_ret(c, (uint32_t)fd);
}

/* epoll_ctl(epfd, op, fd, struct epoll_event *ev): on ARM the event struct is
 * {u32 events; u32 pad; u64 data} (16 bytes, not packed). */
static void h_epoll_ctl(cpu *c) {
  int epfd = (int)cpu_arg(c, 0), op = (int)cpu_arg(c, 1), fd = (int)cpu_arg(c, 2);
  uint32_t ev = cpu_arg(c, 3);
  pthread_mutex_lock(&ep_lock);
  int e = ep_slot(epfd);
  int err = 0;
  if (e < 0) {
    err = L_EBADF;
  } else {
    int k = -1;
    for (int j = 0; j < eps[e].n; j++)
      if (eps[e].w[j].fd == fd) k = j;
    uint32_t events = ev ? gm_r32(ev) : 0;
    uint64_t data = ev ? ((uint64_t)gm_r32(ev + 8) | (uint64_t)gm_r32(ev + 12) << 32) : 0;
    switch (op) {
    case 1: /* EPOLL_CTL_ADD */
      if (k >= 0) err = 17 /* EEXIST */;
      else if (eps[e].n == MAX_WATCH) err = L_ENOSPC;
      else eps[e].w[eps[e].n++] = (watch){fd, events, data, 0};
      break;
    case 2: /* EPOLL_CTL_DEL */
      if (k < 0) err = 2 /* ENOENT */;
      else eps[e].w[k] = eps[e].w[--eps[e].n];
      break;
    case 3: /* EPOLL_CTL_MOD */
      if (k < 0) err = 2;
      else eps[e].w[k] = (watch){fd, events, data, 0};
      break;
    default:
      err = L_EINVAL;
    }
  }
  pthread_mutex_unlock(&ep_lock);
  if (err) fail(c, err);
  else cpu_ret(c, 0);
}

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* One pass over the interest list; returns the events stored. */
static int ep_scan(int e, uint32_t out, int max, int wait_ms) {
  struct pollfd p[MAX_WATCH];
  int idx[MAX_WATCH], n = 0;
  pthread_mutex_lock(&ep_lock);
  for (int j = 0; j < eps[e].n; j++) {
    uint32_t want = eps[e].w[j].events;
    if (!(want & (L_EPOLLIN | L_EPOLLOUT | L_EPOLLRDHUP))) continue; /* oneshot fired */
    p[n].fd = eps[e].w[j].fd;
    p[n].events = (short)((want & L_EPOLLIN ? POLLIN : 0) | (want & L_EPOLLOUT ? POLLOUT : 0));
    p[n].revents = 0;
    idx[n++] = j;
  }
  pthread_mutex_unlock(&ep_lock);
  int r = poll(p, (nfds_t)n, wait_ms);
  if (r <= 0) return r;
  int got = 0;
  pthread_mutex_lock(&ep_lock);
  for (int i = 0; i < n && got < max; i++) {
    int j = idx[i];
    if (j >= eps[e].n || eps[e].w[j].fd != p[i].fd) continue; /* changed meanwhile */
    watch *w = &eps[e].w[j];
    uint32_t rev = 0;
    if (p[i].revents & POLLIN) rev |= L_EPOLLIN;
    if (p[i].revents & POLLERR) rev |= L_EPOLLERR;
    if (p[i].revents & POLLHUP) rev |= L_EPOLLHUP | (w->events & L_EPOLLRDHUP);
    if (p[i].revents & POLLNVAL) rev |= L_EPOLLERR;
    int writable = (p[i].revents & POLLOUT) != 0;
    if (writable && (!(w->events & L_EPOLLET) || !w->was_writable)) rev |= L_EPOLLOUT;
    w->was_writable = writable;
    if (!rev) continue;
    uint32_t g = out + 16 * (uint32_t)got;
    gm_w32(g, rev);
    gm_w32(g + 4, 0);
    gm_w32(g + 8, (uint32_t)w->data);
    gm_w32(g + 12, (uint32_t)(w->data >> 32));
    if (w->events & L_EPOLLONESHOT) w->events &= ~(L_EPOLLIN | L_EPOLLOUT | L_EPOLLRDHUP);
    got++;
  }
  pthread_mutex_unlock(&ep_lock);
  return got;
}

/* epoll_wait(epfd, struct epoll_event *events, maxevents, timeout_ms). Waits
 * in short slices so fds added by other threads are picked up. */
static void h_epoll_wait(cpu *c) {
  int epfd = (int)cpu_arg(c, 0), max = (int)cpu_arg(c, 2), timeout = (int32_t)cpu_arg(c, 3);
  uint32_t out = cpu_arg(c, 1);
  pthread_mutex_lock(&ep_lock);
  int e = ep_slot(epfd);
  pthread_mutex_unlock(&ep_lock);
  if (e < 0 || max <= 0) {
    fail(c, e < 0 ? L_EBADF : L_EINVAL);
    return;
  }
  long deadline = timeout >= 0 ? now_ms() + timeout : -1;
  for (;;) {
    long left = deadline < 0 ? 50 : deadline - now_ms();
    if (left < 0) left = 0;
    int slice = left > 50 ? 50 : (int)left;
    int r = ep_scan(e, out, max, slice);
    if (r < 0 && errno != EINTR) {
      ret_host(c, -1);
      return;
    }
    if (r > 0) {
      cpu_ret(c, (uint32_t)r);
      return;
    }
    if (deadline >= 0 && now_ms() >= deadline) {
      cpu_ret(c, 0);
      return;
    }
  }
}

const dynlib_impl reimpl_net[] = {
    {"accept", h_accept},
    {"bind", h_bind},
    {"connect", h_connect},
    {"epoll_create", h_epoll_create},
    {"epoll_ctl", h_epoll_ctl},
    {"epoll_wait", h_epoll_wait},
    {"freeaddrinfo", h_freeaddrinfo},
    {"getaddrinfo", h_getaddrinfo},
    {"gethostbyname", h_gethostbyname},
    {"gethostbyname_r", h_gethostbyname_r},
    {"gethostname", h_gethostname},
    {"getpeername", h_getpeername},
    {"getsockname", h_getsockname},
    {"getsockopt", h_getsockopt},
    {"if_nametoindex", h_if_nametoindex},
    {"inet_addr", h_inet_addr},
    {"inet_ntoa", h_inet_ntoa},
    {"inet_ntop", h_inet_ntop},
    {"inet_pton", h_inet_pton},
    {"ioctl", h_ioctl},
    {"listen", h_listen},
    {"recv", h_recv},
    {"recvfrom", h_recvfrom},
    {"select", h_select},
    {"send", h_send},
    {"sendto", h_sendto},
    {"setsockopt", h_setsockopt},
    {"shutdown", h_shutdown},
    {"socket", h_socket},
    {"socketpair", h_socketpair},
    {NULL, NULL},
};
