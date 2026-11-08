/*
 * Thumb and Thumb-2 (T16/T32), decoded as in the ARMv7-A Architecture
 * Reference Manual, chapter A6, with IT blocks. Coprocessor, VFP and NEON
 * encodings are converted to their ARM form and shared with the ARM side.
 */
#include "interp.h"

#define BIT(x, n) (((x) >> (n)) & 1u)
#define BITS(x, hi, lo) (((x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))

/* ThumbExpandImm_C. */
static uint32_t thumb_imm(uint32_t imm12, uint32_t cin, uint32_t *cout) {
  uint32_t imm8 = imm12 & 0xFF;
  if ((imm12 >> 10) == 0) {
    *cout = cin;
    switch ((imm12 >> 8) & 3) {
    case 0: return imm8;
    case 1: return imm8 << 16 | imm8;
    case 2: return imm8 << 24 | imm8 << 8;
    default: return imm8 * 0x01010101u;
    }
  }
  uint32_t v = 0x80 | (imm12 & 0x7F), rot = imm12 >> 7;
  uint32_t r = (v >> rot) | (v << (32 - rot));
  *cout = r >> 31;
  return r;
}

/* Thumb-2 data-processing op (hw1 bits 8:5) to the shared DP opcodes. */
static int t2_dp_op(uint32_t op, int rn15, int rd15, int S, int *test) {
  *test = 0;
  switch (op) {
  case 0: if (rd15 && S) { *test = 1; return DP_TST; } return DP_AND;
  case 1: return DP_BIC;
  case 2: return rn15 ? DP_MOV : DP_ORR;
  case 3: return rn15 ? DP_MVN : DP_ORN;
  case 4: if (rd15 && S) { *test = 1; return DP_TEQ; } return DP_EOR;
  case 8: if (rd15 && S) { *test = 1; return DP_CMN; } return DP_ADD;
  case 10: return DP_ADC;
  case 11: return DP_SBC;
  case 13: if (rd15 && S) { *test = 1; return DP_CMP; } return DP_SUB;
  case 14: return DP_RSB;
  default: return -1;
  }
}

static void t32_dp(icpu *s, uint32_t insn, uint32_t op, uint32_t b, uint32_t sc) {
  int S = (int)BIT(insn, 20), n = (int)BITS(insn, 19, 16), d = (int)BITS(insn, 11, 8);
  int test;
  int dp = t2_dp_op(op, n == 15, d == 15, S, &test);
  if (dp < 0 || (d == 15 && !test)) {
    interp_undefined(s, insn, "data processing");
    return;
  }
  interp_dp(s, dp, S, d, s->r[n], b, sc);
}

/* ---- 32-bit groups ---- */

static void t32_ldst_multiple(icpu *s, uint32_t insn) {
  int op = (int)BITS(insn, 24, 23), L = (int)BIT(insn, 20), W = (int)BIT(insn, 21);
  int n = (int)BITS(insn, 19, 16);
  uint32_t list = insn & 0xFFFF;
  if (op == 1 || op == 2) {
    if (L) interp_ldm(s, n, list, op == 1, op == 2, W);
    else interp_stm(s, n, list, op == 1, op == 2, W);
    return;
  }
  interp_undefined(s, insn, "SRS/RFE");
}

