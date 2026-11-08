/*
 * Small libc functions: the environment, number parsing, and the wide-character
 * classification calls libstdc++'s locale setup makes.
 *
 * Android apps run in the "C" locale with ASCII character classes, so the
 * wide-character calls only classify 0..127.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cpu.h"
#include "../guest_mem.h"
#include "reimpl.h"

#define G_EOF 0xFFFFFFFFu
#define G_WEOF 0xFFFFFFFFu
#define G_ERANGE 34

/* bionic's (OpenBSD's) <ctype.h> macros index _ctype_[1 + c] for these class
 * bits, and tolower/toupper inline through _tolower_tab_/_toupper_tab_ (shorts,
 * entry 0 for EOF). The game links its own copies of isspace and friends, so
 * these tables must hold real data. */
#define CT_U 0x01
#define CT_L 0x02
#define CT_N 0x04
#define CT_S 0x08
#define CT_P 0x10
#define CT_C 0x20
#define CT_X 0x40
#define CT_B 0x80

static uint8_t ctype_bits(int ch) {
  if (ch >= 0x80) return 0;
  if (ch == ' ') return CT_S | CT_B;
  if (ch >= '\t' && ch <= '\r') return CT_C | CT_S;
  if (ch < 0x20 || ch == 0x7f) return CT_C;
  if (ch >= '0' && ch <= '9') return CT_N;
  if (ch >= 'A' && ch <= 'Z') return CT_U | (ch <= 'F' ? CT_X : 0);
  if (ch >= 'a' && ch <= 'z') return CT_L | (ch <= 'f' ? CT_X : 0);
  return CT_P;
}

int libc_init_data(void) {
  uint32_t ct = guest_malloc(257), lo = guest_malloc(257 * 2), up = guest_malloc(257 * 2);
  if (!ct || !lo || !up) return -1;
  uint8_t *ctp = g2h(ct);
  int16_t *lop = g2h(lo), *upp = g2h(up);
  ctp[0] = 0;
  lop[0] = upp[0] = -1;
  for (int ch = 0; ch < 256; ch++) {
    ctp[1 + ch] = ctype_bits(ch);
    lop[1 + ch] = (int16_t)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
    upp[1 + ch] = (int16_t)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
  }
  gm_w32(dynlib_find("_ctype_")->addr, ct);
  gm_w32(dynlib_find("_tolower_tab_")->addr, lo);
  gm_w32(dynlib_find("_toupper_tab_")->addr, up);
  return 0;
}

/* The game has no environment variables of its own; log what it asks for. */
static void h_getenv(cpu *c) {
  fprintf(stderr, "[libc] getenv(\"%s\") -> NULL\n", (const char *)g2h(cpu_arg(c, 0)));
  cpu_ret(c, 0);
}

/* getauxval(type): the hwcaps of a plain ARMv7 Cortex-A with VFPv3-D32 and
 * NEON, which dynarmic and our recompiler both implement. Answering here keeps
 * OpenSSL from probing features with SIGILL handlers. */
#define AT_HWCAP 16
#define AT_HWCAP2 26
#define HWCAP_ARMV7 (1u << 0 /* swp */ | 1u << 1 /* half */ | 1u << 2 /* thumb */ |              \
                     1u << 4 /* fast_mult */ | 1u << 6 /* vfp */ | 1u << 7 /* edsp */ |           \
                     1u << 12 /* neon */ | 1u << 13 /* vfpv3 */ | 1u << 15 /* tls */ |            \
                     1u << 19 /* vfpd32 */)

static void h_getauxval(cpu *c) {
  uint32_t type = cpu_arg(c, 0);
  cpu_ret(c, type == AT_HWCAP ? HWCAP_ARMV7 : 0);
  if (type != AT_HWCAP && type != AT_HWCAP2) fprintf(stderr, "[libc] getauxval(%u) -> 0\n", type);
}

static void set_errno_from_host(void) {
  if (errno == ERANGE) gm_w32(guest_errno_addr(), G_ERANGE);
}

/* strto*(const char *s, char **end, int base): end is a guest char **. */
static void finish_strto(cpu *c, const char *s, char *end) {
  if (cpu_arg(c, 1)) gm_w32(cpu_arg(c, 1), cpu_arg(c, 0) + (uint32_t)(end - s));
  set_errno_from_host();
}

/* The guest's long is 32-bit: clamp the host's 64-bit result like bionic. */
static void h_strtol(cpu *c) {
  const char *s = g2h(cpu_arg(c, 0));
  char *end;
  errno = 0;
  long long v = strtoll(s, &end, (int)cpu_arg(c, 2));
  if (v > INT32_MAX || v < INT32_MIN) {
    v = v > 0 ? INT32_MAX : INT32_MIN;
    errno = ERANGE;
  }
  finish_strto(c, s, end);
  cpu_ret(c, (uint32_t)(int32_t)v);
}

