/*
 * ARM (A32) instruction set, decoded as in the ARMv7-A Architecture
 * Reference Manual, chapter A5. VFP, NEON and the coprocessor space are
 * passed on to interp_vfp.c / interp_neon.c.
 */
#include "interp.h"

#define BIT(x, n) (((x) >> (n)) & 1u)
#define BITS(x, hi, lo) (((x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))

/* ---- shared bodies ---- */

void interp_svc(icpu *s, uint32_t imm) {
  s->r[15] = s->next_pc;
  interp_fp_leave(s);
  if (s->svc_hook) s->svc_hook(s, imm);
  else cpu_svc(&s->base, imm);
  interp_fp_enter(s);
  s->next_pc = s->r[15];
}

void interp_barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

uint32_t interp_mrs(icpu *s) { return interp_get_cpsr(s) & ~0x06F0FC20u; }

void interp_msr_apsr(icpu *s, uint32_t v, int mask) {
  if (mask & 8) {
    s->n = v >> 31;
    s->z = (v >> 30) & 1;
    s->c = (v >> 29) & 1;
    s->v = (v >> 28) & 1;
    s->q = (v >> 27) & 1;
  }
  if (mask & 4) s->ge = (v >> 16) & 0xF;
  if (mask & 2) s->cpsr_rest = (s->cpsr_rest & ~0x200u) | (v & 0x200u); /* E, user-writable */
}

void interp_ldm(icpu *s, int n, uint32_t list, int inc, int before, int wback) {
  uint32_t cnt = (uint32_t)__builtin_popcount(list);
  uint32_t base = s->r[n];
  uint32_t a = inc ? base + (before ? 4 : 0) : base - 4 * cnt + (before ? 0 : 4);
  uint32_t newbase = inc ? base + 4 * cnt : base - 4 * cnt;
  uint32_t pcval = 0;
  int loads_pc = 0;
  for (int i = 0; i < 16; i++) {
    if (!(list & (1u << i))) continue;
    uint32_t v = rd32(s, a);
    a += 4;
    if (i == 15) {
      pcval = v;
      loads_pc = 1;
    } else {
      s->r[i] = v;
    }
  }
  if (wback && !(list & (1u << n))) s->r[n] = newbase;
  if (loads_pc) bx_to(s, pcval);
}

void interp_stm(icpu *s, int n, uint32_t list, int inc, int before, int wback) {
  uint32_t cnt = (uint32_t)__builtin_popcount(list);
  uint32_t base = s->r[n];
  uint32_t a = inc ? base + (before ? 4 : 0) : base - 4 * cnt + (before ? 0 : 4);
  for (int i = 0; i < 16; i++) {
    if (!(list & (1u << i))) continue;
    /* r15 reads as the PC store value; the base as its original value. */
    wr32(s, a, s->r[i]);
    a += 4;
  }
  if (wback) s->r[n] = inc ? base + 4 * cnt : base - 4 * cnt;
}

/* ---- data processing ---- */

static void dp_reg(icpu *s, uint32_t insn) {
  int op = (int)BITS(insn, 24, 21), S = (int)BIT(insn, 20);
  int n = (int)BITS(insn, 19, 16), d = (int)BITS(insn, 15, 12), m = (int)BITS(insn, 3, 0);
  int type;
  uint32_t amt, sc;
  if (S && d == 15 && (op < DP_TST || op > DP_CMN)) {
    interp_undefined(s, insn, "flag-setting write to the PC");
    return;
  }
  if (BIT(insn, 4)) { /* register-shifted register */
    int rs = (int)BITS(insn, 11, 8);
    type = (int)BITS(insn, 6, 5);
    amt = s->r[rs] & 0xFF;
    if (d == 15 || n == 15 || m == 15 || rs == 15) {
      interp_undefined(s, insn, "PC in a register-shifted register operation");
      return;
    }
  } else {
    decode_imm_shift((int)BITS(insn, 6, 5), BITS(insn, 11, 7), &type, &amt);
  }
  uint32_t b = shift_c(s->r[m], type, amt, s->c, &sc);
  interp_dp(s, op, S, d, s->r[n], b, sc);
}

