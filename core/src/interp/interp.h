/*
 * Our ARMv7 interpreter: the no-JIT CPU backend (7).
 *
 * User-mode ARMv7-A as Android runs it: ARM and Thumb-2 with interworking,
 * VFPv3 and NEON, the CP15 registers user code may touch, and ldrex/strex as
 * compare-and-swap like the dynarmic backend. Pure C: no JIT and no
 * executable memory, so it runs on iOS as a normal app.
 *
 * Guest memory is g_mem + address, like dynarmic's fastmem. In "checked"
 * mode (differential tests) every access is checked against the guest
 * regions first and an unmapped one faults the guest instead of the host.
 *
 * Internal header shared by the interpreter's source files.
 */
#ifndef INTERP_H
#define INTERP_H

#include <stdint.h>
#include <string.h>

#include "../cpu.h"
#include "../guest_mem.h"

#if defined(__GNUC__)
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define likely(x) (x)
#define unlikely(x) (x)
#define ALWAYS_INLINE inline
#endif

typedef struct icpu {
  cpu base; /* must stay first: cpu * <-> icpu * */
  /* r[15] holds the address of the next instruction between instructions.
   * While one executes, it reads as that instruction's address + 8 (ARM) or
   * + 4 (Thumb), and writes to the PC go to next_pc. */
  uint32_t r[16];
  uint32_t next_pc;
  uint32_t n, z, c, v, q; /* APSR flags, each 0 or 1 */
  uint32_t ge;            /* APSR.GE, 4 bits */
  uint32_t t;             /* CPSR.T */
  uint32_t it;            /* ITSTATE, 8 bits */
  uint32_t cpsr_rest;     /* mode, E, A, I, F and the other CPSR bits, kept as set */
  union {
    uint64_t d[32];
    uint32_t s[64];
    uint8_t b[256];
  } x __attribute__((aligned(16)));
  uint32_t fpscr;
  /* Exclusive monitor: the address, size and value the last ldrex saw. */
  uint32_t excl_addr, excl_size, excl_valid;
  uint64_t excl_val;
  uint32_t tpidrurw, tpidruro;
  volatile int halt_req;
  int checked;          /* check memory accesses (differential tests) */
  uint32_t win_lo, win_hi; /* checked mode: accesses must also fall in here (if win_hi) */
  int fault;            /* the current run faulted: an INTERP_FAULT_* */
  /* Optional host hooks (the recompiler fallback, interp_rc.c): SVCs and
   * MRC p15 reads go here instead of cpu_svc and the CP15 registers. raw is
   * the instruction in ARM form. */
  void (*svc_hook)(struct icpu *s, uint32_t imm);
  uint32_t (*cp15_read_hook)(struct icpu *s, uint32_t raw);
} icpu;

static inline icpu *ICPU(cpu *c) { return (icpu *)c; }

/* ---- faults ---- */

enum { INTERP_FAULT_NONE, INTERP_FAULT_MEMORY, INTERP_FAULT_WINDOW, INTERP_FAULT_UNDEFINED,
       INTERP_FAULT_OTHER };

