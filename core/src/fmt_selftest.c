/*
 * The printf and scanf shims against the host libc.
 *
 * Every case runs twice: through the guest thunk (snprintf, vsnprintf,
 * sprintf, sscanf, fscanf), with its arguments laid out the way 32-bit ARM
 * code passes them (AAPCS softfp: 64-bit values in an even register pair or
 * an 8-byte aligned stack slot, va_lists likewise), and through the host's
 * own function with the matching host types. The two must agree byte for
 * byte: the output and return value for printf; for scanf the return value,
 * every byte of every destination (pre-filled alike) and, for fscanf, the
 * next character left in the stream.
 *
 * The host libc is the reference except where bionic differs on purpose
 * (printf.c's header): %p and L on integers are checked against fixed
 * strings instead. Guest long, size_t and ptrdiff_t are 32-bit, so a guest
 * %ld is compared with a host %d; guest long double is double.
 *
 * Covered: every conversion, flag, width/precision form (including '*' and
 * negative '*'), length modifier and many edge values (INT_MIN, LLONG_MIN,
 * -0.0, denormals, DBL_MAX, inf, nan, values just past the float and int
 * ranges); the exact format strings and value ranges the game uses for its
 * save profile ("%f" of its float timers, "%04d-%02d-%02d 00:00:00", "%lld",
 * and the "%f,%f,%f" / "%d,%d,%d,%d" parsers); floats that a double would
 * round twice; matching failures, EOF and %n at every position; and a
 * printf("%f") -> sscanf("%f") round trip over a few thousand floats.
 */
#include "fmt_selftest.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "dynlib.h"
#include "guest_mem.h"
#include "reimpl/reimpl.h"

#pragma clang diagnostic ignored "-Wformat-nonliteral"

static int failures, cases;
static cpu *C;
static uint32_t g_out, g_strs, g_slots, g_va; /* guest scratch areas */

#define OUT_SIZE 1024
#define NSLOT 6
#define SLOT 256

#define FAIL(...)                                                                                  \
  do {                                                                                             \
    if (failures++ < 40) {                                                                         \
      fprintf(stderr, "fmt-test FAIL: " __VA_ARGS__);                                              \
      fputc('\n', stderr);                                                                         \
    }                                                                                              \
  } while (0)

static uint32_t thunk(const char *name) {
  const dynlib_entry *e = dynlib_find(name);
  return e ? e->addr : 0;
}

/* Guest copies of strings; the area is reused per case. */
static uint32_t str_at;
static uint32_t gstr(const char *s) {
  if (!s) return 0;
  uint32_t a = str_at;
  strcpy(g2h(a), s);
  str_at += ((uint32_t)strlen(s) + 8) & ~7u;
  return a;
}

/* Argument words of one guest call. A 64-bit value starts at an even word:
 * an even register, or (words 4 on, at the 8-aligned sp) an aligned slot. */
typedef struct {
  uint32_t w[40];
  int n;
} gcall;

static void push32(gcall *g, uint32_t v) { g->w[g->n++] = v; }
static void push64(gcall *g, uint64_t v) {
  if (g->n & 1) g->w[g->n++] = 0xdeadbeef;
  push32(g, (uint32_t)v);
  push32(g, (uint32_t)(v >> 32));
}

static uint64_t dbits(double d) {
  uint64_t b;
  memcpy(&b, &d, 8);
  return b;
}

/* --- formats: guest spec -> host spec --- */

/* Rewrites a guest format for the host: l, z, t and L on integers mean a
 * 32-bit int (host: none), q and j mean long long; on floats printf drops l
 * and L (double), scanf turns L and q into l. */
