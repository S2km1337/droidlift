/*
 * Helpers and macros for the C that droidlift-recomp generates.
 *
 * Every generated file includes this header. The generated functions keep
 * r0-r14 and the NZCV flags in C locals (so clang allocates them to host
 * registers) and the VFP/NEON registers in s->ext; the macros below assume
 * those local names: s, m (g_mem), B (the module's load address), r0..r14,
 * fn fz fc fv, npc, and ex_a/ex_v/ex_on for the exclusive monitor.
 *
 * Build generated code with -ffp-contract=off -fno-math-errno: VFP
 * multiply-accumulate rounds twice, and sqrt must stay one instruction.
 */
#ifndef RC_GEN_H
#define RC_GEN_H

#include <stdint.h>
#include <string.h>

#include "rc_runtime.h"

#define RC_SYNC_IN()                                                                                         \
  (r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3], r4 = s->r[4], r5 = s->r[5], r6 = s->r[6],         \
   r7 = s->r[7], r8 = s->r[8], r9 = s->r[9], r10 = s->r[10], r11 = s->r[11], r12 = s->r[12], r13 = s->r[13], \
   r14 = s->r[14], fn = s->cpsr >> 31, fz = (s->cpsr >> 30) & 1, fc = (s->cpsr >> 29) & 1,                   \
   fv = (s->cpsr >> 28) & 1)
#define RC_SYNC_OUT()                                                                                       \
  (s->r[0] = r0, s->r[1] = r1, s->r[2] = r2, s->r[3] = r3, s->r[4] = r4, s->r[5] = r5, s->r[6] = r6,        \
   s->r[7] = r7, s->r[8] = r8, s->r[9] = r9, s->r[10] = r10, s->r[11] = r11, s->r[12] = r12,                \
   s->r[13] = r13, s->r[14] = r14,                                                                          \
   s->cpsr = (s->cpsr & ~RC_CPSR_NZCV) | fn << 31 | fz << 30 | fc << 29 | fv << 28)

/* The locals every generated function starts with. */
#define RC_LOCALS(base)                                                                     \
  uint8_t *const m = g_mem;                                                                 \
  const uint32_t B = (base);                                                                \
  uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14, fn, fz, fc, fv; \
  uint32_t npc;                                                                             \
  uint32_t ex_a = 0, ex_on = 0;                                                             \
  uint64_t ex_v = 0;                                                                        \
  (void)m;                                                                                  \
  (void)ex_a;                                                                               \
  (void)ex_on;                                                                              \
  (void)ex_v

/* Test builds (the differential test) count function entries and backward
 * branches and give up on runaway inputs; release builds compile it away. */
#ifdef RC_TEST_BUDGET
extern int64_t rc_test_budget;
void rc_test_bailout(void);
void rc_test_exclusive(uint32_t addr); /* sees every LDREX/STREX address */
#define RC_TICK()                                \
  do {                                           \
    if (--rc_test_budget < 0) rc_test_bailout(); \
  } while (0)
#define RC_EXCL(a) rc_test_exclusive(a)
void rc_test_block(uint32_t addr); /* each basic block entered (a label) */
#define RC_BLOCK(a) rc_test_block(a)
#else
#define RC_TICK() ((void)0)
#define RC_EXCL(a) ((void)0)
#define RC_BLOCK(a) ((void)0)
#endif

/* Leaves the function: control continues at interworking address pc. */
#define EXIT(pc)    \
  do {              \
    npc = (pc);     \
    goto L_exit;    \
  } while (0)

/* The guest code at this point couldn't be recompiled: hand it to the
 * dispatcher (and so the interpreter fallback) at interworking address pc,
 * with ITSTATE it (0 outside IT blocks) in the CPSR. */
#define UNSUP(pc, it)                                                                            \
  do {                                                                                           \
    RC_SYNC_OUT();                                                                               \
    s->cpsr = (s->cpsr & ~0x0600fc00u) | ((uint32_t)(it) & 3u) << 25 | ((uint32_t)(it) >> 2) << 10; \
    rc_set_pc(s, pc);                                                                            \
    return;                                                                                      \
  } while (0)

/* A call (BL/BLX) to recompiled function f (CALL) or through the dispatcher
 * (CALLX). After the call the function continues, unless the guest stack
 * frame was unwound past this one (longjmp, exceptions): then it leaves for
 * wherever the pc now is (rc_call). */
#define CALL(f, tgt, ret)                                  \
  do {                                                     \
    uint32_t sp_ = r13;                                    \
    RC_SYNC_OUT();                                         \
    rc_set_pc(s, tgt);                                     \
    f(s);                                                  \
    if (rc_get_pc(s) != (ret) && rc_call(s, ret, sp_)) {   \
      RC_SYNC_IN();                                        \
      EXIT(rc_get_pc(s));                                  \
    }                                                      \
    RC_SYNC_IN();                                          \
  } while (0)