void interp_fault(icpu *s, int kind, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void interp_undefined(icpu *s, uint32_t insn, const char *what);
int interp_mapped(icpu *s, uint32_t a, uint32_t n, int write);

/* ---- memory ---- */

#define MEM_LOAD(T, name)                                                                          \
  static ALWAYS_INLINE T name(icpu *s, uint32_t a) {                                               \
    T v;                                                                                           \
    if (unlikely(s->checked) && !interp_mapped(s, a, sizeof(T), 0)) return 0;                      \
    memcpy(&v, g_mem + a, sizeof(T));                                                              \
    return v;                                                                                      \
  }
#define MEM_STORE(T, name)                                                                         \
  static ALWAYS_INLINE void name(icpu *s, uint32_t a, T v) {                                       \
    if (unlikely(s->checked) && !interp_mapped(s, a, sizeof(T), 1)) return;                        \
    memcpy(g_mem + a, &v, sizeof(T));                                                              \
  }
MEM_LOAD(uint8_t, rd8)
MEM_LOAD(uint16_t, rd16)
MEM_LOAD(uint32_t, rd32)
MEM_LOAD(uint64_t, rd64)
MEM_STORE(uint8_t, wr8)
MEM_STORE(uint16_t, wr16)
MEM_STORE(uint32_t, wr32)
MEM_STORE(uint64_t, wr64)

/* ldrex/strex/clrex. size in bytes (1, 2, 4, 8). */
int interp_aligned(icpu *s, uint32_t a, int size);
uint64_t interp_ldrex(icpu *s, uint32_t a, int size);
/* Returns 0 if the store happened, 1 if not (the strex status). */
uint32_t interp_strex(icpu *s, uint32_t a, int size, uint64_t v);
static inline void interp_clrex(icpu *s) { s->excl_valid = 0; }

/* ---- registers and the PC ---- */

/* Branch to an address in the current state (BranchWritePC). */
static ALWAYS_INLINE void branch_to(icpu *s, uint32_t a) { s->next_pc = a & (s->t ? ~1u : ~3u); }

/* Branch with interworking (BXWritePC, also LoadWritePC on ARMv7). */
static ALWAYS_INLINE void bx_to(icpu *s, uint32_t a) {
  if (a & 1) {
    s->t = 1;
    s->next_pc = a & ~1u;
  } else {
    s->t = 0;
    s->next_pc = a & ~3u;
  }
}

/* Register write from a load (LoadWritePC for the PC). */
static ALWAYS_INLINE void set_reg_load(icpu *s, int d, uint32_t v) {
  if (d == 15) bx_to(s, v);
  else s->r[d] = v;
}

/* Register write from a data-processing instruction (ALUWritePC: ARM state
 * interworks, Thumb state doesn't). */
static ALWAYS_INLINE void set_reg_alu(icpu *s, int d, uint32_t v) {
  if (d == 15) {
    if (s->t) branch_to(s, v);
    else bx_to(s, v);
  } else {
    s->r[d] = v;
  }
}

/* ---- flags and conditions ---- */

static ALWAYS_INLINE void set_nz(icpu *s, uint32_t r) {
  s->n = r >> 31;
  s->z = r == 0;
}

static ALWAYS_INLINE int cond_pass(icpu *s, uint32_t cond) {
  int r;
  switch (cond >> 1) {
  case 0: r = s->z; break;
  case 1: r = s->c; break;
  case 2: r = s->n; break;
  case 3: r = s->v; break;
  case 4: r = s->c && !s->z; break;
  case 5: r = s->n == s->v; break;
  case 6: r = !s->z && s->n == s->v; break;
  default: return 1;
  }
  return (cond & 1) ? !r : r;
}

uint32_t interp_get_cpsr(icpu *s);
void interp_set_cpsr(icpu *s, uint32_t v);

/* ---- ALU helpers ---- */

static ALWAYS_INLINE uint32_t add_c(uint32_t a, uint32_t b, uint32_t cin, uint32_t *cout,
                                    uint32_t *vout) {
  uint64_t u = (uint64_t)a + b + cin;
  int64_t si = (int64_t)(int32_t)a + (int32_t)b + cin;
  uint32_t r = (uint32_t)u;
  *cout = (uint32_t)(u >> 32);
  *vout = (int64_t)(int32_t)r != si;
  return r;
}

enum { SH_LSL, SH_LSR, SH_ASR, SH_ROR, SH_RRX };

/* Shift_C with an amount already decoded (DecodeImmShift or a register). */
static ALWAYS_INLINE uint32_t shift_c(uint32_t v, int type, uint32_t amt, uint32_t cin,
                                      uint32_t *cout) {
  if (type == SH_RRX) {
    *cout = v & 1;
    return (v >> 1) | (cin << 31);
  }
  if (amt == 0) {
    *cout = cin;
    return v;
  }
  switch (type) {
  case SH_LSL:
    if (amt < 32) {
      *cout = (v >> (32 - amt)) & 1;
      return v << amt;
    }
    *cout = amt == 32 ? v & 1 : 0;
    return 0;
  case SH_LSR:
    if (amt < 32) {
      *cout = (v >> (amt - 1)) & 1;
      return v >> amt;
    }
    *cout = amt == 32 ? v >> 31 : 0;
    return 0;
  case SH_ASR:
    if (amt < 32) {
      *cout = ((int32_t)v >> (amt - 1)) & 1;
      return (uint32_t)((int32_t)v >> amt);
    }
    *cout = v >> 31;
    return (uint32_t)((int32_t)v >> 31);
  default: { /* ROR */
    uint32_t a = amt & 31;
    uint32_t r = a ? (v >> a) | (v << (32 - a)) : v;
    *cout = r >> 31;
    return r;
  }
  }
}

/* DecodeImmShift: type and imm5 of an immediate shift to (type, amount). */
static ALWAYS_INLINE void decode_imm_shift(int type, uint32_t imm5, int *t, uint32_t *amt) {
  switch (type) {
  case 0: *t = SH_LSL; *amt = imm5; break;
  case 1: *t = SH_LSR; *amt = imm5 ? imm5 : 32; break;
  case 2: *t = SH_ASR; *amt = imm5 ? imm5 : 32; break;
  default:
    if (imm5) {
      *t = SH_ROR;
      *amt = imm5;
    } else {
      *t = SH_RRX;
      *amt = 1;
    }
  }
}

/* Data-processing opcodes, in the ARM encoding's order. */
enum { DP_AND, DP_EOR, DP_SUB, DP_RSB, DP_ADD, DP_ADC, DP_SBC, DP_RSC,
       DP_TST, DP_TEQ, DP_CMP, DP_CMN, DP_ORR, DP_MOV, DP_BIC, DP_MVN, DP_ORN };

/* Runs a data-processing operation on a and the shifted operand b (whose
 * shifter carry is sc), writes Rd unless it's a test, and sets the flags if
 * asked. */
void interp_dp(icpu *s, int op, int setflags, int d, uint32_t a, uint32_t b, uint32_t sc);

/* ---- shared ARM/Thumb-2 operations (interp_media.c) ---- */

/* Parallel add/subtract. kind: 0 S, 1 Q, 2 SH, 4 U, 5 UQ, 6 UH;
 * op: 0 ADD16, 1 ASX, 2 SAX, 3 SUB16, 4 ADD8, 7 SUB8. */
uint32_t interp_parallel(icpu *s, int kind, int op, uint32_t a, uint32_t b);
uint32_t interp_ssat(icpu *s, int32_t v, int bits);  /* bits 1..32, sets Q */
uint32_t interp_usat(icpu *s, int32_t v, int bits);  /* bits 0..31, sets Q */
uint32_t interp_ssat16(icpu *s, uint32_t v, int bits);
uint32_t interp_usat16(icpu *s, uint32_t v, int bits);
uint32_t interp_qadd(icpu *s, uint32_t a, uint32_t b, int sub, int dbl);
uint32_t interp_sel(icpu *s, uint32_t a, uint32_t b);
uint32_t interp_usad8(uint32_t a, uint32_t b);
uint32_t interp_rbit(uint32_t v);
uint32_t interp_clz(uint32_t v);
/* Extend: op 0 sxtb16, 2 sxtb, 3 sxth, 4 uxtb16, 6 uxtb, 7 uxth (bits 22:20
 * of the ARM encoding); v is already rotated, add is 0 for the plain forms. */
uint32_t interp_extend(int op, uint32_t add, uint32_t v);
/* Signed multiplies of the SMLA/SMUL/SMLAW/SMUAD family; sets Q as needed. */
uint32_t interp_smla(icpu *s, int nhigh, int mhigh, uint32_t n, uint32_t m, uint32_t a, int acc);
uint32_t interp_smlaw(icpu *s, int mhigh, uint32_t n, uint32_t m, uint32_t a, int acc);
uint32_t interp_smlad(icpu *s, int swap, int sub, uint32_t n, uint32_t m, uint32_t a, int acc);
uint64_t interp_smlald(int swap, int sub, uint32_t n, uint32_t m, uint64_t a);
uint32_t interp_smmla(int round, int op, uint32_t n, uint32_t m, uint32_t a, int acc);
uint32_t interp_sdiv(uint32_t n, uint32_t m);
uint32_t interp_udiv(uint32_t n, uint32_t m);

/* ---- shared instruction bodies ---- */

/* SVC: the handler sees the PC of the next instruction and may change it. */
void interp_svc(icpu *s, uint32_t imm);
/* LDM/STM. before: the DB/IB forms; inc: the IA/IB forms. */
void interp_ldm(icpu *s, int n, uint32_t list, int inc, int before, int wback);
void interp_stm(icpu *s, int n, uint32_t list, int inc, int before, int wback);
/* MSR CPSR from a value, as user mode may: mask (the instruction's 4-bit
 * field) bit 3 writes NZCVQ, bit 2 the GE bits, bit 1 the E bit. */
void interp_msr_apsr(icpu *s, uint32_t v, int mask);
/* MRS: the APSR as user code reads it. */
uint32_t interp_mrs(icpu *s);
/* Memory barriers and hints. */
void interp_barrier(void);

/* ---- instruction sets ---- */

void interp_arm_exec(icpu *s, uint32_t insn);
void interp_thumb_step(icpu *s);
/* Coprocessor space in ARM encoding (Thumb-2 converts to it): CP15, VFP. */
void interp_coproc(icpu *s, uint32_t insn);
/* VFP data processing, transfers and loads/stores (cp10/cp11), ARM encoding. */
void interp_vfp(icpu *s, uint32_t insn);
/* NEON data processing (ARM encoding 1111 001x) and element loads/stores
 * (1111 0100). */
void interp_neon_dp(icpu *s, uint32_t insn);
void interp_neon_ls(icpu *s, uint32_t insn);

/* ---- the host FP environment (interp_vfp.c) ---- */

/* While guest code runs, the host FPCR mirrors the guest FPSCR's rounding
 * mode, FZ, DN and AHP and the host FPSR accumulates the exception flags.
 * Host code (SVC handlers) runs with the host's own environment. */
void interp_fp_enter(icpu *s);
void interp_fp_leave(icpu *s);
/* NEON arithmetic uses the "standard FPSCR value": FZ, DN, round to nearest. */
void interp_fp_neon_begin(icpu *s);
void interp_fp_neon_end(icpu *s);

#endif
