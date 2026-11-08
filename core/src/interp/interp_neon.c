/*
 * Advanced SIMD (NEON), ARM encodings (Thumb-2 converts to them), decoded
 * as in the ARMv7-A Architecture Reference Manual, chapter A7.
 *
 * Vectors are 8 or 16 bytes of the extension register file; lanes are
 * little-endian, so lane i of size e is bytes [i*e, i*e+e). Integer
 * operations are portable C; saturation sets FPSCR.QC. Float arithmetic
 * runs on the host FPU under the "standard FPSCR value" (flush-to-zero,
 * default NaN, round to nearest, see interp_vfp.c), and the estimate and
 * step instructions use the AArch64 ones, which are defined identically.
 */
#include "interp.h"

#pragma STDC FENV_ACCESS ON
#pragma clang fp contract(off)

#define BIT(x, n) (((x) >> (n)) & 1u)
#define BITS(x, hi, lo) (((x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))
#define FPSCR_QC (1u << 27)

typedef __int128 i128;

/* ---- vectors and lanes ---- */

typedef struct vec {
  uint8_t b[32];
} vec;

static void vget(icpu *s, int reg, int nregs, vec *v) {
  for (int i = 0; i < nregs; i++) memcpy(v->b + 8 * i, &s->x.d[(reg + i) & 31], 8);
}
static void vput(icpu *s, int reg, int nregs, const vec *v) {
  for (int i = 0; i < nregs; i++) memcpy(&s->x.d[(reg + i) & 31], v->b + 8 * i, 8);
}
static uint64_t lu(const vec *v, int eb, int i) {
  uint64_t x = 0;
  memcpy(&x, v->b + eb * i, (size_t)eb);
  return x;
}
static int64_t ls(const vec *v, int eb, int i) {
  uint64_t x = lu(v, eb, i);
  int sh = 64 - 8 * eb;
  return (int64_t)(x << sh) >> sh;
}
static void lset(vec *v, int eb, int i, uint64_t x) { memcpy(v->b + eb * i, &x, (size_t)eb); }
static float lf(const vec *v, int i) {
  float f;
  memcpy(&f, v->b + 4 * i, 4);
  return f;
}
static void lsetf(vec *v, int i, float f) { memcpy(v->b + 4 * i, &f, 4); }

static uint64_t emask(int eb) { return eb == 8 ? ~0ull : (1ull << (8 * eb)) - 1; }

static int64_t sat_s(icpu *s, i128 v, int bits) {
  i128 hi = ((i128)1 << (bits - 1)) - 1, lo = -((i128)1 << (bits - 1));
  if (v > hi) { s->fpscr |= FPSCR_QC; return (int64_t)hi; }
  if (v < lo) { s->fpscr |= FPSCR_QC; return (int64_t)lo; }
  return (int64_t)v;
}
static uint64_t sat_u(icpu *s, i128 v, int bits) {
  i128 hi = ((i128)1 << bits) - 1;
  if (v > hi) { s->fpscr |= FPSCR_QC; return (uint64_t)hi; }
  if (v < 0) { s->fpscr |= FPSCR_QC; return 0; }
  return (uint64_t)v;
}

/* Shift by a signed amount (VSHL/VRSHL/VQSHL/VQRSHL by register). */
static i128 shift_round(i128 x, int sh, int round) {
  if (sh >= 0) {
    if (sh < 64) return x << sh;
    /* Beyond any lane: zero once truncated, saturating if nonzero. */
    return x == 0 ? 0 : x < 0 ? -((i128)1 << 100) : ((i128)1 << 100);
  }
  int r = -sh > 120 ? 120 : -sh;
  i128 add = round ? ((i128)1 << (r - 1)) : 0;
  return (x + add) >> r;
}

/* ---- float helpers (AArch64 instructions with ARMv7 NEON semantics) ---- */

#define ASM2(op, a, b, r) __asm__ volatile(op " %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b))
static float f_max(float a, float b) { float r; ASM2("fmax", a, b, r); return r; }
static float f_min(float a, float b) { float r; ASM2("fmin", a, b, r); return r; }
static float f_recps(float a, float b) { float r; ASM2("frecps", a, b, r); return r; }
static float f_rsqrts(float a, float b) { float r; ASM2("frsqrts", a, b, r); return r; }
static float f_recpe(float a) { float r; __asm__ volatile("frecpe %s0, %s1" : "=w"(r) : "w"(a)); return r; }
static float f_rsqrte(float a) { float r; __asm__ volatile("frsqrte %s0, %s1" : "=w"(r) : "w"(a)); return r; }
static float f_fma(float a, float b, float c) {
  float r;
  __asm__ volatile("fmadd %s0, %s1, %s2, %s3" : "=w"(r) : "w"(a), "w"(b), "w"(c));
  return r;
}
static uint32_t u_recpe(uint32_t a) {
  uint32_t r;
  __asm__ volatile("fmov s0, %w1\n\turecpe v0.2s, v0.2s\n\tfmov %w0, s0" : "=r"(r) : "r"(a) : "v0");
  return r;
}
static uint32_t u_rsqrte(uint32_t a) {
  uint32_t r;
  __asm__ volatile("fmov s0, %w1\n\tursqrte v0.2s, v0.2s\n\tfmov %w0, s0" : "=r"(r) : "r"(a) : "v0");
  return r;
}
static uint32_t f_cvt_s(float v, int uns) {
  uint32_t r;
  if (uns) __asm__ volatile("fcvtzu %w0, %s1" : "=r"(r) : "w"(v));
  else __asm__ volatile("fcvtzs %w0, %s1" : "=r"(r) : "w"(v));
  return r;
}
static float f_abs(float a) {
  uint32_t u;
  memcpy(&u, &a, 4);
  u &= 0x7FFFFFFFu;
  memcpy(&a, &u, 4);
  return a;
}
static float f_neg(float a) {
  uint32_t u;
  memcpy(&u, &a, 4);
  u ^= 0x80000000u;
  memcpy(&a, &u, 4);
  return a;
}

/* ---- three registers of the same length (A7.4.1) ---- */

