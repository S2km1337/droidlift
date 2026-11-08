/*
 * The printf family over guest arguments: one formatter shared by printf,
 * fprintf, sprintf and snprintf and their va_list forms.
 *
 * Arguments come through the gargs cursor (gargs.h). long, size_t and
 * pointers are 32-bit; long double is double.
 *
 * Each conversion is rebuilt as a host format spec and formatted by the host's
 * snprintf, so flags, width and precision behave like any C printf. Where
 * bionic differs from C99 or the host, bionic wins: L on an integer
 * conversion reads a plain int (only ll, q and j are 64-bit), %c prints the
 * low byte of its int, and %p is 0x-prefixed hex (0x0 for NULL).
 *
 * DL_TRACE_FMT=1 logs each format string and its result (debug aid).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../cpu.h"
#include "../guest_mem.h"
#include "gargs.h"
#include "reimpl.h"

/* Growable host output buffer. */
typedef struct obuf {
  char *p;
  size_t len, cap;
} obuf;

static void put(obuf *o, const char *s, size_t n) {
  if (o->len + n + 1 > o->cap) {
    while (o->len + n + 1 > o->cap) o->cap = o->cap ? o->cap * 2 : 256;
    o->p = realloc(o->p, o->cap);
  }
  memcpy(o->p + o->len, s, n);
  o->len += n;
  o->p[o->len] = 0;
}

static void putf(obuf *o, const char *spec, ...) {
  char small[256];
  va_list ap;
  va_start(ap, spec);
  int n = vsnprintf(small, sizeof small, spec, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n < sizeof small) {
    put(o, small, (size_t)n);
    return;
  }
  char *big = malloc((size_t)n + 1);
  va_start(ap, spec);
  vsnprintf(big, (size_t)n + 1, spec, ap);
  va_end(ap);
  put(o, big, (size_t)n);
  free(big);
}

int printf_trace_on(void) {
  static int on = -1;
  if (on < 0) on = getenv("DL_TRACE_FMT") != NULL;
  return on;
}

/* Formats the guest format string fmt with arguments from a into a host
 * string the caller frees. Returns its length. */
static size_t guest_format(const char *fmt, gargs *a, char **out) {
  obuf o = {0};
  put(&o, "", 0);
  for (const char *p = fmt; *p;) {
    if (*p != '%') {
      const char *q = strchr(p, '%');
      size_t n = q ? (size_t)(q - p) : strlen(p);
      put(&o, p, n);
      p += n;
      continue;
    }
    const char *start = p++;
    char flags[8];
    int nflags = 0;
    while (*p && strchr("-+ #0'", *p) && nflags < 7) flags[nflags++] = *p++;
    flags[nflags] = 0;
    int width = -1, prec = -1;
    if (*p == '*') {
      width = (int32_t)next32(a);
      if (width < 0) {
        if (nflags < 7) flags[nflags++] = '-', flags[nflags] = 0;
        width = -width;
      }
      p++;
    } else if (*p >= '0' && *p <= '9') {
      width = (int)strtol(p, (char **)&p, 10);
    }
    if (*p == '.') {
      p++;
      if (*p == '*') {
        prec = (int32_t)next32(a);
        if (prec < 0) prec = -1; /* a negative precision is taken as omitted */
        p++;
      } else {
        prec = (int)strtol(p, (char **)&p, 10);
      }
    }
    /* Length modifier: 'H' hh, 'h', 'q' 64-bit (ll, q, j), '-' 32-bit (l, z,
     * t), 'L' long double, which is double (and a plain int for bionic's
     * integer conversions). */
    char len = 0;
    if (p[0] == 'h' && p[1] == 'h') len = 'H', p += 2;
    else if (*p == 'h') len = 'h', p++;
    else if (p[0] == 'l' && p[1] == 'l') len = 'q', p += 2;
    else if (*p == 'q' || *p == 'j') len = 'q', p++;
    else if (*p == 'L') len = 'L', p++;
    else if (*p == 'l') len = 'l', p++;
    else if (*p == 'z' || *p == 't') len = '-', p++;
    char conv = *p ? *p++ : 0;

    /* The host spec: flags, width and precision, then our own length. */
    char spec[48];
    int sl = snprintf(spec, sizeof spec, "%%%s", flags);
    if (width >= 0) sl += snprintf(spec + sl, sizeof spec - (size_t)sl, "%d", width);
    if (prec >= 0) sl += snprintf(spec + sl, sizeof spec - (size_t)sl, ".%d", prec);

    switch (conv) {
    case 'd':
    case 'i':
    case 'u':
    case 'o':
    case 'x':
    case 'X': {
      int is_signed = conv == 'd' || conv == 'i';
      uint64_t v = len == 'q' ? next64(a) : next32(a);
      if (len != 'q') v = is_signed ? (uint64_t)(int64_t)(int32_t)v : (uint32_t)v;
      if (len == 'h') v = is_signed ? (uint64_t)(int64_t)(int16_t)v : (uint16_t)v;
      if (len == 'H') v = is_signed ? (uint64_t)(int64_t)(int8_t)v : (uint8_t)v;
      snprintf(spec + sl, sizeof spec - (size_t)sl, "ll%c", conv);
      putf(&o, spec, v);
      break;
    }
    case 'c': {
      uint32_t ch = next32(a);
      snprintf(spec + sl, sizeof spec - (size_t)sl, "c");
      putf(&o, spec, (int)(unsigned char)ch);
      break;
    }
    case 's': {
      uint32_t s = next32(a);
      if (len == 'l') {
        /* Guest wchar_t is 32-bit like the host's. */
        snprintf(spec + sl, sizeof spec - (size_t)sl, "ls");
        putf(&o, spec, s ? (const wchar_t *)g2h(s) : L"(null)");
      } else {
        snprintf(spec + sl, sizeof spec - (size_t)sl, "s");
        putf(&o, spec, s ? (const char *)g2h(s) : "(null)");
      }
      break;
    }
    case 'p': {
      /* bionic: "0x" and the hex digits, NULL included ("0x0"); precision
       * and the 0 flag work on the digits as for %x, width on the whole. */
      uint32_t v = next32(a);
      char digits[16], all[64] = "0x";
      int nd = snprintf(digits, sizeof digits, "%.*x", prec >= 0 ? prec : 1, v);
      int left = strchr(flags, '-') != NULL, zero = !left && prec < 0 && strchr(flags, '0');
      int nz = zero && width > nd + 2 ? width - nd - 2 : 0;
      if (nz > (int)sizeof all - 20) nz = (int)sizeof all - 20;
      memset(all + 2, '0', (size_t)nz);
      memcpy(all + 2 + nz, digits, (size_t)nd + 1);
      putf(&o, left ? "%-*s" : "%*s", width > 0 ? width : 0, all);
      break;
    }
    case 'f':
    case 'F':
    case 'e':
    case 'E':
    case 'g':
    case 'G':
    case 'a':
    case 'A': {
      uint64_t bits = next64(a);
      double d;
      memcpy(&d, &bits, sizeof d);
      snprintf(spec + sl, sizeof spec - (size_t)sl, "%c", conv);
      putf(&o, spec, d);
      break;
    }
    case 'n': {
      uint32_t ptr = next32(a);
      if (len == 'H') *(uint8_t *)g2h(ptr) = (uint8_t)o.len;
      else if (len == 'h') *(uint16_t *)g2h(ptr) = (uint16_t)o.len;
      else if (len == 'q') gm_w32(ptr, (uint32_t)o.len), gm_w32(ptr + 4, 0);
      /* l, z, t, L and none: a 32-bit int */
      else gm_w32(ptr, (uint32_t)o.len);
      break;
    }
    case '%':
      put(&o, "%", 1);
      break;
    default:
      fprintf(stderr, "[printf] unsupported conversion in \"%s\"\n", fmt);
      put(&o, start, (size_t)(p - start));
      break;
    }
  }
  *out = o.p;
  if (printf_trace_on()) fprintf(stderr, "[fmt] printf \"%s\" -> \"%s\"\n", fmt, o.p);
  return o.len;
}

