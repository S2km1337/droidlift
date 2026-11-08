/*
 * The interpreter as the recompiler's fallback (recomp/runtime/
 * rc_runtime.h): rc_run and rc_call call rc_fallback when control reaches
 * guest code with no recompiled function.
 *
 * The fallback doesn't know what its dispatcher is waiting for (a stop
 * address, a call's return address and stack pointer), so it hands control
 * back whenever one of those could have become true: when control reaches
 * recompiled code (every call's return address in recompiled code is an
 * entry), when it reaches the LR it was entered with (the function
 * returned), when the stack pointer rises above where it was (the frame was
 * left: a return from mid-function, a longjmp), or when the guest halts.
 * Between those it runs at full interpreter speed. SVCs and CP15 reads in
 * interpreted code go to the same rc_svc and rc_cp15_read hooks as
 * recompiled code.
 *
 * One interpreter cpu per host thread, reused: guest threads are host
 * threads, and creating one is a small calloc anyway.
 */
#include <stddef.h>

#include "../cpu_interp.h"
#include "interp.h"
#include "rc_runtime.h"

_Static_assert(sizeof(rc_state) == sizeof(cpu_interp_regs), "rc_state layout");
_Static_assert(offsetof(rc_state, cpsr) == offsetof(cpu_interp_regs, cpsr), "rc_state layout");
_Static_assert(offsetof(rc_state, ext) == offsetof(cpu_interp_regs, ext), "rc_state layout");
_Static_assert(offsetof(rc_state, fpscr) == offsetof(cpu_interp_regs, fpscr), "rc_state layout");

static __thread cpu *rc_cpu;
static __thread rc_state *rc_cur;

static void svc_hook(icpu *s, uint32_t imm) {
  cpu_interp_store(&s->base, (cpu_interp_regs *)rc_cur);
  rc_svc(rc_cur, imm);
  cpu_interp_load(&s->base, (const cpu_interp_regs *)rc_cur);
}

static uint32_t cp15_hook(icpu *s, uint32_t raw) {
  cpu_interp_store(&s->base, (cpu_interp_regs *)rc_cur);
  return rc_cp15_read(rc_cur, raw);
}

typedef struct stop_ctx {
  cpu *c;
  uint32_t entry_lr, entry_sp, last_pc;
} stop_ctx;

static int stop_here(uint32_t pc, void *p) {
  stop_ctx *x = p;
  uint32_t last = x->last_pc;
  x->last_pc = pc;
  if (x->c->ops->get_reg(x->c, CPU_REG_SP) > x->entry_sp) return 1;
  uint32_t d = (pc & ~1u) - (last & ~1u);
  if ((d == 2 || d == 4) && (pc & 1) == (last & 1)) return 0; /* fell through */
  return pc == x->entry_lr || rc_lookup(pc) != NULL;
}

int interp_rc_fallback(rc_state *s) {
  if (!rc_cpu) {
    rc_cpu = cpu_interp_create();
    if (!rc_cpu) return -1;
    ICPU(rc_cpu)->svc_hook = svc_hook;
    ICPU(rc_cpu)->cp15_read_hook = cp15_hook;
  }
  rc_state *outer = rc_cur; /* a nested dispatch from inside rc_svc */
  rc_cur = s;
  cpu_interp_load(rc_cpu, (const cpu_interp_regs *)s);
  stop_ctx x = {rc_cpu, s->r[14], s->r[13], rc_get_pc(s)};
  rc_cpu->faulted = 0;
  int r = cpu_interp_run_until(rc_cpu, stop_here, &x);
  cpu_interp_store(rc_cpu, (cpu_interp_regs *)s);
  rc_cur = outer;
  return r < 0 ? -1 : 0;
}
