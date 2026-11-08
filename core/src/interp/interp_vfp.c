/*
 * Coprocessor space: CP15 (TLS registers, barriers, the virtual counter) and
 * VFPv3 (cp10/cp11): data processing, register transfers, loads and stores.
 *
 * Arithmetic runs on the host FPU. On arm64 the AArch64 FPCR has the AArch32
 * FPSCR's mode bits (AHP, DN, FZ, RMode) at the same positions, and FPSR
 * the same cumulative exception bits, so the guest's FP environment is
 * installed in the host's while guest code runs (interp_fp_enter) and the
 * host computes exactly what ARMv7 VFP would: same rounding, flushing,
 * NaN propagation and exception flags. Comparisons and float-to-integer
 * conversions use the AArch64 instructions that match ARMv7's semantics.
 */
#include "interp.h"

#include <math.h>
#include <mach/mach_time.h>

#pragma STDC FENV_ACCESS ON
#pragma clang fp contract(off)

#define BIT(x, n) (((x) >> (n)) & 1u)
#define BITS(x, hi, lo) (((x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))

#define FPSCR_MODE_BITS 0x07C00000u /* AHP, DN, FZ, RMode */
#define FPSCR_EXC_BITS 0x0000009Fu  /* IDC, IXC, UFC, OFC, DZC, IOC */
#define FPSCR_QC (1u << 27)

/* ---- host FP environment ---- */

#if defined(__aarch64__)
static inline uint64_t get_fpcr(void) {
  uint64_t v;
  __asm__ volatile("mrs %0, fpcr" : "=r"(v));
  return v;
}
static inline void set_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" ::"r"(v) : "memory"); }
static inline uint64_t get_fpsr(void) {
  uint64_t v;
  __asm__ volatile("mrs %0, fpsr" : "=r"(v));
  return v;
}
static inline void set_fpsr(uint64_t v) { __asm__ volatile("msr fpsr, %0" ::"r"(v) : "memory"); }
#else
#error "the interpreter's FP environment needs an arm64 host"
#endif

static __thread uint64_t host_fpcr;

void interp_fp_enter(icpu *s) {
  host_fpcr = get_fpcr();
  set_fpcr((host_fpcr & ~(uint64_t)FPSCR_MODE_BITS) | (s->fpscr & FPSCR_MODE_BITS));
  set_fpsr(0);
}

void interp_fp_leave(icpu *s) {
  s->fpscr |= (uint32_t)get_fpsr() & FPSCR_EXC_BITS;
  set_fpcr(host_fpcr);
}

/* Folds the host's flags into the FPSCR (for VMRS) and starts afresh. */
static void fp_sync(icpu *s) {
  s->fpscr |= (uint32_t)get_fpsr() & FPSCR_EXC_BITS;
  set_fpsr(0);
}

void interp_fp_neon_begin(icpu *s) {
  (void)s;
  set_fpcr((get_fpcr() & ~(uint64_t)FPSCR_MODE_BITS) | (3u << 24)); /* DN, FZ, nearest */
}

void interp_fp_neon_end(icpu *s) {
  set_fpcr((get_fpcr() & ~(uint64_t)FPSCR_MODE_BITS) | (s->fpscr & FPSCR_MODE_BITS));
}

/* ---- register access ---- */

static inline float getS(icpu *s, int i) {
  float f;
  memcpy(&f, &s->x.s[i], 4);
  return f;
}
static inline void putS(icpu *s, int i, float f) { memcpy(&s->x.s[i], &f, 4); }
static inline double getD(icpu *s, int i) {
  double f;
  memcpy(&f, &s->x.d[i], 8);
  return f;
}
static inline void putD(icpu *s, int i, double f) { memcpy(&s->x.d[i], &f, 8); }

/* ---- AArch64 helpers with ARMv7 semantics ---- */

/* FCMP/FCMPE set NZCV exactly as VCMP/VCMPE set the FPSCR flags. */
static uint32_t cmp_s(float a, float b, int e) {
  uint64_t f;
  if (e) __asm__ volatile("fcmpe %s1, %s2\n\tmrs %0, nzcv" : "=r"(f) : "w"(a), "w"(b) : "cc");
  else __asm__ volatile("fcmp %s1, %s2\n\tmrs %0, nzcv" : "=r"(f) : "w"(a), "w"(b) : "cc");
  return (uint32_t)f & 0xF0000000u;
}
static uint32_t cmp_d(double a, double b, int e) {
  uint64_t f;
  if (e) __asm__ volatile("fcmpe %d1, %d2\n\tmrs %0, nzcv" : "=r"(f) : "w"(a), "w"(b) : "cc");
  else __asm__ volatile("fcmp %d1, %d2\n\tmrs %0, nzcv" : "=r"(f) : "w"(a), "w"(b) : "cc");
  return (uint32_t)f & 0xF0000000u;
}