static void h_strtoul(cpu *c) {
  const char *s = g2h(cpu_arg(c, 0));
  char *end;
  errno = 0;
  unsigned long long v = strtoull(s, &end, (int)cpu_arg(c, 2));
  /* strtoul accepts a minus sign and negates in the result type. */
  int neg = 0;
  for (const char *p = s; p < end && isspace((unsigned char)*p); p++) neg = p[1] == '-';
  if (*s == '-') neg = 1;
  if (!neg && v > UINT32_MAX) {
    v = UINT32_MAX;
    errno = ERANGE;
  } else if (neg && (uint64_t)-v > UINT32_MAX) {
    v = UINT32_MAX;
    errno = ERANGE;
  }
  finish_strto(c, s, end);
  cpu_ret(c, (uint32_t)v);
}

static void h_strtoll(cpu *c) {
  const char *s = g2h(cpu_arg(c, 0));
  char *end;
  errno = 0;
  long long v = strtoll(s, &end, (int)cpu_arg(c, 2));
  finish_strto(c, s, end);
  cpu_ret64(c, (uint64_t)v);
}

/* --- wide characters, C locale --- */

static void h_btowc(cpu *c) {
  uint32_t ch = cpu_arg(c, 0);
  cpu_ret(c, ch < 0x80 ? ch : G_WEOF);
}

static void h_wctob(cpu *c) {
  uint32_t wc = cpu_arg(c, 0);
  cpu_ret(c, wc < 0x80 ? wc : G_EOF);
}

static const char *const class_names[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph",
                                          "lower", "print", "punct", "space", "upper", "xdigit"};
#define N_CLASSES (sizeof(class_names) / sizeof(class_names[0]))

/* wctype_t is 1 + the class index; 0 means unknown. */
static void h_wctype(cpu *c) {
  const char *name = g2h(cpu_arg(c, 0));
  uint32_t r = 0;
  for (uint32_t i = 0; i < N_CLASSES; i++)
    if (!strcmp(name, class_names[i])) r = i + 1;
  cpu_ret(c, r);
}

static void h_iswctype(cpu *c) {
  uint32_t wc = cpu_arg(c, 0), desc = cpu_arg(c, 1);
  int r = 0;
  if (wc < 0x80 && desc >= 1 && desc <= N_CLASSES) {
    int ch = (int)wc;
    switch (desc - 1) {
    case 0: r = isalnum(ch); break;
    case 1: r = isalpha(ch); break;
    case 2: r = isblank(ch); break;
    case 3: r = iscntrl(ch); break;
    case 4: r = isdigit(ch); break;
    case 5: r = isgraph(ch); break;
    case 6: r = islower(ch); break;
    case 7: r = isprint(ch); break;
    case 8: r = ispunct(ch); break;
    case 9: r = isspace(ch); break;
    case 10: r = isupper(ch); break;
    case 11: r = isxdigit(ch); break;
    }
  }
  cpu_ret(c, r != 0);
}

/* --- qsort: a heapsort in guest memory; the comparator is guest code called
 * through cpu_callback with pointers into the array, as bionic's qsort does. */

static void swap_elems(uint32_t a, uint32_t b, uint32_t size) {
  uint8_t *x = g2h(a), *y = g2h(b);
  for (uint32_t i = 0; i < size; i++) {
    uint8_t t = x[i];
    x[i] = y[i];
    y[i] = t;
  }
}

/* Returns 0, or -1 once the comparator has faulted. */
static int sift_down(cpu *c, uint32_t base, uint32_t size, uint32_t cmp, uint32_t root,
                     uint32_t n) {
  for (;;) {
    uint32_t child = 2 * root + 1;
    if (child >= n) return 0;
    uint32_t args[2] = {base + child * size, base + (child + 1) * size};
    if (child + 1 < n && (int32_t)cpu_callback(c, cmp, 2, args) < 0) child++;
    uint32_t args2[2] = {base + root * size, base + child * size};
    int32_t r = (int32_t)cpu_callback(c, cmp, 2, args2);
    if (c->faulted) return -1;
    if (r >= 0) return 0;
    swap_elems(base + root * size, base + child * size, size);
    root = child;
  }
}

/* qsort(base, n, size, int (*cmp)(const void *, const void *)) */
static void h_qsort(cpu *c) {
  uint32_t base = cpu_arg(c, 0), n = cpu_arg(c, 1), size = cpu_arg(c, 2), cmp = cpu_arg(c, 3);
  if (n < 2 || size == 0) return;
  for (uint32_t i = n / 2; i-- > 0;)
    if (sift_down(c, base, size, cmp, i, n) != 0) return;
  for (uint32_t end = n - 1; end > 0; end--) {
    swap_elems(base, base + end * size, size);
    if (sift_down(c, base, size, cmp, 0, end) != 0) return;
  }
}

const dynlib_impl reimpl_libc[] = {
    {"btowc", h_btowc},     {"getauxval", h_getauxval}, {"getenv", h_getenv},   {"iswctype", h_iswctype},
    {"qsort", h_qsort},
    {"strtol", h_strtol},   {"strtoll", h_strtoll}, {"strtoul", h_strtoul},
    {"wctob", h_wctob},     {"wctype", h_wctype},   {NULL, NULL},
};