static void three_same(icpu *s, uint32_t insn, int d, int n, int m, int Q) {
  int U = (int)BIT(insn, 24), A = (int)BITS(insn, 11, 8), B = (int)BIT(insn, 4);
  int size = (int)BITS(insn, 21, 20), eb = 1 << size, regs = Q ? 2 : 1;
  int lanes = 8 * regs / eb;
  vec vn, vm, vd, r;
  vget(s, n, regs, &vn);
  vget(s, m, regs, &vm);
  vget(s, d, regs, &vd);
  r = vd;
  int bits = 8 * eb;
  uint64_t mask = emask(eb);

  if (A == 1 && B) { /* logical */
    for (int i = 0; i < 8 * regs; i++) {
      uint8_t a = vn.b[i], b = vm.b[i], c = vd.b[i];
      if (!U) {
        switch (size) {
        case 0: r.b[i] = a & b; break;
        case 1: r.b[i] = a & ~b; break;
        case 2: r.b[i] = a | b; break;
        default: r.b[i] = a | ~b; break;
        }
      } else {
        switch (size) {
        case 0: r.b[i] = a ^ b; break;
        case 1: r.b[i] = (uint8_t)((a & c) | (b & ~c)); break; /* VBSL */
        case 2: r.b[i] = (uint8_t)((a & b) | (c & ~b)); break; /* VBIT */
        default: r.b[i] = (uint8_t)((a & ~b) | (c & b)); break; /* VBIF */
        }
      }
    }
    vput(s, d, regs, &r);
    return;
  }

  if (A >= 0xD || (A == 0xC && B && !U)) { /* float */
    if (size & 1) goto undef;
    int op = (int)BIT(insn, 21);
    lanes = 2 * regs;
    interp_fp_neon_begin(s);
    if (A == 0xD && !B && !U) { /* VADD, VSUB */
      for (int i = 0; i < lanes; i++) lsetf(&r, i, op ? lf(&vn, i) - lf(&vm, i) : lf(&vn, i) + lf(&vm, i));
    } else if (A == 0xD && !B && U) {
      if (!op) { /* VPADD */
        vec t;
        for (int i = 0; i < lanes / 2; i++) {
          lsetf(&t, i, lf(&vn, 2 * i) + lf(&vn, 2 * i + 1));
          lsetf(&t, i + lanes / 2, lf(&vm, 2 * i) + lf(&vm, 2 * i + 1));
        }
        r = t;
      } else { /* VABD */
        for (int i = 0; i < lanes; i++) lsetf(&r, i, f_abs(lf(&vn, i) - lf(&vm, i)));
      }
    } else if (A == 0xD && B && !U) { /* VMLA, VMLS */
      for (int i = 0; i < lanes; i++) {
        float p = lf(&vn, i) * lf(&vm, i);
        lsetf(&r, i, lf(&vd, i) + (op ? f_neg(p) : p));
      }
    } else if (A == 0xD && B && U && !op) { /* VMUL */
      for (int i = 0; i < lanes; i++) lsetf(&r, i, lf(&vn, i) * lf(&vm, i));
    } else if (A == 0xE && !B) { /* VCEQ, VCGE, VCGT */
      if (!U && op) goto fundef;
      for (int i = 0; i < lanes; i++) {
        float a = lf(&vn, i), b = lf(&vm, i);
        int t = !U ? a == b : op ? a > b : a >= b;
        lset(&r, 4, i, t ? 0xFFFFFFFFu : 0);
      }
    } else if (A == 0xE && B && U) { /* VACGE, VACGT */
      for (int i = 0; i < lanes; i++) {
        float a = f_abs(lf(&vn, i)), b = f_abs(lf(&vm, i));
        lset(&r, 4, i, (op ? a > b : a >= b) ? 0xFFFFFFFFu : 0);
      }
    } else if (A == 0xF && !B) { /* VMAX, VMIN, VPMAX, VPMIN */
      if (!U) {
        for (int i = 0; i < lanes; i++) lsetf(&r, i, op ? f_min(lf(&vn, i), lf(&vm, i)) : f_max(lf(&vn, i), lf(&vm, i)));
      } else {
        vec t;
        for (int i = 0; i < lanes / 2; i++) {
          float a = lf(&vn, 2 * i), b = lf(&vn, 2 * i + 1), c = lf(&vm, 2 * i), e = lf(&vm, 2 * i + 1);
          lsetf(&t, i, op ? f_min(a, b) : f_max(a, b));
          lsetf(&t, i + lanes / 2, op ? f_min(c, e) : f_max(c, e));
        }
        r = t;
      }
    } else if (A == 0xF && B && !U) { /* VRECPS, VRSQRTS */
      for (int i = 0; i < lanes; i++)
        lsetf(&r, i, op ? f_rsqrts(lf(&vn, i), lf(&vm, i)) : f_recps(lf(&vn, i), lf(&vm, i)));
    } else if (A == 0xC && B && !U) { /* VFMA, VFMS */
      for (int i = 0; i < lanes; i++)
        lsetf(&r, i, f_fma(op ? f_neg(lf(&vn, i)) : lf(&vn, i), lf(&vm, i), lf(&vd, i)));
    } else {
    fundef:
      interp_fp_neon_end(s);
      goto undef;
    }
    interp_fp_neon_end(s);
    vput(s, d, regs, &r);
    return;
  }

  if (A == 0xA || (A == 0xB && B)) { /* pairwise integer: VPMAX, VPMIN, VPADD */
    if (Q || (A == 0xB && U) || size == 3) goto undef;
    vec t;
    int h = lanes / 2;
    for (int i = 0; i < h; i++) {
      for (int k = 0; k < 2; k++) {
        const vec *src = k ? &vm : &vn;
        uint64_t x;
        if (A == 0xB) {
          x = lu(src, eb, 2 * i) + lu(src, eb, 2 * i + 1);
        } else if (U) {
          uint64_t a = lu(src, eb, 2 * i), b = lu(src, eb, 2 * i + 1);
          x = B ? (a < b ? a : b) : (a > b ? a : b);
        } else {
          int64_t a = ls(src, eb, 2 * i), b = ls(src, eb, 2 * i + 1);
          x = (uint64_t)(B ? (a < b ? a : b) : (a > b ? a : b));
        }
        lset(&t, eb, i + k * h, x);
      }
    }
    vput(s, d, regs, &t);
    return;
  }

  for (int i = 0; i < lanes; i++) {
    i128 a = U ? (i128)lu(&vn, eb, i) : (i128)ls(&vn, eb, i);
    i128 b = U ? (i128)lu(&vm, eb, i) : (i128)ls(&vm, eb, i);
    uint64_t x;
    switch (A << 1 | B) {
    case 0: x = (uint64_t)((a + b) >> 1); if (size == 3) goto undef; break;     /* VHADD */
    case 1: x = U ? sat_u(s, a + b, bits) : (uint64_t)sat_s(s, a + b, bits); break; /* VQADD */
    case 2: x = (uint64_t)((a + b + 1) >> 1); if (size == 3) goto undef; break; /* VRHADD */
    case 4: x = (uint64_t)((a - b) >> 1); if (size == 3) goto undef; break;     /* VHSUB */
    case 5: x = U ? sat_u(s, a - b, bits) : (uint64_t)sat_s(s, a - b, bits); break; /* VQSUB */
    case 6: if (size == 3) goto undef; x = a > b ? mask : 0; break;            /* VCGT */
    case 7: if (size == 3) goto undef; x = a >= b ? mask : 0; break;           /* VCGE */
    case 8: case 9: case 10: case 11: { /* VSHL, VQSHL, VRSHL, VQRSHL (register): Vm by Vn */
      int sh = (int8_t)lu(&vn, eb, i);
      i128 v = shift_round(b, sh, A == 5);
      if (B) x = U ? sat_u(s, v, bits) : (uint64_t)sat_s(s, v, bits);
      else x = (uint64_t)v;
      break;
    }
    case 12: case 13: /* VMAX, VMIN */
      if (size == 3) goto undef;
      x = (uint64_t)(B ? (a < b ? a : b) : (a > b ? a : b));
      break;
    case 14: case 15: { /* VABD, VABA */
      if (size == 3) goto undef;
      i128 ad = a > b ? a - b : b - a;
      x = (uint64_t)ad;
      if (B) x += lu(&vd, eb, i);
      break;
    }
    case 16: x = U ? lu(&vn, eb, i) - lu(&vm, eb, i) : lu(&vn, eb, i) + lu(&vm, eb, i); break; /* VADD/VSUB */
    case 17: /* VTST, VCEQ */
      if (size == 3) goto undef;
      x = U ? (lu(&vn, eb, i) == lu(&vm, eb, i) ? mask : 0) : ((lu(&vn, eb, i) & lu(&vm, eb, i)) ? mask : 0);
      break;
    case 18: { /* VMLA, VMLS */
      if (size == 3) goto undef;
      uint64_t p = lu(&vn, eb, i) * lu(&vm, eb, i);
      x = U ? lu(&vd, eb, i) - p : lu(&vd, eb, i) + p;
      break;
    }
    case 19: /* VMUL, VMUL.P8 */
      if (size == 3 || (U && size)) goto undef;
      if (U) {
        uint32_t pa = (uint32_t)lu(&vn, 1, i), pb = (uint32_t)lu(&vm, 1, i), pr = 0;
        for (int k = 0; k < 8; k++)
          if (pb & (1u << k)) pr ^= pa << k;
        x = pr;
      } else {
        x = lu(&vn, eb, i) * lu(&vm, eb, i);
      }
      break;
    case 22: { /* VQDMULH, VQRDMULH */
      if (size == 0 || size == 3) goto undef;
      i128 sa = ls(&vn, eb, i), sb = ls(&vm, eb, i);
      i128 p = 2 * sa * sb + (U ? ((i128)1 << (bits - 1)) : 0);
      x = (uint64_t)sat_s(s, p >> bits, bits);
      break;
    }
    default: goto undef;
    }
    lset(&r, eb, i, x);
  }
  vput(s, d, regs, &r);
  return;
undef:
  interp_undefined(s, insn, "NEON three registers, same length");
}

