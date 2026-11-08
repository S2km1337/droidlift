/*
 * Cursor over a guest call's variadic arguments, shared by the printf and
 * scanf families.
 *
 * The guest is 32-bit ARM with the softfp variadic ABI (AAPCS): variadic
 * arguments continue in r0-r3 after the fixed ones, then on the stack;
 * 64-bit values (long long, double) start at an even register or an 8-byte
 * aligned stack slot and never straddle r3 and the stack. A guest va_list is
 * a plain pointer to the next argument in memory.
 */
#ifndef GARGS_H
#define GARGS_H

#include <stdint.h>

#include "../cpu.h"
#include "../guest_mem.h"

/* Core registers first, then memory. */
typedef struct gargs {
  cpu *c;
  int reg;      /* next core register; 4 once arguments moved to memory */
  uint32_t mem; /* next stack (or va_list) address */
} gargs;

/* Variadic arguments starting at register `first` (the fixed-argument count). */
static inline gargs variadic_args(cpu *c, int first) {
  gargs a = {c, first, c->ops->get_reg(c, CPU_REG_SP)};
  return a;
}

static inline gargs va_list_args(cpu *c, uint32_t ap) {
  gargs a = {c, 4, ap};
  return a;
}

static inline uint32_t next32(gargs *a) {
  if (a->reg < 4) return a->c->ops->get_reg(a->c, a->reg++);
  uint32_t v = gm_r32(a->mem);
  a->mem += 4;
  return v;
}

static inline uint64_t next64(gargs *a) {
  if (a->reg & 1) a->reg++;
  if (a->reg < 4) {
    uint64_t lo = a->c->ops->get_reg(a->c, a->reg), hi = a->c->ops->get_reg(a->c, a->reg + 1);
    a->reg += 2;
    return lo | hi << 32;
  }
  a->mem = (a->mem + 7) & ~7u;
  uint64_t v = gm_r32(a->mem) | (uint64_t)gm_r32(a->mem + 4) << 32;
  a->mem += 8;
  return v;
}

#endif
