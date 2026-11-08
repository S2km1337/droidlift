/*
 * Differential tests of the interpreter against dynarmic. See
 * interp_selftest.h.
 *
 * Random single instructions: an instruction (or a short sequence, such as
 * an IT block or ldrex/strex) drawn from an encoding class is stepped once on
 * each backend from the same random state: registers that point into a
 * scratch buffer or hold small, special or random values, random flags, and
 * extension registers with special floating-point values. dynarmic is the
 * reference: where it raises an exception (undefined or unpredictable
 * encodings, hints, unmapped memory) the case is skipped. Otherwise the
 * interpreter must not fault and must end in the same state, with the same
 * scratch memory. The interpreter runs in checked mode with a window around
 * the code and scratch buffers, so a wild access faults it instead of
 * corrupting the process.
 */
#include "interp_selftest.h"

#pragma clang diagnostic ignored "-Wmissing-field-initializers"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <CommonCrypto/CommonDigest.h>
#include <zlib.h>

#include "cpu.h"
#include "cpu_interp.h"
#include "droidlift.h"
#include "dynlib.h"
#include "guest_mem.h"
#include "rc_runtime.h"
#include "reimpl/reimpl.h"

/* ---- common ---- */

static FILE *out; /* stderr, even while the random tests silence fd 2 */
static int failures;

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
  rng ^= rng >> 12;
  rng ^= rng << 25;
  rng ^= rng >> 27;
  return (uint32_t)((rng * 0x2545F4914F6CDD1Dull) >> 32);
}

static void batch_pre(int i);
static int batch_item;

typedef struct cstate {
  uint32_t r[16], cpsr, fpscr;
  uint32_t ext[64];
} cstate;

static void put_state(cpu *c, const cstate *st) {
  for (int i = 0; i < 16; i++) c->ops->set_reg(c, i, st->r[i]);
  c->ops->set_cpsr(c, st->cpsr);
  for (int i = 0; i < 64; i++) c->ops->set_ext(c, i, st->ext[i]);
  c->ops->set_fpscr(c, st->fpscr);
}

static void get_state(cpu *c, cstate *st) {
  for (int i = 0; i < 16; i++) st->r[i] = c->ops->get_reg(c, i);
  st->cpsr = c->ops->get_cpsr(c);
  for (int i = 0; i < 64; i++) st->ext[i] = c->ops->get_ext(c, i);
  st->fpscr = c->ops->get_fpscr(c);
}

/* First difference between two states, as text; returns 0 if none. */
static int diff_state(const cstate *init, const cstate *a, const cstate *b, char *buf, size_t n) {
  for (int i = 0; i < 16; i++)
    if (a->r[i] != b->r[i]) {
      snprintf(buf, n, "r%d dynarmic %08x interp %08x (was %08x)", i, a->r[i], b->r[i], init->r[i]);
      return 1;
    }
  if (a->cpsr != b->cpsr) {
    snprintf(buf, n, "cpsr dynarmic %08x interp %08x (was %08x)", a->cpsr, b->cpsr, init->cpsr);
    return 1;
  }
  for (int i = 0; i < 64; i++)
    if (a->ext[i] != b->ext[i]) {
      snprintf(buf, n, "s%d dynarmic %08x interp %08x (was %08x)", i, a->ext[i], b->ext[i], init->ext[i]);
      return 1;
    }
  if (a->fpscr != b->fpscr) {
    snprintf(buf, n, "fpscr dynarmic %08x interp %08x (was %08x)", a->fpscr, b->fpscr, init->fpscr);
    return 1;
  }
  return 0;
}

/* ---- random single instructions ---- */

#define CODE_SIZE 0x1000u
#define SCRATCH_SIZE 0x10000u

static cpu *dyn, *itp;
static uint32_t code_base, scratch;
static uint8_t *pattern, *dyn_mem;

static const uint32_t special_words[] = {
    0, 1, 2, 0x1F, 0x20, 0x21, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
    0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFF8000, 0x00FF00FF,
};

static uint32_t rand_reg(void) {
  switch (rnd() % 8) {
  case 0: case 1: case 2: {
    uint32_t p = scratch + 0x2000 + rnd() % (SCRATCH_SIZE - 0x4000);
    return (rnd() & 3) ? p & ~3u : p;
  }
  case 3: return rnd() % 64;
  case 4: return special_words[rnd() % (sizeof special_words / sizeof *special_words)];
  default: return rnd();
  }
}

static const uint32_t special_singles[] = {
    0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000,
    0xFFC00001, 0x7F800001, 0x7FA00000, 0x00000001, 0x807FFFFF, 0x00800000, 0x4B000000,
    0x4F000000, 0xCF000000, 0x4F800000, 0x5F000000, 0x3F000000, 0x3FC00000, 0x40490FDB,
    0x7F7FFFFF, 0x47000000, 0x46FFFE00, 0xC7000100,
};
static const uint64_t special_doubles[] = {
    0, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull,
    0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull, 0x7FF0000000000001ull,
    0x0000000000000001ull, 0x000FFFFFFFFFFFFFull, 0x41E0000000000000ull, 0xC1E0000000000000ull,
    0x41F0000000000000ull, 0x3FE0000000000000ull, 0x3FF8000000000000ull, 0x400921FB54442D18ull,
    0x7FEFFFFFFFFFFFFFull, 0x47EFFFFFE0000000ull, 0x36A0000000000000ull, 0x41DFFFFFFFC00000ull,
};

static void rand_ext(uint32_t *e) {
  switch (rnd() % 6) {
  case 0: e[0] = rnd(); e[1] = rnd(); break;
  case 1: case 2:
    e[0] = special_singles[rnd() % (sizeof special_singles / sizeof *special_singles)];
    e[1] = special_singles[rnd() % (sizeof special_singles / sizeof *special_singles)];
    break;
  case 3: {
    uint64_t d = special_doubles[rnd() % (sizeof special_doubles / sizeof *special_doubles)];
    e[0] = (uint32_t)d;
    e[1] = (uint32_t)(d >> 32);
    break;
  }
  case 4: { /* ordinary floats */
    float f[2] = {(float)((int)(rnd() % 2001) - 1000) / 8.0f, (float)(int32_t)rnd() / 65536.0f};
    memcpy(e, f, 8);
    break;
  }
  default: { /* an ordinary double */
    double d = (double)(int32_t)rnd() / (double)(rnd() % 1000 + 1);
    memcpy(e, &d, 8);
    break;
  }
  }
}

static void rand_state(cstate *st, int thumb) {
  for (int i = 0; i < 15; i++) st->r[i] = rand_reg();
  st->r[13] = scratch + SCRATCH_SIZE / 2 + (rnd() % 0x1000 & ~7u);
  st->cpsr = (rnd() & 0xF80F0000u) | 0x10u | (thumb ? 0x20u : 0);
  for (int i = 0; i < 64; i += 2) rand_ext(&st->ext[i]);
  uint32_t f = rnd() & 0xF0000000u;
  if (rnd() % 4 == 0) f |= rnd() & 0x9Fu;          /* cumulative exception flags */
  if (rnd() % 4 == 0) f |= (rnd() % 4) << 22;      /* rounding mode */
  if (rnd() % 4 == 0) f |= 1u << 24;               /* FZ */
  if (rnd() % 4 == 0) f |= 1u << 25;               /* DN */
  st->fpscr = f;
}

typedef struct gen_class {
  const char *name;
  int thumb;
  uint32_t mask, value; /* fixed bits of the (first) instruction */
  int kind;             /* see gen() */
  int enabled;
  /* results */
  uint32_t run, compared, dyn_skip, outside, unsupported, mismatched, deviation;
} gen_class;