/* ---- three registers of different lengths (A7.4.2) ---- */

static uint64_t pmull8(uint64_t a, uint64_t b) {
  uint64_t r = 0;
  for (int k = 0; k < 8; k++)
    if (b & (1u << k)) r ^= a << k;
  return r & 0xFFFF;
}

static void three_diff(icpu *s, uint32_t insn, int d, int n, int m) {
  int U = (int)BIT(insn, 24), A = (int)BITS(insn, 11, 8), size = (int)BITS(insn, 21, 20);
  int eb = 1 << size, lanes = 8 / eb, bits = 8 * eb;
  vec vn, vm, vd, r;
  int wide_n = A == 1 || A == 3; /* VADDW, VSUBW */
  int narrow = A == 4 || A == 6;
  if ((d & 1) && !narrow) goto undef;
  if (wide_n && (n & 1)) goto undef;
  if (narrow && ((n & 1) || (m & 1))) goto undef;
  vget(s, n, (wide_n || narrow) ? 2 : 1, &vn);
  vget(s, m, narrow ? 2 : 1, &vm);
  vget(s, d, narrow ? 1 : 2, &vd);
  memset(&r, 0, sizeof r);
  if (narrow) { /* VADDHN, VRADDHN, VSUBHN, VRSUBHN */
    for (int i = 0; i < lanes; i++) {
      uint64_t a = lu(&vn, 2 * eb, i), b = lu(&vm, 2 * eb, i);
      uint64_t x = A == 4 ? a + b : a - b;
      if (U) x += 1ull << (bits - 1);
      lset(&r, eb, i, x >> bits);
    }
    vput(s, d, 1, &r);
    return;
  }
  for (int i = 0; i < lanes; i++) {
    i128 a = wide_n ? (U ? (i128)lu(&vn, 2 * eb, i) : (i128)ls(&vn, 2 * eb, i))
                    : (U ? (i128)lu(&vn, eb, i) : (i128)ls(&vn, eb, i));
    i128 b = U ? (i128)lu(&vm, eb, i) : (i128)ls(&vm, eb, i);
    i128 acc = U ? (i128)lu(&vd, 2 * eb, i) : (i128)ls(&vd, 2 * eb, i);
    uint64_t x;
    switch (A) {
    case 0: case 1: x = (uint64_t)(a + b); break;
    case 2: case 3: x = (uint64_t)(a - b); break;
    case 5: x = (uint64_t)(acc + (a > b ? a - b : b - a)); break; /* VABAL */
    case 7: x = (uint64_t)(a > b ? a - b : b - a); break;       /* VABDL */
    case 8: x = (uint64_t)(acc + a * b); break;                 /* VMLAL */
    case 10: x = (uint64_t)(acc - a * b); break;                /* VMLSL */
    case 12: x = (uint64_t)(a * b); break;                      /* VMULL */
    case 9: case 11: case 13: { /* VQDMLAL, VQDMLSL, VQDMULL */
      if (U || size == 0) goto undef;
      i128 p = sat_s(s, 2 * a * b, 2 * bits);
      if (A == 13) x = (uint64_t)p;
      else x = (uint64_t)sat_s(s, A == 9 ? (i128)ls(&vd, 2 * eb, i) + p : (i128)ls(&vd, 2 * eb, i) - p, 2 * bits);
      break;
    }
    case 14: /* VMULL.P8 */
      if (U || size) goto undef;
      x = pmull8(lu(&vn, 1, i), lu(&vm, 1, i));
      break;
    default: goto undef;
    }
    lset(&r, 2 * eb, i, x);
  }
  vput(s, d, 2, &r);
  return;
undef:
  interp_undefined(s, insn, "NEON three registers, different lengths");
}