#define CALLX(tgt, ret)                                    \
  do {                                                     \
    uint32_t sp_ = r13;                                    \
    RC_SYNC_OUT();                                         \
    rc_set_pc(s, tgt);                                     \
    if (rc_call(s, ret, sp_)) {                            \
      RC_SYNC_IN();                                        \
      EXIT(rc_get_pc(s));                                  \
    }                                                      \
    RC_SYNC_IN();                                          \
  } while (0)

/* SVC: the host handles it (rc_svc); it may redirect the pc. */
#define SVC(imm, next)                                    \
  do {                                                    \
    RC_SYNC_OUT();                                        \
    rc_set_pc(s, next);                                   \
    rc_svc(s, imm);                                       \
    RC_SYNC_IN();                                         \
    if (rc_get_pc(s) != (next)) EXIT(rc_get_pc(s));      \
  } while (0)

/* ---- Memory --------------------------------------------------------------- */

#define LD8(a) rc_ld8(m, (a))
#define LD16(a) rc_ld16(m, (a))
#define LD32(a) rc_ld32(m, (a))
#define LDS8(a) ((uint32_t)(int32_t)(int8_t)rc_ld8(m, (a)))
#define LDS16(a) ((uint32_t)(int32_t)(int16_t)rc_ld16(m, (a)))
#define ST8(a, v) rc_st8(m, (a), (v))
#define ST16(a, v) rc_st16(m, (a), (v))
#define ST32(a, v) rc_st32(m, (a), (v))
static inline uint64_t rc_ld64(uint8_t *m, uint32_t a) {
  uint64_t v;
  memcpy(&v, m + a, 8);
  return v;
}
static inline void rc_st64(uint8_t *m, uint32_t a, uint64_t v) { memcpy(m + a, &v, 8); }

/* Exclusive store: succeeds if memory still holds what the LDREX saw (a
 * compare-and-swap, like the dynarmic backend's monitor). 0 = stored. */
#define STREX_(T, a, v)                                                                          \
  (ex_on && ex_a == (a) &&                                                                       \
           __atomic_compare_exchange_n((T *)(m + (a)), &(T){(T)ex_v}, (T)(v), 0, __ATOMIC_SEQ_CST, \
                                       __ATOMIC_SEQ_CST)                                         \
       ? 0u                                                                                      \
       : 1u)

/* ---- Integer ALU ---------------------------------------------------------- */

#define NZ(v) (fn = (uint32_t)(v) >> 31, fz = (uint32_t)(v) == 0)

static inline uint32_t rc_adds(uint32_t a, uint32_t b, uint32_t cin, uint32_t *n, uint32_t *z, uint32_t *c,
                               uint32_t *v) {
  uint64_t u = (uint64_t)a + b + cin;
  uint32_t r = (uint32_t)u;
  *n = r >> 31;
  *z = r == 0;
  *c = (uint32_t)(u >> 32);
  *v = ((a ^ r) & (b ^ r)) >> 31;
  return r;
}
#define ADDS(a, b, cin) rc_adds((a), (b), (cin), &fn, &fz, &fc, &fv)

/* Saturation; *q set when it saturates. */
static inline int32_t rc_ssat(int64_t v, uint32_t bits, uint32_t *q) {
  int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -((int64_t)1 << (bits - 1));
  if (v > hi) return *q = 1, (int32_t)hi;
  if (v < lo) return *q = 1, (int32_t)lo;
  return (int32_t)v;
}
static inline uint32_t rc_usat(int64_t v, uint32_t bits, uint32_t *q) {
  int64_t hi = bits >= 32 ? 0xffffffffll : ((int64_t)1 << bits) - 1;
  if (v > hi) return *q = 1, (uint32_t)hi;
  if (v < 0) return *q = 1, 0;
  return (uint32_t)v;
}
#define SETQ(q) (s->cpsr |= (q) ? 0x08000000u : 0u)

/* Parallel add/subtract (SADD16 ... UHSUB8). pre: 1 S, 2 Q, 3 SH, 5 U, 6 UQ,
 * 7 UH; op: 0 ADD16, 1 ASX, 2 SAX, 3 SUB16, 4 ADD8, 7 SUB8. Sets GE for S and
 * U. */
