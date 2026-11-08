/*
 * Differential tests of the ARMv7 interpreter against dynarmic: real
 * functions from the game's libraries (the profile's fn_tests), a broad
 * sample of the game's functions run in lockstep, and randomized single
 * instructions. Registers, flags, extension registers, FPSCR and guest
 * memory must match.
 */
#ifndef INTERP_SELFTEST_H
#define INTERP_SELFTEST_H

#include "droidlift.h"
#include "so_util.h"

/* mods: the loaded libraries (the main one last); tests: the game's real
 * functions, NULL-terminated by name (may be NULL). Returns 0 if everything
 * matched. */
int interp_selftest(const so_module *mods, int n_mods, const dl_fn_test *tests);

#endif
