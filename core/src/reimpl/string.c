/*
 * The rest of <string.h> and <stdlib.h>'s string conversions: copies,
 * searches, case-insensitive compares, strtok, strerror, atoi and strtod.
 *
 * Results that point into an argument are returned as the matching guest
 * address. The guest runs in the "C" locale, so strcoll and strxfrm are plain
 * byte operations.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../cpu.h"
#include "../guest_mem.h"
#include "reimpl.h"

static const char *str(cpu *c, int i) { return g2h(cpu_arg(c, i)); }

/* The guest address of host pointer p into argument i's string, or 0. */
static uint32_t at(cpu *c, int i, const char *p) {
  return p ? cpu_arg(c, i) + (uint32_t)(p - str(c, i)) : 0;
}

static void h_strcpy(cpu *c) {
  strcpy(g2h(cpu_arg(c, 0)), str(c, 1));
  cpu_ret(c, cpu_arg(c, 0));
}

static void h_strncpy(cpu *c) {
  strncpy(g2h(cpu_arg(c, 0)), str(c, 1), cpu_arg(c, 2));
  cpu_ret(c, cpu_arg(c, 0));
}

static void h_strcat(cpu *c) {
  strcat(g2h(cpu_arg(c, 0)), str(c, 1));
  cpu_ret(c, cpu_arg(c, 0));
}

static void h_strncat(cpu *c) {
  strncat(g2h(cpu_arg(c, 0)), str(c, 1), cpu_arg(c, 2));
  cpu_ret(c, cpu_arg(c, 0));
}

static void h_strdup(cpu *c) {
  size_t n = strlen(str(c, 0)) + 1;
  uint32_t p = guest_malloc((uint32_t)n);
  if (p) memcpy(g2h(p), str(c, 0), n);
  cpu_ret(c, p);
}

static void h_strchr(cpu *c) { cpu_ret(c, at(c, 0, strchr(str(c, 0), (int)cpu_arg(c, 1)))); }
static void h_strrchr(cpu *c) { cpu_ret(c, at(c, 0, strrchr(str(c, 0), (int)cpu_arg(c, 1)))); }
static void h_strstr(cpu *c) { cpu_ret(c, at(c, 0, strstr(str(c, 0), str(c, 1)))); }
static void h_strpbrk(cpu *c) { cpu_ret(c, at(c, 0, strpbrk(str(c, 0), str(c, 1)))); }
static void h_strcspn(cpu *c) { cpu_ret(c, (uint32_t)strcspn(str(c, 0), str(c, 1))); }

static void h_strncmp(cpu *c) { cpu_ret(c, (uint32_t)strncmp(str(c, 0), str(c, 1), cpu_arg(c, 2))); }
static void h_strcasecmp(cpu *c) { cpu_ret(c, (uint32_t)strcasecmp(str(c, 0), str(c, 1))); }
static void h_strncasecmp(cpu *c) {
  cpu_ret(c, (uint32_t)strncasecmp(str(c, 0), str(c, 1), cpu_arg(c, 2)));
}
static void h_strcoll(cpu *c) { cpu_ret(c, (uint32_t)strcmp(str(c, 0), str(c, 1))); }

/* strxfrm(dst, src, n) in the C locale: a bounded copy returning strlen(src). */
static void h_strxfrm(cpu *c) {
  size_t len = strlen(str(c, 1)), n = cpu_arg(c, 2);
  if (len < n) memcpy(g2h(cpu_arg(c, 0)), str(c, 1), len + 1);
  cpu_ret(c, (uint32_t)len);
}

/* strtok_r(s, delim, char **save) with a guest save pointer. */
static uint32_t tok(uint32_t s, uint32_t delim, uint32_t save) {
  if (!s) s = gm_r32(save);
  if (!s) return 0;
  const char *d = g2h(delim);
  s += (uint32_t)strspn(g2h(s), d);
  if (!*(char *)g2h(s)) {
    gm_w32(save, 0);
    return 0;
  }
  uint32_t end = s + (uint32_t)strcspn(g2h(s), d);
  if (*(char *)g2h(end)) {
    *(char *)g2h(end) = 0;
    gm_w32(save, end + 1);
  } else {
    gm_w32(save, 0);
  }
  return s;
}