static void t32_dual_excl_tb(icpu *s, uint32_t insn) {
  int P = (int)BIT(insn, 24), U = (int)BIT(insn, 23), W = (int)BIT(insn, 21), L = (int)BIT(insn, 20);
  int n = (int)BITS(insn, 19, 16), t = (int)BITS(insn, 15, 12), t2 = (int)BITS(insn, 11, 8);
  if (P || W) { /* LDRD/STRD immediate, literal */
    uint32_t imm = (insn & 0xFF) << 2;
    uint32_t base = n == 15 ? s->r[15] & ~3u : s->r[n];
    uint32_t oa = U ? base + imm : base - imm;
    uint32_t a = P ? oa : base;
    if (L) {
      uint32_t lo = rd32(s, a), hi = rd32(s, a + 4);
      if (W) s->r[n] = oa;
      s->r[t] = lo;
      s->r[t2] = hi;
    } else {
      wr32(s, a, s->r[t]);
      wr32(s, a + 4, s->r[t2]);
      if (W) s->r[n] = oa;
    }
    return;
  }
  uint32_t a = s->r[n];
  if (!U) { /* STREX / LDREX */
    a += (insn & 0xFF) << 2;
    if (L) s->r[t] = (uint32_t)interp_ldrex(s, a, 4);
    else s->r[t2] = interp_strex(s, a, 4, s->r[t]);
    return;
  }
  uint32_t op3 = BITS(insn, 7, 4);
  int rd = (int)BITS(insn, 3, 0);
  if (!L) {
    switch (op3) {
    case 4: s->r[rd] = interp_strex(s, a, 1, s->r[t] & 0xFF); return;
    case 5: s->r[rd] = interp_strex(s, a, 2, s->r[t] & 0xFFFF); return;
    case 7: s->r[rd] = interp_strex(s, a, 8, (uint64_t)s->r[t2] << 32 | s->r[t]); return;
    }
  } else {
    switch (op3) {
    case 0: /* TBB */
    case 1: { /* TBH */
      uint32_t m = s->r[insn & 0xF];
      uint32_t h = op3 ? rd16(s, a + 2 * m) : rd8(s, a + m);
      s->next_pc = s->r[15] + 2 * h;
      return;
    }
    case 4: s->r[t] = (uint32_t)interp_ldrex(s, a, 1); return;
    case 5: s->r[t] = (uint32_t)interp_ldrex(s, a, 2); return;
    case 7: {
      uint64_t v = interp_ldrex(s, a, 8);
      s->r[t] = (uint32_t)v;
      s->r[t2] = (uint32_t)(v >> 32);
      return;
    }
    }
  }
  interp_undefined(s, insn, "load/store exclusive");
}

static void t32_dp_shifted(icpu *s, uint32_t insn) {
  uint32_t op = BITS(insn, 24, 21);
  int type;
  uint32_t amt, sc;
  uint32_t imm5 = BITS(insn, 14, 12) << 2 | BITS(insn, 7, 6);
  int m = (int)BITS(insn, 3, 0);
  if (op == 6) { /* PKHBT/PKHTB */
    int tb = (int)BIT(insn, 5), d = (int)BITS(insn, 11, 8), n = (int)BITS(insn, 19, 16);
    decode_imm_shift(tb ? 2 : 0, imm5, &type, &amt);
    uint32_t v = shift_c(s->r[m], type, amt, s->c, &sc);
    if (tb) s->r[d] = (s->r[n] & 0xFFFF0000u) | (v & 0xFFFF);
    else s->r[d] = (v & 0xFFFF0000u) | (s->r[n] & 0xFFFF);
    return;
  }
  decode_imm_shift((int)BITS(insn, 5, 4), imm5, &type, &amt);
  uint32_t b = shift_c(s->r[m], type, amt, s->c, &sc);
  t32_dp(s, insn, op, b, sc);
}

static void t32_dp_modimm(icpu *s, uint32_t insn) {
  uint32_t imm12 = BIT(insn, 26) << 11 | BITS(insn, 14, 12) << 8 | (insn & 0xFF), sc;
  uint32_t v = thumb_imm(imm12, s->c, &sc);
  t32_dp(s, insn, BITS(insn, 24, 21), v, sc);
}

static void t32_dp_plainimm(icpu *s, uint32_t insn) {
  uint32_t op = BITS(insn, 24, 20);
  int n = (int)BITS(insn, 19, 16), d = (int)BITS(insn, 11, 8);
  uint32_t imm12 = BIT(insn, 26) << 11 | BITS(insn, 14, 12) << 8 | (insn & 0xFF);
  uint32_t imm5 = BITS(insn, 14, 12) << 2 | BITS(insn, 7, 6);
  uint32_t low5 = insn & 0x1F;
  switch (op) {
  case 0: /* ADDW, ADR */
    s->r[d] = (n == 15 ? s->r[15] & ~3u : s->r[n]) + imm12;
    return;
  case 10: /* SUBW, ADR */
    s->r[d] = (n == 15 ? s->r[15] & ~3u : s->r[n]) - imm12;
    return;
  case 4: /* MOVW */
    s->r[d] = (uint32_t)n << 12 | imm12;
    return;
  case 12: /* MOVT */
    s->r[d] = (s->r[d] & 0xFFFF) | ((uint32_t)n << 12 | imm12) << 16;
    return;
  case 16: case 18: case 24: case 26: { /* SSAT, SSAT16, USAT, USAT16 */
    int sh = (int)BIT(insn, 21), uns = op >= 24;
    if (sh && imm5 == 0) {
      s->r[d] = uns ? interp_usat16(s, s->r[n], (int)(low5 & 0xF))
                    : interp_ssat16(s, s->r[n], (int)(low5 & 0xF) + 1);
      return;
    }
    int type;
    uint32_t amt, sc;
    decode_imm_shift(sh ? 2 : 0, imm5, &type, &amt);
    int32_t v = (int32_t)shift_c(s->r[n], type, amt, s->c, &sc);
    s->r[d] = uns ? interp_usat(s, v, (int)low5) : interp_ssat(s, v, (int)low5 + 1);
    return;
  }
  case 20: case 28: { /* SBFX, UBFX */
    uint32_t lsb = imm5, w = low5 + 1;
    if (lsb + w > 32) break;
    if (op == 20) s->r[d] = (uint32_t)((int32_t)(s->r[n] << (32 - lsb - w)) >> (32 - w));
    else s->r[d] = (uint32_t)(((uint64_t)s->r[n] >> lsb) & (((uint64_t)1 << w) - 1));
    return;
  }
  case 22: { /* BFI, BFC */
    uint32_t lsb = imm5, msb = low5;
    if (msb < lsb) break;
    uint32_t mask = (uint32_t)(((uint64_t)1 << (msb - lsb + 1)) - 1) << lsb;
    uint32_t src = n == 15 ? 0 : s->r[n] << lsb;
    s->r[d] = (s->r[d] & ~mask) | (src & mask);
    return;
  }
  }
  interp_undefined(s, insn, "plain binary immediate");
}