enum { G_ARM, G_T16, G_T32, G_IT, G_ARM_EXCL, G_T32_EXCL, G_ARM_HINT, G_ARM_DIRECTED, G_VFP_XFER, G_NEON_EVEN };

static gen_class classes[] = {
    {"arm.any", 0, 0, 0, G_ARM, 1},
    {"arm.dp_imm", 0, 0x0E000000, 0x02000000, G_ARM, 1},
    {"arm.dp_reg", 0, 0x0E000010, 0x00000000, G_ARM, 1},
    {"arm.dp_rsr", 0, 0x0E000090, 0x00000010, G_ARM, 1},
    {"arm.misc", 0, 0x0F900080, 0x01000000, G_ARM, 1},
    {"arm.mul", 0, 0x0F0000F0, 0x00000090, G_ARM, 1},
    {"arm.hmul", 0, 0x0F900090, 0x01000080, G_ARM, 1},
    {"arm.sync", 0, 0x0F0000F0, 0x01000090, G_ARM, 1},
    {"arm.extra_ls", 0, 0x0E000090, 0x00000090, G_ARM, 1},
    {"arm.ls_imm", 0, 0x0E000000, 0x04000000, G_ARM, 1},
    {"arm.ls_reg", 0, 0x0E000010, 0x06000000, G_ARM, 1},
    {"arm.media", 0, 0x0E000010, 0x06000010, G_ARM, 1},
    {"arm.ldm", 0, 0x0E000000, 0x08000000, G_ARM, 1},
    {"arm.branch", 0, 0x0E000000, 0x0A000000, G_ARM, 1},
    {"arm.blx_imm", 0, 0xFE000000, 0xFA000000, G_ARM, 1},
    {"arm.hints", 0, 0, 0, G_ARM_HINT, 1},
    {"arm.excl", 0, 0, 0, G_ARM_EXCL, 1},
    {"arm.vfp_dp", 0, 0x0F000E10, 0x0E000A00, G_ARM, 1},
    {"arm.vfp_other", 0, 0x0FB00E50, 0x0EB00A40, G_ARM, 1},
    {"arm.vfp_xfer", 0, 0x0F000E10, 0x0E000A10, G_ARM, 1},
    {"arm.vfp_xfer2", 0, 0, 0, G_VFP_XFER, 1},
    {"t32.vfp_xfer2", 1, 0, 0, G_VFP_XFER, 1},
    {"arm.directed", 0, 0, 0, G_ARM_DIRECTED, 1},
    {"arm.vfp_ls", 0, 0x0E000E00, 0x0C000A00, G_ARM, 1},
    {"arm.neon_dp", 0, 0xFE000000, 0xF2000000, G_ARM, 1},
    {"arm.neon_ls", 0, 0xFF100000, 0xF4000000, G_ARM, 1},
    {"arm.neon_dp_q", 0, 0xFE000000, 0xF2000000, G_NEON_EVEN, 1},
    {"t16.any", 1, 0, 0, G_T16, 1},
    {"t16.it", 1, 0, 0, G_IT, 1},
    {"t32.ldst_mult", 1, 0xFE400000, 0xE8000000, G_T32, 1},
    {"t32.dual_excl", 1, 0xFE400000, 0xE8400000, G_T32, 1},
    {"t32.excl", 1, 0, 0, G_T32_EXCL, 1},
    {"t32.dp_shift", 1, 0xFE000000, 0xEA000000, G_T32, 1},
    {"t32.dp_modimm", 1, 0xFA008000, 0xF0000000, G_T32, 1},
    {"t32.dp_plain", 1, 0xFA008000, 0xF2000000, G_T32, 1},
    {"t32.branch", 1, 0xF8008000, 0xF0008000, G_T32, 1},
    {"t32.ldst", 1, 0xFE000000, 0xF8000000, G_T32, 1},
    {"t32.dp_reg", 1, 0xFF00F000, 0xFA00F000, G_T32, 1},
    {"t32.mul", 1, 0xFF8000C0, 0xFB000000, G_T32, 1},
    {"t32.lmul", 1, 0xFF800000, 0xFB800000, G_T32, 1},
    {"t32.vfp", 1, 0xFC000E00, 0xEC000A00, G_T32, 1},
    {"t32.neon_dp", 1, 0xEF000000, 0xEF000000, G_T32, 1},
    {"t32.neon_ls", 1, 0xFF100000, 0xF9000000, G_T32, 1},
};
#define N_CLASSES (int)(sizeof classes / sizeof *classes)

static int neon_enabled = 1; /* NEON encodings are generated */

/* ARMv8 VFP encodings dynarmic decodes even for ARMv7: VRINT{R,Z,X} and
 * VCVT{B,T} to and from double precision. */
static int vfp_v8(uint32_t w) {
  if ((w & 0x0FB00E50u) != 0x0EB00A40u) return 0; /* not "other VFP data processing" */
  uint32_t opc2 = (w >> 16) & 0xF;
  if (opc2 == 6 || (opc2 == 7 && !(w & 0x80))) return 1;
  if ((opc2 & 0xE) == 2 && (w & 0x100)) return 1;
  return 0;
}

/* ARMv8 NEON encodings (crypto, VMAXNM, VRINT, VCVTA...) that dynarmic
 * decodes even for ARMv7; w in ARM form. */
static int neon_v8(uint32_t w) {
  if ((w & 0xFE000000u) != 0xF2000000u) return 0;
  uint32_t U = (w >> 24) & 1, A = (w >> 19) & 0x1F, B = (w >> 8) & 0xF, C = (w >> 4) & 0xF;
  if (!(A & 0x10)) {
    if (B == 0xF && (C & 1) && U) return 1; /* VMAXNM, VMINNM */
    if (B == 0xC && !(C & 1)) return 1;      /* SHA1/SHA256 */
    return 0;
  }
  if (C & 1) return 0;
  if ((A & 0x16) != 0x16) return !(C & 4) && B == 0xE && ((w >> 20) & 3); /* VMULL.P64 */
  if (!U || (B & 8)) return 0;
  uint32_t a = (w >> 16) & 3, b = (w >> 6) & 0x1F;
  if (a == 3 && b < 16) return 1;                                   /* VRINT, VCVTA... */
  if (a == 0 && ((b >> 1) == 3 || (b >> 2) == 3)) return 1;         /* AES */
  if (a == 1 && ((b >> 1) & 7) == 5) return 1;                      /* SHA1H */
  if (a == 2 && (b == 13 || b == 14 || b == 15)) return 1;          /* SHA1SU1, SHA256SU0 */
  if (a == 2 && b >= 16 && (b & 0x1B) != 0x18) return 1;            /* VRINT */
  return 0;
}

static int arm_excluded(uint32_t w) {
  if (neon_v8(w)) return 1;
  uint32_t cond = w >> 28;
  if (!neon_enabled && cond == 0xF && ((w >> 25) & 0x7F) == 0x79) return 1;      /* NEON dp */
  if (!neon_enabled && cond == 0xF && (w & 0x0F100000u) == 0x04000000u) return 1; /* NEON ls */
  if (cond != 0xF && vfp_v8(w)) return 1;
  if (cond != 0xF && ((w >> 24) & 0xF) == 0xF) return 1; /* SVC */
  /* dynarmic's arm64 backend aborts on the encodings its frontend can only
   * interpret: CPS, SETEND, SRS, RFE and LDM/STM of the user registers. */
  if (cond == 0xF && (((w >> 25) & 7) == 4 || (w & 0x0FF00000u) == 0x01000000u)) return 1;
  if (cond != 0xF && ((w >> 25) & 7) == 4 && (w & (1u << 22))) return 1;
  /* ARMv8's CRC32, which dynarmic decodes even for ARMv7. */
  if (cond != 0xF && (w & 0x0F9000F0u) == 0x01000040u) return 1;
  /* Synchronization primitives with the PC as a register: unpredictable. */
  if (cond != 0xF && (w & 0x0F0000F0u) == 0x01000090u &&
      ((w & 0xF000u) == 0xF000u || (w & 0xFu) == 0xFu || (w & 0xF0000u) == 0xF0000u))
    return 1;
  if (((w >> 26) & 3) == 3 && ((w >> 8) & 0xE) != 0xA) { /* coprocessors other than VFP */
    if (cond != 0xF) return 1;
    if (((w >> 25) & 7) == 6 || ((w >> 24) & 0xF) == 0xE) return 1; /* LDC2/STC2/MCR2... */
  }
  return 0;
}