static void dp_imm(icpu *s, uint32_t insn) {
  int op = (int)BITS(insn, 24, 21), S = (int)BIT(insn, 20);
  int n = (int)BITS(insn, 19, 16), d = (int)BITS(insn, 15, 12);
  uint32_t rot = BITS(insn, 11, 8) * 2, imm = BITS(insn, 7, 0);
  uint32_t v = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
  uint32_t sc = rot ? v >> 31 : s->c;
  if (S && d == 15 && (op < DP_TST || op > DP_CMN)) {
    interp_undefined(s, insn, "flag-setting write to the PC");
    return;
  }
  uint32_t a = s->r[n];
  if (n == 15) a &= ~3u; /* ADR */
  interp_dp(s, op, S, d, a, v, sc);
}

/* ---- multiplies ---- */

static void multiply(icpu *s, uint32_t insn) {
  int op = (int)BITS(insn, 23, 20);
  int dh = (int)BITS(insn, 19, 16), a = (int)BITS(insn, 15, 12);
  int m = (int)BITS(insn, 11, 8), n = (int)BITS(insn, 3, 0);
  int S = op & 1;
  uint32_t rn = s->r[n], rm = s->r[m];
  switch (op >> 1) {
  case 0: { /* MUL */
    uint32_t r = rn * rm;
    s->r[dh] = r;
    if (S) set_nz(s, r);
    return;
  }
  case 1: { /* MLA */
    uint32_t r = rn * rm + s->r[a];
    s->r[dh] = r;
    if (S) set_nz(s, r);
    return;
  }
  case 2:
    if (!S) { /* UMAAL */
      uint64_t r = (uint64_t)rn * rm + s->r[a] + s->r[dh];
      s->r[a] = (uint32_t)r;
      s->r[dh] = (uint32_t)(r >> 32);
      return;
    }
    break;
  case 3:
    if (!S) { /* MLS */
      s->r[dh] = s->r[a] - rn * rm;
      return;
    }
    break;
  case 4: case 5: case 6: case 7: {
    int sgn = op & 4, acc = op & 2;
    uint64_t r = sgn ? (uint64_t)((int64_t)(int32_t)rn * (int32_t)rm) : (uint64_t)rn * rm;
    if (acc) r += (uint64_t)s->r[dh] << 32 | s->r[a];
    s->r[a] = (uint32_t)r;
    s->r[dh] = (uint32_t)(r >> 32);
    if (S) {
      s->n = (uint32_t)(r >> 63);
      s->z = r == 0;
    }
    return;
  }
  }
  interp_undefined(s, insn, "multiply");
}

static void halfword_multiply(icpu *s, uint32_t insn) {
  int op1 = (int)BITS(insn, 22, 21);
  int d = (int)BITS(insn, 19, 16), a = (int)BITS(insn, 15, 12);
  int m = (int)BITS(insn, 11, 8), n = (int)BITS(insn, 3, 0);
  int x = (int)BIT(insn, 5), y = (int)BIT(insn, 6);
  switch (op1) {
  case 0: s->r[d] = interp_smla(s, x, y, s->r[n], s->r[m], s->r[a], 1); break;
  case 1:
    if (x) s->r[d] = interp_smlaw(s, y, s->r[n], s->r[m], 0, 0); /* SMULW */
    else s->r[d] = interp_smlaw(s, y, s->r[n], s->r[m], s->r[a], 1);
    break;
  case 2: { /* SMLAL<x><y> */
    int32_t nn = x ? (int32_t)s->r[n] >> 16 : (int16_t)s->r[n];
    int32_t mm = y ? (int32_t)s->r[m] >> 16 : (int16_t)s->r[m];
    uint64_t acc = (uint64_t)s->r[d] << 32 | s->r[a];
    acc += (uint64_t)((int64_t)nn * mm);
    s->r[a] = (uint32_t)acc;
    s->r[d] = (uint32_t)(acc >> 32);
    break;
  }
  default: s->r[d] = interp_smla(s, x, y, s->r[n], s->r[m], 0, 0); break;
  }
}

