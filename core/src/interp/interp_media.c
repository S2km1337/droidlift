/*
 * Operations shared by the ARM and Thumb-2 decoders: the ARMv6 media
 * instructions (parallel add/subtract, saturation, extends, the DSP
 * multiplies) and divides.
 */
#include "interp.h"

static int32_t sat_s(icpu *s, int64_t v, int bits) {
  int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -((int64_t)1 << (bits - 1));
  if (v > hi) {
    s->q = 1;
    return (int32_t)hi;
  }
  if (v < lo) {
    s->q = 1;
    return (int32_t)lo;
  }
  return (int32_t)v;
}

static uint32_t sat_u(icpu *s, int64_t v, int bits) {
  int64_t hi = ((int64_t)1 << bits) - 1;
  if (v > hi) {
    s->q = 1;
    return (uint32_t)hi;
  }
  if (v < 0) {
    s->q = 1;
    return 0;
  }
  return (uint32_t)v;
}

/* Saturation without touching Q (the Q/UQ parallel forms don't set it). */
static int32_t clamp_s(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

uint32_t interp_parallel(icpu *s, int kind, int op, uint32_t a, uint32_t b) {
  int sgn = kind < 4;
  int k = kind & 3; /* 0 modular, 1 saturating, 2 halving */
  uint32_t res = 0, ge = 0;
  if (op < 4) { /* halfwords */
    int32_t x[2], y[2], r[2];
    int add[2];
    for (int i = 0; i < 2; i++) {
      x[i] = sgn ? (int16_t)(a >> (16 * i)) : (int32_t)((a >> (16 * i)) & 0xFFFF);
      y[i] = sgn ? (int16_t)(b >> (16 * i)) : (int32_t)((b >> (16 * i)) & 0xFFFF);
    }
    switch (op) {
    case 0: r[0] = x[0] + y[0]; r[1] = x[1] + y[1]; add[0] = add[1] = 1; break;
    case 1: r[0] = x[0] - y[1]; r[1] = x[1] + y[0]; add[0] = 0; add[1] = 1; break;
    case 2: r[0] = x[0] + y[1]; r[1] = x[1] - y[0]; add[0] = 1; add[1] = 0; break;
    default: r[0] = x[0] - y[0]; r[1] = x[1] - y[1]; add[0] = add[1] = 0; break;
    }
    for (int i = 0; i < 2; i++) {
      int32_t v = r[i];
      if (k == 0) {
        int g = sgn ? v >= 0 : (add[i] ? v >= 0x10000 : v >= 0);
        if (g) ge |= 3u << (2 * i);
      } else if (k == 1) {
        v = sgn ? clamp_s(v, -32768, 32767) : clamp_s(v, 0, 65535);
      } else {
        v >>= 1;
      }
      res |= ((uint32_t)v & 0xFFFF) << (16 * i);
    }
  } else { /* bytes: ADD8 (4) or SUB8 (7) */
    for (int i = 0; i < 4; i++) {
      int32_t x = sgn ? (int8_t)(a >> (8 * i)) : (int32_t)((a >> (8 * i)) & 0xFF);
      int32_t y = sgn ? (int8_t)(b >> (8 * i)) : (int32_t)((b >> (8 * i)) & 0xFF);
      int32_t v = op == 4 ? x + y : x - y;
      if (k == 0) {
        int g = sgn ? v >= 0 : (op == 4 ? v >= 0x100 : v >= 0);
        if (g) ge |= 1u << i;
      } else if (k == 1) {
        v = sgn ? clamp_s(v, -128, 127) : clamp_s(v, 0, 255);
      } else {
        v >>= 1;
      }
      res |= ((uint32_t)v & 0xFF) << (8 * i);
    }
  }
  if (k == 0) s->ge = ge;
  return res;
}

uint32_t interp_ssat(icpu *s, int32_t v, int bits) { return (uint32_t)sat_s(s, v, bits); }
uint32_t interp_usat(icpu *s, int32_t v, int bits) { return sat_u(s, v, bits); }

uint32_t interp_ssat16(icpu *s, uint32_t v, int bits) {
  uint32_t lo = (uint32_t)sat_s(s, (int16_t)v, bits) & 0xFFFF;
  uint32_t hi = (uint32_t)sat_s(s, (int16_t)(v >> 16), bits) & 0xFFFF;
  return lo | hi << 16;
}

uint32_t interp_usat16(icpu *s, uint32_t v, int bits) {
  uint32_t lo = sat_u(s, (int16_t)v, bits) & 0xFFFF;
  uint32_t hi = sat_u(s, (int16_t)(v >> 16), bits) & 0xFFFF;
  return lo | hi << 16;
}

uint32_t interp_qadd(icpu *s, uint32_t a, uint32_t b, int sub, int dbl) {
  int64_t bb = (int32_t)b;
  if (dbl) bb = sat_s(s, 2 * bb, 32);
  return (uint32_t)sat_s(s, sub ? (int64_t)(int32_t)a - bb : (int64_t)(int32_t)a + bb, 32);
}

uint32_t interp_sel(icpu *s, uint32_t a, uint32_t b) {
  uint32_t r = 0;
  for (int i = 0; i < 4; i++) r |= (((s->ge >> i) & 1) ? a : b) & (0xFFu << (8 * i));
  return r;
}

uint32_t interp_usad8(uint32_t a, uint32_t b) {
  uint32_t r = 0;
  for (int i = 0; i < 4; i++) {
    int x = (int)((a >> (8 * i)) & 0xFF), y = (int)((b >> (8 * i)) & 0xFF);
    r += (uint32_t)(x > y ? x - y : y - x);
  }
  return r;
}

uint32_t interp_rbit(uint32_t v) {
  v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
  v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
  v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
  return __builtin_bswap32(v);
}

uint32_t interp_clz(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }

uint32_t interp_extend(int op, uint32_t add, uint32_t v) {
  switch (op) {
  case 0: { /* SXTAB16 */
    uint32_t lo = (add + (uint32_t)(int32_t)(int8_t)v) & 0xFFFF;
    uint32_t hi = ((add >> 16) + (uint32_t)(int32_t)(int8_t)(v >> 16)) & 0xFFFF;
    return lo | hi << 16;
  }
  case 2: return add + (uint32_t)(int32_t)(int8_t)v;
  case 3: return add + (uint32_t)(int32_t)(int16_t)v;
  case 4: { /* UXTAB16 */
    uint32_t lo = (add + (v & 0xFF)) & 0xFFFF;
    uint32_t hi = ((add >> 16) + ((v >> 16) & 0xFF)) & 0xFFFF;
    return lo | hi << 16;
  }
  case 6: return add + (v & 0xFF);
  default: return add + (v & 0xFFFF);
  }
}

static int32_t half(uint32_t v, int high) { return high ? (int32_t)v >> 16 : (int16_t)v; }

uint32_t interp_smla(icpu *s, int nhigh, int mhigh, uint32_t n, uint32_t m, uint32_t a, int acc) {
  int64_t p = (int64_t)half(n, nhigh) * half(m, mhigh);
  if (!acc) return (uint32_t)p;
  int64_t r = p + (int32_t)a;
  if (r != (int32_t)r) s->q = 1;
  return (uint32_t)r;
}

uint32_t interp_smlaw(icpu *s, int mhigh, uint32_t n, uint32_t m, uint32_t a, int acc) {
  int64_t p = ((int64_t)(int32_t)n * half(m, mhigh)) >> 16;
  if (!acc) return (uint32_t)p;
  int64_t r = p + (int32_t)a;
  if (r != (int32_t)r) s->q = 1;
  return (uint32_t)r;
}

uint32_t interp_smlad(icpu *s, int swap, int sub, uint32_t n, uint32_t m, uint32_t a, int acc) {
  if (swap) m = (m >> 16) | (m << 16);
  int64_t p1 = (int64_t)half(n, 0) * half(m, 0), p2 = (int64_t)half(n, 1) * half(m, 1);
  int64_t r = sub ? p1 - p2 : p1 + p2;
  if (acc) r += (int32_t)a;
  if (r != (int32_t)r) s->q = 1;
  return (uint32_t)r;
}

uint64_t interp_smlald(int swap, int sub, uint32_t n, uint32_t m, uint64_t a) {
  if (swap) m = (m >> 16) | (m << 16);
  int64_t p1 = (int64_t)half(n, 0) * half(m, 0), p2 = (int64_t)half(n, 1) * half(m, 1);
  return a + (uint64_t)(sub ? p1 - p2 : p1 + p2);
}

uint32_t interp_smmla(int round, int op, uint32_t n, uint32_t m, uint32_t a, int acc) {
  uint64_t p = (uint64_t)((int64_t)(int32_t)n * (int32_t)m);
  uint64_t r = acc ? (uint64_t)a << 32 : 0;
  r = op ? r - p : r + p;
  if (round) r += 0x80000000u;
  return (uint32_t)(r >> 32);
}

uint32_t interp_sdiv(uint32_t n, uint32_t m) {
  if (m == 0) return 0;
  if (n == 0x80000000u && m == 0xFFFFFFFFu) return n;
  return (uint32_t)((int32_t)n / (int32_t)m);
}

uint32_t interp_udiv(uint32_t n, uint32_t m) { return m ? n / m : 0; }