static int t32_excluded(uint32_t w) {
  uint32_t h1 = w >> 16;
  if ((h1 & 0xFF10) == 0xF900) return !neon_enabled; /* NEON element load/store */
  if ((h1 & 0xEF00) == 0xEF00) /* NEON data processing */
    return !neon_enabled || neon_v8(0xF2000000u | (w & 0x10000000u) >> 4 | (w & 0x00FFFFFFu));
  if ((h1 & 0xFC00) == 0xFC00 || (h1 & 0xFF00) == 0xFE00) return 1; /* LDC2, MCR2, ... */
  if ((h1 & 0xEC00) == 0xEC00 && vfp_v8(0xE0000000u | (w & 0x0FFFFFFFu))) return 1;
  if ((h1 & 0xEC00) == 0xEC00 && ((w >> 8) & 0xE) != 0xA) return 1;
  if ((w & 0xFFF0F0C0u) == 0xFAC0F080u) return 1; /* CRC32 (ARMv8) */
  return 0;
}

static int t16_excluded(uint32_t h) {
  return (h & 0xFF00) == 0xDF00 || (h & 0xFF00) == 0xBE00 || ((h & 0xFF00) == 0xBF00 && (h & 0xF));
}

static uint32_t rand_cond(void) { return rnd() % 10 < 7 ? 0xE : rnd() % 0xF; }

static void put16(uint8_t *p, uint32_t h) {
  p[0] = (uint8_t)h;
  p[1] = (uint8_t)(h >> 8);
}
static void put32(uint8_t *p, uint32_t w) {
  put16(p, w);
  put16(p + 2, w >> 16);
}
static void put_t32(uint8_t *p, uint32_t w) {
  put16(p, w >> 16);
  put16(p + 2, w);
}

static uint32_t rand_t16(void) {
  uint32_t h;
  do h = rnd() % 0xE800; while (t16_excluded(h));
  return h;
}

static uint32_t rand_t32(uint32_t mask, uint32_t value) {
  uint32_t w;
  do {
    w = (rnd() & ~mask) | value;
  } while ((w >> 27) < 0x1D || t32_excluded(w));
  return w;
}

static uint32_t R(void) { return rnd() % 15; } /* a register other than the PC */

/* ARM miscellaneous, media and divide instructions with valid fixed fields
 * (random words in these spaces are mostly unpredictable). */
static uint32_t arm_directed(void) {
  uint32_t d = R(), n = R(), m = R(), a = R();
  switch (rnd() % 26) {
  case 0: return 0x012FFF10 | m;                                      /* BX */
  case 1: return 0x012FFF30 | m;                                      /* BLX reg */
  case 2: return 0x016F0F10 | d << 12 | m;                            /* CLZ */
  case 3: return 0x01000050 | (rnd() % 4) << 21 | n << 16 | d << 12 | m; /* QADD... */
  case 4: return 0x010F0000 | d << 12;                                /* MRS */
  case 5: return 0x0120F000 | (rnd() & 0xF) << 16 | m;                /* MSR reg */
  case 6: return 0x06800010 | n << 16 | d << 12 | (rnd() & 0x1F) << 7 | (rnd() & 1) << 6 | m; /* PKH */
  case 7: { static const uint32_t o[6] = {0, 2, 3, 4, 6, 7};        /* extends */
    return 0x06800070 | o[rnd() % 6] << 20 | (rnd() % 4 ? 15 : n) << 16 | d << 12 | (rnd() % 4) << 10 | m; }
  case 8: return 0x06800FB0 | n << 16 | d << 12 | m;                  /* SEL */
  case 9: return 0x06A00010 | (rnd() & 0x1F) << 16 | d << 12 | (rnd() & 0x1F) << 7 | (rnd() & 1) << 6 | n;
  case 10: return 0x06E00010 | (rnd() & 0x1F) << 16 | d << 12 | (rnd() & 0x1F) << 7 | (rnd() & 1) << 6 | n;
  case 11: return 0x06A00F30 | (rnd() & 0xF) << 16 | d << 12 | n;    /* SSAT16 */
  case 12: return 0x06E00F30 | (rnd() & 0xF) << 16 | d << 12 | n;    /* USAT16 */
  case 13: { static const uint32_t o[4] = {0x06BF0F30, 0x06BF0FB0, 0x06FF0F30, 0x06FF0FB0};
    return o[rnd() % 4] | d << 12 | m; }                               /* REV, REV16, RBIT, REVSH */
  case 14: { static const uint32_t op[6] = {0, 1, 2, 3, 4, 7};       /* parallel add/sub */
    return 0x06000F10 | (rnd() & 1) << 22 | (rnd() % 3 + 1) << 20 | n << 16 | d << 12 | op[rnd() % 6] << 5 | m; }
  case 15: return 0x07000010 | d << 16 | (rnd() % 3 ? a : 15) << 12 | m << 8 | (rnd() % 4) << 5 | n; /* SMLAD.. */
  case 16: return 0x07400010 | d << 16 | a << 12 | m << 8 | (rnd() % 4) << 5 | n;  /* SMLALD.. */
  case 17: { static const uint32_t op[4] = {0, 1, 6, 7};             /* SMMLA, SMMUL, SMMLS */
    uint32_t o = op[rnd() % 4];
    return 0x07500010 | d << 16 | (o < 2 && rnd() % 3 == 0 ? 15 : a) << 12 | m << 8 | o << 5 | n; }
  case 18: return 0x07800010 | d << 16 | (rnd() % 2 ? 15 : a) << 12 | m << 8 | n; /* USAD8, USADA8 */
  case 19: { uint32_t lsb = rnd() % 32, w = rnd() % (32 - lsb);       /* SBFX, UBFX */
    return (rnd() & 1 ? 0x07A00050 : 0x07E00050) | w << 16 | d << 12 | lsb << 7 | n; }
  case 20: { uint32_t lsb = rnd() % 32, msb = lsb + rnd() % (32 - lsb); /* BFI, BFC */
    return 0x07C00010 | msb << 16 | d << 12 | lsb << 7 | (rnd() % 4 ? n : 15); }
  case 21: return (rnd() & 1 ? 0x0710F010 : 0x0730F010) | d << 16 | m << 8 | n; /* SDIV, UDIV */
  case 22: { uint32_t op = rnd() % 4, x = rnd() & 3;                 /* halfword multiplies */
    return 0x01000080 | op << 21 | d << 16 | a << 12 | m << 8 | x << 5 | n; }
  case 23: return 0x00000090 | (rnd() & 0xF) << 20 | d << 16 | a << 12 | m << 8 | n; /* multiplies */
  case 24: return 0x03200000 | (rnd() & 0xF) << 16 | 0xF000 | (rnd() & 0xFFF); /* MSR imm */
  default: return 0x0320F000 | (rnd() % 5);                           /* NOP, YIELD, WFE, WFI, SEV */
  }
}