/* ---- miscellaneous (A5.2.12) ---- */

static void misc(icpu *s, uint32_t insn) {
  int op = (int)BITS(insn, 22, 21), op2 = (int)BITS(insn, 6, 4);
  int d = (int)BITS(insn, 15, 12), m = (int)BITS(insn, 3, 0), n = (int)BITS(insn, 19, 16);
  switch (op2) {
  case 0:
    if (BIT(insn, 9)) break; /* banked registers */
    if (!(op & 1)) {         /* MRS */
      if (op & 2) break;     /* SPSR */
      s->r[d] = interp_mrs(s);
      return;
    }
    if (op == 1) { /* MSR CPSR, register: user mode writes only the APSR bits */
      interp_msr_apsr(s, s->r[m], (int)BITS(insn, 19, 16));
      return;
    }
    break;
  case 1:
    if (op == 1) { /* BX */
      bx_to(s, s->r[m]);
      return;
    }
    if (op == 3) { /* CLZ */
      s->r[d] = interp_clz(s->r[m]);
      return;
    }
    break;
  case 3:
    if (op == 1) { /* BLX register */
      uint32_t target = s->r[m];
      s->r[14] = s->next_pc;
      bx_to(s, target);
      return;
    }
    break;
  case 5: /* QADD, QSUB, QDADD, QDSUB */
    s->r[d] = interp_qadd(s, s->r[m], s->r[n], op & 1, op >> 1);
    return;
  case 7:
    if (op == 1) {
      interp_fault(s, INTERP_FAULT_OTHER, "bkpt #0x%x", BITS(insn, 19, 8) << 4 | BITS(insn, 3, 0));
      return;
    }
    break;
  }
  interp_undefined(s, insn, "miscellaneous");
}

/* ---- loads and stores ---- */

static void sync_prim(icpu *s, uint32_t insn) {
  int op = (int)BITS(insn, 23, 20);
  int n = (int)BITS(insn, 19, 16), t = (int)BITS(insn, 15, 12), t2 = (int)BITS(insn, 3, 0);
  uint32_t a = s->r[n];
  if ((op & 0xB) == 0) { /* SWP, SWPB */
    if (op & 4) {
      if (unlikely(s->checked) && !interp_mapped(s, a, 1, 1)) return;
      /* SWPB */
      s->r[t] = __atomic_exchange_n(g_mem + a, (uint8_t)s->r[t2], __ATOMIC_SEQ_CST);
    } else {
      if (!interp_aligned(s, a, 4)) return;
      if (unlikely(s->checked) && !interp_mapped(s, a, 4, 1)) return;
      uint32_t v = s->r[t2], old;
      uint32_t *p = (uint32_t *)(void *)(g_mem + a);
      old = __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
      s->r[t] = old;
    }
    return;
  }
  switch (op) {
  case 8: s->r[t] = interp_strex(s, a, 4, s->r[t2]); return;
  case 9: s->r[t] = (uint32_t)interp_ldrex(s, a, 4); return;
  case 10: s->r[t] = interp_strex(s, a, 8, (uint64_t)s->r[t2 + 1] << 32 | s->r[t2]); return;
  case 11: {
    uint64_t v = interp_ldrex(s, a, 8);
    s->r[t] = (uint32_t)v;
    s->r[t + 1] = (uint32_t)(v >> 32);
    return;
  }
  case 12: s->r[t] = interp_strex(s, a, 1, s->r[t2] & 0xFF); return;
  case 13: s->r[t] = (uint32_t)interp_ldrex(s, a, 1); return;
  case 14: s->r[t] = interp_strex(s, a, 2, s->r[t2] & 0xFFFF); return;
  case 15: s->r[t] = (uint32_t)interp_ldrex(s, a, 2); return;
  }
  interp_undefined(s, insn, "synchronization");
}