/* Copies a formatted result into a guest buffer of size n, like snprintf. */
static void store(uint32_t buf, uint32_t n, const char *s, size_t len) {
  if (n == 0) return;
  size_t k = len < n - 1 ? len : n - 1;
  memcpy(g2h(buf), s, k);
  ((char *)g2h(buf))[k] = 0;
}

static void format_to_buffer(cpu *c, uint32_t buf, uint32_t n, uint32_t fmt, gargs a) {
  char *s;
  size_t len = guest_format(g2h(fmt), &a, &s);
  store(buf, n, s, len);
  free(s);
  cpu_ret(c, (uint32_t)len);
}

static void h_snprintf(cpu *c) {
  format_to_buffer(c, cpu_arg(c, 0), cpu_arg(c, 1), cpu_arg(c, 2), variadic_args(c, 3));
}

static void h_vsnprintf(cpu *c) {
  format_to_buffer(c, cpu_arg(c, 0), cpu_arg(c, 1), cpu_arg(c, 2), va_list_args(c, cpu_arg(c, 3)));
}

static void h_sprintf(cpu *c) {
  format_to_buffer(c, cpu_arg(c, 0), UINT32_MAX, cpu_arg(c, 1), variadic_args(c, 2));
}

static void h_vsprintf(cpu *c) {
  format_to_buffer(c, cpu_arg(c, 0), UINT32_MAX, cpu_arg(c, 1), va_list_args(c, cpu_arg(c, 2)));
}

/* --- to FILE streams --- */

static void format_to_stream(cpu *c, uint32_t file, uint32_t fmt, gargs a) {
  char *s;
  size_t len = guest_format(g2h(fmt), &a, &s);
  cpu_ret(c, (uint32_t)stdio_write(file, s, len));
  free(s);
}

static void h_printf(cpu *c) { format_to_stream(c, stdio_stream(1), cpu_arg(c, 0), variadic_args(c, 1)); }

static void h_fprintf(cpu *c) { format_to_stream(c, cpu_arg(c, 0), cpu_arg(c, 1), variadic_args(c, 2)); }

static void h_vfprintf(cpu *c) {
  format_to_stream(c, cpu_arg(c, 0), cpu_arg(c, 1), va_list_args(c, cpu_arg(c, 2)));
}

const dynlib_impl reimpl_printf[] = {
    {"fprintf", h_fprintf},   {"printf", h_printf},     {"snprintf", h_snprintf}, {"sprintf", h_sprintf},
    {"vfprintf", h_vfprintf}, {"vsnprintf", h_vsnprintf}, {"vsprintf", h_vsprintf}, {NULL, NULL},
};