/* VFP and NEON core <-> extension register transfers, valid fields. */
static uint32_t vfp_xfer(void) {
  uint32_t t = R(), t2 = R(), vn = rnd() & 0xF, N = rnd() & 1;
  switch (rnd() % 8) {
  case 0: return 0x0E000A10 | vn << 16 | t << 12 | N << 7;            /* VMOV s, r */
  case 1: return 0x0E100A10 | vn << 16 | t << 12 | N << 7;            /* VMOV r, s */
  case 2: return 0x0EF10A10 | (rnd() % 4 ? t : 15) << 12;             /* VMRS */
  case 3: return 0x0EE10A10 | t << 12;                                /* VMSR */
  case 4: { uint32_t opc = rnd() & 0xF;                               /* VMOV scalar, r */
    if ((opc & 0xB) == 2 || (opc & 0xB) == 0xA) opc &= ~2u;
    return 0x0E000B10 | (opc >> 2) << 21 | vn << 16 | t << 12 | N << 7 | (opc & 3) << 5; }
  case 5: { uint32_t opc = rnd() & 0xF, U = rnd() & 1;                /* VMOV r, scalar */
    if ((opc & 0xB) == 2 || (opc & 0xB) == 0xA) opc &= ~2u;
    if ((opc & 0xB) == 0) U = 0;
    return 0x0E100B10 | U << 23 | (opc >> 2) << 21 | vn << 16 | t << 12 | N << 7 | (opc & 3) << 5; }
  case 6: { uint32_t be = rnd() % 3, Q = rnd() & 1;                   /* VDUP */
    if (Q) vn &= ~1u;
    return 0x0E800B10 | (be >> 1) << 22 | Q << 21 | vn << 16 | t << 12 | N << 7 | (be & 1) << 5; }
  default: {                                                          /* VMOV two regs */
    uint32_t L = rnd() & 1, C = rnd() & 1, m = rnd() & 0xF, M = rnd() & 1;
    if (L && t2 == t) t2 = (t + 1) % 15;
    if (!C && m == 15 && M) M = 0;
    return 0x0C400A10 | L << 20 | t2 << 16 | t << 12 | C << 8 | M << 5 | m; }
  }
}

/* Fills code, returns its length in bytes; *steps instructions to run. */
static int gen(const gen_class *g, uint8_t *code, int *steps) {
  *steps = 1;
  switch (g->kind) {
  case G_ARM: {
    uint32_t w;
    do {
      w = (rnd() & ~g->mask) | g->value;
      if (!(g->mask & 0xF0000000u)) w = (w & 0x0FFFFFFFu) | rand_cond() << 28;
    } while (arm_excluded(w));
    put32(code, w);
    return 4;
  }
  case G_ARM_EXCL: { /* ldrex{,b,h,d} then strex of the same size at the same address */
    static const uint32_t ld[4] = {0x01900F9F, 0x01D00F9F, 0x01F00F9F, 0x01B00F9F};
    static const uint32_t st[4] = {0x01800F90, 0x01C00F90, 0x01E00F90, 0x01A00F90};
    int sz = (int)(rnd() % 4), n = (int)(rnd() % 13);
    int t = (int)(rnd() % 7) * 2, d = (int)(rnd() % 13), t2 = (int)(rnd() % 6) * 2;
    put32(code, 0xE0000000u | ld[sz] | (uint32_t)n << 16 | (uint32_t)t << 12);
    uint32_t n2 = rnd() % 8 ? (uint32_t)n : rnd() % 13;
    put32(code + 4, 0xE0000000u | st[sz] | n2 << 16 | (uint32_t)d << 12 | (uint32_t)t2);
    *steps = 2;
    return 8;
  }
  case G_ARM_HINT: { /* PLD, PLDW, PLI (immediate, register), barriers, CLREX
                     * (random hint encodings trip an assertion in dynarmic) */
    static const uint32_t base[] = {0xF510F000, 0xF450F000, 0xF710F000, 0xF650F000,
                                    0xF57FF040, 0xF57FF050, 0xF57FF060, 0xF57FF01F};
    int k = (int)(rnd() % 8);
    uint32_t w = base[k];
    if (k < 4) {
      w |= (rnd() & 1) << 23 | (uint32_t)(rnd() % 15) << 16;
      if (k == 0 || k == 2) w |= (rnd() & 1) << 22;
      w |= k < 2 ? rnd() & 0xFFF : (rnd() & 0xFE0) | (rnd() % 15);
    } else if (k < 7) {
      w |= rnd() & 0xF;
    }
    put32(code, w);
    return 4;
  }
  case G_NEON_EVEN: { /* NEON data processing with even (Q-usable) registers */
    uint32_t w;
    do w = ((rnd() & ~g->mask) | g->value) & ~0x00011001u; while (arm_excluded(w));
    put32(code, w);
    return 4;
  }
  case G_ARM_DIRECTED:
    put32(code, rand_cond() << 28 | arm_directed());
    return 4;
  case G_VFP_XFER:
    if (g->thumb) put_t32(code, 0xE0000000u | vfp_xfer());
    else put32(code, rand_cond() << 28 | vfp_xfer());
    return 4;
  case G_T16:
    put16(code, rand_t16());
    return 2;
  case G_T32:
    put_t32(code, rand_t32(g->mask, g->value));
    return 4;
  case G_T32_EXCL: { /* LDREX/STREX (word), and the B/H/D forms */
    int sz = (int)(rnd() % 4), n = (int)(rnd() % 13);
    int t = (int)(rnd() % 13), t2 = (int)(rnd() % 13), d = (int)(rnd() % 13);
    uint32_t ld, st;
    if (sz == 0) {
      uint32_t imm = rnd() % 4;
      ld = 0xE8500F00u | (uint32_t)n << 16 | (uint32_t)t << 12 | imm;
      st = 0xE8400000u | (uint32_t)n << 16 | (uint32_t)t2 << 12 | (uint32_t)d << 8 | imm;
    } else {
      static const uint32_t op[4] = {0, 0x40, 0x50, 0x70};
      ld = 0xE8D00F0Fu | (uint32_t)n << 16 | (uint32_t)t << 12 | op[sz] | (sz == 3 ? 0 : 0xF00);
      if (sz == 3) ld = (ld & ~0xF00u) | (uint32_t)((t + 1) % 13) << 8;
      st = 0xE8C00F00u | (uint32_t)n << 16 | (uint32_t)t2 << 12 | op[sz] | (uint32_t)d;
      if (sz == 3) st = (st & ~0xF00u) | (uint32_t)((t2 + 1) % 13) << 8;
    }
    put_t32(code, ld);
    put_t32(code + 4, st);
    *steps = 2;
    return 8;
  }
  default: { /* IT followed by the instructions of its block */
    uint32_t firstcond = rnd() % 15, mask = rnd() % 15 + 1;
    if (firstcond == 0xE) mask = 8 >> (rnd() % 4) << 0; /* AL: no else-branches */
    if (firstcond == 0xE) mask = (mask & 8) ? 8 : (mask & 4) ? 0xC : (mask & 2) ? 0xE : 0xF;
    put16(code, 0xBF00 | firstcond << 4 | mask);
    int n = 4 - __builtin_ctz(mask), len = 2;
    for (int i = 0; i < n; i++) {
      if (rnd() % 3 == 0) {
        static const uint32_t m32[][2] = {{0xFE000000, 0xEA000000}, {0xFA008000, 0xF0000000},
                                          {0xFE000000, 0xF8000000}, {0xFF00F000, 0xFA00F000}};
        int k = (int)(rnd() % 4);
        put_t32(code + len, rand_t32(m32[k][0], m32[k][1]));
        len += 4;
      } else {
        put16(code + len, rand_t16());
        len += 2;
      }
    }
    *steps = 1 + n;
    return len;
  }
  }
}

static int verbose;
static int saved_stderr = -1;

/* The tests provoke guest faults on purpose; both backends report those on
 * stderr. DL_INTERP_VERBOSE=1 keeps them. */