/* LDRH/STRH/LDRD/STRD/LDRSB/LDRSH and their unprivileged forms. */
static void extra_ls(icpu *s, uint32_t insn) {
  int P = (int)BIT(insn, 24), U = (int)BIT(insn, 23), I = (int)BIT(insn, 22);
  int W = (int)BIT(insn, 21), L = (int)BIT(insn, 20);
  int n = (int)BITS(insn, 19, 16), t = (int)BITS(insn, 15, 12);
  int op2 = (int)BITS(insn, 6, 5);
  uint32_t off = I ? (BITS(insn, 11, 8) << 4 | BITS(insn, 3, 0)) : s->r[BITS(insn, 3, 0)];
  uint32_t base = s->r[n];
  if (n == 15) base &= ~3u;
  uint32_t oa = U ? base + off : base - off;
  uint32_t a = P ? oa : base;
  int wback = !P || W;
  if (op2 == 1) {
    if (L) {
      uint32_t v = rd16(s, a);
      if (wback) s->r[n] = oa;
      s->r[t] = v;
    } else {
      wr16(s, a, (uint16_t)s->r[t]);
      if (wback) s->r[n] = oa;
    }
  } else if (op2 == 2 && L) { /* LDRSB */
    uint32_t v = (uint32_t)(int8_t)rd8(s, a);
    if (wback) s->r[n] = oa;
    s->r[t] = v;
  } else if (op2 == 3 && L) { /* LDRSH */
    uint32_t v = (uint32_t)(int16_t)rd16(s, a);
    if (wback) s->r[n] = oa;
    s->r[t] = v;
  } else { /* LDRD (op2 2) / STRD (op2 3) */
    if ((t & 1) || t == 14 || (!P && W)) {
      interp_undefined(s, insn, "LDRD/STRD register pair");
      return;
    }
    if (op2 == 2) {
      uint32_t lo = rd32(s, a), hi = rd32(s, a + 4);
      if (wback) s->r[n] = oa;
      s->r[t] = lo;
      s->r[t + 1] = hi;
    } else {
      wr32(s, a, s->r[t]);
      wr32(s, a + 4, s->r[t + 1]);
      if (wback) s->r[n] = oa;
    }
  }
}

static void ls_word_byte(icpu *s, uint32_t insn) {
  int I = (int)BIT(insn, 25), P = (int)BIT(insn, 24), U = (int)BIT(insn, 23);
  int B = (int)BIT(insn, 22), W = (int)BIT(insn, 21), L = (int)BIT(insn, 20);
  int n = (int)BITS(insn, 19, 16), t = (int)BITS(insn, 15, 12);
  uint32_t off;
  if (I) {
    int type;
    uint32_t amt, sc;
    decode_imm_shift((int)BITS(insn, 6, 5), BITS(insn, 11, 7), &type, &amt);
    off = shift_c(s->r[BITS(insn, 3, 0)], type, amt, s->c, &sc);
  } else {
    off = BITS(insn, 11, 0);
  }
  uint32_t base = s->r[n];
  if (n == 15) base &= ~3u;
  uint32_t oa = U ? base + off : base - off;
  uint32_t a = P ? oa : base;
  int wback = !P || W;
  if (L) {
    uint32_t v = B ? rd8(s, a) : rd32(s, a);
    if (wback) s->r[n] = oa;
    if (B) s->r[t] = v;
    else set_reg_load(s, t, v);
  } else {
    if (B) wr8(s, a, (uint8_t)s->r[t]);
    else wr32(s, a, s->r[t]);
    if (wback) s->r[n] = oa;
  }
}

/* ---- media (A5.4) ---- */

