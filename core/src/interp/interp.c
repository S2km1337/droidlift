/*
 * The interpreter backend's cpu_ops, run loop, CPSR, faults and the
 * exclusive monitor. See interp.h.
 */
#include "interp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "../cpu_interp.h"

/* ---- faults ---- */

int interp_quiet; /* differential tests: no fault messages */

void interp_fault(icpu *s, int kind, const char *fmt, ...) {
  if (!s->fault && !interp_quiet) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[interp] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, " (pc 0x%08x, lr 0x%08x)\n", s->r[15] - (s->t ? 4 : 8), s->r[14]);
    va_end(ap);
  }
  if (!s->fault) s->fault = kind;
  s->base.faulted = 1;
  s->halt_req = 1;
}

void interp_undefined(icpu *s, uint32_t insn, const char *what) {
  interp_fault(s, INTERP_FAULT_UNDEFINED, "undefined or unsupported %s instruction 0x%08x (%s)", s->t ? "Thumb" : "ARM",
               insn, what);
}

int interp_mapped(icpu *s, uint32_t a, uint32_t n, int write) {
  if (s->win_hi && (a < s->win_lo || a + n - 1 > s->win_hi || a + n - 1 < a)) {
    interp_fault(s, INTERP_FAULT_WINDOW, "guest %s of %u bytes at 0x%08x outside the test window",
                 write ? "write" : "read", n, a);
    return 0;
  }
  if (gm_region_tag(a) && gm_region_tag(a + n - 1)) return 1;
  interp_fault(s, INTERP_FAULT_MEMORY, "guest %s of %u bytes at unmapped 0x%08x", write ? "write" : "read", n, a);
  return 0;
}

/* ---- exclusives: compare-and-swap against the value ldrex saw, like the
 * dynarmic backend, so they stay correct across host threads ---- */

/* Exclusive and swap accesses must be naturally aligned, on ARM (an
 * alignment fault) and for the host's atomics alike. */
int interp_aligned(icpu *s, uint32_t a, int size) {
  if (!(a & (uint32_t)(size - 1))) return 1;
  interp_fault(s, INTERP_FAULT_MEMORY, "misaligned exclusive access of %d bytes at 0x%08x", size, a);
  return 0;
}

uint64_t interp_ldrex(icpu *s, uint32_t a, int size) {
  uint64_t v = 0;
  if (!interp_aligned(s, a, size)) return 0;
  if (unlikely(s->checked) && !interp_mapped(s, a, (uint32_t)size, 0)) return 0;
  void *p = g_mem + a;
  switch (size) {
  case 1: v = __atomic_load_n((uint8_t *)p, __ATOMIC_SEQ_CST); break;
  case 2: v = __atomic_load_n((uint16_t *)p, __ATOMIC_SEQ_CST); break;
  case 4: v = __atomic_load_n((uint32_t *)p, __ATOMIC_SEQ_CST); break;
  default: v = __atomic_load_n((uint64_t *)p, __ATOMIC_SEQ_CST); break;
  }
  s->excl_addr = a;
  s->excl_size = (uint32_t)size;
  s->excl_val = v;
  s->excl_valid = 1;
  return v;
}