static void silence_begin(void) {
  fflush(stderr);
  verbose = getenv("DL_INTERP_VERBOSE") != NULL;
  saved_stderr = dup(2);
  int devnull = open("/dev/null", O_WRONLY);
  if (!verbose) dup2(devnull, 2);
  close(devnull);
  cpu_interp_set_quiet(!verbose);
}

static void silence_end(void) {
  fflush(stderr);
  dup2(saved_stderr, 2);
  close(saved_stderr);
  cpu_interp_set_quiet(0);
}

static uint32_t sentinel_addr;

/* One dynarmic step of the global dyn cpu (see run_batched for aborts). */
static int dyn_step(cpu *unused) {
  (void)unused;
  return cpu_dynarmic_step(dyn);
}

static uint8_t code_copy[CODE_SIZE];

/* Clears both exclusive monitors: a CLREX at the start of the code buffer. */
static void clear_monitors(void) {
  put32(code_copy, 0xF57FF01Fu);
  memcpy(g2h(code_base), code_copy, 4);
  dyn->ops->set_reg(dyn, 15, code_base);
  dyn->ops->set_cpsr(dyn, 0x10);
  cpu_dynarmic_step(dyn);
  itp->ops->set_reg(itp, 15, code_base);
  itp->ops->set_cpsr(itp, 0x10);
  cpu_interp_step(itp);
}

/* Exclusive and swap instructions need aligned addresses, or both hosts'
 * atomics take a bus error; real code aligns them. */
static int is_exclusive(const uint8_t *code, int len, int thumb) {
  for (int i = 0; i < len; i += 2) {
    uint32_t h1 = code[i] | code[i + 1] << 8;
    if (thumb) {
      if (h1 < 0xE800 || i + 4 > len) continue;
      uint32_t w = h1 << 16 | code[i + 2] | code[i + 3] << 8;
      if ((w & 0xFFE00000u) == 0xE8400000u || (w & 0xFFE000C0u) == 0xE8C00040u) return 1;
      i += 2;
    } else if (!(i & 2)) {
      uint32_t w = h1 | (uint32_t)(code[i + 2] | code[i + 3] << 8) << 16;
      if ((w & 0x0F0000F0u) == 0x01000090u) return 1;
    }
  }
  return 0;
}

static void dump_state(const char *what, const cstate *st) {
  fprintf(out, "  %s: cpsr %08x fpscr %08x\n ", what, st->cpsr, st->fpscr);
  for (int i = 0; i < 16; i++) fprintf(out, " r%d=%08x%s", i, st->r[i], i % 8 == 7 ? "\n " : "");
  for (int i = 0; i < 64; i++) fprintf(out, " s%d=%08x%s", i, st->ext[i], i % 8 == 7 ? "\n " : "");
  fprintf(out, "\n");
}

/* Known differences where dynarmic departs from the ARM ARM; the
 * interpreter follows the manual. */
static int deviation(const uint8_t *code, int len, int thumb, const cstate *st, const cstate *sd,
                     const cstate *si) {
  if (len != 4) return 0;
  uint32_t w = thumb ? (uint32_t)(code[0] | code[1] << 8) << 16 | code[2] | code[3] << 8
                     : (uint32_t)(code[0] | code[1] << 8 | code[2] << 16 | (uint32_t)code[3] << 24);
  /* VCVT fixed -> float: dynarmic always rounds to nearest, ARM by FPSCR.RMode. */
  if ((w & 0x0FBA0E50u) == 0x0EBA0A40u && (st->fpscr & 0x00C00000u)) return 1;
  /* VCVT between float and fixed point (VFP and NEON): dynarmic's
   * exception flags are off (no Inexact; Invalid for results ARM rounds to
   * zero and flags Inexact). Results must still match. */
  if (thumb && (w & 0xEF000000u) == 0xEF000000u)
    w = 0xF2000000u | (w & 0x10000000u) >> 4 | (w & 0x00FFFFFFu);
  if ((w & 0x0FBA0E50u) == 0x0EBA0A40u) {
    cstate a = *sd, b = *si;
    a.fpscr &= ~0x9Fu;
    b.fpscr &= ~0x9Fu;
    if (!memcmp(&a, &b, sizeof a)) return 1;
  }
  /* NEON float operations: dynarmic misses Inexact on conversions and
   * computes D-register operations on 128 bits, raising flags from the
   * unused upper half. Only the cumulative exception flags may differ. */
  if ((w & 0xFE000000u) == 0xF2000000u) {
    cstate a = *sd, b = *si;
    a.fpscr &= ~0x9Fu;
    b.fpscr &= ~0x9Fu;
    if (!memcmp(&a, &b, sizeof a)) return 1;
  }
  return 0;
}

static void one_case(gen_class *g) {
  uint8_t code[32];
  int steps;
  int len = gen(g, code, &steps);
  cstate st, sd, si;
  rand_state(&st, g->thumb);
  uint32_t pc = code_base + 0x100 + (g->thumb && (rnd() & 1) ? 2 : 0);
  st.r[15] = pc;
  if (is_exclusive(code, len, g->thumb))
    for (int i = 0; i < 13; i++)
      st.r[i] = (scratch + 0x2000 + rnd() % (SCRATCH_SIZE - 0x4000)) & ~7u;
  g->run++;

  char what[160];
  char insn[40];
  if (len == 2) snprintf(insn, sizeof insn, "%04x", code[0] | code[1] << 8);
  else if (g->thumb) {
    int o = 0;
    insn[0] = 0;
    for (int i = 0; i < len; i += 2)
      o += snprintf(insn + o, sizeof insn - (size_t)o, "%s%04x", i ? " " : "", code[i] | code[i + 1] << 8);
  } else {
    snprintf(insn, sizeof insn, "%08x", code[0] | code[1] << 8 | code[2] << 16 | (uint32_t)code[3] << 24);
    if (len == 8)
      snprintf(insn + 8, sizeof insn - 8, " %08x",
               code[4] | code[5] << 8 | code[6] << 16 | (uint32_t)code[7] << 24);
  }
  if (verbose) fprintf(out, "case %s %s\n", g->name, insn);
  batch_pre(batch_item);

  clear_monitors();
  memset(code_copy + 4, 0, CODE_SIZE - 4);
  memcpy(code_copy + (pc - code_base), code, (size_t)len);
  memcpy(g2h(code_base), code_copy, CODE_SIZE);
  dyn->ops->invalidate(dyn, pc, 32);
  memcpy(g2h(scratch), pattern, SCRATCH_SIZE);
  put_state(dyn, &st);
  dyn->faulted = 0;
  int r = 0;
  for (int i = 0; i < steps && r == 0; i++) r = dyn_step(dyn);
  if (r != 0 || dyn->faulted) {
    g->dyn_skip++;
    return;
  }
  get_state(dyn, &sd);
  memcpy(dyn_mem, g2h(scratch), SCRATCH_SIZE);

  memcpy(g2h(code_base), code_copy, CODE_SIZE);
  memcpy(g2h(scratch), pattern, SCRATCH_SIZE);
  put_state(itp, &st);
  itp->faulted = 0;
  r = 0;
  for (int i = 0; i < steps && r == 0; i++) r = cpu_interp_step(itp);
  if (r != 0) {
    if (r < 0 && cpu_interp_fault_kind(itp) == 2) {
      g->outside++;
      return;
    }
    g->unsupported++;
    if (g->unsupported <= 4)
      fprintf(out, "interp-test: %s %s: the interpreter %s, dynarmic ran it\n", g->name, insn,
              r < 0 ? (cpu_interp_fault_kind(itp) == 3 ? "has no such instruction" : "faulted")
                    : "halted");
    return;
  }
  g->compared++;
  get_state(itp, &si);
  int bad = diff_state(&st, &sd, &si, what, sizeof what);
  if (!bad && memcmp(dyn_mem, g2h(scratch), SCRATCH_SIZE) != 0) {
    const uint8_t *m = g2h(scratch);
    uint32_t o = 0;
    while (dyn_mem[o] == m[o]) o++;
    snprintf(what, sizeof what, "memory at 0x%08x: dynarmic %02x interp %02x (was %02x)", scratch + o,
             dyn_mem[o], m[o], pattern[o]);
    bad = 1;
  }
  if (bad && deviation(code, len, g->thumb, &st, &sd, &si)) {
    g->deviation++;
    return;
  }
  if (bad) {
    g->mismatched++;
    if (g->mismatched <= 6) {
      fprintf(out, "interp-test: %s %s: %s\n", g->name, insn, what);
      if (getenv("DL_INTERP_DUMP")) dump_state("before", &st);
    }
  }
}

