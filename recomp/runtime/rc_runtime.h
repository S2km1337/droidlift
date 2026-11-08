/*
 * Runtime for statically recompiled guest code.
 *
 * The recompiler (recomp/src) turns each guest function into a C function
 * `void <prefix>_f_<vaddr>(rc_state *s)`. It runs on the guest-state struct
 * below and on guest memory (g_mem, the loader's 4 GiB guest reservation, see
 * loader/src/guest_mem.h), keeps registers and flags in locals while it runs,
 * and returns with s->r[15] (and CPSR.T) set to wherever control goes next:
 * the caller's return address, a tail-called function, or a computed target.
 * rc_run() keeps dispatching until the pc reaches a stop address.
 *
 * rc_state mirrors what the CPU-backend interface (loader/src/cpu.h) exposes
 * through its accessors: r0-r15, CPSR, the 64 VFP/NEON words and FPSCR, so a
 * cpu_ops backend over it is a field copy.
 */
#ifndef RC_RUNTIME_H
#define RC_RUNTIME_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rc_state {
  uint32_t r[16];
  uint32_t cpsr;
  uint32_t ext[64]; /* s0..s63 as words (d0..d31) */
  uint32_t fpscr;
} rc_state;

#define RC_CPSR_T (1u << 5)
#define RC_CPSR_NZCV 0xF0000000u

typedef void (*rc_fn)(rc_state *s);

/* One recompiled function. addr is the ELF virtual address (bit 0 set for
 * Thumb); it runs at *module->base + addr. */
typedef struct rc_func {
  uint32_t addr;
  rc_fn fn;
  const char *name;
} rc_func;

/* The functions recompiled from one library, sorted by addr. */
typedef struct rc_module {
  const char *soname;
  uint32_t *base; /* guest address the library is loaded at; set by the loader */
  const rc_func *funcs;
  uint32_t count;
} rc_module;

/* Guest memory: the loader's guest reservation (guest_mem.h). */
extern uint8_t *g_mem;

/* ---- Dispatch -------------------------------------------------------------- */

void rc_register_module(const rc_module *m);

/* Host code at guest addresses: every `stride`-aligned address in [lo, hi)
 * dispatches to fn (which reads s->r[15] to tell them apart). The loader's
 * import and JNI thunks (`svc #imm; bx lr`) run natively this way instead of
 * in the interpreter. Lookups may run concurrently with a registration, but
 * registrations must not run concurrently with each other. */
void rc_register_range(uint32_t lo, uint32_t hi, uint32_t stride, rc_fn fn);

/* The recompiled function entered at guest address addr (bit 0 = Thumb), or
 * NULL. Used for indirect branches and returns. */
rc_fn rc_lookup(uint32_t addr);

/* Called when control reaches code with no recompiled function. By design the
 * ARMv7 interpreter goes here; it runs at least one instruction and returns
 * 0, or returns -1 to give up. NULL: give up. */
extern int (*rc_fallback)(rc_state *s);

/* Runs from s->r[15] (Thumb if CPSR.T) until the pc equals stop (bit 0 =
 * Thumb, as in rc_func.addr). Returns 0, or -1 if control reached code that
 * neither a recompiled function nor the fallback could run (s->r[15] says
 * where). */
int rc_run(rc_state *s, uint32_t stop);

/* A guest call made by recompiled code (BL/BLX) whose callee didn't simply
 * return: dispatches from s->r[15] until the callee returns to ret with the
 * stack pointer back at sp (the SP when the call was made). Returns 0 then,
 * or 1 if the guest stack was unwound past the call (SP above sp: longjmp or
 * an exception left the callee for an outer frame); the caller then carries
 * on from wherever the pc is. Every call's return address is an entry of
 * its function, so outer frames can be re-entered. Aborts the process if
 * control reaches code nothing can run. */
int rc_call(rc_state *s, uint32_t ret, uint32_t sp);