static void host_format(const char *g, char *h, int scan) {
  while (*g) {
    if (*g != '%') {
      *h++ = *g++;
      continue;
    }
    *h++ = *g++;
    if (*g == '%') {
      *h++ = *g++;
      continue;
    }
    while (*g && strchr(scan ? "*" : "-+ #0'", *g)) *h++ = *g++;
    while (*g == '*' || (*g >= '0' && *g <= '9')) *h++ = *g++;
    if (*g == '.') {
      *h++ = *g++;
      while (*g == '*' || (*g >= '0' && *g <= '9')) *h++ = *g++;
    }
    char len[3] = {0};
    if ((g[0] == 'h' && g[1] == 'h') || (g[0] == 'l' && g[1] == 'l')) len[0] = g[0], len[1] = g[1], g += 2;
    else if (*g && strchr("hlqjztL", *g)) len[0] = *g++;
    char conv = *g;
    if (conv && strchr("diouxXn", conv)) {
      if (!strcmp(len, "q") || !strcmp(len, "j")) strcpy(len, "ll");
      else if (len[0] && strchr("lztL", len[0]) && len[1] != 'l') len[0] = 0;
    } else if (conv && strchr("fFeEgGaA", conv)) {
      if (!scan) len[0] = 0;
      else if (len[0] == 'L' || len[0] == 'q') strcpy(len, "l");
    }
    for (char *l = len; *l; l++) *h++ = *l;
    if (conv == '[') {
      *h++ = *g++;
      if (*g == '^') *h++ = *g++;
      if (*g == ']') *h++ = *g++;
      while (*g && *g != ']') *h++ = *g++;
    }
    if (*g) *h++ = *g++;
  }
  *h = 0;
}

/* --- printf --- */

enum { I32, I64, DBL, STR };

typedef struct {
  int kind;
  int64_t i;
  double d;
  const char *s;
} val;

/* snprintf(buf, n, fmt, stars..., v) through the guest thunk, or through
 * vsnprintf over a guest va_list. Returns the guest's return value. */
static uint32_t guest_printf1(const char *fmt, uint32_t n, int nstar, const int *stars,
                              const val *v, int via_va_list) {
  str_at = g_strs;
  uint32_t gf = gstr(fmt), gs = v->kind == STR ? gstr(v->s) : 0;
  memset(g2h(g_out), 0x5a, OUT_SIZE);
  gcall g = {.n = 0};
  push32(&g, g_out);
  push32(&g, n);
  push32(&g, gf);
  if (!via_va_list) {
    for (int i = 0; i < nstar; i++) push32(&g, (uint32_t)stars[i]);
    if (v->kind == I32) push32(&g, (uint32_t)v->i);
    else if (v->kind == I64) push64(&g, (uint64_t)v->i);
    else if (v->kind == DBL) push64(&g, dbits(v->d));
    else push32(&g, gs);
    return cpu_call(C, thunk("snprintf"), g.n, g.w);
  }
  /* A va_list is a pointer into the argument words: 64-bit ones 8-aligned. */
  uint32_t ap = g_va, p = ap + 4; /* start off 8-alignment to exercise padding */
  for (int i = 0; i < nstar; i++) gm_w32(p, (uint32_t)stars[i]), p += 4;
  if (v->kind == I32 || v->kind == STR) {
    gm_w32(p, v->kind == STR ? gs : (uint32_t)v->i);
  } else {
    uint64_t b = v->kind == DBL ? dbits(v->d) : (uint64_t)v->i;
    p = (p + 7) & ~7u;
    gm_w32(p, (uint32_t)b), gm_w32(p + 4, (uint32_t)(b >> 32));
  }
  push32(&g, ap + 4);
  return cpu_call(C, thunk("vsnprintf"), g.n, g.w);
}

static int host_printf1(char *buf, size_t n, const char *hf, int nstar, const int *st, const val *v) {
#define HP(...)                                                                                    \
  (v->kind == I32   ? snprintf(buf, n, hf, __VA_ARGS__ (int)v->i)                                  \
   : v->kind == I64 ? snprintf(buf, n, hf, __VA_ARGS__ (long long)v->i)                            \
   : v->kind == DBL ? snprintf(buf, n, hf, __VA_ARGS__ v->d)                                       \
                    : snprintf(buf, n, hf, __VA_ARGS__ v->s))
  if (nstar == 0) return HP();
  if (nstar == 1) return HP(st[0], );
  return HP(st[0], st[1], );
#undef HP
}

static void describe(char *d, size_t n, const val *v) {
  if (v->kind == I32 || v->kind == I64) snprintf(d, n, "%lld", (long long)v->i);
  else if (v->kind == DBL) snprintf(d, n, "%.17g", v->d);
  else snprintf(d, n, "\"%s\"", v->s ? v->s : "NULL");
}

/* One printf case: snprintf and vsnprintf into a large buffer, then
 * snprintf truncated to a few sizes, each against the host. */
