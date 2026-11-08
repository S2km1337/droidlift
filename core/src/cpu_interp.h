/*
 * The ARMv7 interpreter backend: the second implementation
 * of cpu_ops, with no JIT and no executable memory. Sources in interp/.
 */
#ifndef CPU_INTERP_H
#define CPU_INTERP_H

#include "cpu.h"

/* Guest state in the layout of the recompiler's rc_state
 * (recomp/runtime/rc_runtime.h), which is what cpu.h's accessors expose:
 * r0-r15, CPSR, s0-s63 as words, FPSCR. For the recompiler's fallback
 *: load the state, run the interpreter until control
 * reaches recompiled code, store it back. */
typedef struct cpu_interp_regs {
  uint32_t r[16];
  uint32_t cpsr;
  uint32_t ext[64];
  uint32_t fpscr;
} cpu_interp_regs;

void cpu_interp_load(cpu *c, const cpu_interp_regs *st);
void cpu_interp_store(cpu *c, cpu_interp_regs *st);

/* Runs from the current state until stop(pc, ctx) returns nonzero before an
 * instruction (pc has bit 0 set in Thumb state), the cpu halts (cpu_halt,
 * the return sentinel) or a guest fault. stop is not asked before the first
 * instruction, so at least one runs. Returns 0 if stop said so, 1 if
 * halted, -1 on a fault. */
int cpu_interp_run_until(cpu *c, int (*stop)(uint32_t pc, void *ctx), void *ctx);

/* The recompiler's rc_fallback (recomp/runtime/rc_runtime.h): interprets
 * from s until control reaches recompiled code, the LR it was entered with,
 * a stack pointer above the one it was entered with, or a halt. SVCs and
 * MRC p15 reads go to rc_svc and rc_cp15_read. Returns 0, or -1 on a guest
 * fault. In dl_interp_rc (interp/interp_rc.c). */
struct rc_state;
int interp_rc_fallback(struct rc_state *s);

/* Test hooks. cpu_interp_step runs one instruction: 0, 1 if it halted the
 * cpu (the return sentinel, cpu_halt), -1 on a fault. In checked mode every
 * guest memory access is checked against the guest regions, so a bad one
 * faults the guest rather than the host. */
int cpu_interp_step(cpu *c);
void cpu_interp_set_checked(cpu *c, int on);
/* Checked mode: accesses outside [lo, hi] fault too (hi 0: no window). */
void cpu_interp_set_window(cpu *c, uint32_t lo, uint32_t hi);
/* Why the last step or run faulted: 0 none, 1 unmapped memory, 2 outside
 * the window, 3 undefined or unsupported instruction, 4 other. */
int cpu_interp_fault_kind(cpu *c);
/* No fault messages (random tests fault on purpose). */
void cpu_interp_set_quiet(int on);

#endif