uint32_t interp_strex(icpu *s, uint32_t a, int size, uint64_t v) {
  if (!s->excl_valid || s->excl_addr != a || s->excl_size != (uint32_t)size) {
    s->excl_valid = 0;
    return 1;
  }
  s->excl_valid = 0;
  if (!interp_aligned(s, a, size)) return 1;
  if (unlikely(s->checked) && !interp_mapped(s, a, (uint32_t)size, 1)) return 1;
  void *p = g_mem + a;
  int ok;
  switch (size) {
  case 1: {
    uint8_t e = (uint8_t)s->excl_val;
    ok = __atomic_compare_exchange_n((uint8_t *)p, &e, (uint8_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    break;
  }
  case 2: {
    uint16_t e = (uint16_t)s->excl_val;
    ok = __atomic_compare_exchange_n((uint16_t *)p, &e, (uint16_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    break;
  }
  case 4: {
    uint32_t e = (uint32_t)s->excl_val;
    ok = __atomic_compare_exchange_n((uint32_t *)p, &e, (uint32_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    break;
  }
  default: {
    uint64_t e = s->excl_val;
    ok = __atomic_compare_exchange_n((uint64_t *)p, &e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    break;
  }
  }
  return ok ? 0 : 1;
}

/* ---- CPSR ---- */

#define CPSR_FLAG_BITS 0xF80F0000u /* N Z C V Q, GE */
#define CPSR_IT_BITS 0x0600FC00u
#define CPSR_T_BIT 0x00000020u

uint32_t interp_get_cpsr(icpu *s) {
  return s->n << 31 | s->z << 30 | s->c << 29 | s->v << 28 | s->q << 27 | (s->it & 3) << 25 |
         s->ge << 16 | (s->it >> 2) << 10 | s->t << 5 | s->cpsr_rest;
}

void interp_set_cpsr(icpu *s, uint32_t v) {
  s->n = v >> 31;
  s->z = (v >> 30) & 1;
  s->c = (v >> 29) & 1;
  s->v = (v >> 28) & 1;
  s->q = (v >> 27) & 1;
  s->ge = (v >> 16) & 0xF;
  s->it = ((v >> 25) & 3) | ((v >> 10) & 0x3F) << 2;
  s->t = (v >> 5) & 1;
  s->cpsr_rest = v & ~(CPSR_FLAG_BITS | CPSR_IT_BITS | CPSR_T_BIT);
}

/* ---- data processing ---- */

void interp_dp(icpu *s, int op, int setflags, int d, uint32_t a, uint32_t b, uint32_t sc) {
  uint32_t r, c = sc, v = s->v;
  int arith = 1, test = 0;
  switch (op) {
  case DP_AND: r = a & b; arith = 0; break;
  case DP_EOR: r = a ^ b; arith = 0; break;
  case DP_SUB: r = add_c(a, ~b, 1, &c, &v); break;
  case DP_RSB: r = add_c(~a, b, 1, &c, &v); break;
  case DP_ADD: r = add_c(a, b, 0, &c, &v); break;
  case DP_ADC: r = add_c(a, b, s->c, &c, &v); break;
  case DP_SBC: r = add_c(a, ~b, s->c, &c, &v); break;
  case DP_RSC: r = add_c(~a, b, s->c, &c, &v); break;
  case DP_TST: r = a & b; arith = 0; test = 1; break;
  case DP_TEQ: r = a ^ b; arith = 0; test = 1; break;
  case DP_CMP: r = add_c(a, ~b, 1, &c, &v); test = 1; break;
  case DP_CMN: r = add_c(a, b, 0, &c, &v); test = 1; break;
  case DP_ORR: r = a | b; arith = 0; break;
  case DP_MOV: r = b; arith = 0; break;
  case DP_BIC: r = a & ~b; arith = 0; break;
  case DP_MVN: r = ~b; arith = 0; break;
  default: r = a | ~b; arith = 0; break; /* ORN */
  }
  if (!test) set_reg_alu(s, d, r);
  if (setflags) {
    set_nz(s, r);
    s->c = c;
    if (arith) s->v = v;
  }
}

/* ---- run loop ---- */

static ALWAYS_INLINE void step_one(icpu *s) {
  if (s->t) {
    interp_thumb_step(s);
  } else {
    uint32_t pc = s->r[15];
    if (unlikely(s->checked) && !interp_mapped(s, pc, 4, 0)) return;
    uint32_t insn;
    memcpy(&insn, g_mem + pc, 4);
    s->r[15] = pc + 8;
    s->next_pc = pc + 4;
    interp_arm_exec(s, insn);
    s->r[15] = s->next_pc;
  }
}

static int op_run(cpu *c) {
  icpu *s = ICPU(c);
  s->halt_req = 0;
  s->fault = 0;
  interp_fp_enter(s);
  while (!s->halt_req) step_one(s);
  interp_fp_leave(s);
  return s->fault ? -1 : 0;
}

int cpu_interp_step(cpu *c) {
  icpu *s = ICPU(c);
  s->halt_req = 0;
  s->fault = 0;
  interp_fp_enter(s);
  step_one(s);
  interp_fp_leave(s);
  if (s->fault) return -1;
  return s->halt_req ? 1 : 0;
}

int cpu_interp_run_until(cpu *c, int (*stop)(uint32_t pc, void *ctx), void *ctx) {
  icpu *s = ICPU(c);
  s->halt_req = 0;
  s->fault = 0;
  interp_fp_enter(s);
  int r = 1;
  for (;;) {
    step_one(s);
    if (s->halt_req) break;
    if (stop(s->r[15] | s->t, ctx)) {
      r = 0;
      break;
    }
  }
  interp_fp_leave(s);
  return s->fault ? -1 : r;
}

void cpu_interp_load(cpu *c, const cpu_interp_regs *st) {
  icpu *s = ICPU(c);
  memcpy(s->r, st->r, sizeof s->r);
  interp_set_cpsr(s, st->cpsr);
  memcpy(s->x.s, st->ext, sizeof st->ext);
  s->fpscr = st->fpscr;
}

void cpu_interp_store(cpu *c, cpu_interp_regs *st) {
  icpu *s = ICPU(c);
  memcpy(st->r, s->r, sizeof s->r);
  st->cpsr = interp_get_cpsr(s);
  memcpy(st->ext, s->x.s, sizeof st->ext);
  st->fpscr = s->fpscr;
}

void cpu_interp_set_checked(cpu *c, int on) { ICPU(c)->checked = on; }

void cpu_interp_set_window(cpu *c, uint32_t lo, uint32_t hi) {
  ICPU(c)->win_lo = lo;
  ICPU(c)->win_hi = hi;
}

int cpu_interp_fault_kind(cpu *c) { return ICPU(c)->fault; }

void cpu_interp_set_quiet(int on) { interp_quiet = on; }

static void op_halt(cpu *c) { ICPU(c)->halt_req = 1; }

static void op_invalidate(cpu *c, uint32_t addr, uint32_t size) {
  /* Nothing is cached per address yet. */
  (void)addr;
  (void)size;
  if (c->nested) c->nested->ops->invalidate(c->nested, addr, size);
}

static void op_destroy(cpu *c) {
  if (c->nested) c->nested->ops->destroy(c->nested);
  free(c);
}

static uint32_t op_get_reg(cpu *c, int r) { return ICPU(c)->r[r]; }
static void op_set_reg(cpu *c, int r, uint32_t v) { ICPU(c)->r[r] = v; }
static uint32_t op_get_cpsr(cpu *c) { return interp_get_cpsr(ICPU(c)); }
static void op_set_cpsr(cpu *c, uint32_t v) { interp_set_cpsr(ICPU(c), v); }
static uint32_t op_get_ext(cpu *c, int i) { return ICPU(c)->x.s[i]; }
static void op_set_ext(cpu *c, int i, uint32_t v) { ICPU(c)->x.s[i] = v; }
static uint32_t op_get_fpscr(cpu *c) { return ICPU(c)->fpscr; }
static void op_set_fpscr(cpu *c, uint32_t v) { ICPU(c)->fpscr = v; }

static const cpu_ops interp_ops = {
    "interp",     op_destroy,   op_get_reg, op_set_reg, op_get_cpsr, op_set_cpsr, op_get_ext,
    op_set_ext,   op_get_fpscr, op_set_fpscr, op_run,   op_halt,     op_invalidate,
    cpu_interp_create,
};

cpu *cpu_interp_create(void) {
  icpu *s = calloc(1, sizeof *s);
  if (!s) return NULL;
  s->base.ops = &interp_ops;
  return &s->base;
}
