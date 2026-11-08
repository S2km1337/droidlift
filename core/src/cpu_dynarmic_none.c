/*
 * Builds without dynarmic (DL_WITH_DYNARMIC=OFF: the iPhone, which has no
 * JIT). cpu_select_backend refuses "dynarmic" there, so only the
 * dynarmic-based self-tests (--interp-test) can reach these.
 */
#include <stdio.h>
#include <stdlib.h>

#include "cpu.h"

static void no_dynarmic(void) {
  fprintf(stderr, "[cpu] this build has no dynarmic (DL_WITH_DYNARMIC=OFF)\n");
  abort();
}

cpu *cpu_dynarmic_create(void) {
  no_dynarmic();
  return NULL;
}

cpu *cpu_dynarmic_create_checked(void) {
  no_dynarmic();
  return NULL;
}

int cpu_dynarmic_step(cpu *c) {
  (void)c;
  no_dynarmic();
  return -1;
}

/* No dynarmic, so no JIT cpus (the recomp backend's no-JIT check). */
int cpu_dynarmic_jits(void) { return 0; }