/* Float to 32-bit integer, saturating, NaN to 0, with ARM's flags. rmode:
 * 0 nearest, 1 +inf, 2 -inf, 3 zero (FPSCR RMode encoding). */
#define CVT_FN(name, T, reg)                                                                       \
  static uint32_t name(T v, int uns, int rmode) {                                                  \
    uint32_t r;                                                                                    \
    switch (rmode | uns << 2) {                                                                    \
    case 0: __asm__ volatile("fcvtns %w0, %" reg "1" : "=r"(r) : "w"(v)); break;                   \
    case 1: __asm__ volatile("fcvtps %w0, %" reg "1" : "=r"(r) : "w"(v)); break;                   \
    case 2: __asm__ volatile("fcvtms %w0, %" reg "1" : "=r"(r) : "w"(v)); break;                   \
    case 3: __asm__ volatile("fcvtzs %w0, %" reg "1" : "=r"(r) : "w"(v)); break;                   \
    case 4: __asm__ volatile("fcvtnu %w0, %" reg "1" : "=r"(r) : "w"(v)); break;                   \
    case 5: __asm__ volatile("fcvtpu %w0, %" reg "1" : "=r"(r) : "w"(v)); break;                   \
    case 6: __asm__ volatile("fcvtmu %w0, %" reg "1" : "=r"(r) : "w"(v)); break;                   \
    default: __asm__ volatile("fcvtzu %w0, %" reg "1" : "=r"(r) : "w"(v)); break;                  \
    }                                                                                              \
    return r;                                                                                      \
  }
CVT_FN(cvt_s, float, "s")
CVT_FN(cvt_d, double, "d")

/* Fused multiply-add, addend first in NaN priority like FPMulAdd. */
static float fma_s(float a, float b, float c) {
  float r;
  __asm__ volatile("fmadd %s0, %s1, %s2, %s3" : "=w"(r) : "w"(a), "w"(b), "w"(c));
  return r;
}
static double fma_d(double a, double b, double c) {
  double r;
  __asm__ volatile("fmadd %d0, %d1, %d2, %d3" : "=w"(r) : "w"(a), "w"(b), "w"(c));
  return r;
}

/* VFPExpandImm. */
static uint32_t expand_imm_s(uint32_t imm8) {
  uint32_t b = BIT(imm8, 6);
  return BIT(imm8, 7) << 31 | (b ^ 1) << 30 | (b ? 0x1Fu << 25 : 0) | (imm8 & 0x3F) << 19;
}
static uint64_t expand_imm_d(uint32_t imm8) {
  uint64_t b = BIT(imm8, 6);
  return (uint64_t)BIT(imm8, 7) << 63 | (b ^ 1) << 62 | (b ? 0xFFull << 54 : 0) |
         (uint64_t)(imm8 & 0x3F) << 48;
}

/* ---- VFP data processing ---- */