static void t32_branch_misc(icpu *s, uint32_t insn) {
  uint32_t op1 = BITS(insn, 14, 12), op = BITS(insn, 26, 20);
  uint32_t S = BIT(insn, 26), J1 = BIT(insn, 13), J2 = BIT(insn, 11);
  if ((op1 & 5) == 0) {
    if ((op & 0x38) != 0x38) { /* B<cond>, T3 */
      uint32_t imm = S << 20 | J2 << 19 | J1 << 18 | BITS(insn, 21, 16) << 12 | BITS(insn, 10, 0) << 1;
      imm = (uint32_t)((int32_t)(imm << 11) >> 11);
      if (cond_pass(s, BITS(insn, 25, 22))) s->next_pc = s->r[15] + imm;
      return;
    }
    switch (op) {
    case 0x38: case 0x39: { /* MSR register */
      int mask = (int)BITS(insn, 11, 8);
      if (BIT(insn, 20)) break;
      interp_msr_apsr(s, s->r[BITS(insn, 19, 16)], mask);
      return;
    }
    case 0x3A: /* hints, CPS */
      if (BITS(insn, 10, 8) == 0) return; /* NOP, YIELD, WFE, WFI, SEV, DBG */
      break;
    case 0x3B: /* CLREX, DSB, DMB, ISB */
      switch (BITS(insn, 7, 4)) {
      case 2: interp_clrex(s); return;
      case 4: case 5: case 6: interp_barrier(); return;
      }
      break;
    case 0x3E: case 0x3F: /* MRS */
      if (BIT(insn, 20)) break;
      s->r[BITS(insn, 11, 8)] = interp_mrs(s);
      return;
    }
    if (op1 == 2 && op == 0x7F) {
      interp_undefined(s, insn, "UDF");
      return;
    }
    interp_undefined(s, insn, "branch/misc control");
    return;
  }
  uint32_t I1 = !(J1 ^ S), I2 = !(J2 ^ S);
  uint32_t imm = S << 24 | I1 << 23 | I2 << 22 | BITS(insn, 25, 16) << 12 | BITS(insn, 10, 0) << 1;
  imm = (uint32_t)((int32_t)(imm << 7) >> 7);
  if (op1 & 4) { /* BL, BLX */
    s->r[14] = s->next_pc | 1;
    if (op1 & 1) {
      s->next_pc = s->r[15] + imm;
    } else {
      if (insn & 1) {
        interp_undefined(s, insn, "BLX with H set");
        return;
      }
      s->t = 0;
      s->next_pc = (s->r[15] & ~3u) + imm;
    }
    return;
  }
  s->next_pc = s->r[15] + imm; /* B, T4 */
}