/* ---- crash-proof batches ----
 *
 * dynarmic's translator asserts on a few undefined encodings, and abort()
 * can't be caught on this macOS. So the random and lockstep tests run in a
 * forked child that reports, through a pipe, the rng state before each case
 * steps and its counters after it. If the child dies, only its current case
 * is lost: the parent counts it as skipped and forks a new child from there.
 * (The child's writes to guest memory stay in the child too.) */
typedef struct batch_rec {
  int kind, next; /* kind 1: about to step case next; 2: done up to next */
  uint64_t rng;
  uint8_t stats[192];
} batch_rec;

static int batch_fd = -1, batch_first;

static void batch_pre(int i) {
  if (batch_fd < 0) return;
  batch_rec r = {1, i, rng, {0}};
  if (write(batch_fd, &r, sizeof r) != sizeof r) _exit(3);
}

static int run_batched(int total, void (*item)(int i, void *stats), void *stats, size_t size,
                       void (*on_crash)(int i, void *stats)) {
  int next = 0, deaths = 0;
  while (next < total) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    fflush(out);
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
      close(fds[0]);
      batch_fd = fds[1];
      batch_first = next;
      /* macOS carries neither JIT memory nor dynarmic's fastmem fault handler
       * thread into a fork child: a fresh dynarmic cpu without fastmem
       * (the parent's stays untouched). */
      dyn = cpu_dynarmic_create_checked();
      dyn->ret_sentinel = sentinel_addr;
      for (int i = next; i < total; i++) {
        item(i, stats);
        batch_rec r = {2, i + 1, rng, {0}};
        memcpy(r.stats, stats, size);
        if (write(batch_fd, &r, sizeof r) != sizeof r) _exit(3);
      }
      fflush(out);
      _exit(0);
    }
    close(fds[1]);
    batch_rec r, pre = {0, -1, 0, {0}};
    ssize_t got;
    while ((got = read(fds[0], &r, sizeof r)) == sizeof r) {
      if (r.kind == 1) {
        pre = r;
      } else {
        memcpy(stats, r.stats, size);
        next = r.next;
        rng = r.rng;
      }
    }
    close(fds[0]);
    int st;
    waitpid(pid, &st, 0);
    if (next >= total) break;
    if (pre.next != next) return -1; /* died outside a case */
    rng = pre.rng;
    on_crash(next, stats);
    next++;
    deaths++;
  }
  return deaths;
}

static gen_class *batch_class;
static int batch_item;

static void random_item(int i, void *stats) {
  (void)stats;
  if (i % 256 == 0 || i == batch_first)
    for (uint32_t k = 0; k < SCRATCH_SIZE; k += 4) {
      uint32_t v = rnd();
      memcpy(pattern + k, &v, 4);
    }
  batch_item = i;
  one_case(batch_class);
}

static void random_crash(int i, void *stats) {
  (void)i;
  gen_class *g = stats;
  g->run++;
  g->dyn_skip++;
}

static const char *only;

static int random_tests(int per_class) {
  code_base = gm_alloc(CODE_SIZE, 0, "interp-test code");
  scratch = gm_alloc(SCRATCH_SIZE, 0, "interp-test scratch");
  pattern = malloc(SCRATCH_SIZE);
  dyn_mem = malloc(SCRATCH_SIZE);
  if (!code_base || !scratch || !pattern || !dyn_mem) return -1;
  cpu_interp_set_checked(itp, 1);
  cpu_interp_set_window(itp, code_base, scratch + SCRATCH_SIZE - 1);

  /* dynarmic reports the faults these tests provoke on stderr. */
  silence_begin();

  for (int c = 0; c < N_CLASSES; c++) {
    gen_class *g = &classes[c];
    if (!g->enabled) continue;
    if (only && !strstr(g->name, only)) {
      g->enabled = 0;
      continue;
    }
    batch_class = g;
    if (run_batched(per_class, random_item, g, sizeof *g, random_crash) < 0) {
      fprintf(out, "interp-test: %s: the test process died outside a case\n", g->name);
      g->unsupported++;
    }
  }

  silence_end();
  cpu_interp_set_window(itp, 0, 0);
  cpu_interp_set_checked(itp, 0);

  int bad = 0;
  fprintf(out, "interp-test: %-15s %8s %8s %8s %8s %8s %8s %8s\n", "class", "run", "compared",
          "skipped", "outside", "unsupp.", "differ", "dynarmic");
  for (int c = 0; c < N_CLASSES; c++) {
    gen_class *g = &classes[c];
    if (!g->enabled) continue;
    fprintf(out, "interp-test: %-15s %8u %8u %8u %8u %8u %8u %8u\n", g->name, g->run, g->compared,
            g->dyn_skip, g->outside, g->unsupported, g->mismatched, g->deviation);
    bad += (int)(g->unsupported + g->mismatched);
  }
  return bad ? -1 : 0;
}

/* ---- whole guest memory snapshots ---- */

/* Every committed guest region (only the used part of the heap), so a run on
 * one backend can be undone before the same run on the other, and the two
 * results compared. */
#define MAX_SNAP 1024
typedef struct snapshot {
  int n;
  uint32_t start[MAX_SNAP], len[MAX_SNAP];
  uint8_t *data[MAX_SNAP];
  guest_heap_state heap;
} snapshot;

static void snap_take(snapshot *s) {
  guest_heap_save(&s->heap);
  uint32_t a, b;
  const char *tag;
  int n = 0;
  for (int i = 0; (tag = gm_region_at(i, &a, &b)) && n < MAX_SNAP; i++) {
    uint32_t len = b - a + 1;
    if (s->heap.base && a == s->heap.base) len = s->heap.next - a;
    if (!strcmp(tag, "interp-test code") || !strcmp(tag, "interp-test scratch")) continue;
    if (s->n > n && s->start[n] == a && s->len[n] >= len) {
      s->len[n] = len;
    } else {
      if (s->n > n) free(s->data[n]);
      s->start[n] = a;
      s->len[n] = len;
      s->data[n] = malloc(len ? len : 1);
    }
    memcpy(s->data[n], g2h(a), len);
    n++;
  }
  for (int i = n; i < s->n; i++) free(s->data[i]);
  s->n = n;
}

static void snap_restore(const snapshot *s) {
  for (int i = 0; i < s->n; i++) memcpy(g2h(s->start[i]), s->data[i], s->len[i]);
  guest_heap_restore(&s->heap);
}

/* Compares guest memory now with a snapshot; describes the first difference. */
static int snap_diff(const snapshot *s, char *what, size_t n) {
  guest_heap_state h;
  guest_heap_save(&h);
  if (h.next != s->heap.next) {
    snprintf(what, n, "heap top dynarmic %08x interp %08x", s->heap.next, h.next);
    return 1;
  }
  for (int i = 0; i < s->n; i++) {
    const uint8_t *m = g2h(s->start[i]);
    if (!memcmp(m, s->data[i], s->len[i])) continue;
    uint32_t o = 0;
    while (m[o] == s->data[i][o]) o++;
    snprintf(what, n, "memory at 0x%08x (%s): dynarmic %02x interp %02x", s->start[i] + o,
             gm_region_tag(s->start[i]), s->data[i][o], m[o]);
    return 1;
  }
  return 0;
}

