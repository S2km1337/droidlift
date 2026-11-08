#include "cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dynlib.h"
#include "fake_jni.h"
#include "guest_mem.h"

#define CPSR_MODE_USR 0x10u

/* The build's default backend (CMake's DL_DEFAULT_CPU): dynarmic on the
 * Mac and in the Simulator, recomp on the iPhone (no JIT; tickets 14, 15). */
#ifndef DL_DEFAULT_CPU
#define DL_DEFAULT_CPU "dynarmic"
#endif

static cpu *(*create_backend)(void);
static const char *backend_name = DL_DEFAULT_CPU;

#ifdef DL_HAVE_RECOMP
uint64_t cpu_recomp_report(FILE *out, int limit);
#endif

int cpu_select_backend(const char *name) {
#ifndef DL_NO_DYNARMIC
  if (!strcmp(name, "dynarmic")) create_backend = cpu_dynarmic_create;
  else
#endif
  if (!strcmp(name, "interp")) create_backend = cpu_interp_create;
#ifdef DL_HAVE_RECOMP
  else if (!strcmp(name, "recomp")) create_backend = cpu_recomp_create;
#endif
  else return -1;
  backend_name = create_backend == cpu_dynarmic_create ? "dynarmic"
                 : create_backend == cpu_interp_create ? "interp"
                                                       : "recomp";
  return 0;
}

const char *cpu_default_backend(void) { return DL_DEFAULT_CPU; }

cpu *cpu_create(void) {
  if (!create_backend && cpu_select_backend(DL_DEFAULT_CPU) != 0) {
    fprintf(stderr, "[cpu] this build has no backend \"%s\"\n", DL_DEFAULT_CPU);
    abort();
  }
  cpu *c = create_backend();
  if (!c) {
    fprintf(stderr, "[cpu] can't create a %s cpu\n", backend_name);
    abort();
  }
#ifdef DL_IOS
  /* Which backend ran is the first question about a device log. */
  static int logged;
  if (!logged++) printf("[cpu] backend %s\n", c->ops->name);
#endif
  return c;
}

const char *cpu_backend_name(void) { return backend_name; }

unsigned long cpu_host_stack_size(void) {
  /* Recompiled code: one host frame (or a few) per guest call level. */
  return strcmp(backend_name, "recomp") == 0 ? 16ul << 20 : 1ul << 20;
}

void cpu_backend_report(void) {
#ifdef DL_HAVE_RECOMP
  if (strcmp(backend_name, "recomp") != 0) return;
  fflush(stdout);
  uint64_t entries = cpu_recomp_report(stderr, 25);
  /* Tests: no JIT may have run, and DL_RECOMP_MAX_FALLBACK=N bounds the
   * interpreter fallback's entries (what the recompiler missed). */
  int jits = cpu_dynarmic_jits();
  const char *max = getenv("DL_RECOMP_MAX_FALLBACK");
  if (jits) fprintf(stderr, "[recomp] RECOMP FAILED: %d dynarmic JIT cpus were created\n", jits);
  else if (max && *max && entries > strtoull(max, NULL, 10))
    fprintf(stderr, "[recomp] RECOMP FAILED: %llu fallback entries, over the limit of %s\n",
            (unsigned long long)entries, max);
  else
    fprintf(stderr, "[recomp] RECOMP OK: no JIT, %llu interpreter fallback entries\n", (unsigned long long)entries);
  const char *path = getenv("DL_RECOMP_REPORT");
  if (path && *path) {
    FILE *f = fopen(path, "w");
    if (f) {
      cpu_recomp_report(f, 0);
      fclose(f);
    } else {
      perror(path);
    }
  }
#endif
}

int cpu_setup(cpu *c, uint32_t stack_size) {
  uint32_t stack = gm_alloc(stack_size, 0, "guest stack");
  uint32_t sentinel = gm_alloc(4, 0, "cpu return sentinel");
  if (!stack || !sentinel) return -1;
  gm_w32(sentinel, 0xEF000000u | CPU_SVC_RETURN); /* svc #CPU_SVC_RETURN (ARM) */
  c->ret_sentinel = sentinel;
  c->ops->set_reg(c, CPU_REG_SP, stack + stack_size);
  c->ops->set_cpsr(c, CPSR_MODE_USR);
  return 0;
}