/* Loads and stores of a single item (byte, halfword, word). */
static void t32_ldst_single(icpu *s, uint32_t insn) {
  int L = (int)BIT(insn, 20), sgn = (int)BIT(insn, 24), size = (int)BITS(insn, 22, 21);
  int n = (int)BITS(insn, 19, 16), t = (int)BITS(insn, 15, 12);
  uint32_t a, oa = 0;
  int wback = 0;
  if (size == 3 || (sgn && (!L || size == 2))) goto undef;
  if (n == 15) {
    if (!L) goto undef;
    uint32_t base = s->r[15] & ~3u, imm = insn & 0xFFF;
    a = BIT(insn, 23) ? base + imm : base - imm;
  } else if (BIT(insn, 23)) {
    a = s->r[n] + (insn & 0xFFF);
  } else if (BIT(insn, 11)) {
    int P = (int)BIT(insn, 10), U = (int)BIT(insn, 9), W = (int)BIT(insn, 8);
    uint32_t imm = insn & 0xFF;
    if (!P && !W) goto undef;
    oa = U ? s->r[n] + imm : s->r[n] - imm;
    a = P ? oa : s->r[n];
    wback = W;
  } else if (BITS(insn, 11, 6) == 0) {
    a = s->r[n] + (s->r[insn & 0xF] << BITS(insn, 5, 4));
  } else {
    goto undef;
  }
  if (L) {
    if (t == 15 && size < 2) return; /* PLD, PLI, unallocated hints */
    uint32_t v;
    switch (size) {
    case 0: v = sgn ? (uint32_t)(int8_t)rd8(s, a) : rd8(s, a); break;
    case 1: v = sgn ? (uint32_t)(int16_t)rd16(s, a) : rd16(s, a); break;
    default: v = rd32(s, a); break;
    }
    if (wback) s->r[n] = oa;
    set_reg_load(s, t, v);
  } else {
    switch (size) {
    case 0: wr8(s, a, (uint8_t)s->r[t]); break;
    case 1: wr16(s, a, (uint16_t)s->r[t]); break;
    default: wr32(s, a, s->r[t]); break;
    }
    if (wback) s->r[n] = oa;
  }
  return;
undef:
  interp_undefined(s, insn, "load/store single");
}

static void t32_dp_reg(icpu *s, uint32_t insn) {
  uint32_t op1 = BITS(insn, 23, 20), op2 = BITS(insn, 7, 4);
  int n = (int)BITS(insn, 19, 16), d = (int)BITS(insn, 11, 8), m = (int)BITS(insn, 3, 0);
  if (BITS(insn, 15, 12) != 0xF) goto undef;
  if (op2 == 0 && op1 < 8) { /* LSL, LSR, ASR, ROR (register) */
    uint32_t sc;
    uint32_t r = shift_c(s->r[n], (int)(op1 >> 1), s->r[m] & 0xFF, s->c, &sc);
    s->r[d] = r;
    if (op1 & 1) {
      set_nz(s, r);
      s->c = sc;
    }
    return;
  }
  if (op1 < 6 && (op2 & 8)) { /* extends */
    static const int ext_op[6] = {3, 7, 0, 4, 2, 6};
    uint32_t rot = BITS(insn, 5, 4) * 8, rm = s->r[m];
    uint32_t v = rot ? (rm >> rot) | (rm << (32 - rot)) : rm;
    s->r[d] = interp_extend(ext_op[op1], n == 15 ? 0 : s->r[n], v);
    return;
  }
  if ((op1 & 8) && op2 < 8) { /* parallel add/subtract */
    static const int par_op[8] = {4, 0, 1, -1, 7, 3, 2, -1};
    int op = par_op[op1 & 7], k = (int)(op2 & 3);
    if (op < 0 || k == 3) goto undef;
    s->r[d] = interp_parallel(s, (int)(op2 & 4) | k, op, s->r[n], s->r[m]);
    return;
  }
  if ((op1 & 0xC) == 8 && (op2 & 0xC) == 8) { /* miscellaneous */
    uint32_t a = op1 & 3, b = op2 & 3, rm = s->r[m];
    switch (a) {
    case 0: s->r[d] = interp_qadd(s, rm, s->r[n], (int)(b >> 1), (int)(b & 1)); return;
    case 1:
      switch (b) {
      case 0: s->r[d] = __builtin_bswap32(rm); return;
      case 1: s->r[d] = ((rm >> 8) & 0x00FF00FFu) | ((rm << 8) & 0xFF00FF00u); return;
      case 2: s->r[d] = interp_rbit(rm); return;
      default: s->r[d] = (uint32_t)(int32_t)(int16_t)(((rm >> 8) & 0xFF) | ((rm & 0xFF) << 8)); return;
      }
    case 2: if (b == 0) { s->r[d] = interp_sel(s, s->r[n], rm); return; } break;
    case 3: if (b == 0) { s->r[d] = interp_clz(rm); return; } break;
    }
  }
undef:
  interp_undefined(s, insn, "data processing (register)");
}