static void vfp_dp(icpu *s, uint32_t insn) {
  int dbl = (int)BIT(insn, 8);
  uint32_t opc1 = BIT(insn, 23) << 2 | BITS(insn, 21, 20), opc2 = BITS(insn, 19, 16);
  int op6 = (int)BIT(insn, 6);
  int d, n, m;
  if (dbl) {
    d = (int)(BIT(insn, 22) << 4 | BITS(insn, 15, 12));
    n = (int)(BIT(insn, 7) << 4 | BITS(insn, 19, 16));
    m = (int)(BIT(insn, 5) << 4 | BITS(insn, 3, 0));
  } else {
    d = (int)(BITS(insn, 15, 12) << 1 | BIT(insn, 22));
    n = (int)(BITS(insn, 19, 16) << 1 | BIT(insn, 7));
    m = (int)(BITS(insn, 3, 0) << 1 | BIT(insn, 5));
  }
  if (opc1 != 7) {
    if (dbl) {
      double a = getD(s, n), b = getD(s, m), r;
      uint64_t bits;
      switch (opc1) {
      case 0: case 1: { /* VMLA, VMLS, VNMLS, VNMLA */
        double p = a * b, acc = getD(s, d);
        uint64_t pb, ab;
        memcpy(&pb, &p, 8);
        memcpy(&ab, &acc, 8);
        if (op6) pb ^= 1ull << 63;
        if (opc1 == 1) ab ^= 1ull << 63;
        memcpy(&p, &pb, 8);
        memcpy(&acc, &ab, 8);
        r = acc + p;
        break;
      }
      case 2:
        r = a * b;
        if (op6) {
          memcpy(&bits, &r, 8);
          bits ^= 1ull << 63;
          memcpy(&r, &bits, 8);
        }
        break;
      case 3: r = op6 ? a - b : a + b; break;
      case 4: if (op6) goto undef; r = a / b; break;
      case 5: r = op6 ? fma_d(-a, b, -getD(s, d)) : fma_d(a, b, -getD(s, d)); break; /* VFNMA, VFNMS */
      default: r = op6 ? fma_d(-a, b, getD(s, d)) : fma_d(a, b, getD(s, d)); break;  /* VFMS, VFMA */
      }
      putD(s, d, r);
    } else {
      float a = getS(s, n), b = getS(s, m), r;
      uint32_t bits;
      switch (opc1) {
      case 0: case 1: {
        float p = a * b, acc = getS(s, d);
        uint32_t pb, ab;
        memcpy(&pb, &p, 4);
        memcpy(&ab, &acc, 4);
        if (op6) pb ^= 1u << 31;
        if (opc1 == 1) ab ^= 1u << 31;
        memcpy(&p, &pb, 4);
        memcpy(&acc, &ab, 4);
        r = acc + p;
        break;
      }
      case 2:
        r = a * b;
        if (op6) {
          memcpy(&bits, &r, 4);
          bits ^= 1u << 31;
          memcpy(&r, &bits, 4);
        }
        break;
      case 3: r = op6 ? a - b : a + b; break;
      case 4: if (op6) goto undef; r = a / b; break;
      case 5: r = op6 ? fma_s(-a, b, -getS(s, d)) : fma_s(a, b, -getS(s, d)); break;
      default: r = op6 ? fma_s(-a, b, getS(s, d)) : fma_s(a, b, getS(s, d)); break;
      }
      putS(s, d, r);
    }
    return;
  }
  /* opc1 == 7: other VFP data-processing instructions */
  if (!op6) { /* VMOV immediate */
    uint32_t imm8 = BITS(insn, 19, 16) << 4 | BITS(insn, 3, 0);
    if (dbl) s->x.d[d] = expand_imm_d(imm8);
    else s->x.s[d] = expand_imm_s(imm8);
    return;
  }
  int sm = (int)(BITS(insn, 3, 0) << 1 | BIT(insn, 5));  /* m as a single */
  int sd = (int)(BITS(insn, 15, 12) << 1 | BIT(insn, 22)); /* d as a single */
  int dd = (int)(BIT(insn, 22) << 4 | BITS(insn, 15, 12)); /* d as a double */
  int dm = (int)(BIT(insn, 5) << 4 | BITS(insn, 3, 0));    /* m as a double */
  int e7 = (int)BIT(insn, 7);
  switch (opc2) {
  case 0: /* VMOV register, VABS */
    if (dbl) s->x.d[d] = e7 ? s->x.d[m] & ~(1ull << 63) : s->x.d[m];
    else s->x.s[d] = e7 ? s->x.s[m] & ~(1u << 31) : s->x.s[m];
    return;
  case 1: /* VNEG, VSQRT */
    if (e7) {
      if (dbl) putD(s, d, __builtin_sqrt(getD(s, m)));
      else putS(s, d, __builtin_sqrtf(getS(s, m)));
    } else {
      if (dbl) s->x.d[d] = s->x.d[m] ^ (1ull << 63);
      else s->x.s[d] = s->x.s[m] ^ (1u << 31);
    }
    return;
  case 2: case 3: { /* VCVTB, VCVTT: half <-> single */
    if (dbl) goto undef;
    int top = e7;
    if (opc2 == 2) { /* half to single */
      uint16_t h = (uint16_t)(s->x.s[sm] >> (top ? 16 : 0));
      __fp16 hf;
      memcpy(&hf, &h, 2);
      putS(s, sd, (float)hf);
    } else {
      __fp16 hf = (__fp16)getS(s, sm);
      uint16_t h;
      memcpy(&h, &hf, 2);
      if (top) s->x.s[sd] = (s->x.s[sd] & 0xFFFF) | (uint32_t)h << 16;
      else s->x.s[sd] = (s->x.s[sd] & 0xFFFF0000u) | h;
    }
    return;
  }
  case 4: case 5: { /* VCMP, VCMPE */
    uint32_t f;
    if (dbl) f = cmp_d(getD(s, d), opc2 == 5 ? 0.0 : getD(s, m), e7);
    else f = cmp_s(getS(s, d), opc2 == 5 ? 0.0f : getS(s, m), e7);
    s->fpscr = (s->fpscr & 0x0FFFFFFFu) | f;
    return;
  }
  case 7: /* VCVT double <-> single */
    if (!e7) goto undef;
    if (dbl) putS(s, sd, (float)getD(s, dm));
    else putD(s, dd, (double)getS(s, sm));
    return;
  case 8: { /* VCVT int -> float; source always a single register */
    uint32_t v = s->x.s[sm];
    if (dbl) putD(s, dd, e7 ? (double)(int32_t)v : (double)v);
    else putS(s, sd, e7 ? (float)(int32_t)v : (float)v);
    return;
  }
  case 10: case 11: case 14: case 15: { /* VCVT fixed <-> float, in place */
    int to_fixed = (int)BIT(insn, 18), uns = (int)BIT(insn, 16);
    int size = e7 ? 32 : 16;
    int frac = size - (int)(BITS(insn, 3, 0) << 1 | BIT(insn, 5));
    if (frac < 0) goto undef;
    double scale = ldexp(1.0, frac);
    if (to_fixed) {
      uint32_t r;
      if (size == 16) fp_sync(s);
      /* Scaling by 2^frac is exact below 2^33; anything larger saturates
       * anyway, and converting it unscaled keeps the multiply from
       * overflowing and raising flags ARM doesn't. */
      double v = dbl ? getD(s, dd) : (double)getS(s, sd); /* exact; FZ flushes like ARM */
      r = cvt_d(__builtin_fabs(v) < 0x1p33 ? v * scale : v, uns, 3);
      if (size == 16) {
        int64_t lo = uns ? 0 : -32768, hi = uns ? 65535 : 32767;
        int64_t x = uns ? (int64_t)r : (int64_t)(int32_t)r;
        if (x < lo || x > hi) {
          x = x < lo ? lo : hi;
          s->fpscr |= 1; /* IOC */
          set_fpsr(get_fpsr() & ~(uint64_t)0x10); /* the inexact went with the overflow */
        }
        r = uns ? (uint32_t)x : (uint32_t)(int32_t)(int16_t)x;
      }
      if (dbl) s->x.d[dd] = uns ? (uint64_t)r : (uint64_t)(int64_t)(int32_t)r;
      else s->x.s[sd] = r;
    } else {
      uint32_t raw = dbl ? (uint32_t)s->x.d[dd] : s->x.s[sd];
      int64_t x;
      if (size == 16) x = uns ? (int64_t)(raw & 0xFFFF) : (int64_t)(int16_t)raw;
      else x = uns ? (int64_t)raw : (int64_t)(int32_t)raw;
      if (dbl) putD(s, dd, (double)x / scale);
      else putS(s, sd, (float)((double)x / scale));
    }
    return;
  }
  case 12: case 13: { /* VCVT, VCVTR float -> int; destination always a single */
    int rmode = e7 ? 3 : (int)BITS(s->fpscr, 23, 22);
    int uns = opc2 == 12;
    if (dbl) s->x.s[sd] = cvt_d(getD(s, dm), uns, rmode);
    else s->x.s[sd] = cvt_s(getS(s, sm), uns, rmode);
    return;
  }
  }
undef:
  interp_undefined(s, insn, "VFP data processing");
}