static snapshot snap_before, snap_dyn;
static uint32_t stack_top;

/* The same starting state for both backends: one stack (dynarmic's), one
 * return sentinel, user mode, FPSCR 0. */
static void reset_cpu(cpu *c) {
  for (int i = 0; i < 15; i++) c->ops->set_reg(c, i, 0);
  c->ops->set_reg(c, CPU_REG_SP, stack_top);
  c->ops->set_cpsr(c, 0x10);
  for (int i = 0; i < 64; i++) c->ops->set_ext(c, i, 0);
  c->ops->set_fpscr(c, 0);
  c->ret_sentinel = dyn->ret_sentinel;
  c->faulted = 0;
}

/* ---- real functions from the game's libraries (the profile's fn_tests) ---- */

/* The game's (droidlift.h: dl_fn_test), NULL-terminated by name. */
static const dl_fn_test *fn_tests;
static uint32_t testbuf; /* guest buffer for arguments, 1 MiB */
#define TESTBUF_SIZE 0x100000u

static void fill(uint8_t *p, uint32_t n, uint32_t seed) {
  for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 7u + seed * 13u + (i >> 5) * (i >> 9));
}

static int compare_cpus(const char *name, char *what, size_t n) {
  cstate a, b;
  get_state(dyn, &a);
  get_state(itp, &b);
  (void)name;
  return diff_state(&a, &a, &b, what, n);
}

/* The recompiler-fallback API (cpu_interp_load/store/run_until): a byte sum run
 * in slices of 37 instructions, the state stored and loaded into a fresh
 * interpreter cpu between slices, must give the host's sum. */
static int slice_left;
static int stop_after_slice(uint32_t pc, void *ctx) {
  (void)pc;
  (void)ctx;
  return --slice_left <= 0;
}

/* A byte sum in ARM code: r0 = sum of the r2 bytes at r1.
 *   mov r0, #0; loop: cmp r2, #0; beq done; ldrb r3, [r1], #1;
 *   add r0, r0, r3; sub r2, r2, #1; b loop; done: bx lr */
static const uint32_t byte_sum_code[] = {0xE3A00000, 0xE3520000, 0x0A000003, 0xE4D13001,
                                         0xE0800003, 0xE2422001, 0xEAFFFFF9, 0xE12FFF1E};

static int fallback_api_test(void) {
  uint32_t code = gm_alloc(0x1000, 0, "interp-test byte sum");
  if (!code) return 1;
  memcpy(g2h(code), byte_sum_code, sizeof byte_sum_code);
  fill(g2h(testbuf), 5000, 77);
  uint32_t want = 0;
  for (uint32_t i = 0; i < 5000; i++) want += ((uint8_t *)g2h(testbuf))[i];
  cpu_interp_regs st;
  memset(&st, 0, sizeof st);
  st.r[1] = testbuf;
  st.r[2] = 5000;
  st.r[13] = stack_top;
  st.r[14] = dyn->ret_sentinel;
  st.r[15] = code;
  st.cpsr = 0x10;
  int r = 0, slices = 0;
  while (r == 0 && slices < 100000) {
    cpu *c = cpu_interp_create();
    c->ret_sentinel = dyn->ret_sentinel;
    cpu_interp_load(c, &st);
    slice_left = 37;
    r = cpu_interp_run_until(c, stop_after_slice, NULL);
    cpu_interp_store(c, &st);
    c->ops->destroy(c);
    slices++;
  }
  int ok = r == 1 && st.r[0] == want && slices > 10;
  fprintf(out, "interp-test: fallback API (a byte sum in %d slices): %s\n", slices, ok ? "matched" : "WRONG");
  return !ok;
}

/* The interpreter as the recompiler's rc_fallback: guest ARM code
 *   f: push {lr}; bl g; add r0, r0, #1; svc #0x123; pop {pc}
 *   g: lsl r0, r0, #1; bx lr  (never interpreted: g is "recompiled")
 * with g registered as a recompiled function (a host C function), run by
 * rc_run from f to a stop address. Control must go interpreter -> g ->
 * interpreter, the SVC through rc_svc, and stop at f's return. */
static uint32_t rcf_base;
static int rcf_g_calls, rcf_svcs;
static uint32_t rcf_svc_pc;

static void rcf_g(rc_state *s) {
  rcf_g_calls++;
  s->r[0] <<= 1;
  rc_set_pc(s, s->r[14]);
}

static void rcf_svc(rc_state *s, uint32_t imm) {
  if (imm == 0x123) rcf_svcs++;
  rcf_svc_pc = s->r[15];
  s->r[0] += 100;
}

static int rc_fallback_test(void) {
  static const uint32_t f[] = {0xE92D4000, 0xEB000002, 0xE2800001, 0xEF000123,
                               0xE8BD8000, 0xE1A00080, 0xE12FFF1E};
  uint32_t code = gm_alloc(0x1000, 0, "interp-test rc code");
  if (!code) return 1;
  memcpy(g2h(code), f, sizeof f);
  rcf_base = code;
  static const rc_func funcs[] = {{0x14, rcf_g, "g"}};
  static rc_module mod = {"interp-test", &rcf_base, funcs, 1};
  rc_register_module(&mod);
  rc_fallback = interp_rc_fallback;
  rc_svc = rcf_svc;

  rc_state s;
  memset(&s, 0, sizeof s);
  uint32_t stop = code + 0x800;
  s.r[0] = 20;
  s.r[13] = stack_top;
  s.r[14] = stop;
  s.r[15] = code;
  s.cpsr = 0x10;
  int r = rc_run(&s, stop);
  int ok = r == 0 && s.r[0] == 141 && rcf_g_calls == 1 && rcf_svcs == 1 &&
           rcf_svc_pc == code + 0x10 && s.r[13] == stack_top && s.r[15] == stop;
  fprintf(out, "interp-test: recompiler fallback (interpreter -> recompiled -> interpreter): %s\n",
          ok ? "matched" : "WRONG");
  if (!ok)
    fprintf(out, "interp-test:   rc_run %d, r0 %u, g %d, svc %d at %08x, sp %08x, pc %08x\n", r, s.r[0],
            rcf_g_calls, rcf_svcs, rcf_svc_pc, s.r[13], s.r[15]);
  return !ok;
}

static int function_tests(void) {
  int bad = 0;
  for (const dl_fn_test *t = fn_tests; t && t->name; t++) {
    char what[200];
    /* Warm-up on dynarmic: lazy one-time setup (errno blocks, OpenSSL's CPU
     * probe) happens outside the compared runs. */
    reset_cpu(dyn);
    t->run(dyn, testbuf);
    snap_take(&snap_before);
    reset_cpu(dyn);
    int rd = t->run(dyn, testbuf);
    snap_take(&snap_dyn);
    snap_restore(&snap_before);
    reset_cpu(itp);
    int ri = t->run(itp, testbuf);
    int diff = compare_cpus(t->name, what, sizeof what);
    if (!diff) diff = snap_diff(&snap_dyn, what, sizeof what);
    if (rd != 0 || ri != 0 || diff) {
      bad++;
      fprintf(out, "interp-test: %s: %s\n", t->name,
              rd ? "wrong under dynarmic" : ri ? "wrong under the interpreter" : what);
    } else {
      fprintf(out, "interp-test: %s: matched\n", t->name);
    }
    snap_restore(&snap_before);
  }
  return bad;
}

/* ---- a broad sample of the game's functions, in lockstep ---- */