/* ---- two registers and a scalar (A7.4.3) ---- */

static void two_scalar(icpu *s, uint32_t insn, int d, int n) {
  int U = (int)BIT(insn, 24), A = (int)BITS(insn, 11, 8), size = (int)BITS(insn, 21, 20);
  int eb = 1 << size, bits = 8 * eb;
  if (size == 0 || size == 3) goto undef;
  uint32_t Vm = BITS(insn, 3, 0), M = BIT(insn, 5);
  int m, idx;
  if (size == 1) {
    m = (int)(Vm & 7);
    idx = (int)(M << 1 | (Vm >> 3));
  } else {
    m = (int)Vm;
    idx = (int)M;
  }
  vec vs;
  vget(s, m, 1, &vs);
  int long_op = (A & 3) == 2 || A == 3 || A == 7 || A == 11;
  int Q = long_op ? 0 : U; /* the same-length forms put Q in the U bit */
  int regs = Q ? 2 : 1;
  vec vn, vd, r;
  if (Q && ((d & 1) || (n & 1))) goto undef;
  if (long_op && (d & 1)) goto undef;
  vget(s, n, regs, &vn);
  vget(s, d, long_op ? 2 : regs, &vd);
  r = vd;
  int lanes = 8 * regs / eb;
  if (A == 0 || A == 1 || A == 4 || A == 5 || A == 8 || A == 9) {
    int F = (int)(A & 1);
    if (F) {
      if (size != 2) goto undef;
      float sc = lf(&vs, idx);
      interp_fp_neon_begin(s);
      for (int i = 0; i < lanes; i++) {
        float p = lf(&vn, i) * sc;
        if (A == 9) lsetf(&r, i, p);
        else lsetf(&r, i, lf(&vd, i) + (A == 5 ? f_neg(p) : p));
      }
      interp_fp_neon_end(s);
    } else {
      uint64_t sc = lu(&vs, eb, idx);
      for (int i = 0; i < lanes; i++) {
        uint64_t p = lu(&vn, eb, i) * sc;
        lset(&r, eb, i, A == 8 ? p : A == 4 ? lu(&vd, eb, i) - p : lu(&vd, eb, i) + p);
      }
    }
    vput(s, d, regs, &r);
    return;
  }
  if (A == 12 || A == 13) { /* VQDMULH, VQRDMULH */
    i128 sc = ls(&vs, eb, idx);
    for (int i = 0; i < lanes; i++) {
      i128 p = 2 * (i128)ls(&vn, eb, i) * sc + (A == 13 ? ((i128)1 << (bits - 1)) : 0);
      lset(&r, eb, i, (uint64_t)sat_s(s, p >> bits, bits));
    }
    vput(s, d, regs, &r);
    return;
  }
  /* long: VMLAL (2), VMLSL (6), VQDMLAL (3), VQDMLSL (7), VMULL (10), VQDMULL (11) */
  if (A >= 14) goto undef;
  {
    int sU = (A == 2 || A == 6 || A == 10) ? U : 0;
    if ((A == 3 || A == 7 || A == 11) && U) goto undef;
    i128 sc = sU ? (i128)lu(&vs, eb, idx) : (i128)ls(&vs, eb, idx);
    for (int i = 0; i < 8 / eb; i++) {
      i128 a = sU ? (i128)lu(&vn, eb, i) : (i128)ls(&vn, eb, i);
      i128 acc = sU ? (i128)lu(&vd, 2 * eb, i) : (i128)ls(&vd, 2 * eb, i);
      uint64_t x;
      switch (A) {
      case 2: x = (uint64_t)(acc + a * sc); break;
      case 6: x = (uint64_t)(acc - a * sc); break;
      case 10: x = (uint64_t)(a * sc); break;
      default: {
        i128 p = sat_s(s, 2 * a * sc, 2 * bits);
        if (A == 11) x = (uint64_t)p;
        else x = (uint64_t)sat_s(s, A == 3 ? acc + p : acc - p, 2 * bits);
        break;
      }
      }
      lset(&r, 2 * eb, i, x);
    }
    vput(s, d, 2, &r);
    return;
  }
undef:
  interp_undefined(s, insn, "NEON two registers and a scalar");
}

/* ---- two registers and a shift amount (A7.4.4) ---- */

