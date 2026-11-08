#ifndef FMT_SELFTEST_H
#define FMT_SELFTEST_H

#include "cpu.h"

/* Runs the printf and scanf families through their guest thunks and compares
 * every result with the host libc on the same formats and values.
 * Needs the imports bound and the data root set. Returns 0 if all agree. */
int fmt_selftest(cpu *c);

#endif