static inline uint32_t rc_parallel(int pre, int op, uint32_t n, uint32_t m, uint32_t *cpsr) {
  int sgn = pre <= 3, sat = pre == 2 || pre == 6, halve = pre == 3 || pre == 7, ge = pre == 1 || pre == 5;
  int lanes = op >= 4 ? 4 : 2, w = op >= 4 ? 8 : 16;
  uint32_t res = 0, gebits = 0;
  for (int i = 0; i < lanes; i++) {
    int64_t a, b, r;
    uint32_t mask = (1u << w) - 1;
    uint32_t ua = (n >> (i * w)) & mask, ub;
    int sub;
    if (op == 1 || op == 2) { /* ASX: lo = a.lo - b.hi, hi = a.hi + b.lo; SAX the other way */
      ub = (m >> ((1 - i) * w)) & mask;
      sub = (op == 1) ? (i == 0) : (i == 1);
    } else {
      ub = (m >> (i * w)) & mask;
      sub = op == 3 || op == 7;
    }
    if (sgn) {
      a = (int64_t)((int32_t)(ua << (32 - w)) >> (32 - w));
      b = (int64_t)((int32_t)(ub << (32 - w)) >> (32 - w));
    } else {
      a = ua;
      b = ub;
    }
    r = sub ? a - b : a + b;
    if (ge && (sgn ? r >= 0 : (sub ? r >= 0 : r >= (int64_t)1 << w)))
      gebits |= (w == 16 ? 3u : 1u) << (i * (w / 8));
    if (halve) r >>= 1;
    if (sat) {
      int64_t hi = sgn ? ((int64_t)1 << (w - 1)) - 1 : ((int64_t)1 << w) - 1, lo = sgn ? -((int64_t)1 << (w - 1)) : 0;
      if (r > hi) r = hi;
      if (r < lo) r = lo;
    }
    res |= ((uint32_t)r & mask) << (i * w);
  }
  if (ge) *cpsr = (*cpsr & ~0x000f0000u) | gebits << 16;
  return res;
}

/* ---- VFP -------------------------------------------------------------------- */

/* Register reads are opaque to the optimizer: LLVM treats the sign and
 * payload of a NaN result as unspecified, so with a known operand (a folded
 * literal) it may rewrite c - x as (-x) + c, which flips a propagated NaN's
 * sign where ARM keeps it. Opaque operands keep each VFP operation one
 * arm64 instruction with ARM's NaN rules. */
static inline float rc_getf(const rc_state *s, int i) {
  float f;
  memcpy(&f, &s->ext[i], 4);
  __asm__("" : "+w"(f));
  return f;
}
static inline void rc_setf(rc_state *s, int i, float f) { memcpy(&s->ext[i], &f, 4); }
static inline double rc_getd(const rc_state *s, int i) {
  double f;
  memcpy(&f, &s->ext[2 * i], 8);
  __asm__("" : "+w"(f));
  return f;
}
static inline void rc_setd(rc_state *s, int i, double f) { memcpy(&s->ext[2 * i], &f, 8); }
#define FS(i) rc_getf(s, i)
#define FD(i) rc_getd(s, i)
#define SFS(i, v) rc_setf(s, i, v)
#define SFD(i, v) rc_setd(s, i, v)
#define DW(i) (s->ext[2 * (i)] | (uint64_t)s->ext[2 * (i) + 1] << 32)
#define SDW(i, v) (s->ext[2 * (i)] = (uint32_t)(v), s->ext[2 * (i) + 1] = (uint32_t)((uint64_t)(v) >> 32))

/* Arithmetic with ARM's operand order. With two NaN operands ARM returns the
 * first (FPProcessNaNs), and arm64 does too, but LLVM treats fadd/fmul as
 * commutative and may swap them. So on arm64 each operation is the one
 * instruction, operands as written. */
#if defined(__aarch64__)
#define RC_FOP(name, ins, T, mod)                                    \
  static inline T name(T a, T b) {                                   \
    T r;                                                             \
    __asm__(ins " %" mod "0, %" mod "1, %" mod "2" : "=w"(r) : "w"(a), "w"(b)); \
    return r;                                                        \
  }
RC_FOP(rc_fadd, "fadd", float, "s")
RC_FOP(rc_fsub, "fsub", float, "s")
RC_FOP(rc_fmul, "fmul", float, "s")
RC_FOP(rc_fdiv, "fdiv", float, "s")
RC_FOP(rc_dadd, "fadd", double, "d")
RC_FOP(rc_dsub, "fsub", double, "d")
RC_FOP(rc_dmul, "fmul", double, "d")
RC_FOP(rc_ddiv, "fdiv", double, "d")
#undef RC_FOP
#else
static inline float rc_fadd(float a, float b) { return a + b; }
static inline float rc_fsub(float a, float b) { return a - b; }
static inline float rc_fmul(float a, float b) { return a * b; }
static inline float rc_fdiv(float a, float b) { return a / b; }
static inline double rc_dadd(double a, double b) { return a + b; }
static inline double rc_dsub(double a, double b) { return a - b; }
static inline double rc_dmul(double a, double b) { return a * b; }
static inline double rc_ddiv(double a, double b) { return a / b; }
#endif