static void two_shift(icpu *s, uint32_t insn, int d, int m, int Q) {
  int U = (int)BIT(insn, 24), A = (int)BITS(insn, 11, 8), L = (int)BIT(insn, 7), B = (int)BIT(insn, 6);
  uint32_t imm6 = BITS(insn, 21, 16);
  int eb, esize;
  if (L) eb = 8;
  else if (imm6 & 0x20) eb = 4;
  else if (imm6 & 0x10) eb = 2;
  else eb = 1;
  esize = 8 * eb;
  int rshift = L ? 64 - (int)imm6 : 2 * esize - (int)imm6; /* right shifts */
  int lshift = L ? (int)imm6 : (int)imm6 - esize;           /* left shifts */
  uint64_t mask = emask(eb);
  int regs = Q ? 2 : 1;
  vec vm, vd, r;

  if (A >= 8 && A <= 10) { /* narrowing and lengthening */
    if (L) goto undef;
    if (A == 10) { /* VSHLL, VMOVL */
      if (d & 1) goto undef;
      vget(s, m, 1, &vm);
      for (int i = 0; i < 8 / eb; i++) {
        uint64_t v = U ? lu(&vm, eb, i) : (uint64_t)ls(&vm, eb, i);
        lset(&r, 2 * eb, i, v << lshift);
      }
      vput(s, d, 2, &r);
      return;
    }
    /* VSHRN, VRSHRN, VQSHRN, VQRSHRN, VQSHRUN, VQRSHRUN: the source has
     * elements of twice the size, so the shift is relative to that. */
    if (m & 1) goto undef;
    vget(s, m, 2, &vm);
    int rs = rshift; /* esize is the destination's */
    for (int i = 0; i < 8 / eb; i++) {
      i128 v;
      if (A == 8 && !U) v = (i128)lu(&vm, 2 * eb, i); /* VSHRN: sign doesn't matter */
      else if (A == 8) v = ls(&vm, 2 * eb, i);         /* VQSHRUN: signed source */
      else v = U ? (i128)lu(&vm, 2 * eb, i) : (i128)ls(&vm, 2 * eb, i);
      v = shift_round(v, -rs, B);
      uint64_t x;
      if (A == 8 && !U) x = (uint64_t)v;                          /* VSHRN, VRSHRN */
      else if (A == 8) x = sat_u(s, v, esize);                    /* VQSHRUN, VQRSHRUN */
      else x = U ? sat_u(s, v, esize) : (uint64_t)sat_s(s, v, esize); /* VQSHRN, VQRSHRN */
      lset(&r, eb, i, x);
    }
    vput(s, d, 1, &r);
    return;
  }
  if (Q && ((d & 1) || (m & 1))) goto undef;
  vget(s, m, regs, &vm);
  vget(s, d, regs, &vd);
  r = vd;
  int lanes = 8 * regs / eb;
  if (A >= 14) { /* VCVT fixed <-> float */
    if (L || !(imm6 & 0x20)) goto undef;
    int frac = 64 - (int)imm6, to_fixed = (int)BIT(insn, 8);
    interp_fp_neon_begin(s);
    for (int i = 0; i < 2 * regs; i++) {
      if (to_fixed) {
        double v = (double)lf(&vm, i);
        uint32_t x;
        double sv = __builtin_fabs(v) < 0x1p33 ? v * (double)(1ull << frac) : v;
        if (U) __asm__ volatile("fcvtzu %w0, %d1" : "=r"(x) : "w"(sv));
        else __asm__ volatile("fcvtzs %w0, %d1" : "=r"(x) : "w"(sv));
        lset(&r, 4, i, x);
      } else {
        uint32_t x = (uint32_t)lu(&vm, 4, i);
        double v = U ? (double)x : (double)(int32_t)x;
        lsetf(&r, i, (float)(v / (double)(1ull << frac)));
      }
    }
    interp_fp_neon_end(s);
    vput(s, d, regs, &r);
    return;
  }
  for (int i = 0; i < lanes; i++) {
    i128 v = U ? (i128)lu(&vm, eb, i) : (i128)ls(&vm, eb, i);
    uint64_t x, old = lu(&vd, eb, i);
    switch (A) {
    case 0: x = (uint64_t)shift_round(v, -rshift, 0); break;                     /* VSHR */
    case 1: x = old + (uint64_t)shift_round(v, -rshift, 0); break;               /* VSRA */
    case 2: x = (uint64_t)shift_round(v, -rshift, 1); break;                     /* VRSHR */
    case 3: x = old + (uint64_t)shift_round(v, -rshift, 1); break;               /* VRSRA */
    case 4: { /* VSRI */
      if (!U) goto undef;
      uint64_t ins = rshift >= 64 ? 0 : mask >> rshift;
      uint64_t val = rshift >= 64 ? 0 : lu(&vm, eb, i) >> rshift;
      x = (old & ~ins) | (val & ins);
      break;
    }
    case 5: /* VSHL, VSLI */
      if (U) {
        uint64_t ins = (mask << lshift) & mask;
        x = (old & ~ins) | ((lu(&vm, eb, i) << lshift) & ins);
      } else {
        x = lu(&vm, eb, i) << lshift;
      }
      break;
    case 6: /* VQSHLU */
      if (!U) goto undef;
      x = sat_u(s, (i128)ls(&vm, eb, i) << lshift, esize);
      break;
    case 7: /* VQSHL */
      x = U ? sat_u(s, v << lshift, esize) : (uint64_t)sat_s(s, v << lshift, esize);
      break;
    default: goto undef;
    }
    lset(&r, eb, i, x & mask);
  }
  vput(s, d, regs, &r);
  return;
undef:
  interp_undefined(s, insn, "NEON two registers and a shift");
}

/* ---- one register and a modified immediate (A7.4.6) ---- */

static void one_imm(icpu *s, uint32_t insn, int d, int Q) {
  uint32_t imm8 = BIT(insn, 24) << 7 | BITS(insn, 18, 16) << 4 | BITS(insn, 3, 0);
  uint32_t cmode = BITS(insn, 11, 8), op = BIT(insn, 5);
  uint64_t imm;
  switch (cmode >> 1) {
  case 0: imm = imm8; imm |= imm << 32; break;
  case 1: imm = (uint64_t)imm8 << 8; imm |= imm << 32; break;
  case 2: imm = (uint64_t)imm8 << 16; imm |= imm << 32; break;
  case 3: imm = (uint64_t)imm8 << 24; imm |= imm << 32; break;
  case 4: imm = imm8 * 0x0001000100010001ull; break;
  case 5: imm = (uint64_t)imm8 * 0x0100010001000100ull; break;
  case 6:
    imm = (cmode & 1) ? (uint64_t)(imm8 << 16 | 0xFFFF) : (uint64_t)(imm8 << 8 | 0xFF);
    imm |= imm << 32;
    break;
  default:
    if (!(cmode & 1)) {
      if (!op) {
        imm = imm8 * 0x0101010101010101ull;
      } else {
        imm = 0;
        for (int i = 0; i < 8; i++)
          if (imm8 & (1u << i)) imm |= 0xFFull << (8 * i);
      }
    } else {
      if (op) goto undef;
      uint32_t b = BIT(imm8, 6);
      uint32_t f = BIT(imm8, 7) << 31 | (b ^ 1) << 30 | (b ? 0x1Fu << 25 : 0) | (imm8 & 0x3F) << 19;
      imm = (uint64_t)f << 32 | f;
    }
  }
  if (Q && (d & 1)) goto undef;
  int orr_bic = (cmode < 12) && (cmode & 1);
  for (int i = 0; i < (Q ? 2 : 1); i++) {
    uint64_t *p = &s->x.d[d + i];
    if (orr_bic) *p = op ? *p & ~imm : *p | imm;
    else if (op && cmode != 14) *p = ~imm;
    else *p = imm;
  }
  return;
undef:
  interp_undefined(s, insn, "NEON modified immediate");
}

/* ---- two registers, miscellaneous (A7.4.5) ---- */

static int cls64(int64_t v, int bits) {
  int n = 0;
  int64_t sign = (v >> (bits - 1)) & 1;
  for (int b = bits - 2; b >= 0 && ((v >> b) & 1) == sign; b--) n++;
  return n;
}

