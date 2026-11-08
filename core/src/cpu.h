/*
 * The swappable CPU backend (7): runs guest ARMv7 code (ARM and
 * Thumb-2, VFP/NEON) on the guest address space in guest_mem.h.
 *
 * dynarmic is the first implementation (cpu_dynarmic.cpp). The static
 * recompiler and the ARM interpreter implement the same ops table later.
 *
 * Guest SVCs reach the host through cpu_svc(): dynlib thunks
 * (svc #(DYNLIB_SVC_BASE | index)) go to dynlib_dispatch, fake JNI thunks
 * (svc #(JNI_SVC_BASE | slot), #(JNI_SVC_VM | slot)) to jni_dispatch, and the
 * return sentinel used by cpu_call halts the run.
 */
#ifndef CPU_H
#define CPU_H

#include <stdint.h>

#define CPU_REG_SP 13
#define CPU_REG_LR 14
#define CPU_REG_PC 15
#define CPU_CPSR_T (1u << 5)

/* svc immediate of the return sentinel cpu_call puts in LR. */
#define CPU_SVC_RETURN 0x7FFFFFu

typedef struct cpu cpu;

typedef struct cpu_ops {
  const char *name;
  void (*destroy)(cpu *c);
  uint32_t (*get_reg)(cpu *c, int r);
  void (*set_reg)(cpu *c, int r, uint32_t v);
  uint32_t (*get_cpsr)(cpu *c);
  void (*set_cpsr)(cpu *c, uint32_t v);
  /* VFP/NEON: s0..s63 viewed as 32-bit words (d0..d31). */
  uint32_t (*get_ext)(cpu *c, int i);
  void (*set_ext)(cpu *c, int i, uint32_t v);
  uint32_t (*get_fpscr)(cpu *c);
  void (*set_fpscr)(cpu *c, uint32_t v);
  /* Runs from the current PC until cpu_halt; returns 0 or -1 on a guest fault. */
  int (*run)(cpu *c);
  /* Called from inside run (e.g. from an SVC handler). */
  void (*halt)(cpu *c);
  /* Guest code at [addr, addr+size) changed. */
  void (*invalidate)(cpu *c, uint32_t addr, uint32_t size);
  /* A fresh cpu of the same backend (for nested callbacks). */
  cpu *(*create)(void);
} cpu_ops;

struct cpu {
  const cpu_ops *ops;
  uint32_t ret_sentinel; /* guest address of the return-sentinel thunk */
  int faulted;
  cpu *nested; /* runs guest callbacks made from inside a shim; lazily created */
};

/* Backend constructors. */
cpu *cpu_dynarmic_create(void);
cpu *cpu_interp_create(void); /* our ARMv7 interpreter (cpu_interp.h) */
/* Statically recompiled code (cpu_recomp.c), with the interpreter
 * as its fallback. The libraries must be loaded (so_load) before the first
 * one is created. NULL in builds without the recompiled libraries. */
cpu *cpu_recomp_create(void);

/* The backend cpu_create makes (--cpu): "dynarmic", "interp" or "recomp";
 * without a call, the build's default (cpu_default_backend: dynarmic on the
 * Mac, recomp on the iPhone, where dynarmic isn't built: DL_NO_DYNARMIC).
 * Returns -1 for an unknown name or one this build lacks ("recomp" needs
 * DL_RECOMP=ON). */
int cpu_select_backend(const char *name);
const char *cpu_default_backend(void);
cpu *cpu_create(void);
const char *cpu_backend_name(void);

/* Host stack size for a host thread that runs guest code. Recompiled guest
 * calls are host C calls, so that backend needs more than the JIT. */
unsigned long cpu_host_stack_size(void);

/* Backend statistics at the end of a run (the recompiler's interpreter
 * fallback hits), to stderr; DL_RECOMP_REPORT=path also writes all of
 * them to a file. Nothing for the other backends. */
void cpu_backend_report(void);

/* Test hook: dynarmic without fastmem, so every guest access goes through
 * the region check and a bad one faults the guest, never the host (safe in
 * a fork child, where dynarmic's fault handler thread doesn't exist). */
cpu *cpu_dynarmic_create_checked(void);

/* Test hook: runs one guest instruction under dynarmic. Returns 0, 1 if it
 * halted the cpu, -1 on a fault (also sets c->faulted). */
int cpu_dynarmic_step(cpu *c);

/* dynarmic JIT instances created so far (0 in a run without a JIT). */
int cpu_dynarmic_jits(void);

/* Allocates the guest stack and the return sentinel. Call once per cpu. */
int cpu_setup(cpu *c, uint32_t stack_size);

/* Calls a guest function (bit 0 set = Thumb) with AAPCS integer arguments
 * (r0-r3, then the stack). Returns r0, or r0 | r1 << 32 via cpu_call64. */
uint32_t cpu_call(cpu *c, uint32_t func, int nargs, const uint32_t *args);
uint64_t cpu_call64(cpu *c, uint32_t func, int nargs, const uint32_t *args);

/* Calls a guest function from inside a host shim (e.g. pthread_once's init
 * routine, qsort's comparator) while c is suspended in that shim. The callee
 * runs on a nested cpu on c's stack, below c's current SP, like a real nested
 * call. Sets c->faulted if the callee faults. */
uint32_t cpu_callback(cpu *c, uint32_t func, int nargs, const uint32_t *args);

/* Integer argument i of the function a host shim is servicing. */
uint32_t cpu_arg(cpu *c, int i);

/* Return value of a host shim: r0, or r0/r1 for 64-bit results. */
static inline void cpu_ret(cpu *c, uint32_t v) { c->ops->set_reg(c, 0, v); }
static inline void cpu_ret64(cpu *c, uint64_t v) {
  c->ops->set_reg(c, 0, (uint32_t)v);
  c->ops->set_reg(c, 1, (uint32_t)(v >> 32));
}

/* Entry point for every guest SVC, called by the backends. */
void cpu_svc(cpu *c, uint32_t imm);

#endif