static void t32_multiply(icpu *s, uint32_t insn) {
  uint32_t op1 = BITS(insn, 22, 20), op2 = BITS(insn, 5, 4);
  int n = (int)BITS(insn, 19, 16), a = (int)BITS(insn, 15, 12), d = (int)BITS(insn, 11, 8);
  int m = (int)BITS(insn, 3, 0);
  uint32_t vn = s->r[n], vm = s->r[m], va = s->r[a];
  int acc = a != 15;
  if (BITS(insn, 7, 6) != 0) goto undef;
  switch (op1) {
  case 0:
    if (op2 == 0) { s->r[d] = vn * vm + (acc ? va : 0); return; }
    if (op2 == 1) { s->r[d] = va - vn * vm; return; }
    break;
  case 1: s->r[d] = interp_smla(s, (int)BIT(insn, 5), (int)BIT(insn, 4), vn, vm, va, acc); return;
  case 2: if (op2 < 2) { s->r[d] = interp_smlad(s, (int)BIT(insn, 4), 0, vn, vm, va, acc); return; } break;
  case 3: if (op2 < 2) { s->r[d] = interp_smlaw(s, (int)BIT(insn, 4), vn, vm, va, acc); return; } break;
  case 4: if (op2 < 2) { s->r[d] = interp_smlad(s, (int)BIT(insn, 4), 1, vn, vm, va, acc); return; } break;
  case 5: if (op2 < 2) { s->r[d] = interp_smmla((int)BIT(insn, 4), 0, vn, vm, va, acc); return; } break;
  case 6: if (op2 < 2) { s->r[d] = interp_smmla((int)BIT(insn, 4), 1, vn, vm, va, 1); return; } break;
  case 7:
    if (op2 == 0) {
      uint32_t r = interp_usad8(vn, vm);
      s->r[d] = acc ? r + va : r;
      return;
    }
    break;
  }
undef:
  interp_undefined(s, insn, "multiply");
}

static void t32_long_multiply(icpu *s, uint32_t insn) {
  uint32_t op1 = BITS(insn, 22, 20), op2 = BITS(insn, 7, 4);
  int n = (int)BITS(insn, 19, 16), lo = (int)BITS(insn, 15, 12), hi = (int)BITS(insn, 11, 8);
  int m = (int)BITS(insn, 3, 0);
  uint32_t vn = s->r[n], vm = s->r[m];
  uint64_t acc = (uint64_t)s->r[hi] << 32 | s->r[lo], r;
  switch (op1) {
  case 0: if (op2) goto undef; r = (uint64_t)((int64_t)(int32_t)vn * (int32_t)vm); break;
  case 1: if (op2 != 15) goto undef; s->r[hi] = interp_sdiv(vn, vm); return;
  case 2: if (op2) goto undef; r = (uint64_t)vn * vm; break;
  case 3: if (op2 != 15) goto undef; s->r[hi] = interp_udiv(vn, vm); return;
  case 4:
    if (op2 == 0) {
      r = acc + (uint64_t)((int64_t)(int32_t)vn * (int32_t)vm);
    } else if ((op2 & 0xC) == 8) {
      int32_t x = BIT(op2, 1) ? (int32_t)vn >> 16 : (int16_t)vn;
      int32_t y = BIT(op2, 0) ? (int32_t)vm >> 16 : (int16_t)vm;
      r = acc + (uint64_t)((int64_t)x * y);
    } else if ((op2 & 0xE) == 0xC) {
      r = interp_smlald((int)(op2 & 1), 0, vn, vm, acc);
    } else {
      goto undef;
    }
    break;
  case 5: if ((op2 & 0xE) != 0xC) goto undef; r = interp_smlald((int)(op2 & 1), 1, vn, vm, acc); break;
  case 6:
    if (op2 == 0) r = acc + (uint64_t)vn * vm;
    else if (op2 == 6) r = (uint64_t)vn * vm + s->r[lo] + s->r[hi];
    else goto undef;
    break;
  default: goto undef;
  }
  s->r[lo] = (uint32_t)r;
  s->r[hi] = (uint32_t)(r >> 32);
  return;
undef:
  interp_undefined(s, insn, "long multiply");
}