uint64_t cpu_call64(cpu *c, uint32_t func, int nargs, const uint32_t *args) {
  const cpu_ops *o = c->ops;
  uint32_t entry_sp = o->get_reg(c, CPU_REG_SP);
  uint32_t sp = entry_sp;
  if (nargs > 4) {
    sp -= (uint32_t)(nargs - 4) * 4;
    sp &= ~7u; /* AAPCS: 8-byte aligned at public interfaces */
    for (int i = 4; i < nargs; i++) gm_w32(sp + (uint32_t)(i - 4) * 4, args[i]);
  } else {
    sp &= ~7u;
  }
  for (int i = 0; i < nargs && i < 4; i++) o->set_reg(c, i, args[i]);
  o->set_reg(c, CPU_REG_SP, sp);
  o->set_reg(c, CPU_REG_LR, c->ret_sentinel);
  o->set_reg(c, CPU_REG_PC, func & ~1u);
  uint32_t cpsr = o->get_cpsr(c);
  o->set_cpsr(c, (func & 1) ? cpsr | CPU_CPSR_T : cpsr & ~CPU_CPSR_T);

  c->faulted = 0;
  if (o->run(c) != 0 || c->faulted) {
    fprintf(stderr, "[cpu] guest call 0x%08x faulted at pc 0x%08x\n", func,
            o->get_reg(c, CPU_REG_PC));
    c->faulted = 1;
  }
  uint64_t r = o->get_reg(c, 0) | (uint64_t)o->get_reg(c, 1) << 32;
  /* Drop our stack arguments (and anything a faulting callee left behind). */
  o->set_reg(c, CPU_REG_SP, entry_sp);
  return r;
}

uint32_t cpu_call(cpu *c, uint32_t func, int nargs, const uint32_t *args) {
  return (uint32_t)cpu_call64(c, func, nargs, args);
}

uint32_t cpu_callback(cpu *c, uint32_t func, int nargs, const uint32_t *args) {
  if (!c->nested) {
    c->nested = c->ops->create();
    c->nested->ret_sentinel = c->ret_sentinel;
  }
  cpu *n = c->nested;
  /* Leave a red zone below the suspended caller's SP. */
  n->ops->set_reg(n, CPU_REG_SP, (c->ops->get_reg(c, CPU_REG_SP) - 64) & ~7u);
  n->ops->set_cpsr(n, c->ops->get_cpsr(c) & ~CPU_CPSR_T);
  n->ops->set_fpscr(n, c->ops->get_fpscr(c));
  uint32_t r = cpu_call(n, func, nargs, args);
  if (n->faulted) {
    c->faulted = 1;
    c->ops->halt(c);
  }
  return r;
}

uint32_t cpu_arg(cpu *c, int i) {
  if (i < 4) return c->ops->get_reg(c, i);
  return gm_r32(c->ops->get_reg(c, CPU_REG_SP) + (uint32_t)(i - 4) * 4);
}

void cpu_svc(cpu *c, uint32_t imm) {
  if (imm == CPU_SVC_RETURN) {
    c->ops->halt(c);
  } else if (imm & DYNLIB_SVC_BASE) {
    dynlib_dispatch(c, imm & ~DYNLIB_SVC_BASE);
  } else if ((imm & DYNLIB_HOST_MASK) == DYNLIB_HOST_SVC) {
    dynlib_host_dispatch(c, imm & ~DYNLIB_HOST_MASK);
  } else if ((imm & JNI_SVC_MASK) == JNI_SVC_BASE || (imm & JNI_SVC_MASK) == JNI_SVC_VM) {
    jni_dispatch(c, imm);
  } else {
    fprintf(stderr, "[cpu] unexpected svc #0x%x\n", imm);
    c->faulted = 1;
    c->ops->halt(c);
  }
}
