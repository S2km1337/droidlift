#ifndef CPU_SELFTEST_H
#define CPU_SELFTEST_H

#include "cpu.h"
#include "so_util.h"

/* Runs the CPU-backend round-trip checks (main: the game's main library);
 * 0 if all pass. */
int cpu_selftest(cpu *c, const so_module *main);

#endif