static void t32_exec(icpu *s, uint32_t insn) {
  uint32_t hw1 = insn >> 16;
  uint32_t op1 = BITS(hw1, 12, 11), op2 = BITS(hw1, 10, 4);
  if (op1 == 1) {
    if ((op2 & 0x64) == 0) t32_ldst_multiple(s, insn);
    else if ((op2 & 0x64) == 4) t32_dual_excl_tb(s, insn);
    else if ((op2 & 0x60) == 0x20) t32_dp_shifted(s, insn);
    else goto coproc;
    return;
  }
  if (op1 == 2) {
    if (BIT(insn, 15)) t32_branch_misc(s, insn);
    else if (op2 & 0x20) t32_dp_plainimm(s, insn);
    else t32_dp_modimm(s, insn);
    return;
  }
  /* op1 == 3 */
  if ((op2 & 0x71) == 0x10) { /* 001xxx0: NEON element load/store */
    interp_neon_ls(s, 0xF4000000u | (insn & 0x00FFFFFFu));
    return;
  }
  if ((op2 & 0x60) == 0) {
    t32_ldst_single(s, insn);
    return;
  }
  if ((op2 & 0x70) == 0x20) {
    t32_dp_reg(s, insn);
    return;
  }
  if ((op2 & 0x78) == 0x30) {
    t32_multiply(s, insn);
    return;
  }
  if ((op2 & 0x78) == 0x38) {
    t32_long_multiply(s, insn);
    return;
  }
coproc:
  if ((hw1 & 0xEF00) == 0xEF00) { /* 111U 1111: NEON data processing */
    interp_neon_dp(s, 0xF2000000u | BIT(hw1, 12) << 24 | (insn & 0x00FFFFFFu));
    return;
  }
  if ((hw1 & 0xEC00) == 0xEC00) {
    interp_coproc(s, (BIT(hw1, 12) ? 0xF0000000u : 0xE0000000u) | (insn & 0x0FFFFFFFu));
    return;
  }
  interp_undefined(s, insn, "32-bit");
}

/* ---- 16-bit ---- */