/* Host hooks for generated code. The loader's recompiler backend
 * sets them; the defaults abort. rc_svc: SVC #imm, with s->r[15] at the next
 * instruction. rc_cp15_read: MRC p15 (TPIDRURO, ...), raw = the instruction
 * in ARM form. */
extern void (*rc_svc)(rc_state *s, uint32_t imm);
extern uint32_t (*rc_cp15_read)(rc_state *s, uint32_t raw);

/* A function was entered at an address that is not one of its entries. */
void rc_bad_entry(rc_state *s);

/* Sets the pc from an interworking address: bit 0 selects Thumb. */
static inline void rc_set_pc(rc_state *s, uint32_t a) {
  s->r[15] = a & ~1u;
  s->cpsr = (a & 1) ? (s->cpsr | RC_CPSR_T) : (s->cpsr & ~RC_CPSR_T);
}

/* The pc as an interworking address. */
static inline uint32_t rc_get_pc(const rc_state *s) {
  return s->r[15] | ((s->cpsr & RC_CPSR_T) ? 1u : 0u);
}

/* ---- Guest memory ---------------------------------------------------------- */

static inline uint32_t rc_ld8(uint8_t *m, uint32_t a) { return m[a]; }
static inline uint32_t rc_ld16(uint8_t *m, uint32_t a) {
  uint16_t v;
  memcpy(&v, m + a, 2);
  return v;
}
static inline uint32_t rc_ld32(uint8_t *m, uint32_t a) {
  uint32_t v;
  memcpy(&v, m + a, 4);
  return v;
}
static inline void rc_st8(uint8_t *m, uint32_t a, uint32_t v) { m[a] = (uint8_t)v; }
static inline void rc_st16(uint8_t *m, uint32_t a, uint32_t v) {
  uint16_t h = (uint16_t)v;
  memcpy(m + a, &h, 2);
}
static inline void rc_st32(uint8_t *m, uint32_t a, uint32_t v) { memcpy(m + a, &v, 4); }

/* ---- ALU helpers ----------------------------------------------------------- */

/* AddWithCarry() from the ARM ARM: a + b + cin with carry and overflow out. */
static inline uint32_t rc_addc(uint32_t a, uint32_t b, uint32_t cin, uint32_t *c, uint32_t *v) {
  uint64_t u = (uint64_t)a + b + cin;
  uint32_t r = (uint32_t)u;
  *c = (uint32_t)(u >> 32);
  *v = ((a ^ r) & (b ^ r)) >> 31;
  return r;
}

static inline uint32_t rc_ror(uint32_t v, uint32_t n) {
  n &= 31;
  return n ? (v >> n) | (v << (32 - n)) : v;
}

static inline uint32_t rc_rbit(uint32_t v) {
  v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
  v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
  v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
  return __builtin_bswap32(v);
}

/* Shift_C() for a register-specified amount (bottom byte of Rs): type 0 LSL,
 * 1 LSR, 2 ASR, 3 ROR. */
static inline uint32_t rc_shift_reg(uint32_t v, uint32_t type, uint32_t n, uint32_t cin,
                                    uint32_t *cout) {
  n &= 0xff;
  if (n == 0) {
    *cout = cin;
    return v;
  }
  switch (type) {
  case 0:
    if (n < 32) {
      *cout = (v >> (32 - n)) & 1;
      return v << n;
    }
    *cout = n == 32 ? (v & 1) : 0;
    return 0;
  case 1:
    if (n < 32) {
      *cout = (v >> (n - 1)) & 1;
      return v >> n;
    }
    *cout = n == 32 ? (v >> 31) : 0;
    return 0;
  case 2:
    if (n < 32) {
      *cout = (v >> (n - 1)) & 1;
      return (uint32_t)((int32_t)v >> n);
    }
    *cout = v >> 31;
    return (uint32_t)((int32_t)v >> 31);
  default:
    n &= 31;
    if (n == 0) {
      *cout = v >> 31;
      return v;
    }
    *cout = (v >> (n - 1)) & 1;
    return rc_ror(v, n);
  }
}

#ifdef __cplusplus
}
#endif

#endif
