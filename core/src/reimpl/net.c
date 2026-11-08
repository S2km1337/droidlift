/*
 * Sockets and name lookup that fail cleanly: the game runs offline.
 *
 * socket() succeeds with a fake fd from its own range, as on a phone without
 * a connection, and every attempt to reach anything fails: connect gives
 * ECONNREFUSED for local (AF_UNIX) names and ENETUNREACH otherwise, data calls
 * give ENOTCONN, and getaddrinfo and gethostbyname find nothing. No host
 * socket is ever created.
 */
#include <stdio.h>

#include "../cpu.h"
#include "../guest_mem.h"
#include "reimpl.h"

#define SOCK_BASE 1008 /* fake fds 1008-1023, clear of the host's own fds */
#define MAX_SOCKS 16

#define L_AF_UNIX 1
#define L_EBADF 9
#define L_EMFILE 24
#define L_ENETUNREACH 101
#define L_ENOTCONN 107
#define L_ECONNREFUSED 111
#define L_EAI_NONAME 8

static uint32_t sock_domain[MAX_SOCKS]; /* 0: free */

int net_is_socket(int fd) {
  return fd >= SOCK_BASE && fd < SOCK_BASE + MAX_SOCKS && sock_domain[fd - SOCK_BASE];
}

static void fail(cpu *c, int linux_errno) {
  gm_w32(guest_errno_addr(), (uint32_t)linux_errno);
  cpu_ret(c, (uint32_t)-1);
}

int net_close(int fd) {
  if (!net_is_socket(fd)) {
    gm_w32(guest_errno_addr(), L_EBADF);
    return -1;
  }
  __atomic_store_n(&sock_domain[fd - SOCK_BASE], 0, __ATOMIC_RELEASE);
  return 0;
}

/* socket(domain, type, protocol) */
static void h_socket(cpu *c) {
  uint32_t domain = cpu_arg(c, 0);
  for (int i = 0; i < MAX_SOCKS; i++) {
    uint32_t expect = 0;
    if (__atomic_compare_exchange_n(&sock_domain[i], &expect, domain, 0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_RELAXED)) {
      cpu_ret(c, SOCK_BASE + (uint32_t)i);
      return;
    }
  }
  fail(c, L_EMFILE);
}

/* connect(fd, addr, len) */
static void h_connect(cpu *c) {
  int fd = (int)cpu_arg(c, 0);
  if (!net_is_socket(fd)) {
    fail(c, L_EBADF);
    return;
  }
  int local = sock_domain[fd - SOCK_BASE] == L_AF_UNIX;
  fprintf(stderr, "[net] connect on a %s socket: offline, %s\n", local ? "local" : "network",
          local ? "ECONNREFUSED" : "ENETUNREACH");
  fail(c, local ? L_ECONNREFUSED : L_ENETUNREACH);
}

/* send, sendto, recv, recvfrom, shutdown: nothing is ever connected. */
static void h_not_connected(cpu *c) {
  fail(c, net_is_socket((int)cpu_arg(c, 0)) ? L_ENOTCONN : L_EBADF);
}

static void h_setsockopt(cpu *c) {
  if (!net_is_socket((int)cpu_arg(c, 0))) fail(c, L_EBADF);
  else cpu_ret(c, 0);
}

/* getsockopt(fd, level, name, void *val, socklen_t *len): every option reads
 * as 0, including SO_ERROR. */
static void h_getsockopt(cpu *c) {
  if (!net_is_socket((int)cpu_arg(c, 0))) {
    fail(c, L_EBADF);
    return;
  }
  uint32_t val = cpu_arg(c, 3), len = cpu_arg(c, 4);
  if (val && len && gm_r32(len) >= 4) {
    gm_w32(val, 0);
    gm_w32(len, 4);
  }
  cpu_ret(c, 0);
}

/* getaddrinfo(node, service, hints, struct addrinfo **res) */
static void h_getaddrinfo(cpu *c) {
  uint32_t node = cpu_arg(c, 0), res = cpu_arg(c, 3);
  fprintf(stderr, "[net] getaddrinfo(\"%s\"): offline, EAI_NONAME\n",
          node ? (const char *)g2h(node) : "(null)");
  if (res) gm_w32(res, 0);
  cpu_ret(c, L_EAI_NONAME);
}

static void h_freeaddrinfo(cpu *c) { (void)c; }

static void h_gethostbyname(cpu *c) {
  fprintf(stderr, "[net] gethostbyname(\"%s\"): offline, NULL\n", (const char *)g2h(cpu_arg(c, 0)));
  cpu_ret(c, 0);
}

const dynlib_impl reimpl_net[] = {
    {"connect", h_connect},         {"freeaddrinfo", h_freeaddrinfo},
    {"getaddrinfo", h_getaddrinfo}, {"gethostbyname", h_gethostbyname},
    {"getsockopt", h_getsockopt},   {"recv", h_not_connected},
    {"recvfrom", h_not_connected},  {"send", h_not_connected},
    {"sendto", h_not_connected},    {"setsockopt", h_setsockopt},
    {"shutdown", h_not_connected},  {"socket", h_socket},
    {NULL, NULL},
};