static void two_misc(icpu *s, uint32_t insn, int d, int m, int Q) {
  int A = (int)BITS(insn, 17, 16), B = (int)BITS(insn, 10, 6), size = (int)BITS(insn, 19, 18);
  int eb = 1 << size, regs = Q ? 2 : 1, lanes = 8 * regs / eb, bits = 8 * eb;
  uint64_t mask = emask(eb);
  vec vm, vd, r;
  int narrow = A == 2 && (B >> 1) >= 4 && (B >> 1) <= 6; /* VMOVN, VQMOVUN, VQMOVN, VSHLL */
  if (!narrow && A != 2 && Q && ((d & 1) || (m & 1))) goto undef;
  vget(s, m, 2, &vm);
  vget(s, d, 2, &vd);
  memset(&r, 0, sizeof r);
  if (A == 0) {
    int op = B >> 1;
    if (op <= 2) { /* VREV64, VREV32, VREV16 */
      int group = 8 >> op; /* bytes per reversed group */
      if (eb >= group) goto undef;
      for (int g = 0; g < 8 * regs; g += group)
        for (int i = 0; i < group / eb; i++) lset(&r, eb, g / eb + i, lu(&vm, eb, g / eb + group / eb - 1 - i));
      vput(s, d, regs, &r);
      return;
    }
    if ((B >> 2) == 2 || (B >> 2) == 6) { /* VPADDL, VPADAL */
      if (size == 3) goto undef;
      int U = (int)BIT(insn, 7);
      for (int i = 0; i < lanes / 2; i++) {
        i128 a = U ? (i128)lu(&vm, eb, 2 * i) : (i128)ls(&vm, eb, 2 * i);
        i128 b = U ? (i128)lu(&vm, eb, 2 * i + 1) : (i128)ls(&vm, eb, 2 * i + 1);
        uint64_t x = (uint64_t)(a + b);
        if ((B >> 2) == 6) x += lu(&vd, 2 * eb, i);
        lset(&r, 2 * eb, i, x);
      }
      vput(s, d, regs, &r);
      return;
    }
    for (int i = 0; i < lanes; i++) {
      uint64_t x;
      switch (op) {
      case 8: if (size == 3) goto undef; x = (uint64_t)cls64(ls(&vm, eb, i), bits); break; /* VCLS */
      case 9: { /* VCLZ */
        if (size == 3) goto undef;
        uint64_t v = lu(&vm, eb, i);
        x = v ? (uint64_t)(__builtin_clzll(v) - (64 - bits)) : (uint64_t)bits;
        break;
      }
      case 10: if (size) goto undef; x = (uint64_t)__builtin_popcount((unsigned)lu(&vm, 1, i)); break; /* VCNT */
      case 11: if (size) goto undef; x = ~lu(&vm, eb, i); break; /* VMVN */
      case 14: if (size == 3) goto undef; { i128 v = ls(&vm, eb, i); x = (uint64_t)sat_s(s, v < 0 ? -v : v, bits); } break;
      case 15: if (size == 3) goto undef; x = (uint64_t)sat_s(s, -(i128)ls(&vm, eb, i), bits); break;
      default: goto undef;
      }
      lset(&r, eb, i, x & mask);
    }
    vput(s, d, regs, &r);
    return;
  }
  if (A == 1) {
    int F = (int)BIT(insn, 10), op = (int)BITS(insn, 9, 7);
    if (size == 3 || (F && size != 2) || op == 5) goto undef;
    if (F) interp_fp_neon_begin(s);
    for (int i = 0; i < lanes; i++) {
      uint64_t x;
      if (F) {
        float v = lf(&vm, i), z = 0.0f;
        switch (op) {
        case 0: x = v > z ? mask : 0; break;
        case 1: x = v >= z ? mask : 0; break;
        case 2: x = v == z ? mask : 0; break;
        case 3: x = v <= z ? mask : 0; break;
        case 4: x = v < z ? mask : 0; break;
        case 6: { float a = f_abs(v); memcpy(&x, &a, 4); x &= 0xFFFFFFFFu; break; }
        default: { float a = f_neg(v); memcpy(&x, &a, 4); x &= 0xFFFFFFFFu; break; }
        }
      } else {
        int64_t v = ls(&vm, eb, i);
        switch (op) {
        case 0: x = v > 0 ? mask : 0; break;
        case 1: x = v >= 0 ? mask : 0; break;
        case 2: x = v == 0 ? mask : 0; break;
        case 3: x = v <= 0 ? mask : 0; break;
        case 4: x = v < 0 ? mask : 0; break;
        case 6: x = (uint64_t)(v < 0 ? -v : v); break;
        default: x = (uint64_t)-v; break;
        }
      }
      lset(&r, eb, i, x & mask);
    }
    if (F) interp_fp_neon_end(s);
    vput(s, d, regs, &r);
    return;
  }
  if (A == 2) {
    int op = B >> 1;
    if (op <= 3) { /* VSWP, VTRN, VUZP, VZIP */
      if (Q && ((d & 1) || (m & 1))) goto undef;
      vec a = vd, b = vm, ra = vd, rb = vm;
      if (op == 0) {
        if (size) goto undef;
        vput(s, d, regs, &vm);
        vput(s, m, regs, &vd);
        return;
      }
      if (size == 3 || (op != 1 && !Q && size == 2)) goto undef;
      int h = lanes / 2;
      switch (op) {
      case 1: /* VTRN */
        for (int i = 0; i < h; i++) {
          lset(&ra, eb, 2 * i + 1, lu(&b, eb, 2 * i));
          lset(&rb, eb, 2 * i, lu(&a, eb, 2 * i + 1));
        }
        break;
      case 2: /* VUZP */
        for (int i = 0; i < h; i++) {
          lset(&ra, eb, i, lu(&a, eb, 2 * i));
          lset(&ra, eb, h + i, lu(&b, eb, 2 * i));
          lset(&rb, eb, i, lu(&a, eb, 2 * i + 1));
          lset(&rb, eb, h + i, lu(&b, eb, 2 * i + 1));
        }
        break;
      default: /* VZIP */
        for (int i = 0; i < h; i++) {
          lset(&ra, eb, 2 * i, lu(&a, eb, i));
          lset(&ra, eb, 2 * i + 1, lu(&b, eb, i));
          lset(&rb, eb, 2 * i, lu(&a, eb, h + i));
          lset(&rb, eb, 2 * i + 1, lu(&b, eb, h + i));
        }
        break;
      }
      vput(s, d, regs, &ra);
      vput(s, m, regs, &rb);
      return;
    }
    if (op == 4 || op == 5) { /* VMOVN, VQMOVUN, VQMOVN */
      if (size == 3 || (m & 1)) goto undef;
      int kind = B & 3; /* 0 VMOVN, 1 VQMOVUN, 2 VQMOVN signed, 3 VQMOVN unsigned */
      if (op == 4 && kind > 1) goto undef;
      if (op == 5) kind = 2 + (B & 1);
      for (int i = 0; i < 8 / eb; i++) {
        uint64_t x;
        switch (kind) {
        case 0: x = lu(&vm, 2 * eb, i); break;
        case 1: x = sat_u(s, ls(&vm, 2 * eb, i), bits); break;
        case 2: x = (uint64_t)sat_s(s, ls(&vm, 2 * eb, i), bits); break;
        default: x = sat_u(s, lu(&vm, 2 * eb, i), bits); break;
        }
        lset(&r, eb, i, x);
      }
      vput(s, d, 1, &r);
      return;
    }
    if (B == 12) { /* VSHLL (maximum shift) */
      if (size == 3 || (d & 1)) goto undef;
      for (int i = 0; i < 8 / eb; i++) lset(&r, 2 * eb, i, lu(&vm, eb, i) << bits);
      vput(s, d, 2, &r);
      return;
    }
    if ((B & 0x1B) == 0x18 && size == 1) { /* VCVT half <-> single */
      interp_fp_neon_begin(s);
      if (!BIT(insn, 8)) { /* single to half */
        if (m & 1) { interp_fp_neon_end(s); goto undef; }
        for (int i = 0; i < 4; i++) {
          __fp16 h = (__fp16)lf(&vm, i);
          uint16_t u;
          memcpy(&u, &h, 2);
          lset(&r, 2, i, u);
        }
        vput(s, d, 1, &r);
      } else {
        if (d & 1) { interp_fp_neon_end(s); goto undef; }
        for (int i = 0; i < 4; i++) {
          uint16_t u = (uint16_t)lu(&vm, 2, i);
          __fp16 h;
          memcpy(&h, &u, 2);
          lsetf(&r, i, (float)h);
        }
        vput(s, d, 2, &r);
      }
      interp_fp_neon_end(s);
      return;
    }
    goto undef;
  }
  /* A == 3: VRECPE, VRSQRTE, VCVT float <-> integer */
  if (size != 2) goto undef;
  interp_fp_neon_begin(s);
  for (int i = 0; i < lanes; i++) {
    uint32_t x = (uint32_t)lu(&vm, 4, i);
    float f;
    memcpy(&f, &x, 4);
    switch (B >> 1) {
    case 8: x = u_recpe(x); break;                                            /* VRECPE.U32 */
    case 9: x = u_rsqrte(x); break;                                           /* VRSQRTE.U32 */
    case 10: { float t = f_recpe(f); memcpy(&x, &t, 4); break; }             /* VRECPE.F32 */
    case 11: { float t = f_rsqrte(f); memcpy(&x, &t, 4); break; }            /* VRSQRTE.F32 */
    case 12: { float t = (float)(int32_t)x; memcpy(&x, &t, 4); break; }      /* VCVT.F32.S32 */
    case 13: { float t = (float)x; memcpy(&x, &t, 4); break; }               /* VCVT.F32.U32 */
    case 14: x = f_cvt_s(f, 0); break;                                        /* VCVT.S32.F32 */
    case 15: x = f_cvt_s(f, 1); break;                                        /* VCVT.U32.F32 */
    default: interp_fp_neon_end(s); goto undef;
    }
    lset(&r, 4, i, x);
  }
  interp_fp_neon_end(s);
  vput(s, d, regs, &r);
  return;
undef:
  interp_undefined(s, insn, "NEON two registers, miscellaneous");
}

