/*
 * libm over the host's, with the softfp calling convention bionic uses on
 * 32-bit ARM: a double argument takes a register pair (r0/r1, then r2/r3), a
 * float one register, and results come back in r0 (and r1).
 */
#include <math.h>
#include <string.h>

#include "../cpu.h"
#include "../guest_mem.h"
#include "reimpl.h"

static double darg(cpu *c, int i) {
  uint64_t b = (uint64_t)cpu_arg(c, i) | (uint64_t)cpu_arg(c, i + 1) << 32;
  double d;
  memcpy(&d, &b, sizeof d);
  return d;
}

static float farg(cpu *c, int i) {
  uint32_t b = cpu_arg(c, i);
  float f;
  memcpy(&f, &b, sizeof f);
  return f;
}

static void dret(cpu *c, double d) {
  uint64_t b;
  memcpy(&b, &d, sizeof b);
  cpu_ret64(c, b);
}

static void fret(cpu *c, float f) {
  uint32_t b;
  memcpy(&b, &f, sizeof b);
  cpu_ret(c, b);
}

#define UNARY(name) \
  static void h_##name(cpu *c) { dret(c, name(darg(c, 0))); }
#define BINARY(name) \
  static void h_##name(cpu *c) { dret(c, name(darg(c, 0), darg(c, 2))); }

UNARY(acos) UNARY(asin) UNARY(atan) UNARY(ceil) UNARY(cos) UNARY(cosh) UNARY(exp)
UNARY(floor) UNARY(log) UNARY(log10) UNARY(rint) UNARY(sin) UNARY(sinh) UNARY(sqrt)
UNARY(tan) UNARY(tanh)
BINARY(atan2) BINARY(fmod) BINARY(pow)

/* frexp(double, int *exp) */
static void h_frexp(cpu *c) {
  int e;
  dret(c, frexp(darg(c, 0), &e));
  gm_w32(cpu_arg(c, 2), (uint32_t)e);
}

/* ldexp(double, int) */
static void h_ldexp(cpu *c) { dret(c, ldexp(darg(c, 0), (int32_t)cpu_arg(c, 2))); }

/* modf(double, double *int_part) */
static void h_modf(cpu *c) {
  double ip, r = modf(darg(c, 0), &ip);
  uint32_t out = cpu_arg(c, 2);
  memcpy(g2h(out), &ip, sizeof ip);
  dret(c, r);
}

static void h_log10f(cpu *c) { fret(c, log10f(farg(c, 0))); }

UNARY(exp2)

#define UNARYF(name) \
  static void h_##name(cpu *c) { fret(c, name(farg(c, 0))); }
#define BINARYF(name) \
  static void h_##name(cpu *c) { fret(c, name(farg(c, 0), farg(c, 1))); }

UNARYF(acosf) UNARYF(asinf) UNARYF(cosf) UNARYF(expf) UNARYF(floorf) UNARYF(logf)
UNARYF(sinf) UNARYF(sqrtf) UNARYF(tanf)
BINARYF(atan2f) BINARYF(fmodf) BINARYF(powf)

/* __isinf(double): 1 for +inf, -1 for -inf (bionic's isinf), 0 otherwise. */
static void h___isinf(cpu *c) {
  double d = darg(c, 0);
  cpu_ret(c, isinf(d) ? (uint32_t)(d > 0 ? 1 : -1) : 0);
}

static void h_lrintf(cpu *c) { cpu_ret(c, (uint32_t)(int32_t)lrintf(farg(c, 0))); }

static void h_isfinitef(cpu *c) { cpu_ret(c, isfinite(farg(c, 0)) ? 1 : 0); }

const dynlib_impl reimpl_libm[] = {
    {"__isfinitef", h_isfinitef}, {"acos", h_acos},   {"asin", h_asin},   {"atan", h_atan},
    {"atan2", h_atan2},           {"ceil", h_ceil},   {"cos", h_cos},     {"cosh", h_cosh},
    {"exp", h_exp},               {"floor", h_floor}, {"fmod", h_fmod},   {"frexp", h_frexp},
    {"ldexp", h_ldexp},           {"log", h_log},     {"log10", h_log10}, {"log10f", h_log10f},
    {"lrintf", h_lrintf},         {"modf", h_modf},   {"pow", h_pow},     {"rint", h_rint},
    {"sin", h_sin},               {"sinh", h_sinh},   {"sqrt", h_sqrt},   {"tan", h_tan},
    {"tanh", h_tanh},             {"exp2", h_exp2},   {"acosf", h_acosf}, {"asinf", h_asinf},
    {"cosf", h_cosf},             {"expf", h_expf},   {"floorf", h_floorf}, {"logf", h_logf},
    {"sinf", h_sinf},             {"sqrtf", h_sqrtf}, {"tanf", h_tanf},   {"atan2f", h_atan2f},
    {"fmodf", h_fmodf},           {"powf", h_powf},   {"__isinf", h___isinf},
    {NULL, NULL},
};