static void check_printf1(const char *fmt, int nstar, const int *stars, const val *v) {
  char hf[256], want[OUT_SIZE], d[64];
  host_format(fmt, hf, 0);
  describe(d, sizeof d, v);
  int hr = host_printf1(want, sizeof want, hf, nstar, stars, v);
  for (int via = 0; via < 2; via++) {
    cases++;
    uint32_t r = guest_printf1(fmt, OUT_SIZE, nstar, stars, v, via);
    const char *got = g2h(g_out);
    if (C->faulted || (int)r != hr || strcmp(got, want))
      FAIL("%s(\"%s\", %s) gave \"%s\" (%d), host \"%s\" (%d)", via ? "vsnprintf" : "snprintf", fmt,
           d, got, (int)r, want, hr);
  }
  static const uint32_t sizes[] = {0, 1, 3, 8};
  for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
    char hb[16];
    memset(hb, 0x5a, sizeof hb);
    int hr2 = host_printf1(hb, sizes[k], hf, nstar, stars, v);
    uint32_t r = guest_printf1(fmt, sizes[k], nstar, stars, v, 0);
    cases++;
    if ((int)r != hr2 || memcmp(g2h(g_out), hb, sizeof hb))
      FAIL("snprintf(n=%u, \"%s\", %s) gave \"%.*s\" (%d), host \"%.*s\" (%d)", sizes[k], fmt, d,
           (int)sizes[k], (const char *)g2h(g_out), (int)r, (int)sizes[k], hb, hr2);
  }
}

static int count_stars(const char *f) {
  int n = 0;
  for (; *f; f++) n += *f == '*';
  return n;
}

static void printf_matrix(void) {
  static const int64_t i32s[] = {0,    1,     -1,      7,        42,         -42,    255,
                                 256,  65535, 65536,   INT_MAX,  INT_MIN,    1234567, -987654,
                                 2026, 11,    (int32_t)0xdeadbeef, 0x141, -129};
  static const char *ifmts[] = {
      "%d",   "%i",    "%u",   "%x",   "%X",    "%o",   "%5d",  "%-5d|", "%05d", "%+d",
      "% d",  "%.3d",  "%.0d", "%8.3d", "%#x",  "%#o",  "%hhd", "%hhu",  "%hd",  "%hu",
      "%ld",  "%lu",   "%lx",  "%zu",  "%zd",   "%td",  "%02d", "%04d",  "%01d", "%02x",
      "%08X", "%03d",  "%-2d|", "%c",  "%3c",   "%-3c|", "%hhx", "%#.3o", "%+.0d", "% 6ld",
      "%*d",  "%-*d|", "%.*d", "%*.*d", "%08lX"};
  static const int64_t i64s[] = {0, 1, -1, 1ll << 40, LLONG_MAX, LLONG_MIN, 1767395000ll,
                                 -1234567890123ll, (int64_t)0x123456789abcdef0ull, 4294967296ll};
  static const char *lfmts[] = {"%lld", "%llu", "%llx", "%llX",  "%qd",  "%jd",  "%20lld",
                                "%-20lld|", "%020lld", "%+lld", "%4lld", "%02lld", "%3lld",
                                "%#llo", "%*lld", "%.*llu"};
  static const double dbls[] = {0.0,
                                -0.0,
                                1.0,
                                -1.0,
                                0.5,
                                0.1,
                                1.0 / 3,
                                2.5,
                                3.5,
                                -2.5,
                                1e-7,
                                0.000001,
                                0.0000005,
                                1.767395,
                                1767395123.0,
                                (double)1767395123.0f,
                                (double)10.025937f,
                                (double)93.399994f,
                                (double)0.583143f,
                                123456.789,
                                999999.9999995,
                                1e15,
                                1e21,
                                1e300,
                                -1e-300,
                                5e-324,
                                DBL_MAX,
                                DBL_MIN,
                                (double)FLT_MAX,
                                INFINITY,
                                -INFINITY,
                                NAN};
  static const char *dfmts[] = {
      "%f",     "%F",     "%e",    "%E",    "%g",     "%G",    "%a",    "%A",   "%.0f",
      "%.1f",   "%.2f",   "%.3f",  "%.6f",  "%.10f",  "%.15le", "%10.4f", "%-10.2f|", "%+f",
      "% f",    "%010.3f", "%#.0f", "%#g",   "%.3g",   "%.10g", "%.17g", "%lf",  "%Lf",
      "%0.4f",  "%4.1f",  "%0.3f", "%.0e",  "%#.0e",  "%-+12.3e|", "%.20g", "%.*f", "%.*Lf",
      "%*.*f",  "%*f",    "%.*g"};
  static const char *strs[] = {"", "a", "droid android", "2026-11-02 00:00:00", NULL};
  static const char *sfmts[] = {"%s",  "%10s", "%-10s|", "%.3s",     "%.*s",  "%-25.25s",
                                "%8s", "%*s",  "%.0s",   "<%-*.*s>", "%12s"};
  /* '*' arguments: plain, zero, and negative (a negative width means '-'; a
   * negative precision means none). */
  static const int stars[][2] = {{7, 3}, {0, 0}, {-9, 2}, {12, -1}, {-1, -5}};

  for (size_t f = 0; f < sizeof ifmts / sizeof *ifmts; f++) {
    for (size_t k = 0; k < sizeof i32s / sizeof *i32s; k++) {
      val v = {I32, i32s[k], 0, NULL};
      int ns = count_stars(ifmts[f]);
      for (size_t s = 0; s < (ns ? sizeof stars / sizeof *stars : 1); s++)
        check_printf1(ifmts[f], ns, stars[s], &v);
    }
  }
  for (size_t f = 0; f < sizeof lfmts / sizeof *lfmts; f++)
    for (size_t k = 0; k < sizeof i64s / sizeof *i64s; k++) {
      val v = {I64, i64s[k], 0, NULL};
      int ns = count_stars(lfmts[f]);
      for (size_t s = 0; s < (ns ? sizeof stars / sizeof *stars : 1); s++)
        check_printf1(lfmts[f], ns, stars[s], &v);
    }
  for (size_t f = 0; f < sizeof dfmts / sizeof *dfmts; f++)
    for (size_t k = 0; k < sizeof dbls / sizeof *dbls; k++) {
      val v = {DBL, 0, dbls[k], NULL};
      int ns = count_stars(dfmts[f]);
      for (size_t s = 0; s < (ns ? sizeof stars / sizeof *stars : 1); s++)
        check_printf1(dfmts[f], ns, stars[s], &v);
    }
  for (size_t f = 0; f < sizeof sfmts / sizeof *sfmts; f++)
    for (size_t k = 0; k < sizeof strs / sizeof *strs; k++) {
      val v = {STR, 0, 0, strs[k]};
      int ns = count_stars(sfmts[f]);
      for (size_t s = 0; s < (ns ? sizeof stars / sizeof *stars : 1); s++)
        check_printf1(sfmts[f], ns, stars[s], &v);
    }
}