/* ---- top level of data processing (A7.4) ---- */

void interp_neon_dp(icpu *s, uint32_t insn) {
  uint32_t U = BIT(insn, 24), A = BITS(insn, 23, 19), B = BITS(insn, 11, 8), C = BITS(insn, 7, 4);
  int d = (int)(BIT(insn, 22) << 4 | BITS(insn, 15, 12));
  int n = (int)(BIT(insn, 7) << 4 | BITS(insn, 19, 16));
  int m = (int)(BIT(insn, 5) << 4 | BITS(insn, 3, 0));
  int Q = (int)BIT(insn, 6);
  if (!(A & 0x10)) {
    if (Q && ((d & 1) || (n & 1) || (m & 1))) goto undef;
    three_same(s, insn, d, n, m, Q);
    return;
  }
  if ((C & 9) == 1) { /* C = xxx1 with bit 7 (L) or the immediate forms */
    if ((A & 0x17) == 0x10 && !(C & 8)) { /* A = 1x000, C = 0xx1 */
      one_imm(s, insn, d, Q);
      return;
    }
    two_shift(s, insn, d, m, Q);
    return;
  }
  if ((C & 9) == 9) { /* C = 1xx1: shifts with L = 1 */
    two_shift(s, insn, d, m, Q);
    return;
  }
  /* C = xxx0 */
  if ((A & 0x16) != 0x16) { /* A = 1x0xx or 1x10x */
    if (!(C & 4)) three_diff(s, insn, d, n, m);
    else two_scalar(s, insn, d, n);
    return;
  }
  /* A = 1x11x */
  if (!U) { /* VEXT */
    if (Q && ((d & 1) || (n & 1) || (m & 1))) goto undef;
    int regs = Q ? 2 : 1, imm = (int)B;
    if (!Q && imm > 7) goto undef;
    vec vn, vm, r;
    vget(s, n, regs, &vn);
    vget(s, m, regs, &vm);
    for (int i = 0; i < 8 * regs; i++) {
      int k = i + imm;
      r.b[i] = k < 8 * regs ? vn.b[k] : vm.b[k - 8 * regs];
    }
    vput(s, d, regs, &r);
    return;
  }
  if (!(B & 8)) {
    two_misc(s, insn, d, m, Q);
    return;
  }
  if ((B & 0xC) == 8) { /* VTBL, VTBX */
    int len = (int)BITS(insn, 9, 8) + 1, tbx = (int)BIT(insn, 6);
    if (n + len > 32) goto undef;
    vec tab, vm, vd, r;
    vget(s, n, len, &tab);
    vget(s, m, 1, &vm);
    vget(s, d, 1, &vd);
    for (int i = 0; i < 8; i++) {
      unsigned idx = vm.b[i];
      r.b[i] = idx < (unsigned)(8 * len) ? tab.b[idx] : (tbx ? vd.b[i] : 0);
    }
    vput(s, d, 1, &r);
    return;
  }
  if (B == 12 && !(C & 8)) { /* VDUP scalar */
    uint32_t imm4 = BITS(insn, 19, 16);
    int eb, idx;
    if (imm4 & 1) { eb = 1; idx = (int)(imm4 >> 1); }
    else if (imm4 & 2) { eb = 2; idx = (int)(imm4 >> 2); }
    else if (imm4 & 4) { eb = 4; idx = (int)(imm4 >> 3); }
    else goto undef;
    if (Q && (d & 1)) goto undef;
    vec vm, r;
    vget(s, m, 1, &vm);
    uint64_t x = lu(&vm, eb, idx);
    for (int i = 0; i < 8 * (Q ? 2 : 1) / eb; i++) lset(&r, eb, i, x);
    vput(s, d, Q ? 2 : 1, &r);
    return;
  }
undef:
  interp_undefined(s, insn, "NEON data processing");
}