/* ---- loads and stores ---- */

static void vfp_ldst(icpu *s, uint32_t insn) {
  int P = (int)BIT(insn, 24), U = (int)BIT(insn, 23), W = (int)BIT(insn, 21), L = (int)BIT(insn, 20);
  int n = (int)BITS(insn, 19, 16), dbl = (int)BIT(insn, 8);
  uint32_t imm8 = insn & 0xFF;
  int d = dbl ? (int)(BIT(insn, 22) << 4 | BITS(insn, 15, 12)) : (int)(BITS(insn, 15, 12) << 1 | BIT(insn, 22));
  uint32_t base = n == 15 ? s->r[15] & ~3u : s->r[n];
  if (P && !W) { /* VLDR, VSTR */
    uint32_t a = U ? base + imm8 * 4 : base - imm8 * 4;
    if (dbl) {
      if (L) {
        uint32_t lo = rd32(s, a), hi = rd32(s, a + 4);
        s->x.d[d] = (uint64_t)hi << 32 | lo;
      } else {
        wr32(s, a, (uint32_t)s->x.d[d]);
        wr32(s, a + 4, (uint32_t)(s->x.d[d] >> 32));
      }
    } else {
      if (L) s->x.s[d] = rd32(s, a);
      else wr32(s, a, s->x.s[d]);
    }
    return;
  }
  if (P == U) { /* P=0,U=0 is the 64-bit transfers; P=1,U=1 undefined */
    interp_undefined(s, insn, "VFP load/store multiple");
    return;
  }
  uint32_t a = U ? base : base - imm8 * 4;
  /* d0-d31 / s0-s63 as words: a double list is words 2d.. */
  uint32_t first = dbl ? (uint32_t)d * 2 : (uint32_t)d;
  uint32_t words = dbl ? (imm8 & ~1u) : imm8;
  if (first + words > 64 || words == 0) {
    interp_undefined(s, insn, "VFP register list");
    return;
  }
  for (uint32_t i = 0; i < words; i++) {
    if (L) s->x.s[first + i] = rd32(s, a + 4 * i);
    else wr32(s, a + 4 * i, s->x.s[first + i]);
  }
  if (W) s->r[n] = U ? base + imm8 * 4 : base - imm8 * 4;
}