/* snprintf(out, OUT_SIZE, fmt, ...) with the variadic words of g, which
 * start at word 3 (ARGS() leaves room for the fixed three), against a fixed
 * result: where bionic is the reference, or the result is known. */
#define ARGS() {.n = 3}
static void check_fixed(const char *fmt, gcall *g, const char *want) {
  cases++;
  g->w[0] = g_out;
  g->w[1] = OUT_SIZE;
  g->w[2] = gstr(fmt);
  uint32_t r = cpu_call(C, thunk("snprintf"), g->n, g->w);
  const char *got = g2h(g_out);
  if (C->faulted || strcmp(got, want) || r != strlen(want))
    FAIL("snprintf(\"%s\") gave \"%s\" (%u), bionic gives \"%s\"", fmt, got, r, want);
}

static void printf_bionic(void) {
  str_at = g_strs;
  gcall a = ARGS();
  push32(&a, 0x1234);
  check_fixed("%p", &a, "0x1234");
  check_fixed("[%10p]", &a, "[    0x1234]");
  check_fixed("[%-10p]", &a, "[0x1234    ]");
  check_fixed("[%010p]", &a, "[0x00001234]");
  check_fixed("[%.8p]", &a, "[0x00001234]");
  a.w[3] = 0;
  check_fixed("%p", &a, "0x0");
  a.w[3] = 0xdeadbeef;
  check_fixed("%p", &a, "0xdeadbeef");
  /* L on an integer is a plain (32-bit) int on bionic; the next word is
   * the next argument. */
  gcall b = ARGS();
  push32(&b, 77);
  push32(&b, 5);
  check_fixed("%Lu|%d", &b, "77|5");
  check_fixed("%Ld|%Lx", &b, "77|5");
  /* %c prints the low byte of its int. */
  gcall c = ARGS();
  push32(&c, 0x141);
  check_fixed("%c", &c, "A");
  /* %n stores the count so far at each width. */
  uint32_t n = g_slots;
  memset(g2h(n), 0xee, 32);
  gcall d = ARGS();
  push32(&d, n);
  push32(&d, n + 8);
  push32(&d, n + 16);
  push32(&d, n + 24);
  check_fixed("ab%nc%hhnde%llnf%hn", &d, "abcdef");
  uint64_t ll;
  memcpy(&ll, g2h(n + 16), 8);
  if (gm_r32(n) != 2 || *(uint8_t *)g2h(n + 8) != 3 || *(uint8_t *)g2h(n + 9) != 0xee ||
      ll != 5 || *(uint16_t *)g2h(n + 24) != 6 || *(uint8_t *)g2h(n + 26) != 0xee)
    FAIL("%%n stored %u, %u, %llu, %u", gm_r32(n), *(uint8_t *)g2h(n + 8), (unsigned long long)ll,
         *(uint16_t *)g2h(n + 24));
}