/* FPNeg kept opaque, so clang can't turn a + (-b) into a - b (which differs
 * from ARM in the sign of a propagated NaN). */
static inline float rc_negf(float x) {
  x = -x;
  __asm__("" : "+w"(x));
  return x;
}
static inline double rc_negd(double x) {
  x = -x;
  __asm__("" : "+w"(x));
  return x;
}

/* VCMP: FPSCR NZCV for a compared with b. */
static inline uint32_t rc_fcmp(double a, double b) {
  if (a != a || b != b) return 0x3u;
  if (a == b) return 0x6u;
  return a < b ? 0x8u : 0x2u;
}
#define FCMP(a, b) (s->fpscr = (s->fpscr & 0x0fffffffu) | rc_fcmp((a), (b)) << 28)

/* Float -> integer, saturating, NaN -> 0 (FPToFixed). round_zero selects
 * truncation; otherwise round to nearest even (FPSCR's default mode). */
static inline double rc_round(double x, int round_zero) {
  return round_zero ? __builtin_trunc(x) : __builtin_rint(x);
}
static inline uint32_t rc_f2s(double x, int round_zero, int bits) {
  if (x != x) return 0;
  x = rc_round(x, round_zero);
  double hi = (double)(((int64_t)1 << (bits - 1)) - 1), lo = -(double)((int64_t)1 << (bits - 1));
  if (x >= hi) return (uint32_t)(int32_t)(int64_t)hi;
  if (x <= lo) return (uint32_t)(int32_t)(int64_t)lo;
  return (uint32_t)(int32_t)(int64_t)x;
}
static inline uint32_t rc_f2u(double x, int round_zero, int bits) {
  if (x != x) return 0;
  x = rc_round(x, round_zero);
  double hi = (double)(((int64_t)1 << bits) - 1);
  if (x >= hi) return (uint32_t)(int64_t)hi;
  if (x <= 0) return 0;
  return (uint32_t)(int64_t)x;
}

/* ---- Advanced SIMD ---------------------------------------------------------- */

/* NEON float arithmetic always uses the "standard FPSCR value": denormal
 * inputs and results flush to zero (keeping the sign), and every NaN result
 * is the default NaN. Lanes are f32 bit patterns. */
static inline float rc_bf(uint32_t x) {
  float f;
  memcpy(&f, &x, 4);
  return f;
}
static inline uint32_t rc_fb(float f) {
  uint32_t x;
  memcpy(&x, &f, 4);
  return x;
}
static inline uint32_t rc_nfz(uint32_t x) { return (x & 0x7f800000u) ? x : x & 0x80000000u; }
static inline uint32_t rc_nres(float f) {
  uint32_t x = rc_fb(f);
  return (x & 0x7fffffffu) > 0x7f800000u ? 0x7fc00000u : rc_nfz(x);
}
static inline uint32_t rc_nadd(uint32_t a, uint32_t b) { return rc_nres(rc_bf(rc_nfz(a)) + rc_bf(rc_nfz(b))); }
static inline uint32_t rc_nsub(uint32_t a, uint32_t b) { return rc_nres(rc_bf(rc_nfz(a)) - rc_bf(rc_nfz(b))); }
static inline uint32_t rc_nmul(uint32_t a, uint32_t b) { return rc_nres(rc_bf(rc_nfz(a)) * rc_bf(rc_nfz(b))); }
/* VMLA/VMLS: the product is rounded (and flushed) before the add. */
static inline uint32_t rc_nmla(uint32_t d, uint32_t a, uint32_t b) { return rc_nadd(d, rc_nmul(a, b)); }
static inline uint32_t rc_nmls(uint32_t d, uint32_t a, uint32_t b) { return rc_nsub(d, rc_nmul(a, b)); }

/* Lane-wise integer add/subtract of 8/16/32-bit lanes packed in a word. */
static inline uint32_t rc_vadd(uint32_t a, uint32_t b, int esize) {
  uint32_t h = esize == 8 ? 0x80808080u : esize == 16 ? 0x80008000u : 0;
  return ((a & ~h) + (b & ~h)) ^ ((a ^ b) & h);
}
static inline uint32_t rc_vsub(uint32_t a, uint32_t b, int esize) {
  uint32_t h = esize == 8 ? 0x80808080u : esize == 16 ? 0x80008000u : 0;
  return ((a | h) - (b & ~h)) ^ ((a ^ ~b) & h);
}

/* Byte address of lane e (ebytes wide) of D register dreg. */
#define LANE(dreg, e, ebytes) ((uint8_t *)&s->ext[2 * (dreg)] + (e) * (ebytes))

#endif