/* ---- transfers between core and extension registers ---- */

static void vfp_transfer(icpu *s, uint32_t insn) {
  int L = (int)BIT(insn, 20), C = (int)BIT(insn, 8), t = (int)BITS(insn, 15, 12);
  uint32_t A = BITS(insn, 23, 21);
  if (!C) {
    if (A == 0) { /* VMOV core <-> single */
      int n = (int)(BITS(insn, 19, 16) << 1 | BIT(insn, 7));
      if (L) s->r[t] = s->x.s[n];
      else s->x.s[n] = s->r[t];
      return;
    }
    if (A == 7 && BITS(insn, 19, 16) == 1) { /* VMRS, VMSR FPSCR */
      if (L) {
        fp_sync(s);
        if (t == 15) {
          s->n = s->fpscr >> 31;
          s->z = (s->fpscr >> 30) & 1;
          s->c = (s->fpscr >> 29) & 1;
          s->v = (s->fpscr >> 28) & 1;
        } else {
          s->r[t] = s->fpscr;
        }
      } else {
        fp_sync(s);
        s->fpscr = s->r[t] & 0xFFF7009Fu; /* trap enables, reserved bits: RAZ */
        interp_fp_neon_end(s); /* installs the new mode bits */
      }
      return;
    }
    interp_undefined(s, insn, "VMRS/VMSR register");
    return;
  }
  int dreg = (int)(BIT(insn, 7) << 4 | BITS(insn, 19, 16));
  if (!L) {
    if (A & 4) { /* VDUP core */
      if (BIT(insn, 6)) goto undef;
      int Q = (int)BIT(insn, 21), be = (int)(BIT(insn, 22) << 1 | BIT(insn, 5));
      uint32_t v = s->r[t];
      uint64_t rep;
      switch (be) {
      case 0: rep = (uint64_t)v << 32 | v; break;
      case 1: rep = (v & 0xFFFF) * 0x0001000100010001ull; break;
      case 2: rep = (v & 0xFF) * 0x0101010101010101ull; break;
      default: goto undef;
      }
      if (Q && (dreg & 1)) goto undef;
      s->x.d[dreg] = rep;
      if (Q) s->x.d[dreg + 1] = rep;
      return;
    }
    uint32_t opc = BITS(insn, 22, 21) << 2 | BITS(insn, 6, 5);
    uint8_t *p = (uint8_t *)&s->x.d[dreg];
    uint32_t v = s->r[t];
    if (opc & 8) p[opc & 7] = (uint8_t)v;
    else if (opc & 1) memcpy(p + 2 * ((opc >> 1) & 3), &v, 2);
    else if ((opc & 3) == 0) memcpy(p + 4 * ((opc >> 2) & 1), &v, 4);
    else goto undef;
    return;
  }
  { /* VMOV scalar to core */
    uint32_t opc = BITS(insn, 22, 21) << 2 | BITS(insn, 6, 5);
    int U = (int)BIT(insn, 23);
    const uint8_t *p = (const uint8_t *)&s->x.d[dreg];
    if (opc & 8) {
      uint8_t b = p[opc & 7];
      s->r[t] = U ? b : (uint32_t)(int8_t)b;
    } else if (opc & 1) {
      uint16_t h;
      memcpy(&h, p + 2 * ((opc >> 1) & 3), 2);
      s->r[t] = U ? h : (uint32_t)(int16_t)h;
    } else if ((opc & 3) == 0 && !U) {
      memcpy(&s->r[t], p + 4 * ((opc >> 2) & 1), 4);
    } else {
      goto undef;
    }
    return;
  }
undef:
  interp_undefined(s, insn, "VFP/NEON register transfer");
}