/* Several arguments of mixed sizes: each 64-bit one must start at an even
 * word wherever it falls (registers, the r3/stack boundary, the stack). */
static void printf_mixed(void) {
  char want[OUT_SIZE];
  str_at = g_strs;
  /* The save profile's date and the game's own formats. */
  gcall g = ARGS();
  push32(&g, 2026), push32(&g, 11), push32(&g, 2);
  check_fixed("%04d-%02d-%02d 00:00:00", &g, "2026-11-02 00:00:00");
  gcall t = ARGS();
  push32(&t, 23), push32(&t, 5), push32(&t, 9);
  check_fixed("NEXT REWARD IN: %02d:%02d:%02d", &t, "NEXT REWARD IN: 23:05:09");
  gcall u = ARGS();
  push64(&u, 1767395123456ll);
  check_fixed("%lld", &u, "1767395123456");

  /* 0-5 ints first, so the double and long long land on every alignment. */
  for (int lead = 0; lead < 6; lead++) {
    str_at = g_strs;
    gcall m = ARGS();
    char fmt[128] = "";
    want[0] = 0;
    for (int i = 0; i < lead; i++) {
      strcat(fmt, "%d,");
      push32(&m, (uint32_t)(i * 11 - 7));
      snprintf(want + strlen(want), 32, "%d,", i * 11 - 7);
    }
    strcat(fmt, "%f|%lld|%s|%c|%.3e|%u");
    push64(&m, dbits((double)10.025937f));
    push64(&m, (uint64_t)-1234567890123ll);
    push32(&m, gstr("pk"));
    push32(&m, 'Z');
    push64(&m, dbits(-0.000123));
    push32(&m, 4000000000u);
    snprintf(want + strlen(want), 128, "%f|%lld|%s|%c|%.3e|%u", (double)10.025937f,
             -1234567890123ll, "pk", 'Z', -0.000123, 4000000000u);
    check_fixed(fmt, &m, want);
  }
}

/* --- scanf --- */

/* Fills the destination slots alike on both sides. */
static void fill_slots(uint8_t *h) { memset(h, 0xa5, NSLOT * SLOT); }

static uint32_t guest_sscanf(const char *in, const char *fmt) {
  str_at = g_strs;
  gcall g = {.n = 0};
  push32(&g, gstr(in));
  push32(&g, gstr(fmt));
  for (int i = 0; i < NSLOT; i++) push32(&g, g_slots + (uint32_t)(i * SLOT));
  fill_slots(g2h(g_slots));
  return cpu_call(C, thunk("sscanf"), g.n, g.w);
}

static void slot_diff(char *d, size_t n, const uint8_t *a, const uint8_t *b) {
  for (int i = 0; i < NSLOT * SLOT; i++)
    if (a[i] != b[i]) {
      snprintf(d, n, "slot %d byte %d: 0x%02x, host 0x%02x", i / SLOT, i % SLOT, a[i], b[i]);
      return;
    }
  snprintf(d, n, "same slots");
}