static void media(icpu *s, uint32_t insn) {
  uint32_t op1 = BITS(insn, 24, 20), op2 = BITS(insn, 7, 5);
  int d = (int)BITS(insn, 15, 12), n = (int)BITS(insn, 3, 0), a = (int)BITS(insn, 19, 16);
  int m = n;
  if (op1 < 8) { /* parallel add/subtract */
    int k = (int)BITS(insn, 21, 20);
    int opc = (int)op2;
    if (k == 0 || opc == 5 || opc == 6) goto undef;
    int kind = (int)(BIT(insn, 22) << 2) | (k - 1);
    s->r[d] = interp_parallel(s, kind, opc, s->r[a], s->r[m]);
    return;
  }
  if (op1 < 16) { /* packing, unpacking, saturation, reversal */
    uint32_t o = op1 & 7;
    uint32_t rot = BITS(insn, 11, 10) * 8;
    uint32_t rm = s->r[m];
    uint32_t rotated = rot ? (rm >> rot) | (rm << (32 - rot)) : rm;
    if (o == 0 && !(op2 & 1)) { /* PKHBT/PKHTB */
      int type;
      uint32_t amt, sc;
      decode_imm_shift(BIT(insn, 6) ? 2 : 0, BITS(insn, 11, 7), &type, &amt);
      uint32_t op = shift_c(rm, type, amt, s->c, &sc);
      if (BIT(insn, 6)) s->r[d] = (s->r[a] & 0xFFFF0000u) | (op & 0xFFFF);
      else s->r[d] = (op & 0xFFFF0000u) | (s->r[a] & 0xFFFF);
      return;
    }
    if ((o & 6) == 2 && !(op2 & 1)) { /* SSAT */
      int type;
      uint32_t amt, sc;
      decode_imm_shift(BIT(insn, 6) ? 2 : 0, BITS(insn, 11, 7), &type, &amt);
      s->r[d] = interp_ssat(s, (int32_t)shift_c(rm, type, amt, s->c, &sc), (int)BITS(insn, 20, 16) + 1);
      return;
    }
    if ((o & 6) == 6 && !(op2 & 1)) { /* USAT */
      int type;
      uint32_t amt, sc;
      decode_imm_shift(BIT(insn, 6) ? 2 : 0, BITS(insn, 11, 7), &type, &amt);
      s->r[d] = interp_usat(s, (int32_t)shift_c(rm, type, amt, s->c, &sc), (int)BITS(insn, 20, 16));
      return;
    }
    if (op2 == 3) { /* extends */
      if (o == 1 || o == 5) goto undef;
      s->r[d] = interp_extend((int)o, a == 15 ? 0 : s->r[a], rotated);
      return;
    }
    if (o == 0 && op2 == 5) { /* SEL */
      s->r[d] = interp_sel(s, s->r[a], rm);
      return;
    }
    if (o == 2 && op2 == 1) { /* SSAT16 */
      s->r[d] = interp_ssat16(s, rm, (int)BITS(insn, 19, 16) + 1);
      return;
    }
    if (o == 6 && op2 == 1) { /* USAT16 */
      s->r[d] = interp_usat16(s, rm, (int)BITS(insn, 19, 16));
      return;
    }
    if (o == 3 && op2 == 1) { /* REV */
      s->r[d] = __builtin_bswap32(rm);
      return;
    }
    if (o == 3 && op2 == 5) { /* REV16 */
      s->r[d] = ((rm >> 8) & 0x00FF00FFu) | ((rm << 8) & 0xFF00FF00u);
      return;
    }
    if (o == 7 && op2 == 1) { /* RBIT */
      s->r[d] = interp_rbit(rm);
      return;
    }
    if (o == 7 && op2 == 5) { /* REVSH */
      s->r[d] = (uint32_t)(int32_t)(int16_t)(((rm >> 8) & 0xFF) | ((rm & 0xFF) << 8));
      return;
    }
    goto undef;
  }
  if (op1 < 24) { /* signed multiplies, divides */
    uint32_t o = op1 & 7;
    int rd = (int)BITS(insn, 19, 16), ra = (int)BITS(insn, 15, 12), rm2 = (int)BITS(insn, 11, 8);
    uint32_t vn = s->r[n], vm = s->r[rm2];
    int M = (int)BIT(insn, 5);
    switch (o) {
    case 0:
      if (op2 >= 4) goto undef;
      s->r[rd] = interp_smlad(s, M, (int)BIT(insn, 6), vn, vm, s->r[ra], ra != 15);
      return;
    case 1:
      if (op2 != 0 || ra != 15) goto undef;
      s->r[rd] = interp_sdiv(vn, vm);
      return;
    case 3:
      if (op2 != 0 || ra != 15) goto undef;
      s->r[rd] = interp_udiv(vn, vm);
      return;
    case 4: {
      if (op2 >= 4) goto undef;
      uint64_t acc = (uint64_t)s->r[rd] << 32 | s->r[ra];
      acc = interp_smlald(M, (int)BIT(insn, 6), vn, vm, acc);
      s->r[ra] = (uint32_t)acc;
      s->r[rd] = (uint32_t)(acc >> 32);
      return;
    }
    case 5:
      if (op2 <= 1) {
        s->r[rd] = interp_smmla(M, 0, vn, vm, s->r[ra], ra != 15);
        return;
      }
      if (op2 >= 6) {
        s->r[rd] = interp_smmla(M, 1, vn, vm, s->r[ra], 1);
        return;
      }
      goto undef;
    }
    goto undef;
  }
  if (op1 == 24 && op2 == 0) { /* USAD8 / USADA8 */
    int rd = (int)BITS(insn, 19, 16), ra = (int)BITS(insn, 15, 12), rm2 = (int)BITS(insn, 11, 8);
    uint32_t r = interp_usad8(s->r[n], s->r[rm2]);
    s->r[rd] = ra == 15 ? r : r + s->r[ra];
    return;
  }
  if ((op1 & 0x1E) == 0x1A && (op2 & 3) == 2) { /* SBFX */
    uint32_t lsb = BITS(insn, 11, 7), w = BITS(insn, 20, 16) + 1;
    if (lsb + w > 32) goto undef;
    s->r[d] = (uint32_t)((int32_t)(s->r[n] << (32 - lsb - w)) >> (32 - w));
    return;
  }
  if ((op1 & 0x1E) == 0x1C && (op2 & 3) == 0) { /* BFC / BFI */
    uint32_t lsb = BITS(insn, 11, 7), msb = BITS(insn, 20, 16);
    if (msb < lsb) goto undef;
    uint32_t mask = (uint32_t)(((uint64_t)1 << (msb - lsb + 1)) - 1) << lsb;
    uint32_t src = n == 15 ? 0 : s->r[n] << lsb;
    s->r[d] = (s->r[d] & ~mask) | (src & mask);
    return;
  }
  if ((op1 & 0x1E) == 0x1E && (op2 & 3) == 2) { /* UBFX */
    uint32_t lsb = BITS(insn, 11, 7), w = BITS(insn, 20, 16) + 1;
    if (lsb + w > 32) goto undef;
    s->r[d] = (uint32_t)(((uint64_t)s->r[n] >> lsb) & (((uint64_t)1 << w) - 1));
    return;
  }
undef:
  interp_undefined(s, insn, "media");
}