static void t16_exec(icpu *s, uint32_t h, int in_it) {
  int setflags = !in_it;
  uint32_t sc;
  switch (h >> 10) {
  case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: /* shift imm */
  {
    int op = (int)BITS(h, 12, 11);
    int type;
    uint32_t amt;
    decode_imm_shift(op, BITS(h, 10, 6), &type, &amt);
    uint32_t r = shift_c(s->r[BITS(h, 5, 3)], type, amt, s->c, &sc);
    s->r[h & 7] = r;
    if (setflags) {
      set_nz(s, r);
      s->c = sc;
    }
    return;
  }
  case 0x06: case 0x07: { /* ADD/SUB register or imm3 */
    int sub = (int)BIT(h, 9);
    uint32_t b = BIT(h, 10) ? BITS(h, 8, 6) : s->r[BITS(h, 8, 6)];
    interp_dp(s, sub ? DP_SUB : DP_ADD, setflags, (int)(h & 7), s->r[BITS(h, 5, 3)], b, s->c);
    return;
  }
  case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: {
    static const int ops[4] = {DP_MOV, DP_CMP, DP_ADD, DP_SUB};
    int op = ops[BITS(h, 12, 11)], d = (int)BITS(h, 10, 8);
    interp_dp(s, op, op == DP_CMP ? 1 : setflags, d, s->r[d], h & 0xFF, s->c);
    return;
  }
  case 0x10: { /* data processing */
    int op = (int)BITS(h, 9, 6), dn = (int)(h & 7), m = (int)BITS(h, 5, 3);
    uint32_t a = s->r[dn], b = s->r[m];
    switch (op) {
    case 0: interp_dp(s, DP_AND, setflags, dn, a, b, s->c); return;
    case 1: interp_dp(s, DP_EOR, setflags, dn, a, b, s->c); return;
    case 2: case 3: case 4: case 7: {
      static const int types[8] = {0, 0, SH_LSL, SH_LSR, SH_ASR, 0, 0, SH_ROR};
      uint32_t r = shift_c(a, types[op], b & 0xFF, s->c, &sc);
      s->r[dn] = r;
      if (setflags) {
        set_nz(s, r);
        s->c = sc;
      }
      return;
    }
    case 5: interp_dp(s, DP_ADC, setflags, dn, a, b, s->c); return;
    case 6: interp_dp(s, DP_SBC, setflags, dn, a, b, s->c); return;
    case 8: interp_dp(s, DP_TST, 1, dn, a, b, s->c); return;
    case 9: interp_dp(s, DP_RSB, setflags, dn, b, 0, s->c); return;
    case 10: interp_dp(s, DP_CMP, 1, dn, a, b, s->c); return;
    case 11: interp_dp(s, DP_CMN, 1, dn, a, b, s->c); return;
    case 12: interp_dp(s, DP_ORR, setflags, dn, a, b, s->c); return;
    case 13: {
      uint32_t r = a * b;
      s->r[dn] = r;
      if (setflags) set_nz(s, r);
      return;
    }
    case 14: interp_dp(s, DP_BIC, setflags, dn, a, b, s->c); return;
    default: interp_dp(s, DP_MVN, setflags, dn, a, b, s->c); return;
    }
  }
  case 0x11: { /* special data processing, BX, BLX */
    int op = (int)BITS(h, 9, 8), m = (int)BITS(h, 6, 3);
    int dn = (int)(BIT(h, 7) << 3 | (h & 7));
    switch (op) {
    case 0: set_reg_alu(s, dn, s->r[dn] + s->r[m]); return;
    case 1: interp_dp(s, DP_CMP, 1, dn, s->r[dn], s->r[m], s->c); return;
    case 2: set_reg_alu(s, dn, s->r[m]); return;
    default: {
      uint32_t target = s->r[m];
      if (BIT(h, 7)) s->r[14] = s->next_pc | 1;
      bx_to(s, target);
      return;
    }
    }
  }
  case 0x12: case 0x13: /* LDR literal */
    s->r[BITS(h, 10, 8)] = rd32(s, (s->r[15] & ~3u) + ((h & 0xFF) << 2));
    return;
  case 0x14: case 0x15: case 0x16: case 0x17: { /* load/store register offset */
    uint32_t a = s->r[BITS(h, 5, 3)] + s->r[BITS(h, 8, 6)];
    int t = (int)(h & 7);
    switch (BITS(h, 11, 9)) {
    case 0: wr32(s, a, s->r[t]); return;
    case 1: wr16(s, a, (uint16_t)s->r[t]); return;
    case 2: wr8(s, a, (uint8_t)s->r[t]); return;
    case 3: s->r[t] = (uint32_t)(int8_t)rd8(s, a); return;
    case 4: s->r[t] = rd32(s, a); return;
    case 5: s->r[t] = rd16(s, a); return;
    case 6: s->r[t] = rd8(s, a); return;
    default: s->r[t] = (uint32_t)(int16_t)rd16(s, a); return;
    }
  }
  case 0x18: case 0x19: case 0x1A: case 0x1B: { /* STR/LDR imm5 */
    uint32_t a = s->r[BITS(h, 5, 3)] + (BITS(h, 10, 6) << 2);
    if (BIT(h, 11)) s->r[h & 7] = rd32(s, a);
    else wr32(s, a, s->r[h & 7]);
    return;
  }
  case 0x1C: case 0x1D: case 0x1E: case 0x1F: { /* STRB/LDRB imm5 */
    uint32_t a = s->r[BITS(h, 5, 3)] + BITS(h, 10, 6);
    if (BIT(h, 11)) s->r[h & 7] = rd8(s, a);
    else wr8(s, a, (uint8_t)s->r[h & 7]);
    return;
  }
  case 0x20: case 0x21: case 0x22: case 0x23: { /* STRH/LDRH imm5 */
    uint32_t a = s->r[BITS(h, 5, 3)] + (BITS(h, 10, 6) << 1);
    if (BIT(h, 11)) s->r[h & 7] = rd16(s, a);
    else wr16(s, a, (uint16_t)s->r[h & 7]);
    return;
  }
  case 0x24: case 0x25: case 0x26: case 0x27: { /* STR/LDR SP-relative */
    uint32_t a = s->r[13] + ((h & 0xFF) << 2);
    if (BIT(h, 11)) s->r[BITS(h, 10, 8)] = rd32(s, a);
    else wr32(s, a, s->r[BITS(h, 10, 8)]);
    return;
  }
  case 0x28: case 0x29: /* ADR */
    s->r[BITS(h, 10, 8)] = (s->r[15] & ~3u) + ((h & 0xFF) << 2);
    return;
  case 0x2A: case 0x2B: /* ADD Rd, SP, imm */
    s->r[BITS(h, 10, 8)] = s->r[13] + ((h & 0xFF) << 2);
    return;
  case 0x2C: case 0x2D: case 0x2E: case 0x2F: { /* miscellaneous */
    uint32_t op = BITS(h, 11, 5);
    if ((op & 0x7C) == 0x00) { s->r[13] += (h & 0x7F) << 2; return; }
    if ((op & 0x7C) == 0x04) { s->r[13] -= (h & 0x7F) << 2; return; }
    if ((op & 0x28) == 0x08) { /* CBZ, CBNZ */
      uint32_t imm = BIT(h, 9) << 6 | BITS(h, 7, 3) << 1;
      if ((s->r[h & 7] == 0) != BIT(h, 11)) s->next_pc = s->r[15] + imm;
      return;
    }
    if ((op & 0x78) == 0x10) { /* SXTH, SXTB, UXTH, UXTB */
      static const int ext[4] = {3, 2, 7, 6};
      s->r[h & 7] = interp_extend(ext[BITS(h, 7, 6)], 0, s->r[BITS(h, 5, 3)]);
      return;
    }
    if ((op & 0x70) == 0x20) { /* PUSH */
      interp_stm(s, 13, (h & 0xFF) | BIT(h, 8) << 14, 0, 1, 1);
      return;
    }
    if ((op & 0x70) == 0x60) { /* POP */
      interp_ldm(s, 13, (h & 0xFF) | BIT(h, 8) << 15, 1, 0, 1);
      return;
    }
    if ((op & 0x78) == 0x50) { /* REV, REV16, REVSH */
      uint32_t rm = s->r[BITS(h, 5, 3)];
      switch (BITS(h, 7, 6)) {
      case 0: s->r[h & 7] = __builtin_bswap32(rm); return;
      case 1: s->r[h & 7] = ((rm >> 8) & 0x00FF00FFu) | ((rm << 8) & 0xFF00FF00u); return;
      case 3: s->r[h & 7] = (uint32_t)(int32_t)(int16_t)(((rm >> 8) & 0xFF) | ((rm & 0xFF) << 8)); return;
      }
      break;
    }
    if (op == 0x33) return; /* CPS: no effect in user mode */
    if (op == 0x32 && (h & 0xF7) == 0x50) { /* SETEND */
      s->cpsr_rest = (s->cpsr_rest & ~0x200u) | BIT(h, 3) << 9;
      return;
    }
    if ((op & 0x78) == 0x70) { /* BKPT */
      interp_fault(s, INTERP_FAULT_OTHER, "bkpt #0x%x", h & 0xFF);
      return;
    }
    if ((op & 0x78) == 0x78) { /* IT, hints */
      if (h & 0xF) {
        if (in_it) break;
        s->it = h & 0xFF;
      }
      return; /* NOP, YIELD, WFE, WFI, SEV */
    }
    break;
  }
  case 0x30: case 0x31: /* STM */
    interp_stm(s, (int)BITS(h, 10, 8), h & 0xFF, 1, 0, 1);
    return;
  case 0x32: case 0x33: { /* LDM */
    int n = (int)BITS(h, 10, 8);
    interp_ldm(s, n, h & 0xFF, 1, 0, !(h & (1u << n)));
    return;
  }
  case 0x34: case 0x35: case 0x36: case 0x37: { /* B<cond>, SVC, UDF */
    uint32_t cond = BITS(h, 11, 8);
    if (cond == 0xF) {
      interp_svc(s, h & 0xFF);
      return;
    }
    if (cond == 0xE) break;
    if (cond_pass(s, cond)) s->next_pc = s->r[15] + (uint32_t)((int32_t)(h << 24) >> 23);
    return;
  }
  case 0x38: case 0x39: /* B */
    s->next_pc = s->r[15] + (uint32_t)((int32_t)(h << 21) >> 20);
    return;
  }
  interp_undefined(s, h, "16-bit");
}

void interp_thumb_step(icpu *s) {
  uint32_t pc = s->r[15];
  if (unlikely(s->checked) && !interp_mapped(s, pc, 2, 0)) return;
  uint16_t h;
  memcpy(&h, g_mem + pc, 2);
  uint32_t insn;
  int wide = h >= 0xE800;
  if (wide) {
    if (unlikely(s->checked) && !interp_mapped(s, pc + 2, 2, 0)) return;
    uint16_t h2;
    memcpy(&h2, g_mem + pc + 2, 2);
    insn = (uint32_t)h << 16 | h2;
    s->next_pc = pc + 4;
  } else {
    insn = h;
    s->next_pc = pc + 2;
  }
  s->r[15] = pc + 4;
  uint32_t it = s->it;
  int in_it = (it & 0xF) != 0;
  if (in_it) {
    s->it = (it & 7) == 0 ? 0 : (it & 0xE0) | ((it << 1) & 0x1F);
    if (!cond_pass(s, it >> 4)) {
      s->r[15] = s->next_pc;
      return;
    }
  }
  if (wide) t32_exec(s, insn);
  else t16_exec(s, insn, in_it);
  s->r[15] = s->next_pc;
}