static void check_sscanf(const char *fmt, const char *in) {
  char hf[256];
  host_format(fmt, hf, 1);
  static uint8_t hs[NSLOT * SLOT];
  fill_slots(hs);
  int hr = sscanf(in, hf, hs, hs + SLOT, hs + 2 * SLOT, hs + 3 * SLOT, hs + 4 * SLOT, hs + 5 * SLOT);
  uint32_t r = guest_sscanf(in, fmt);
  cases++;
  if (C->faulted || (int)r != hr || memcmp(g2h(g_slots), hs, sizeof hs)) {
    char d[96];
    slot_diff(d, sizeof d, g2h(g_slots), hs);
    FAIL("sscanf(\"%s\", \"%s\") returned %d, host %d (%s)", in, fmt, (int)r, hr, d);
  }
}

/* fscanf on a file holding `in`, then getc: the stream must be left at the
 * same place as the host's. */
static char scan_path[1200];
static void check_fscanf(const char *fmt, const char *in) {
  FILE *w = fopen(scan_path, "wb");
  if (!w) {
    FAIL("can't write %s", scan_path);
    return;
  }
  fputs(in, w);
  fclose(w);
  char hf[256];
  host_format(fmt, hf, 1);
  static uint8_t hs[NSLOT * SLOT];
  fill_slots(hs);
  FILE *h = fopen(scan_path, "rb");
  int hr = fscanf(h, hf, hs, hs + SLOT, hs + 2 * SLOT, hs + 3 * SLOT, hs + 4 * SLOT, hs + 5 * SLOT);
  int hnext = getc(h);
  fclose(h);

  str_at = g_strs;
  uint32_t o[] = {gstr(scan_path), gstr("rb")};
  uint32_t f = cpu_call(C, thunk("fopen"), 2, o);
  if (!f) {
    FAIL("guest fopen(%s) failed", scan_path);
    return;
  }
  gcall g = {.n = 0};
  push32(&g, f);
  push32(&g, gstr(fmt));
  for (int i = 0; i < NSLOT; i++) push32(&g, g_slots + (uint32_t)(i * SLOT));
  fill_slots(g2h(g_slots));
  uint32_t r = cpu_call(C, thunk("fscanf"), g.n, g.w);
  uint32_t gf[] = {f};
  int gnext = (int)cpu_call(C, thunk("getc"), 1, gf);
  cpu_call(C, thunk("fclose"), 1, gf);
  cases++;
  if (C->faulted || (int)r != hr || gnext != hnext || memcmp(g2h(g_slots), hs, sizeof hs)) {
    char d[96];
    slot_diff(d, sizeof d, g2h(g_slots), hs);
    FAIL("fscanf(\"%s\", \"%s\") returned %d then getc %d, host %d then %d (%s)", in, fmt, (int)r,
         gnext, hr, hnext, d);
  }
}