static void h_strtok_r(cpu *c) { cpu_ret(c, tok(cpu_arg(c, 0), cpu_arg(c, 1), cpu_arg(c, 2))); }

static void h_strtok(cpu *c) {
  static uint32_t save;
  if (!save) save = guest_malloc(4), gm_w32(save, 0);
  cpu_ret(c, tok(cpu_arg(c, 0), cpu_arg(c, 1), save));
}

/* Guest errno values are Linux's; the first 34 (EPERM..ERANGE) match the
 * host's numbering and messages. */
static const char *errno_message(uint32_t e, char *buf, size_t n) {
  if (e >= 1 && e <= 34) return strerror((int)e);
  snprintf(buf, n, "Unknown error %u", e);
  return buf;
}

static void h_strerror(cpu *c) {
  static uint32_t buf;
  if (!buf) buf = guest_malloc(64);
  char tmp[64];
  snprintf(g2h(buf), 64, "%s", errno_message(cpu_arg(c, 0), tmp, sizeof tmp));
  cpu_ret(c, buf);
}

/* POSIX strerror_r(errnum, buf, n): 0, or ERANGE (34) if buf is too small. */
static void h_strerror_r(cpu *c) {
  char tmp[64];
  const char *m = errno_message(cpu_arg(c, 0), tmp, sizeof tmp);
  uint32_t n = cpu_arg(c, 2);
  if (n == 0) {
    cpu_ret(c, 34);
    return;
  }
  snprintf(g2h(cpu_arg(c, 1)), n, "%s", m);
  cpu_ret(c, strlen(m) < n ? 0 : 34);
}

static void h_atoi(cpu *c) { cpu_ret(c, (uint32_t)(int32_t)strtol(str(c, 0), NULL, 10)); }
static void h_atoll(cpu *c) { cpu_ret64(c, (uint64_t)strtoll(str(c, 0), NULL, 10)); }

/* softfp: the double comes back in r0/r1. */
static void h_strtod(cpu *c) {
  char *end;
  double d = strtod(str(c, 0), &end);
  if (printf_trace_on()) fprintf(stderr, "[fmt] strtod \"%s\" -> %.17g\n", str(c, 0), d);
  if (cpu_arg(c, 1)) gm_w32(cpu_arg(c, 1), at(c, 0, end));
  uint64_t bits;
  memcpy(&bits, &d, sizeof bits);
  cpu_ret64(c, bits);
}

static void h_tolower(cpu *c) {
  uint32_t ch = cpu_arg(c, 0);
  cpu_ret(c, ch < 0x80 ? (uint32_t)tolower((int)ch) : ch);
}

static void h_toupper(cpu *c) {
  uint32_t ch = cpu_arg(c, 0);
  cpu_ret(c, ch < 0x80 ? (uint32_t)toupper((int)ch) : ch);
}

const dynlib_impl reimpl_string[] = {
    {"atoi", h_atoi},           {"atol", h_atoi},
    {"atoll", h_atoll},         {"strcasecmp", h_strcasecmp},
    {"strcat", h_strcat},       {"strchr", h_strchr},
    {"strcoll", h_strcoll},     {"strcpy", h_strcpy},
    {"strcspn", h_strcspn},     {"strdup", h_strdup},
    {"strerror", h_strerror},   {"strerror_r", h_strerror_r},
    {"strncasecmp", h_strncasecmp}, {"strncat", h_strncat},
    {"strncmp", h_strncmp},     {"strncpy", h_strncpy},
    {"strpbrk", h_strpbrk},     {"strrchr", h_strrchr},
    {"strstr", h_strstr},       {"strtod", h_strtod},
    {"strtok", h_strtok},       {"strtok_r", h_strtok_r},
    {"strxfrm", h_strxfrm},     {"tolower", h_tolower},
    {"toupper", h_toupper},     {NULL, NULL},
};