/* ---- unconditional space (cond 1111, A5.7) ---- */

static void unconditional(icpu *s, uint32_t insn) {
  uint32_t op1 = BITS(insn, 27, 20);
  if ((op1 & 0xE0) == 0x20) { /* 001x xxxx: Advanced SIMD data processing */
    interp_neon_dp(s, insn);
    return;
  }
  if ((op1 & 0xF1) == 0x40) { /* 0100 xxx0: Advanced SIMD element load/store */
    interp_neon_ls(s, insn);
    return;
  }
  if ((op1 & 0xE0) == 0xA0) { /* BLX immediate */
    uint32_t imm = (uint32_t)((int32_t)(insn << 8) >> 6) | BIT(insn, 24) << 1;
    s->r[14] = s->next_pc;
    s->t = 1;
    s->next_pc = s->r[15] + imm;
    return;
  }
  if (op1 == 0x57) {
    switch (BITS(insn, 7, 4)) {
    case 1: interp_clrex(s); return;
    case 4: case 5: case 6: interp_barrier(); return;
    }
  }
  /* PLD, PLDW, PLI and the unallocated memory hints: no-ops. */
  uint32_t h = op1 & 0xF7;
  if (h == 0x41 || h == 0x45 || h == 0x51 || h == 0x55) return;
  if ((h == 0x61 || h == 0x65 || h == 0x71 || h == 0x75) && !BIT(insn, 4)) return;
  interp_undefined(s, insn, "unconditional");
}