/* Runs one function from the same state on both backends instruction by
 * instruction, for up to LOCKSTEP_BUDGET instructions, until it returns, or
 * until it is about to call an import (shims with garbage arguments could
 * hurt the host), then compares registers and all of guest memory. The
 * arguments point into a zeroed buffer, so the functions take their
 * early-out and null-object paths: short runs, but through real code. */
#define LOCKSTEP_BUDGET 20000

static int at_boundary(uint32_t pc) {
  const char *tag = gm_region_tag(pc);
  return !tag || !strcmp(tag, "dynlib thunks") || !strcmp(tag, "host thunks") ||
         !strcmp(tag, "jni thunks") || !strcmp(tag, "cpu return sentinel");
}

static int run_steps(cpu *c, int (*step)(cpu *), uint32_t limit, uint32_t *done) {
  uint32_t n = 0;
  int r = 0;
  while (n < limit) {
    if (at_boundary(c->ops->get_reg(c, CPU_REG_PC))) {
      r = 1;
      break;
    }
    r = step(c);
    n++;
    if (r) break;
  }
  *done = n;
  return r;
}

static void lockstep_state(cpu *c, uint32_t fn, const uint32_t *args) {
  reset_cpu(c);
  for (int i = 0; i < 4; i++) c->ops->set_reg(c, i, args[i]);
  c->ops->set_reg(c, CPU_REG_LR, c->ret_sentinel);
  c->ops->set_reg(c, CPU_REG_PC, fn & ~1u);
  c->ops->set_cpsr(c, 0x10 | ((fn & 1) ? CPU_CPSR_T : 0));
}

static int dyn_only; /* dynarmic raised an exception the interpreter didn't */

static int lockstep_one(uint32_t fn, uint64_t *steps_total, int item) {
  uint32_t args[4];
  for (int i = 0; i < 4; i++) args[i] = rnd() % 3 ? testbuf + 0x1000 * (uint32_t)(i + 1) : rnd() % 256;
  memset(g2h(testbuf), 0, TESTBUF_SIZE);
  batch_pre(item);
  snap_take(&snap_before);
  lockstep_state(dyn, fn, args);
  uint32_t nd, ni;
  int rd = run_steps(dyn, dyn_step, LOCKSTEP_BUDGET, &nd);

  if (rd < 0) { /* run again up to the faulting instruction, to compare there */
    snap_restore(&snap_before);
    lockstep_state(dyn, fn, args);
    run_steps(dyn, dyn_step, nd - 1, &nd);
  }
  cstate sd;
  get_state(dyn, &sd);
  snap_take(&snap_dyn);
  snap_restore(&snap_before);

  lockstep_state(itp, fn, args);
  cpu_interp_set_checked(itp, 1);
  int ri = run_steps(itp, cpu_interp_step, nd, &ni);
  cstate si;
  get_state(itp, &si);
  char what[200];
  int bad = 0;
  if (ri < 0 || ni != nd) {
    snprintf(what, sizeof what, "the interpreter stopped after %u of %u instructions (pc %08x)", ni, nd,
             si.r[15]);
    bad = 1;
  } else {
    bad = diff_state(&sd, &sd, &si, what, sizeof what) || snap_diff(&snap_dyn, what, sizeof what);
    /* dynarmic also raises exceptions on encodings it treats as
     * unpredictable; only a memory fault must be matched. */
    if (!bad && rd < 0 && cpu_interp_step(itp) >= 0) dyn_only++;
  }
  cpu_interp_set_checked(itp, 0);
  if (bad) fprintf(out, "interp-test: lockstep %08x (%s+0x%x): %s\n", fn,
                   so_module_containing(fn)->soname, fn - so_module_containing(fn)->base, what);
  snap_restore(&snap_before);
  *steps_total += nd;
  return bad;
}

typedef struct lockstep_stats {
  int bad, ran, dyn_only, aborted;
  uint64_t steps;
} lockstep_stats;
static uint32_t *lockstep_fns;
static int lockstep_n;

static void lockstep_item(int i, void *stats) {
  lockstep_stats *st = stats;
  uint32_t fn = lockstep_fns[rnd() % (uint32_t)lockstep_n];
  dyn_only = 0;
  st->bad += lockstep_one(fn, &st->steps, i);
  st->dyn_only += dyn_only;
  st->ran++;
}

static void lockstep_crash(int i, void *stats) {
  (void)i;
  lockstep_stats *st = stats;
  st->ran++;
  st->aborted++;
}

/* Function starts from .ARM.exidx; ARM or Thumb by their first instruction
 * (a push), others skipped. */
static int lockstep_sample(const so_module *mods, int n_mods, int count) {
  uint32_t *fns = NULL;
  int n = 0;
  for (int m = 0; m < n_mods; m++) {
    const so_module *mod = &mods[m];
    fns = realloc(fns, (size_t)(n + (int)mod->exidx_count) * sizeof *fns);
    for (uint32_t i = 0; i < mod->exidx_count; i++) {
      uint32_t e = mod->exidx + 8 * i, w = gm_r32(e);
      uint32_t a = e + (uint32_t)((int32_t)(w << 1) >> 1);
      if (!gm_region_tag(a)) continue;
      uint16_t h0 = *(uint16_t *)g2h(a), h1 = *(uint16_t *)g2h(a + 2);
      if (h1 == 0xE92D && (h0 >> 11) != 0x09) fns[n++] = a;          /* ARM push */
      else if ((h0 & 0xFE00) == 0xB400 || h0 == 0xE92D) fns[n++] = a | 1; /* Thumb push */
    }
  }
  lockstep_fns = fns;
  lockstep_n = n;
  lockstep_stats st = {0, 0, 0, 0, 0};
  silence_begin();
  int r = n ? run_batched(count, lockstep_item, &st, sizeof st, lockstep_crash) : 0;
  silence_end();
  free(fns);
  fprintf(out, "interp-test: lockstep: %d functions, %llu instructions, %d differ, "
          "%d end in an exception only dynarmic raises, %d skipped (dynarmic aborted)\n", st.ran,
          (unsigned long long)st.steps, st.bad, st.dyn_only, st.aborted);
  return st.bad + (r < 0);
}

/* ---- entry ---- */

int interp_selftest(const so_module *mods, int n_mods, const dl_fn_test *tests) {
  out = fdopen(dup(2), "w");
  setvbuf(out, NULL, _IOLBF, 0);
  const char *seed = getenv("DL_INTERP_SEED");
  if (seed) rng = strtoull(seed, NULL, 0) | 1;
  const char *n = getenv("DL_INTERP_TESTS");
  int per_class = n ? atoi(n) : 4000;

  dyn = cpu_dynarmic_create_checked();
  itp = cpu_interp_create();
  if (!dyn || !itp || cpu_setup(dyn, 1u << 20) != 0 || cpu_setup(itp, 1u << 16) != 0) return -1;
  stack_top = dyn->ops->get_reg(dyn, CPU_REG_SP);
  sentinel_addr = dyn->ret_sentinel;

  fn_tests = tests;
  testbuf = gm_alloc(TESTBUF_SIZE, 0, "interp-test buffer");
  if (!testbuf) return -1;
  guest_errno_addr();

  /* DL_INTERP_CLASSES=substring: only those random classes (debugging). */
  only = getenv("DL_INTERP_CLASSES");
  if (!only && function_tests() != 0) failures++;
  if (!only && fallback_api_test() != 0) failures++;
  if (!only && rc_fallback_test() != 0) failures++;
  const char *ls = getenv("DL_INTERP_LOCKSTEP");
  if (!only && lockstep_sample(mods, n_mods, ls ? atoi(ls) : 300) != 0) failures++;
  if (random_tests(per_class) != 0) failures++;

  fprintf(out, "interp-test: %s\n", failures ? "FAILED" : "all matched");
  fclose(out);
  return failures ? -1 : 0;
}