static void scanf_matrix(void) {
  static const char *iin[] = {"0",          "42",         "-42",        "+7",
                              "  12",       "0x1A",       "077",        "2147483647",
                              "2147483648", "-2147483649", "4294967295", "4294967296",
                              "99999999999999999999",     "-99999999999999999999",
                              "1767395123", "abc",        "",           "   ",
                              "12abc",      "-",          "0x",         "1e5",
                              "300",        "-129",       "65536",      "9223372036854775808",
                              "\t\n 5",     "0X7fFf",     "-0",         "08"};
  static const char *ifmts[] = {"%d",   "%i",  "%u",   "%x",   "%X",   "%o",   "%hhd", "%hhu",
                                "%hd",  "%hu", "%ld",  "%lu",  "%lld", "%llu", "%llx", "%qd",
                                "%jd",  "%zd", "%Ld",  "%3d",  "%*d",  "%d%n", "%lli", "%2hhx",
                                "%1d",  "%hhi", "%d%lln"};
  static const char *fin[] = {"0",
                              "1",
                              "-1.5",
                              "10.025937",
                              "1.767395",
                              "93.399994",
                              "0.583143",
                              "0.1",
                              "3.4028235e38",
                              "3.4028236e38",
                              "1e39",
                              "1e-46",
                              "1.401298464324817e-45",
                              "7e-46",
                              "nan",
                              "-inf",
                              "INFINITY",
                              "0x1.8p1",
                              "1e",
                              ".5",
                              "-.5e-3",
                              "  2.5x",
                              "abc",
                              "",
                              "1767395123",
                              "2026-11-02 00:00:00",
                              /* exactly or just past a float halfway point: a
                               * double in between would round twice */
                              "1.000000059604644775390625",
                              "1.000000059604644775390626",
                              "1.0000000596046447753906250000000001",
                              "16777217",
                              "16777217.000000000000001",
                              "33554435",
                              "0.100000001490116119384765625",
                              "7.038531e-26",
                              "1.17549435e-38",
                              "1.1754942e-38",
                              "1e-320",
                              "123456789012345678901234567890"};
  static const char *ffmts[] = {"%f", "%e", "%g", "%a", "%F", "%E", "%G", "%lf",
                                "%le", "%lg", "%Lf", "%4f", "%*f", "%f%n", "%3lf", "%10f"};
  static const char *sin_[] = {"droid android", "  lead", "", "a", "abc;def", "x]y",
                               "host:80:path", "Nov 20 2018", "line one\nline two", "12345678"};
  static const char *sfmts[] = {"%s",     "%5s",     "%c",    "%3c",     "%[a-z]",  "%[^,]",
                                "%[^\n]", "%15[^:]", "%[]a-z]", "%*s %s", "%[^;];%s", "%2c%s",
                                "%*[^:]:%d:%s", "%255[^:]:%d:%255s", "%s %d %d", "%1[a]"};
  for (size_t f = 0; f < sizeof ifmts / sizeof *ifmts; f++)
    for (size_t k = 0; k < sizeof iin / sizeof *iin; k++) check_sscanf(ifmts[f], iin[k]);
  for (size_t f = 0; f < sizeof ffmts / sizeof *ffmts; f++)
    for (size_t k = 0; k < sizeof fin / sizeof *fin; k++) check_sscanf(ffmts[f], fin[k]);
  for (size_t f = 0; f < sizeof sfmts / sizeof *sfmts; f++)
    for (size_t k = 0; k < sizeof sin_ / sizeof *sin_; k++) check_sscanf(sfmts[f], sin_[k]);

  /* Whole formats: the game's parsers, literals, %%, %n, failures and EOF at
   * each position, and directives after a failed one. */
  static const char *multi[][2] = {
      {"%f,%f,%f", "0.5,0.5,0"},        {"%f,%f,%f", "16,40,0"},      {"%f,%f,%f", "1,0.5"},
      {"%f,%f,%f", "1.4,1.2,1"},        {"%d,%d,%d,%d", "42,11,47"},  {"%d,%d,%d,%d", "255,0,192,7"},
      {"%d,%d,%d,%d", ""},              {"%s %d %d", "Nov 20 2018"},  {"%d:%d:%d", "11:16:25"},
      {"%d %d", "5 apples"},            {"%d", ""},                   {"%d", "   "},
      {"x%d", "y5"},                    {"x%d", "x5"},                {"x%d", ""},
      {"%d%%%d", "5%6"},                {"%d%%%d", "5 % 6"},          {" %d", "  9"},
      {"%dabc%d", "1abc2"},             {"%d abc", "1 abd"},          {"%d abc%n", "1 abc"},
      {"%d%n %d%n", "3 4"},             {"%n", ""},                   {"%n%d", "x"},
      {"%d %n", "7"},                   {"%c%c%c", "ab"},             {"%2d%2d%d", "123456"},
      {"%*d %d", "1 2"},                {"%*d%n", "77"},              {"%s%n", "word rest"},
      {"%lld,%f", "1767395123456,93.4"}, {"%f %lf %Lf", "1.5 2.5 3.5"}, {"%hhd%hd%d%lld", "1 2 3 4"},
      {"%[0-9]-%[0-9]-%[0-9] %d:%d:%d", "2026-11-02 00:00:00"},
      {"%4d-%2d-%2d", "2026-11-02"},    {"abc", "abc"},               {"abc", "abd"},
      {"%d,", "5,"},                    {"%d\n", "5\n\n"},            {"%%", "%"},
      {"%x%n%X", "ffg"},                {"%i %i %i", "010 0x10 10"},  {"%3s%3s", "abcdefg"},
      {"%e%n", "1e+"},                  {"%f", "-"},                  {"%d", "+"},
  };
  for (size_t k = 0; k < sizeof multi / sizeof *multi; k++) check_sscanf(multi[k][0], multi[k][1]);
  for (size_t k = 0; k < sizeof multi / sizeof *multi; k++) check_fscanf(multi[k][0], multi[k][1]);
  static const char *fsc[][2] = {{"%d", "42 rest"},     {"%f", "10.025937\n"}, {"%s", "  word\n"},
                                 {"%[^\n]", "a line\nb"}, {"%d%d", "1 x"},       {"%5c", "abcdefg"},
                                 {"%d", ""},            {"%f", "1e"},          {"%x", "0xz"}};
  for (size_t k = 0; k < sizeof fsc / sizeof *fsc; k++) check_fscanf(fsc[k][0], fsc[k][1]);
}