/* ---- top level ---- */

void interp_arm_exec(icpu *s, uint32_t insn) {
  uint32_t cond = insn >> 28;
  if (cond == 0xF) {
    unconditional(s, insn);
    return;
  }
  if (cond != 0xE && !cond_pass(s, cond)) return;

  switch (BITS(insn, 27, 25)) {
  case 0: {
    uint32_t op1 = BITS(insn, 24, 20), op2 = BITS(insn, 7, 4);
    if ((op2 & 9) == 9 && op2 != 9) { /* 1011, 1101, 1111: extra loads/stores */
      extra_ls(s, insn);
      return;
    }
    if (op2 == 9) {
      if (op1 < 16) multiply(s, insn);
      else sync_prim(s, insn);
      return;
    }
    if ((op1 & 0x19) == 0x10) { /* 10xx0: misc, halfword multiply */
      if (op2 & 8) halfword_multiply(s, insn);
      else misc(s, insn);
      return;
    }
    dp_reg(s, insn);
    return;
  }
  case 1: {
    uint32_t op1 = BITS(insn, 24, 20);
    if (op1 == 0x10) { /* MOVW */
      int d = (int)BITS(insn, 15, 12);
      s->r[d] = BITS(insn, 19, 16) << 12 | BITS(insn, 11, 0);
      return;
    }
    if (op1 == 0x14) { /* MOVT */
      int d = (int)BITS(insn, 15, 12);
      s->r[d] = (s->r[d] & 0xFFFF) | (BITS(insn, 19, 16) << 12 | BITS(insn, 11, 0)) << 16;
      return;
    }
    if ((op1 & 0x1B) == 0x12) { /* MSR immediate, hints */
      int mask = (int)BITS(insn, 19, 16);
      if (BIT(insn, 22)) {
        interp_undefined(s, insn, "MSR SPSR");
        return;
      }
      if (mask == 0) return; /* NOP, YIELD, WFE, WFI, SEV, DBG */
      uint32_t rot = BITS(insn, 11, 8) * 2, imm = BITS(insn, 7, 0);
      interp_msr_apsr(s, rot ? (imm >> rot) | (imm << (32 - rot)) : imm, mask);
      return;
    }
    dp_imm(s, insn);
    return;
  }
  case 2:
    ls_word_byte(s, insn);
    return;
  case 3:
    if (BIT(insn, 4)) media(s, insn);
    else ls_word_byte(s, insn);
    return;
  case 4: {
    if (BIT(insn, 22)) {
      interp_undefined(s, insn, "LDM/STM with user registers");
      return;
    }
    int n = (int)BITS(insn, 19, 16);
    uint32_t list = BITS(insn, 15, 0);
    int P = (int)BIT(insn, 24), U = (int)BIT(insn, 23), W = (int)BIT(insn, 21);
    if (BIT(insn, 20)) interp_ldm(s, n, list, U, P, W);
    else interp_stm(s, n, list, U, P, W);
    return;
  }
  case 5: {
    uint32_t imm = (uint32_t)((int32_t)(insn << 8) >> 6);
    if (BIT(insn, 24)) s->r[14] = s->next_pc;
    s->next_pc = s->r[15] + imm;
    return;
  }
  default:
    if (BITS(insn, 27, 24) == 0xF) {
      interp_svc(s, BITS(insn, 23, 0));
      return;
    }
    interp_coproc(s, insn);
    return;
  }
}