/* ---- element and structure loads and stores (A7.7) ---- */

static void ld_elem(icpu *s, int reg, int eb, int idx, uint32_t a) {
  uint8_t *p = (uint8_t *)&s->x.d[reg & 31] + eb * idx;
  switch (eb) {
  case 1: *p = rd8(s, a); break;
  case 2: { uint16_t v = rd16(s, a); memcpy(p, &v, 2); break; }
  case 4: { uint32_t v = rd32(s, a); memcpy(p, &v, 4); break; }
  default: { uint32_t lo = rd32(s, a), hi = rd32(s, a + 4); memcpy(p, &lo, 4); memcpy(p + 4, &hi, 4); break; }
  }
}

static void st_elem(icpu *s, int reg, int eb, int idx, uint32_t a) {
  const uint8_t *p = (const uint8_t *)&s->x.d[reg & 31] + eb * idx;
  switch (eb) {
  case 1: wr8(s, a, *p); break;
  case 2: { uint16_t v; memcpy(&v, p, 2); wr16(s, a, v); break; }
  case 4: { uint32_t v; memcpy(&v, p, 4); wr32(s, a, v); break; }
  default: { uint32_t lo, hi; memcpy(&lo, p, 4); memcpy(&hi, p + 4, 4); wr32(s, a, lo); wr32(s, a + 4, hi); break; }
  }
}

void interp_neon_ls(icpu *s, uint32_t insn) {
  int A = (int)BIT(insn, 23), L = (int)BIT(insn, 21);
  int n = (int)BITS(insn, 19, 16), mreg = (int)BITS(insn, 3, 0);
  int d = (int)(BIT(insn, 22) << 4 | BITS(insn, 15, 12));
  uint32_t type = BITS(insn, 11, 8);
  int size = (int)BITS(insn, 7, 6), eb = 1 << size;
  uint32_t a = s->r[n], total;
  if (n == 15) goto undef;
  if (!A) { /* multiple structures */
    int nelem, regs, inc;
    switch (type) {
    case 7: nelem = 1; regs = 1; inc = 1; break;
    case 10: nelem = 1; regs = 2; inc = 1; break;
    case 6: nelem = 1; regs = 3; inc = 1; break;
    case 2: nelem = 1; regs = 4; inc = 1; break;
    case 8: nelem = 2; regs = 1; inc = 1; break;
    case 9: nelem = 2; regs = 1; inc = 2; break;
    case 3: nelem = 2; regs = 2; inc = 2; break;
    case 4: nelem = 3; regs = 1; inc = 1; break;
    case 5: nelem = 3; regs = 1; inc = 2; break;
    case 0: nelem = 4; regs = 1; inc = 1; break;
    case 1: nelem = 4; regs = 1; inc = 2; break;
    default: goto undef;
    }
    if (nelem > 1 && size == 3) goto undef;
    int last = nelem == 1 ? d + regs - 1 : (type == 3 ? d + 3 : d + (nelem - 1) * inc);
    if (last > 31) goto undef;
    int lanes = 8 / eb;
    uint32_t p = a;
    for (int r = 0; r < regs; r++)
      for (int e = 0; e < lanes; e++)
        for (int k = 0; k < nelem; k++) {
          int reg = nelem == 1 ? d + r : d + r + k * inc;
          if (L) ld_elem(s, reg, eb, e, p);
          else st_elem(s, reg, eb, e, p);
          p += (uint32_t)eb;
        }
    total = p - a;
  } else {
    int nelem = (int)BITS(insn, 9, 8) + 1;
    if (BITS(insn, 11, 10) == 3) { /* to all lanes */
      if (!L) goto undef;
      int sz = (int)BITS(insn, 7, 6), T = (int)BIT(insn, 5);
      if (sz == 3) {
        if (nelem != 4) goto undef;
        sz = 2;
      }
      int ebb = 1 << sz;
      int regs = nelem == 1 ? T + 1 : 1, inc = nelem == 1 ? 1 : T + 1;
      if (d + (nelem - 1) * inc + regs - 1 > 31) goto undef;
      uint32_t p = a;
      for (int k = 0; k < nelem; k++) {
        vec v;
        uint64_t x = 0;
        switch (ebb) {
        case 1: x = rd8(s, p); break;
        case 2: x = rd16(s, p); break;
        default: x = rd32(s, p); break;
        }
        for (int i = 0; i < 8 / ebb; i++) lset(&v, ebb, i, x);
        for (int r = 0; r < regs; r++) vput(s, d + k * inc + r, 1, &v);
        p += (uint32_t)ebb;
      }
      total = p - a;
    } else { /* single lane */
      int sz = (int)BITS(insn, 11, 10), ebb = 1 << sz;
      uint32_t ia = BITS(insn, 7, 4);
      int idx = (int)(ia >> (sz + 1)), inc = 1;
      if (sz == 1 && (ia & 2)) inc = 2;
      if (sz == 2 && (ia & 4)) inc = 2;
      if (d + (nelem - 1) * inc > 31) goto undef;
      uint32_t p = a;
      for (int k = 0; k < nelem; k++) {
        if (L) ld_elem(s, d + k * inc, ebb, idx, p);
        else st_elem(s, d + k * inc, ebb, idx, p);
        p += (uint32_t)ebb;
      }
      total = p - a;
    }
  }
  if (mreg != 15) s->r[n] = a + (mreg == 13 ? total : s->r[mreg]);
  return;
undef:
  interp_undefined(s, insn, "NEON load/store");
}