/* The game writes its float timers with "%f" and parses floats with "%f":
 * over a few thousand floats in the ranges it uses (and well beyond), the
 * guest round trip printf -> sscanf must equal the host's, bit for bit; a
 * value read back with %f must be what strtof gives. */
static void float_round_trip(void) {
  uint32_t seed = 19;
  for (int i = 0; i < 4000; i++) {
    seed = seed * 1664525u + 1013904223u;
    float f;
    switch (i % 4) {
    case 0: f = (float)(seed % 100000000u) / 1000000.0f; break; /* 0..100 s, like the timer */
    case 1: f = (float)seed / 3.0f; break;                      /* up to ~1.4e9 */
    case 2: { uint32_t b = seed & 0x7f7fffffu; memcpy(&f, &b, 4); } break; /* any finite */
    default: f = -(float)(seed % 1000u) / 7.0f; break;
    }
    val v = {DBL, 0, (double)f, NULL};
    char want[512];
    int hr = snprintf(want, sizeof want, "%f", (double)f);
    uint32_t r = guest_printf1("%f", OUT_SIZE, 0, NULL, &v, 0);
    cases++;
    if ((int)r != hr || strcmp(g2h(g_out), want)) {
      FAIL("snprintf(\"%%f\", %.9g) gave \"%s\", host \"%s\"", (double)f, (const char *)g2h(g_out),
           want);
      continue;
    }
    float hf, sf = strtof(want, NULL);
    sscanf(want, "%f", &hf);
    r = guest_sscanf(want, "%f");
    float gf;
    memcpy(&gf, g2h(g_slots), 4);
    cases++;
    if (r != 1 || memcmp(&gf, &hf, 4) || memcmp(&gf, &sf, 4))
      FAIL("sscanf(\"%s\", \"%%f\") gave %.9g, host %.9g, strtof %.9g", want, (double)gf,
           (double)hf, (double)sf);
  }
  /* The case a double in between gets wrong: just above the halfway point
   * between 1 and the next float, a double rounds down to it exactly and
   * then to 1.0f; strtof (and bionic) give 1.0000001f. */
  uint32_t r = guest_sscanf("1.000000059604644775390626", "%f");
  cases++;
  if (r != 1 || gm_r32(g_slots) != 0x3f800001u)
    FAIL("sscanf(\"1.000000059604644775390626\", \"%%f\") stored 0x%08x, not 0x3f800001",
         gm_r32(g_slots));
}

/* sprintf (no size) and a long result past the shim's small buffer. */
static void printf_misc(void) {
  str_at = g_strs;
  char big[700];
  memset(big, 'q', sizeof big - 1);
  big[sizeof big - 1] = 0;
  uint32_t a[] = {g_out, gstr("<%s>"), gstr(big)};
  memset(g2h(g_out), 0, OUT_SIZE);
  uint32_t r = cpu_call(C, thunk("sprintf"), 3, a);
  cases++;
  if (r != sizeof big + 1 || strlen(g2h(g_out)) != sizeof big + 1 ||
      ((char *)g2h(g_out))[sizeof big] != '>')
    FAIL("sprintf of a %zu-char string returned %u", sizeof big - 1, r);
}

int fmt_selftest(cpu *c) {
  C = c;
  failures = cases = 0;
  uint32_t mem = gm_alloc(0x10000, 0, "fmt-test buffers");
  if (!mem) return -1;
  g_out = mem, g_strs = mem + 0x1000, g_slots = mem + 0x3000, g_va = mem + 0x5000;
  snprintf(scan_path, sizeof scan_path, "%s/files", stdio_data_root());
  mkdir(scan_path, 0755);
  snprintf(scan_path, sizeof scan_path, "%s/files/fmt-selftest.txt", stdio_data_root());

  printf_matrix();
  printf_bionic();
  printf_mixed();
  printf_misc();
  scanf_matrix();
  float_round_trip();
  remove(scan_path);

  printf("fmt-test: %d cases, %d failed\n", cases, failures);
  return failures ? -1 : 0;
}