static void vfp_transfer64(icpu *s, uint32_t insn) {
  int L = (int)BIT(insn, 20), t2 = (int)BITS(insn, 19, 16), t = (int)BITS(insn, 15, 12);
  if (BIT(insn, 8)) { /* double */
    int m = (int)(BIT(insn, 5) << 4 | BITS(insn, 3, 0));
    if (L) {
      s->r[t] = (uint32_t)s->x.d[m];
      s->r[t2] = (uint32_t)(s->x.d[m] >> 32);
    } else {
      s->x.d[m] = (uint64_t)s->r[t2] << 32 | s->r[t];
    }
  } else {
    int m = (int)(BITS(insn, 3, 0) << 1 | BIT(insn, 5));
    if (m == 63) {
      interp_undefined(s, insn, "VMOV two singles");
      return;
    }
    if (L) {
      s->r[t] = s->x.s[m];
      s->r[t2] = s->x.s[m + 1];
    } else {
      s->x.s[m] = s->r[t];
      s->x.s[m + 1] = s->r[t2];
    }
  }
}

void interp_vfp(icpu *s, uint32_t insn) {
  uint32_t op = BITS(insn, 27, 24);
  if (op == 0xE) {
    if (BIT(insn, 4)) vfp_transfer(s, insn);
    else vfp_dp(s, insn);
  } else if (BITS(insn, 27, 21) == 0x62) {
    vfp_transfer64(s, insn);
  } else {
    vfp_ldst(s, insn);
  }
}

/* ---- CP15 ---- */

static void cp15(icpu *s, uint32_t insn) {
  if (BITS(insn, 27, 24) == 0xE && BIT(insn, 4)) { /* MCR, MRC */
    uint32_t opc1 = BITS(insn, 23, 21), crn = BITS(insn, 19, 16), crm = BITS(insn, 3, 0);
    uint32_t opc2 = BITS(insn, 7, 5);
    int L = (int)BIT(insn, 20), t = (int)BITS(insn, 15, 12);
    if (L && s->cp15_read_hook && t != 15) {
      s->r[15] = s->next_pc;
      interp_fp_leave(s);
      s->r[t] = s->cp15_read_hook(s, insn);
      interp_fp_enter(s);
      return;
    }
    if (opc1 == 0 && crn == 13 && crm == 0 && t != 15) {
      if (opc2 == 3 && L) {
        s->r[t] = s->tpidruro;
        return;
      }
      if (opc2 == 2) {
        if (L) s->r[t] = s->tpidrurw;
        else s->tpidrurw = s->r[t];
        return;
      }
    }
    if (!L && opc1 == 0 && crn == 7 &&
        ((crm == 10 && (opc2 == 4 || opc2 == 5)) || (crm == 5 && opc2 == 4))) {
      interp_barrier(); /* CP15 DSB, DMB, ISB */
      return;
    }
  }
  if (BITS(insn, 27, 20) == 0xC5 && BITS(insn, 7, 4) == 1 && BITS(insn, 3, 0) == 14) {
    /* MRRC p15, 1, Rt, Rt2, c14: CNTVCT */
    uint64_t v = mach_absolute_time();
    s->r[BITS(insn, 15, 12)] = (uint32_t)v;
    s->r[BITS(insn, 19, 16)] = (uint32_t)(v >> 32);
    return;
  }
  interp_undefined(s, insn, "CP15");
}

void interp_coproc(icpu *s, uint32_t insn) {
  uint32_t coproc = BITS(insn, 11, 8);
  if ((insn >> 28) != 0xF) {
    if ((coproc & 0xE) == 0xA) {
      interp_vfp(s, insn);
      return;
    }
    if (coproc == 15) {
      cp15(s, insn);
      return;
    }
  }
  interp_undefined(s, insn, "coprocessor");
}
