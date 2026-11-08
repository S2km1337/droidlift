#include "decode.hpp"

#include <cstdio>
#include <cstdlib>

namespace recomp {

namespace {

// Field letters (all tables):
//   c cond      n Rn        d Rd        m Rm        s Rs (shift reg)  t Rt
//   T Rt2       a Ra        h RdHi      l RdLo      i immediate       r rotation
//   y shift type S set flags P/U/W/B/L addressing bits     R register list
//   w width     b lsb / msb k second immediate  H/J/j branch bits  x ignored
//   D/N/M       VFP/SIMD register extension bits  z VFP size (1 = double)
// Rows may use other letters for their own fields; the row's norm reads them.

uint32_t ror32(uint32_t v, uint32_t n) {
  n &= 31;
  return n ? (v >> n) | (v << (32 - n)) : v;
}
int32_t sext(uint32_t v, int bits) { return int32_t(v << (32 - bits)) >> (32 - bits); }

std::vector<InsnDef> *g_build; // table being built

InsnDef make(const char *name, const std::string &pattern, Kind kind, int sub, NormFn norm) {
  if (pattern.size() != 32 && pattern.size() != 16) {
    std::fprintf(stderr, "decoder table: pattern for %s has %zu bits\n", name, pattern.size());
    std::abort();
  }
  InsnDef d;
  d.name = name;
  d.pattern = pattern;
  d.bits = int(pattern.size());
  d.kind = kind;
  d.sub = sub;
  d.norm = norm;
  for (int i = 0; i < d.bits; i++) {
    char ch = pattern[size_t(i)];
    uint32_t bitno = uint32_t(d.bits - 1 - i);
    uint32_t bit = 1u << bitno;
    if (ch == '0' || ch == '1') {
      d.mask |= bit;
      if (ch == '1') d.value |= bit;
      continue;
    }
    if (ch == 'x') continue;
    std::vector<uint8_t> *f = nullptr;
    for (auto &e : d.fields)
      if (e.first == ch) f = &e.second;
    if (!f) {
      d.fields.push_back({ch, {}});
      f = &d.fields.back().second;
    }
    f->push_back(uint8_t(bitno));
  }
  d.has_cond = d.bits == 32 && pattern.compare(0, 4, "cccc") == 0;
  return d;
}

void row(const char *name, const std::string &pattern, Kind kind, int sub = 0, NormFn norm = nullptr) {
  g_build->push_back(make(name, pattern, kind, sub, norm));
}

Fields extract(const InsnDef &d, uint32_t raw) {
  Fields f;
  for (const auto &[letter, bits] : d.fields) {
    uint32_t v = 0;
    for (uint8_t b : bits) v = v << 1 | (raw >> b & 1);
    f.v[uint8_t(letter)] = v;
  }
  return f;
}

bool has(const InsnDef &d, char c) {
  for (const auto &e : d.fields)
    if (e.first == c) return true;
  return false;
}

// Fills the fields every table names the same way.
void generic(Insn &in, const Fields &f) {
  const InsnDef &d = *in.def;
  if (d.has_cond) in.cond = f['c'];
  if (has(d, 'd')) in.d = int8_t(f['d']);
  if (has(d, 'n')) in.n = int8_t(f['n']);
  if (has(d, 'm')) in.m = int8_t(f['m']);
  if (has(d, 's')) in.s = int8_t(f['s']);
  if (has(d, 't')) in.t = int8_t(f['t']);
  if (has(d, 'T')) in.t2 = int8_t(f['T']);
  if (has(d, 'a')) in.a = int8_t(f['a']);
  if (has(d, 'S')) in.S = f['S'];
  if (has(d, 'P')) in.P = f['P'];
  if (has(d, 'U')) in.U = f['U'];
  if (has(d, 'W')) in.W = f['W'];
  if (has(d, 'R')) in.list = f['R'];
}

// ---- Shared norms ---------------------------------------------------------

void n_rm_imm(Insn &in, const Fields &f) { // m shifted by immediate (y, i)
  in.op2 = Op2::RegImm;
  in.sht = uint8_t(f['y']);
  in.sha = uint8_t(f['i']);
}
void n_rm(Insn &in, const Fields &) { in.op2 = Op2::RegImm; } // plain register (LSL #0)
void n_rm_reg(Insn &in, const Fields &f) {
  in.op2 = Op2::RegReg;
  in.sht = uint8_t(f['y']);
}
void n_imm(Insn &in, const Fields &f) {
  in.op2 = Op2::Imm;
  in.imm = f['i'];
}
void n_long(Insn &in, const Fields &f) { // RdHi/RdLo -> d (hi), a (lo)
  in.d = int8_t(f['h']);
  in.a = int8_t(f['l']);
}

// ---- ARM ------------------------------------------------------------------

void a_dpimm(Insn &in, const Fields &f) {
  uint32_t rot = f['r'] * 2, v = ror32(f['i'], rot);
  in.op2 = Op2::Imm;
  in.imm = v;
  in.imm_c = rot ? int8_t(v >> 31) : -1;
}
void a_branch(Insn &in, const Fields &f) { in.target = in.addr + 8 + uint32_t(sext(f['i'], 24) * 4); }
void a_blx(Insn &in, const Fields &f) {
  in.target = (in.addr + 8 + uint32_t(sext(f['i'], 24) * 4) + f['H'] * 2) | 1;
}
void a_rot8(Insn &in, const Fields &f) { in.rot = f['r'] * 8; }
void a_bfx(Insn &in, const Fields &f) {
  in.lsb = f['b'];
  in.width = f['w'] + 1;
}
void a_bfi(Insn &in, const Fields &f) {
  in.lsb = f['b'];
  in.width = f['w']; // msb
}
void a_sat(Insn &in, const Fields &f) {
  in.width = f['w'];
  in.op2 = Op2::RegImm;
  in.sht = uint8_t(f['y'] << 1); // sh: 0 LSL, 1 ASR
  in.sha = uint8_t(f['i']);
  in.m = in.n;
}
void a_half(Insn &in, const Fields &f) { in.rot = f['X'] << 1 | f['Y']; }
void a_half_long(Insn &in, const Fields &f) {
  n_long(in, f);
  a_half(in, f);
}
void a_xflag(Insn &in, const Fields &f) { in.rot = f['X']; }
void a_dual_long(Insn &in, const Fields &f) {
  n_long(in, f);
  in.rot = f['X'];
}
void a_svc(Insn &in, const Fields &f) { in.imm = f['i']; }
void a_msr(Insn &in, const Fields &f) { in.lsb = f['k']; }
void a_cp(Insn &in, const Fields &) { in.imm = in.raw; }
void a_pkh(Insn &in, const Fields &f) {
  in.op2 = Op2::RegImm;
  in.sht = uint8_t(in.def->sub ? 2 : 0);
  in.sha = uint8_t(f['i']);
}

void build_arm() {
  // Branches and calls.
  row("b", "cccc1010iiiiiiiiiiiiiiiiiiiiiiii", Kind::B, 0, a_branch);
  row("bl", "cccc1011iiiiiiiiiiiiiiiiiiiiiiii", Kind::Bl, 0, a_branch);
  row("blx", "1111101Hiiiiiiiiiiiiiiiiiiiiiiii", Kind::Bl, 0, a_blx);
  row("bx", "cccc000100101111111111110001mmmm", Kind::Bx);
  row("blx", "cccc000100101111111111110011mmmm", Kind::BlxReg);
  row("svc", "cccc1111iiiiiiiiiiiiiiiiiiiiiiii", Kind::Svc, 0, a_svc);
  row("udf", "111001111111xxxxxxxxxxxx1111xxxx", Kind::Undef);
  row("bkpt", "cccc00010010xxxxxxxxxxxx0111xxxx", Kind::Bkpt);

  // Hints, preloads, barriers, exclusives.
  row("nop", "cccc0011001000001111000000000xxx", Kind::Nop); // nop, yield, wfe, wfi, sev
  row("pld", "11110101xx01xxxx1111xxxxxxxxxxxx", Kind::Nop);
  row("pld", "11110111xx01xxxx1111xxxxxxx0xxxx", Kind::Nop);
  row("pli", "11110100x101xxxx1111xxxxxxxxxxxx", Kind::Nop);
  row("pli", "11110110x101xxxx1111xxxxxxx0xxxx", Kind::Nop);
  row("clrex", "11110101011111111111000000011111", Kind::Clrex);
  row("dsb", "1111010101111111111100000100xxxx", Kind::Barrier);
  row("dmb", "1111010101111111111100000101xxxx", Kind::Barrier);
  row("isb", "1111010101111111111100000110xxxx", Kind::Barrier);
  row("strex", "cccc00011000nnnndddd11111001tttt", Kind::Strex, 2);
  row("ldrex", "cccc00011001nnnntttt111110011111", Kind::Ldrex, 2);
  row("strexd", "cccc00011010nnnndddd11111001tttt", Kind::Strex, 3);
  row("ldrexd", "cccc00011011nnnntttt111110011111", Kind::Ldrex, 3);
  row("strexb", "cccc00011100nnnndddd11111001tttt", Kind::Strex, 0);
  row("ldrexb", "cccc00011101nnnntttt111110011111", Kind::Ldrex, 0);
  row("strexh", "cccc00011110nnnndddd11111001tttt", Kind::Strex, 1);
  row("ldrexh", "cccc00011111nnnntttt111110011111", Kind::Ldrex, 1);

  // Status registers.
  row("mrs", "cccc000100001111dddd000000000000", Kind::Mrs);
  row("msr", "cccc00010010kk00111100000000nnnn", Kind::Msr, 0, a_msr);
  row("msr", "cccc00110010kk001111rrrriiiiiiii", Kind::Msr, 1,
      [](Insn &in, const Fields &f) {
        a_dpimm(in, f);
        in.imm_c = -1;
        a_msr(in, f);
      });

  // Wide moves.
  row("movw", "cccc00110000iiiiddddiiiiiiiiiiii", Kind::MovW, 0, n_imm);
  row("movt", "cccc00110100iiiiddddiiiiiiiiiiii", Kind::MovT, 0, n_imm);

  // Multiplies and divides.
  row("mul", "cccc0000000Sdddd0000mmmm1001nnnn", Kind::Mul, MUL);
  row("mla", "cccc0000001Sddddaaaammmm1001nnnn", Kind::Mul, MLA);
  row("mls", "cccc00000110ddddaaaammmm1001nnnn", Kind::Mul, MLS);
  row("umaal", "cccc00000100hhhhllllmmmm1001nnnn", Kind::Mul, UMAAL, n_long);
  row("umull", "cccc0000100Shhhhllllmmmm1001nnnn", Kind::Mul, UMULL, n_long);
  row("umlal", "cccc0000101Shhhhllllmmmm1001nnnn", Kind::Mul, UMLAL, n_long);
  row("smull", "cccc0000110Shhhhllllmmmm1001nnnn", Kind::Mul, SMULL, n_long);
  row("smlal", "cccc0000111Shhhhllllmmmm1001nnnn", Kind::Mul, SMLAL, n_long);
  row("smlaxy", "cccc00010000ddddaaaammmm1YX0nnnn", Kind::MulHalf, SMLAXY,
      [](Insn &in, const Fields &f) { in.rot = f['X'] << 1 | f['Y']; });
  row("smlawy", "cccc00010010ddddaaaammmm1Y00nnnn", Kind::MulHalf, SMLAWY, a_half);
  row("smulwy", "cccc00010010dddd0000mmmm1Y10nnnn", Kind::MulHalf, SMULWY, a_half);
  row("smlalxy", "cccc00010100hhhhllllmmmm1YX0nnnn", Kind::MulHalf, SMLALXY, a_half_long);
  row("smulxy", "cccc00010110dddd0000mmmm1YX0nnnn", Kind::MulHalf, SMULXY, a_half);
  row("smuad", "cccc01110000dddd1111mmmm00X1nnnn", Kind::MulDual, SMUAD, a_xflag);
  row("smlad", "cccc01110000ddddaaaammmm00X1nnnn", Kind::MulDual, SMLAD, a_xflag);
  row("smusd", "cccc01110000dddd1111mmmm01X1nnnn", Kind::MulDual, SMUSD, a_xflag);
  row("smlsd", "cccc01110000ddddaaaammmm01X1nnnn", Kind::MulDual, SMLSD, a_xflag);
  row("smlald", "cccc01110100hhhhllllmmmm00X1nnnn", Kind::MulDual, SMLALD, a_dual_long);
  row("smlsld", "cccc01110100hhhhllllmmmm01X1nnnn", Kind::MulDual, SMLSLD, a_dual_long);
  row("smmul", "cccc01110101dddd1111mmmm00X1nnnn", Kind::MulHi, 0, a_xflag);
  row("smmla", "cccc01110101ddddaaaammmm00X1nnnn", Kind::MulHi, 1, a_xflag);
  row("smmls", "cccc01110101ddddaaaammmm11X1nnnn", Kind::MulHi, 2, a_xflag);
  row("sdiv", "cccc01110001dddd1111mmmm0001nnnn", Kind::Div, 1);
  row("udiv", "cccc01110011dddd1111mmmm0001nnnn", Kind::Div, 0);
  row("usad8", "cccc01111000dddd1111mmmm0001nnnn", Kind::Usad8);
  row("usada8", "cccc01111000ddddaaaammmm0001nnnn", Kind::Usad8);

  // Saturation.
  row("qadd", "cccc00010000nnnndddd00000101mmmm", Kind::QArith, QADD);
  row("qsub", "cccc00010010nnnndddd00000101mmmm", Kind::QArith, QSUB);
  row("qdadd", "cccc00010100nnnndddd00000101mmmm", Kind::QArith, QDADD);
  row("qdsub", "cccc00010110nnnndddd00000101mmmm", Kind::QArith, QDSUB);
  row("ssat16", "cccc01101010wwwwdddd11110011nnnn", Kind::Sat16, 1,
      [](Insn &in, const Fields &f) { in.width = f['w']; });
  row("usat16", "cccc01101110wwwwdddd11110011nnnn", Kind::Sat16, 0,
      [](Insn &in, const Fields &f) { in.width = f['w']; });
  row("ssat", "cccc0110101wwwwwddddiiiiiy01nnnn", Kind::Sat, 1, a_sat);
  row("usat", "cccc0110111wwwwwddddiiiiiy01nnnn", Kind::Sat, 0, a_sat);

  // Packing, selection, parallel arithmetic.
  row("sel", "cccc01101000nnnndddd11111011mmmm", Kind::Sel);
  row("pkhbt", "cccc01101000nnnnddddiiiii001mmmm", Kind::Pkh, 0, a_pkh);
  row("pkhtb", "cccc01101000nnnnddddiiiii101mmmm", Kind::Pkh, 1, a_pkh);
  row("parallel", "cccc01100pppnnnndddd1111ooo1mmmm", Kind::Parallel, 0,
      [](Insn &in, const Fields &f) { in.sub = int(f['p'] * 8 + f['o']); });

  // Miscellaneous and media.
  row("clz", "cccc000101101111dddd11110001mmmm", Kind::Clz);
  row("rev", "cccc011010111111dddd11110011mmmm", Kind::Rev, REV);
  row("rev16", "cccc011010111111dddd11111011mmmm", Kind::Rev, REV16);
  row("rbit", "cccc011011111111dddd11110011mmmm", Kind::Rev, RBIT);
  row("revsh", "cccc011011111111dddd11111011mmmm", Kind::Rev, REVSH);
  row("sxtb16", "cccc01101000nnnnddddrr000111mmmm", Kind::Extend, SXTB16, a_rot8);
  row("sxtb", "cccc01101010nnnnddddrr000111mmmm", Kind::Extend, SXTB, a_rot8);
  row("sxth", "cccc01101011nnnnddddrr000111mmmm", Kind::Extend, SXTH, a_rot8);
  row("uxtb16", "cccc01101100nnnnddddrr000111mmmm", Kind::Extend, UXTB16, a_rot8);
  row("uxtb", "cccc01101110nnnnddddrr000111mmmm", Kind::Extend, UXTB, a_rot8);
  row("uxth", "cccc01101111nnnnddddrr000111mmmm", Kind::Extend, UXTH, a_rot8);
  row("sbfx", "cccc0111101wwwwwddddbbbbb101nnnn", Kind::Bfx, 1, a_bfx);
  row("ubfx", "cccc0111111wwwwwddddbbbbb101nnnn", Kind::Bfx, 0, a_bfx);
  row("bfc", "cccc0111110wwwwwddddbbbbb0011111", Kind::Bfc, 0, a_bfi);
  row("bfi", "cccc0111110wwwwwddddbbbbb001nnnn", Kind::Bfi, 0, a_bfi);

  // Loads and stores.
  static const char *const w_names[4] = {"str", "ldr", "strb", "ldrb"};
  static const int w_ops[4] = {STR, LDR, STRB, LDRB};
  for (int k = 0; k < 4; k++) {
    std::string bl = std::string(k >= 2 ? "1" : "0") + "W" + (k & 1 ? "1" : "0");
    row(w_names[k], "cccc010PU" + bl + "nnnnttttiiiiiiiiiiii", Kind::LdSt, w_ops[k], n_imm);
    row(w_names[k], "cccc011PU" + bl + "nnnnttttiiiiiyy0mmmm", Kind::LdSt, w_ops[k], n_rm_imm);
  }
  static const char *const x_names[6] = {"strh", "ldrh", "ldrd", "ldrsb", "strd", "ldrsh"};
  static const int x_ops[6] = {STRH, LDRH, LDRD, LDRSB, STRD, LDRSH};
  static const char *const x_bits[6] = {"0", "1", "0", "1", "0", "1"};
  static const char *const x_op2[6] = {"1011", "1011", "1101", "1101", "1111", "1111"};
  for (int k = 0; k < 6; k++) {
    row(x_names[k], std::string("cccc000PU1W") + x_bits[k] + "nnnnttttiiii" + x_op2[k] + "iiii", Kind::LdSt, x_ops[k],
        n_imm);
    row(x_names[k], std::string("cccc000PU0W") + x_bits[k] + "nnnntttt0000" + x_op2[k] + "mmmm", Kind::LdSt, x_ops[k],
        n_rm);
  }
  row("ldm", "cccc100PU0W1nnnnRRRRRRRRRRRRRRRR", Kind::Ldm);
  row("stm", "cccc100PU0W0nnnnRRRRRRRRRRRRRRRR", Kind::Stm);

  // Data processing: 16 opcodes in three operand forms. The compare ops
  // always set flags (S=0 there is the miscellaneous space), MOV/MVN have no
  // Rn.
  static const char *names[16] = {"and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
                                  "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"};
  for (int op = 0; op < 16; op++) {
    std::string opc;
    for (int b = 3; b >= 0; b--) opc += (op >> b & 1) ? '1' : '0';
    bool compare = op >= TST && op <= CMN;
    bool no_rn = op == MOV || op == MVN;
    std::string s = compare ? "1" : "S";
    std::string n = no_rn ? "xxxx" : "nnnn";
    std::string d = compare ? "xxxx" : "dddd";
    row(names[op], "cccc001" + opc + s + n + d + "rrrriiiiiiii", Kind::Dp, op, a_dpimm);
    row(names[op], "cccc000" + opc + s + n + d + "iiiiiyy0mmmm", Kind::Dp, op, n_rm_imm);
    row(names[op], "cccc000" + opc + s + n + d + "ssss0yy1mmmm", Kind::Dp, op, n_rm_reg);
  }
}

// ---- Thumb ----------------------------------------------------------------

// Flags for 16-bit data processing: set unless inside an IT block.
void t_s_outside_it(Insn &in) { in.S = !in.in_it; }

void t16_shift_imm(Insn &in, const Fields &f) { // LSL/LSR/ASR #imm5: MOV with shifted Rm
  t_s_outside_it(in);
  in.op2 = Op2::RegImm;
  in.sht = uint8_t(in.def->sub);
  in.sha = uint8_t(f['i']);
  in.sub = MOV;
}
void t16_addsub3(Insn &in, const Fields &) { // ADD/SUB Rd, Rn, Rm
  t_s_outside_it(in);
  in.op2 = Op2::RegImm;
}
void t16_addsub_imm3(Insn &in, const Fields &f) {
  t_s_outside_it(in);
  in.op2 = Op2::Imm;
  in.imm = f['i'];
}
void t16_imm8(Insn &in, const Fields &f) { // MOV/CMP/ADD/SUB #imm8 (Rdn)
  if (in.sub != CMP) t_s_outside_it(in);
  else in.S = true;
  in.op2 = Op2::Imm;
  in.imm = f['i'];
  if (in.sub == ADD || in.sub == SUB) in.n = in.d;
  if (in.sub == CMP) in.n = in.d, in.d = -1;
}
// Rdn = Rdn op Rm (and the compare forms).
void t16_dp(Insn &in, const Fields &) {
  int op = in.sub;
  if (op >= TST && op <= CMN) {
    in.S = true;
    in.n = in.d;
    in.d = -1;
  } else {
    t_s_outside_it(in);
    in.n = in.d;
  }
  in.op2 = Op2::RegImm;
  if (op == MOV || op == MVN) in.n = -1;
}
void t16_shift_reg(Insn &in, const Fields &) { // Rdn = Rdn <shift> Rs
  t_s_outside_it(in);
  in.op2 = Op2::RegReg;
  in.m = in.d;
  in.sht = uint8_t(in.sub);
  in.sub = MOV;
}
void t16_neg(Insn &in, const Fields &) { // RSBS Rd, Rn, #0
  t_s_outside_it(in);
  in.op2 = Op2::Imm;
  in.imm = 0;
}
void t16_mul(Insn &in, const Fields &) { // Rdm = Rn * Rdm
  t_s_outside_it(in);
  in.m = in.d;
}
void t16_hi(Insn &in, const Fields &) { // ADD/MOV with high registers (no flags); CMP
  in.op2 = Op2::RegImm;
  if (in.sub == ADD) in.n = in.d;
  if (in.sub == CMP) in.S = true;
}
void t16_ldst_imm(Insn &in, const Fields &f) {
  int sc = in.sub == LDR || in.sub == STR ? 4 : in.sub == LDRH || in.sub == STRH ? 2 : 1;
  in.op2 = Op2::Imm;
  in.imm = f['i'] * uint32_t(sc);
}
void t16_ldst_sp(Insn &in, const Fields &f) {
  in.n = 13;
  in.op2 = Op2::Imm;
  in.imm = f['i'] * 4;
}
void t16_ldr_lit(Insn &in, const Fields &f) {
  in.n = 15;
  in.op2 = Op2::Imm;
  in.imm = f['i'] * 4;
}
void t16_adr(Insn &in, const Fields &f) { in.target = ((in.addr + 4) & ~3u) + f['i'] * 4; }
void t16_add_sp(Insn &in, const Fields &f) { // ADD Rd, SP, #imm8*4
  in.n = 13;
  in.op2 = Op2::Imm;
  in.imm = f['i'] * 4;
}
void t16_sp_imm7(Insn &in, const Fields &f) { // ADD/SUB SP, SP, #imm7*4
  in.d = in.n = 13;
  in.op2 = Op2::Imm;
  in.imm = f['i'] * 4;
}
void t16_push(Insn &in, const Fields &f) { // STMDB SP!
  in.n = 13;
  in.P = true;
  in.U = false;
  in.W = true;
  in.list = f['R'] | f['M'] << 14;
}
void t16_pop(Insn &in, const Fields &f) { // LDMIA SP!
  in.n = 13;
  in.P = false;
  in.U = true;
  in.W = true;
  in.list = f['R'] | f['M'] << 15;
}
void t16_ldstm(Insn &in, const Fields &) { // STMIA Rn! / LDMIA Rn(!)
  in.P = false;
  in.U = true;
  in.W = in.kind == Kind::Stm || !((in.list >> in.n) & 1);
}
void t16_bcond(Insn &in, const Fields &f) {
  in.cond = f['c'];
  in.target = (in.addr + 4 + uint32_t(sext(f['i'], 8) * 2)) | 1;
}
void t16_b(Insn &in, const Fields &f) { in.target = (in.addr + 4 + uint32_t(sext(f['i'], 11) * 2)) | 1; }
void t16_cbz(Insn &in, const Fields &f) { in.target = (in.addr + 4 + f['i'] * 2) | 1; }
void t_imm(Insn &in, const Fields &f) { in.imm = f['i']; }
void t16_ext(Insn &in, const Fields &) { in.n = 15; }

void build_thumb16() {
  row("lsl", "00000iiiiimmmddd", Kind::Dp, 0, t16_shift_imm);
  row("lsr", "00001iiiiimmmddd", Kind::Dp, 1, t16_shift_imm);
  row("asr", "00010iiiiimmmddd", Kind::Dp, 2, t16_shift_imm);
  row("add", "0001100mmmnnnddd", Kind::Dp, ADD, t16_addsub3);
  row("sub", "0001101mmmnnnddd", Kind::Dp, SUB, t16_addsub3);
  row("add", "0001110iiinnnddd", Kind::Dp, ADD, t16_addsub_imm3);
  row("sub", "0001111iiinnnddd", Kind::Dp, SUB, t16_addsub_imm3);
  row("mov", "00100dddiiiiiiii", Kind::Dp, MOV, t16_imm8);
  row("cmp", "00101dddiiiiiiii", Kind::Dp, CMP, t16_imm8);
  row("add", "00110dddiiiiiiii", Kind::Dp, ADD, t16_imm8);
  row("sub", "00111dddiiiiiiii", Kind::Dp, SUB, t16_imm8);
  row("and", "0100000000mmmddd", Kind::Dp, AND, t16_dp);
  row("eor", "0100000001mmmddd", Kind::Dp, EOR, t16_dp);
  row("lsl", "0100000010sssddd", Kind::Dp, 0, t16_shift_reg);
  row("lsr", "0100000011sssddd", Kind::Dp, 1, t16_shift_reg);
  row("asr", "0100000100sssddd", Kind::Dp, 2, t16_shift_reg);
  row("adc", "0100000101mmmddd", Kind::Dp, ADC, t16_dp);
  row("sbc", "0100000110mmmddd", Kind::Dp, SBC, t16_dp);
  row("ror", "0100000111sssddd", Kind::Dp, 3, t16_shift_reg);
  row("tst", "0100001000mmmddd", Kind::Dp, TST, t16_dp);
  row("rsb", "0100001001nnnddd", Kind::Dp, RSB, t16_neg);
  row("cmp", "0100001010mmmddd", Kind::Dp, CMP, t16_dp);
  row("cmn", "0100001011mmmddd", Kind::Dp, CMN, t16_dp);
  row("orr", "0100001100mmmddd", Kind::Dp, ORR, t16_dp);
  row("mul", "0100001101nnnddd", Kind::Mul, MUL, t16_mul);
  row("bic", "0100001110mmmddd", Kind::Dp, BIC, t16_dp);
  row("mvn", "0100001111mmmddd", Kind::Dp, MVN, t16_dp);
  row("add", "01000100dmmmmddd", Kind::Dp, ADD, t16_hi);
  row("cmp", "01000101nmmmmnnn", Kind::Dp, CMP, t16_hi);
  row("mov", "01000110dmmmmddd", Kind::Dp, MOV, t16_hi);
  row("bx", "010001110mmmmxxx", Kind::Bx);
  row("blx", "010001111mmmmxxx", Kind::BlxReg);
  row("ldr", "01001tttiiiiiiii", Kind::LdSt, LDR, t16_ldr_lit);
  static const char *const r_names[8] = {"str", "strh", "strb", "ldrsb", "ldr", "ldrh", "ldrb", "ldrsh"};
  static const int r_ops[8] = {STR, STRH, STRB, LDRSB, LDR, LDRH, LDRB, LDRSH};
  for (int k = 0; k < 8; k++) {
    std::string opc = std::string(k & 4 ? "1" : "0") + (k & 2 ? "1" : "0") + (k & 1 ? "1" : "0");
    row(r_names[k], "0101" + opc + "mmmnnnttt", Kind::LdSt, r_ops[k], n_rm);
  }
  row("str", "01100iiiiinnnttt", Kind::LdSt, STR, t16_ldst_imm);
  row("ldr", "01101iiiiinnnttt", Kind::LdSt, LDR, t16_ldst_imm);
  row("strb", "01110iiiiinnnttt", Kind::LdSt, STRB, t16_ldst_imm);
  row("ldrb", "01111iiiiinnnttt", Kind::LdSt, LDRB, t16_ldst_imm);
  row("strh", "10000iiiiinnnttt", Kind::LdSt, STRH, t16_ldst_imm);
  row("ldrh", "10001iiiiinnnttt", Kind::LdSt, LDRH, t16_ldst_imm);
  row("str", "10010tttiiiiiiii", Kind::LdSt, STR, t16_ldst_sp);
  row("ldr", "10011tttiiiiiiii", Kind::LdSt, LDR, t16_ldst_sp);
  row("adr", "10100dddiiiiiiii", Kind::Adr, 0, t16_adr);
  row("add", "10101dddiiiiiiii", Kind::Dp, ADD, t16_add_sp);
  row("add", "101100000iiiiiii", Kind::Dp, ADD, t16_sp_imm7);
  row("sub", "101100001iiiiiii", Kind::Dp, SUB, t16_sp_imm7);
  row("cbz", "101100i1iiiiinnn", Kind::Cbz, 0, t16_cbz);
  row("cbnz", "101110i1iiiiinnn", Kind::Cbz, 1, t16_cbz);
  row("sxth", "1011001000mmmddd", Kind::Extend, SXTH, t16_ext);
  row("sxtb", "1011001001mmmddd", Kind::Extend, SXTB, t16_ext);
  row("uxth", "1011001010mmmddd", Kind::Extend, UXTH, t16_ext);
  row("uxtb", "1011001011mmmddd", Kind::Extend, UXTB, t16_ext);
  row("push", "1011010MRRRRRRRR", Kind::Stm, 0, t16_push);
  row("pop", "1011110MRRRRRRRR", Kind::Ldm, 0, t16_pop);
  row("rev", "1011101000mmmddd", Kind::Rev, REV);
  row("rev16", "1011101001mmmddd", Kind::Rev, REV16);
  row("revsh", "1011101011mmmddd", Kind::Rev, REVSH);
  row("bkpt", "10111110xxxxxxxx", Kind::Bkpt);
  row("nop", "10111111xxxx0000", Kind::Nop);
  row("it", "10111111iiiiiiii", Kind::It, 0, t_imm);
  row("stmia", "11000nnnRRRRRRRR", Kind::Stm, 0, t16_ldstm);
  row("ldmia", "11001nnnRRRRRRRR", Kind::Ldm, 0, t16_ldstm);
  row("udf", "11011110xxxxxxxx", Kind::Undef);
  row("svc", "11011111iiiiiiii", Kind::Svc, 0, t_imm);
  row("b", "1101cccciiiiiiii", Kind::B, 0, t16_bcond);
  row("b", "11100iiiiiiiiiii", Kind::B, 0, t16_b);
}

// 32-bit Thumb norms.
void t32_dp_reg(Insn &in, const Fields &f) { // shifted register: imm3:imm2, type
  in.op2 = Op2::RegImm;
  in.sht = uint8_t(f['y']);
  in.sha = uint8_t(f['i']);
  if (in.sub >= TST && in.sub <= CMN) in.d = -1, in.S = true;
  if (in.sub == MOV || in.sub == MVN) in.n = -1;
}
void t32_dp_imm(Insn &in, const Fields &f) { // modified immediate
  in.op2 = Op2::Imm;
  in.imm = thumb_expand_imm(f['i'], &in.imm_c);
  if (in.sub >= TST && in.sub <= CMN) in.d = -1, in.S = true;
  if (in.sub == MOV || in.sub == MVN) in.n = -1;
}
void t32_imm12(Insn &in, const Fields &f) { // ADDW/SUBW: zero-extended imm12
  in.op2 = Op2::Imm;
  in.imm = f['i'];
}
void t32_adr(Insn &in, const Fields &f) {
  uint32_t base = (in.addr + 4) & ~3u;
  in.target = in.sub ? base - f['i'] : base + f['i'];
}
void t32_bfx(Insn &in, const Fields &f) {
  in.lsb = f['i'];
  in.width = f['w'] + 1;
}
void t32_bfi(Insn &in, const Fields &f) {
  in.lsb = f['i'];
  in.width = f['b']; // msb
}
void t32_sat(Insn &in, const Fields &f) {
  in.width = f['b'];
  in.op2 = Op2::RegImm;
  in.sht = uint8_t(f['y'] << 1);
  in.sha = uint8_t(f['i']);
  in.m = in.n;
}
void t32_branch(Insn &in, const Fields &f) { // B/BL/BLX T4 form: S:I1:I2:imm10:imm11
  uint32_t S = f['S'], j1 = f['j'] >> 1, j2 = f['j'] & 1;
  uint32_t i1 = !(j1 ^ S), i2 = !(j2 ^ S);
  uint32_t imm = S << 24 | i1 << 23 | i2 << 22 | f['i'] << 1;
  int32_t off = sext(imm, 25);
  in.S = false;
  if (in.sub == 2) { // BLX: to ARM, Align(PC, 4)
    in.target = ((in.addr + 4) & ~3u) + uint32_t(off);
    in.target &= ~3u;
  } else {
    in.target = (in.addr + 4 + uint32_t(off)) | 1;
  }
  in.sub = 0;
}
void t32_bcond(Insn &in, const Fields &f) { // T3: S:J2:J1:imm6:imm11
  uint32_t S = f['S'], j1 = f['j'] >> 1, j2 = f['j'] & 1;
  uint32_t imm = S << 20 | j2 << 19 | j1 << 18 | f['i'] << 1;
  in.S = false;
  in.cond = f['c'];
  in.target = (in.addr + 4 + uint32_t(sext(imm, 21))) | 1;
}
void t32_ldst_imm12(Insn &in, const Fields &f) {
  in.op2 = Op2::Imm;
  in.imm = f['i'];
}
void t32_ldst_imm8(Insn &in, const Fields &f) {
  in.op2 = Op2::Imm;
  in.imm = f['i'];
  if (!in.P && !in.W) in.kind = Kind::Undef;
}
void t32_ldst_lit(Insn &in, const Fields &f) {
  in.n = 15;
  in.op2 = Op2::Imm;
  in.imm = f['i'];
}
void t32_ldst_reg(Insn &in, const Fields &f) {
  in.op2 = Op2::RegImm;
  in.sht = 0;
  in.sha = uint8_t(f['i']);
}
void t32_dual(Insn &in, const Fields &f) {
  in.op2 = Op2::Imm;
  in.imm = f['i'] * 4;
}
void t32_ldm(Insn &in, const Fields &) {
  in.P = in.def->sub == 1; // DB
  in.U = !in.P;
}
void t32_rot(Insn &in, const Fields &f) { in.rot = f['r'] * 8; }
void t32_ex(Insn &in, const Fields &f) { in.imm = f['i'] * 4; }
void t32_half(Insn &in, const Fields &f) { in.rot = f['X'] << 1 | f['Y']; }
void t32_shift_reg(Insn &in, const Fields &) { // LSL.W etc: MOV Rd, Rm <shift> Rs
  in.op2 = Op2::RegReg;
  in.sht = uint8_t(in.sub);
  in.sub = MOV;
}
void t32_mrs_msr(Insn &in, const Fields &f) { in.lsb = f['k']; }
void t32_pkh(Insn &in, const Fields &f) {
  in.op2 = Op2::RegImm;
  in.sht = uint8_t(in.def->sub ? 2 : 0);
  in.sha = uint8_t(f['i']);
}

void build_thumb32() {
  // Load/store multiple, dual, exclusive, table branch.
  row("stm", "1110100010W0nnnn0RRRRRRRRRRRRRRR", Kind::Stm, 0, t32_ldm);
  row("ldm", "1110100010W1nnnnRRRRRRRRRRRRRRRR", Kind::Ldm, 0, t32_ldm);
  row("stmdb", "1110100100W0nnnn0RRRRRRRRRRRRRRR", Kind::Stm, 1, t32_ldm);
  row("ldmdb", "1110100100W1nnnnRRRRRRRRRRRRRRRR", Kind::Ldm, 1, t32_ldm);
  row("strex", "111010000100nnnnttttddddiiiiiiii", Kind::Strex, 2, t32_ex);
  row("ldrex", "111010000101nnnntttt1111iiiiiiii", Kind::Ldrex, 2, t32_ex);
  row("strexb", "111010001100nnnntttt11110100dddd", Kind::Strex, 0);
  row("strexh", "111010001100nnnntttt11110101dddd", Kind::Strex, 1);
  row("strexd", "111010001100nnnnttttTTTT0111dddd", Kind::Strex, 3);
  row("tbb", "111010001101nnnn111100000000mmmm", Kind::Tb, 0);
  row("tbh", "111010001101nnnn111100000001mmmm", Kind::Tb, 1);
  row("ldrexb", "111010001101nnnntttt111101001111", Kind::Ldrex, 0);
  row("ldrexh", "111010001101nnnntttt111101011111", Kind::Ldrex, 1);
  row("ldrexd", "111010001101nnnnttttTTTT01111111", Kind::Ldrex, 3);
  row("strd", "11101000U110nnnnttttTTTTiiiiiiii", Kind::LdSt, STRD,
      [](Insn &in, const Fields &f) { t32_dual(in, f), in.P = false, in.W = true; });
  row("strd", "11101001U1W0nnnnttttTTTTiiiiiiii", Kind::LdSt, STRD, t32_dual);
  row("ldrd", "11101000U111nnnnttttTTTTiiiiiiii", Kind::LdSt, LDRD,
      [](Insn &in, const Fields &f) { t32_dual(in, f), in.P = false, in.W = true; });
  row("ldrd", "11101001U1W1nnnnttttTTTTiiiiiiii", Kind::LdSt, LDRD, t32_dual);

  // Data processing, shifted register.
  static const struct {
    const char *name;
    const char *op;
    int dp;
  } dps[] = {{"tst", "0000", TST}, {"and", "0000", AND}, {"bic", "0001", BIC}, {"mov", "0010", MOV},
             {"orr", "0010", ORR}, {"mvn", "0011", MVN}, {"orn", "0011", ORN}, {"teq", "0100", TEQ},
             {"eor", "0100", EOR}, {"cmn", "1000", CMN}, {"add", "1000", ADD}, {"adc", "1010", ADC},
             {"sbc", "1011", SBC}, {"cmp", "1101", CMP}, {"sub", "1101", SUB}, {"rsb", "1110", RSB}};
  for (const auto &e : dps) {
    bool cmp = e.dp >= TST && e.dp <= CMN, nrn = e.dp == MOV || e.dp == MVN;
    std::string s = cmp ? "1" : "S", n = nrn ? "1111" : "nnnn", d = cmp ? "1111" : "dddd";
    row(e.name, std::string("1110101") + e.op + s + n + "0iii" + d + "iiyymmmm", Kind::Dp, e.dp, t32_dp_reg);
  }
  row("pkhbt", "111010101100nnnn0iiiddddii00mmmm", Kind::Pkh, 0, t32_pkh);
  row("pkhtb", "111010101100nnnn0iiiddddii10mmmm", Kind::Pkh, 1, t32_pkh);

  // Data processing, modified immediate.
  for (const auto &e : dps) {
    bool cmp = e.dp >= TST && e.dp <= CMN, nrn = e.dp == MOV || e.dp == MVN;
    std::string s = cmp ? "1" : "S", n = nrn ? "1111" : "nnnn", d = cmp ? "1111" : "dddd";
    row(e.name, std::string("11110i0") + e.op + s + n + "0iii" + d + "iiiiiiii", Kind::Dp, e.dp, t32_dp_imm);
  }

  // Data processing, plain binary immediate.
  row("adr", "11110i10000011110iiiddddiiiiiiii", Kind::Adr, 0, t32_adr);
  row("addw", "11110i100000nnnn0iiiddddiiiiiiii", Kind::Dp, ADD, t32_imm12);
  row("movw", "11110k100100kkkk0iiiddddiiiiiiii", Kind::MovW, 0,
      [](Insn &in, const Fields &f) { in.imm = (f['k'] & 0xf) << 12 | (f['k'] >> 4) << 11 | f['i']; });
  row("adr", "11110i10101011110iiiddddiiiiiiii", Kind::Adr, 1, t32_adr);
  row("subw", "11110i101010nnnn0iiiddddiiiiiiii", Kind::Dp, SUB, t32_imm12);
  row("movt", "11110k101100kkkk0iiiddddiiiiiiii", Kind::MovT, 0,
      [](Insn &in, const Fields &f) { in.imm = (f['k'] & 0xf) << 12 | (f['k'] >> 4) << 11 | f['i']; });
  row("ssat16", "111100110010nnnn0000dddd0000bbbb", Kind::Sat16, 1,
      [](Insn &in, const Fields &f) { in.width = f['b']; });
  row("usat16", "111100111010nnnn0000dddd0000bbbb", Kind::Sat16, 0,
      [](Insn &in, const Fields &f) { in.width = f['b']; });
  row("ssat", "1111001100y0nnnn0iiiddddii0bbbbb", Kind::Sat, 1, t32_sat);
  row("usat", "1111001110y0nnnn0iiiddddii0bbbbb", Kind::Sat, 0, t32_sat);
  row("sbfx", "111100110100nnnn0iiiddddii0wwwww", Kind::Bfx, 1, t32_bfx);
  row("bfc", "11110011011011110iiiddddii0bbbbb", Kind::Bfc, 0, t32_bfi);
  row("bfi", "111100110110nnnn0iiiddddii0bbbbb", Kind::Bfi, 0, t32_bfi);
  row("ubfx", "111100111100nnnn0iiiddddii0wwwww", Kind::Bfx, 0, t32_bfx);

  // Branches and miscellaneous control.
  row("msr", "11110011100xnnnn10x0kk0000000000", Kind::Msr, 0, t32_mrs_msr);
  row("nop", "111100111010111110x0x000xxxxxxxx", Kind::Nop);
  row("clrex", "111100111011xxxx10x0xxxx0010xxxx", Kind::Clrex);
  row("dsb", "111100111011xxxx10x0xxxx0100xxxx", Kind::Barrier);
  row("dmb", "111100111011xxxx10x0xxxx0101xxxx", Kind::Barrier);
  row("isb", "111100111011xxxx10x0xxxx0110xxxx", Kind::Barrier);
  row("mrs", "11110011111x111110x0dddd00000000", Kind::Mrs);
  row("udf", "111101111111xxxx1010xxxxxxxxxxxx", Kind::Undef);
  row("bl", "11110Siiiiiiiiii11j1jiiiiiiiiiii", Kind::Bl, 1, t32_branch);
  row("blx", "11110Siiiiiiiiii11j0jiiiiiiiiiii", Kind::Bl, 2, t32_branch);
  row("b", "11110Siiiiiiiiii10j1jiiiiiiiiiii", Kind::B, 0, t32_branch);
  row("udf", "11110x111xxxxxxx10x0xxxxxxxxxxxx", Kind::Undef);
  row("b", "11110Scccciiiiii10j0jiiiiiiiiiii", Kind::B, 0, t32_bcond);

  // Preloads (Rt = 1111 in the byte/halfword load space).
  row("pld", "11111000U0x111111111iiiiiiiiiiii", Kind::Nop);
  row("pld", "1111100000x1nnnn1111000000iimmmm", Kind::Nop);
  row("pld", "1111100000x1nnnn11111100iiiiiiii", Kind::Nop);
  row("pld", "1111100010x1nnnn1111iiiiiiiiiiii", Kind::Nop);
  row("pli", "11111001U00111111111iiiiiiiiiiii", Kind::Nop);
  row("pli", "111110010001nnnn1111000000iimmmm", Kind::Nop);
  row("pli", "111110010001nnnn11111100iiiiiiii", Kind::Nop);
  row("pli", "111110011001nnnn1111iiiiiiiiiiii", Kind::Nop);
  row("nop", "11111001x01111111111xxxxxxxxxxxx", Kind::Nop);
  row("nop", "111110010011xxxx1111000000xxxxxx", Kind::Nop);
  row("nop", "111110010011xxxx11111100xxxxxxxx", Kind::Nop);
  row("nop", "111110011011xxxx1111xxxxxxxxxxxx", Kind::Nop);

  // Single loads and stores: literal, imm12, imm8 (P/U/W), register.
  static const struct {
    const char *name;
    const char *bits; // bit 24 (signed), 22:21 (size), 20 (load)
    int op;
  } ls[] = {{"strb", "0000", STRB}, {"strh", "0010", STRH}, {"str", "0100", STR},     {"ldrb", "0001", LDRB},
            {"ldrh", "0011", LDRH}, {"ldr", "0101", LDR},   {"ldrsb", "1001", LDRSB}, {"ldrsh", "1011", LDRSH}};
  for (const auto &e : ls) {
    std::string sgn(1, e.bits[0]), sz = std::string(e.bits + 1, 2), L(1, e.bits[3]);
    if (L == "1") row(e.name, "1111100" + sgn + "U" + sz + "11111ttttiiiiiiiiiiii", Kind::LdSt, e.op, t32_ldst_lit);
    row(e.name, "1111100" + sgn + "1" + sz + L + "nnnnttttiiiiiiiiiiii", Kind::LdSt, e.op, t32_ldst_imm12);
    row(e.name, "1111100" + sgn + "0" + sz + L + "nnnntttt1PUWiiiiiiii", Kind::LdSt, e.op, t32_ldst_imm8);
    row(e.name, "1111100" + sgn + "0" + sz + L + "nnnntttt000000iimmmm", Kind::LdSt, e.op, t32_ldst_reg);
  }

  // Data processing, register.
  row("lsl", "11111010000Smmmm1111dddd0000ssss", Kind::Dp, 0, t32_shift_reg);
  row("lsr", "11111010001Smmmm1111dddd0000ssss", Kind::Dp, 1, t32_shift_reg);
  row("asr", "11111010010Smmmm1111dddd0000ssss", Kind::Dp, 2, t32_shift_reg);
  row("ror", "11111010011Smmmm1111dddd0000ssss", Kind::Dp, 3, t32_shift_reg);
  static const struct {
    const char *name;
    const char *op;
    int ext;
  } exts[] = {{"sxth", "000", SXTH}, {"uxth", "001", UXTH}, {"sxtb16", "010", SXTB16},
              {"uxtb16", "011", UXTB16}, {"sxtb", "100", SXTB}, {"uxtb", "101", UXTB}};
  for (const auto &e : exts)
    row(e.name, std::string("111110100") + e.op + "nnnn1111dddd10rrmmmm", Kind::Extend, e.ext, t32_rot);
  row("parallel", "111110101ooonnnn1111dddd0pppmmmm", Kind::Parallel, 0, [](Insn &in, const Fields &f) {
    // Thumb op1 (22:20) / op2 (6:4) map to ARM's op (sub-op) and prefix.
    static const int8_t aop[8] = {4, 0, 1, -1, 7, 3, 2, -1}; // ADD8 ADD16 ASX . SUB8 SUB16 SAX
    static const int8_t apre[8] = {1, 2, 3, -1, 5, 6, 7, -1};
    int o = aop[f['o']], p = apre[f['p']];
    if (o < 0 || p < 0) in.kind = Kind::Undef;
    in.sub = p * 8 + o;
  });
  row("qadd", "111110101000nnnn1111dddd1000mmmm", Kind::QArith, QADD);
  row("qdadd", "111110101000nnnn1111dddd1001mmmm", Kind::QArith, QDADD);
  row("qsub", "111110101000nnnn1111dddd1010mmmm", Kind::QArith, QSUB);
  row("qdsub", "111110101000nnnn1111dddd1011mmmm", Kind::QArith, QDSUB);
  row("rev", "111110101001xxxx1111dddd1000mmmm", Kind::Rev, REV);
  row("rev16", "111110101001xxxx1111dddd1001mmmm", Kind::Rev, REV16);
  row("rbit", "111110101001xxxx1111dddd1010mmmm", Kind::Rev, RBIT);
  row("revsh", "111110101001xxxx1111dddd1011mmmm", Kind::Rev, REVSH);
  row("sel", "111110101010nnnn1111dddd1000mmmm", Kind::Sel);
  row("clz", "111110101011xxxx1111dddd1000mmmm", Kind::Clz);

  // Multiply, multiply-accumulate, divide.
  row("mul", "111110110000nnnn1111dddd0000mmmm", Kind::Mul, MUL);
  row("mla", "111110110000nnnnaaaadddd0000mmmm", Kind::Mul, MLA);
  row("mls", "111110110000nnnnaaaadddd0001mmmm", Kind::Mul, MLS);
  row("smulxy", "111110110001nnnn1111dddd00XYmmmm", Kind::MulHalf, SMULXY, t32_half);
  row("smlaxy", "111110110001nnnnaaaadddd00XYmmmm", Kind::MulHalf, SMLAXY, t32_half);
  row("smuad", "111110110010nnnn1111dddd000Xmmmm", Kind::MulDual, SMUAD, a_xflag);
  row("smlad", "111110110010nnnnaaaadddd000Xmmmm", Kind::MulDual, SMLAD, a_xflag);
  row("smulwy", "111110110011nnnn1111dddd000Ymmmm", Kind::MulHalf, SMULWY, t32_half);
  row("smlawy", "111110110011nnnnaaaadddd000Ymmmm", Kind::MulHalf, SMLAWY, t32_half);
  row("smusd", "111110110100nnnn1111dddd000Xmmmm", Kind::MulDual, SMUSD, a_xflag);
  row("smlsd", "111110110100nnnnaaaadddd000Xmmmm", Kind::MulDual, SMLSD, a_xflag);
  row("smmul", "111110110101nnnn1111dddd000Xmmmm", Kind::MulHi, 0, a_xflag);
  row("smmla", "111110110101nnnnaaaadddd000Xmmmm", Kind::MulHi, 1, a_xflag);
  row("smmls", "111110110110nnnnaaaadddd000Xmmmm", Kind::MulHi, 2, a_xflag);
  row("usad8", "111110110111nnnn1111dddd0000mmmm", Kind::Usad8);
  row("usada8", "111110110111nnnnaaaadddd0000mmmm", Kind::Usad8);
  row("smull", "111110111000nnnnllllhhhh0000mmmm", Kind::Mul, SMULL, n_long);
  row("sdiv", "111110111001nnnn1111dddd1111mmmm", Kind::Div, 1);
  row("umull", "111110111010nnnnllllhhhh0000mmmm", Kind::Mul, UMULL, n_long);
  row("udiv", "111110111011nnnn1111dddd1111mmmm", Kind::Div, 0);
  row("smlal", "111110111100nnnnllllhhhh0000mmmm", Kind::Mul, SMLAL, n_long);
  row("smlalxy", "111110111100nnnnllllhhhh10XYmmmm", Kind::MulHalf, SMLALXY,
      [](Insn &in, const Fields &f) { n_long(in, f), t32_half(in, f); });
  row("smlald", "111110111100nnnnllllhhhh110Xmmmm", Kind::MulDual, SMLALD, a_dual_long);
  row("smlsld", "111110111101nnnnllllhhhh110Xmmmm", Kind::MulDual, SMLSLD, a_dual_long);
  row("umlal", "111110111110nnnnllllhhhh0000mmmm", Kind::Mul, UMLAL, n_long);
  row("umaal", "111110111110nnnnllllhhhh0110mmmm", Kind::Mul, UMAAL, n_long);
}

// ---- VFP and coprocessor (ARM form) --------------------------------------

uint8_t sreg(uint32_t hi4, uint32_t lo1) { return uint8_t(hi4 << 1 | lo1); }
uint8_t dreg(uint32_t hi1, uint32_t lo4) { return uint8_t(hi1 << 4 | lo4); }

void v_dnm(Insn &in, const Fields &f) {
  in.dp = f['z'];
  in.vd = in.dp ? dreg(f['D'], f['d']) : sreg(f['d'], f['D']);
  in.vn = in.dp ? dreg(f['N'], f['n']) : sreg(f['n'], f['N']);
  in.vm = in.dp ? dreg(f['M'], f['m']) : sreg(f['m'], f['M']);
  in.d = in.n = in.m = -1;
}
void v_ldst(Insn &in, const Fields &f) {
  in.dp = f['z'];
  in.vd = in.dp ? dreg(f['D'], f['d']) : sreg(f['d'], f['D']);
  in.imm = f['i'] * 4;
  in.d = -1;
}
void v_ldm(Insn &in, const Fields &f) {
  in.dp = f['z'];
  in.vd = in.dp ? dreg(f['D'], f['d']) : sreg(f['d'], f['D']);
  in.imm = in.dp ? f['i'] / 2 : f['i'];
  in.d = -1;
  if (in.P == in.U && in.W) in.kind = Kind::Undef;
  if (in.P && !in.W) in.kind = Kind::Undef; // VLDR space, matched earlier
  if (in.imm == 0 || in.vd + in.imm > 32u) in.kind = Kind::Undef;
}
void v_vmovimm(Insn &in, const Fields &f) {
  v_dnm(in, f);
  uint32_t imm8 = f['i'];
  uint32_t b6 = imm8 >> 6 & 1, sign = imm8 >> 7;
  if (in.dp) {
    uint64_t exp = (uint64_t(!b6) << 10) | (b6 ? 0x3fcull : 0) | ((imm8 >> 4) & 3);
    uint64_t v = uint64_t(sign) << 63 | exp << 52 | uint64_t(imm8 & 15) << 48;
    in.x1 = uint32_t(v);
    in.x2 = uint32_t(v >> 32);
  } else {
    uint32_t exp = (uint32_t(!b6) << 7) | (b6 ? 0x7cu : 0) | ((imm8 >> 4) & 3);
    in.imm = sign << 31 | exp << 23 | (imm8 & 15) << 19;
  }
}
void v_cvt_f2f(Insn &in, const Fields &f) { // dp = source is double
  in.dp = f['z'];
  in.vd = in.dp ? sreg(f['d'], f['D']) : dreg(f['D'], f['d']);
  in.vm = in.dp ? dreg(f['M'], f['m']) : sreg(f['m'], f['M']);
}
void v_cvt_i2f(Insn &in, const Fields &f) { // dp = dest double; source S reg; rot = signed
  in.dp = f['z'];
  in.vd = in.dp ? dreg(f['D'], f['d']) : sreg(f['d'], f['D']);
  in.vm = sreg(f['m'], f['M']);
  in.rot = f['o'];
}
void v_cvt_f2i(Insn &in, const Fields &f) { // dp = source double; dest S reg; rot = signed, width = round-zero
  in.dp = f['z'];
  in.vd = sreg(f['d'], f['D']);
  in.vm = in.dp ? dreg(f['M'], f['m']) : sreg(f['m'], f['M']);
  in.rot = f['o'];
  in.width = f['r'];
}
void v_cvt_fixed(Insn &in, const Fields &f) {
  in.dp = f['z'];
  in.vd = in.dp ? dreg(f['D'], f['d']) : sreg(f['d'], f['D']);
  uint32_t size = f['X'] ? 32 : 16;
  in.esize = uint8_t(size);
  uint32_t imm = f['i'] << 1 | f['k']; // imm4:i
  in.imm = size - imm;                  // fraction bits
  in.rot = !f['U'];    // signed
  in.sub = int(f['o']); // 1: to fixed
  if (int(size) - int(imm) < 0) in.kind = Kind::Undef;
}

void build_vfp() {
  row("vmla", "cccc11100D00nnnndddd101zN0M0mmmm", Kind::VArith3, VMLA, v_dnm);
  row("vmls", "cccc11100D00nnnndddd101zN1M0mmmm", Kind::VArith3, VMLS, v_dnm);
  row("vnmls", "cccc11100D01nnnndddd101zN0M0mmmm", Kind::VArith3, VNMLS, v_dnm);
  row("vnmla", "cccc11100D01nnnndddd101zN1M0mmmm", Kind::VArith3, VNMLA, v_dnm);
  row("vmul", "cccc11100D10nnnndddd101zN0M0mmmm", Kind::VArith3, VMUL, v_dnm);
  row("vnmul", "cccc11100D10nnnndddd101zN1M0mmmm", Kind::VArith3, VNMUL, v_dnm);
  row("vadd", "cccc11100D11nnnndddd101zN0M0mmmm", Kind::VArith3, VADD, v_dnm);
  row("vsub", "cccc11100D11nnnndddd101zN1M0mmmm", Kind::VArith3, VSUB, v_dnm);
  row("vdiv", "cccc11101D00nnnndddd101zN0M0mmmm", Kind::VArith3, VDIV, v_dnm);
  row("vfnms", "cccc11101D01nnnndddd101zN0M0mmmm", Kind::VArith3, VFNMS, v_dnm);
  row("vfnma", "cccc11101D01nnnndddd101zN1M0mmmm", Kind::VArith3, VFNMA, v_dnm);
  row("vfma", "cccc11101D10nnnndddd101zN0M0mmmm", Kind::VArith3, VFMA, v_dnm);
  row("vfms", "cccc11101D10nnnndddd101zN1M0mmmm", Kind::VArith3, VFMS, v_dnm);
  row("vmov", "cccc11101D11iiiidddd101z0000iiii", Kind::VMovImm, 0, v_vmovimm);
  row("vmov", "cccc11101D110000dddd101z01M0mmmm", Kind::VArith2, VMOV, v_dnm);
  row("vabs", "cccc11101D110000dddd101z11M0mmmm", Kind::VArith2, VABS, v_dnm);
  row("vneg", "cccc11101D110001dddd101z01M0mmmm", Kind::VArith2, VNEG, v_dnm);
  row("vsqrt", "cccc11101D110001dddd101z11M0mmmm", Kind::VArith2, VSQRT, v_dnm);
  row("vcmp", "cccc11101D110100dddd101zE1M0mmmm", Kind::VCmp, 0,
      [](Insn &in, const Fields &f) { v_dnm(in, f), in.rot = f['E']; });
  row("vcmp", "cccc11101D110101dddd101zE1000000", Kind::VCmp, 1,
      [](Insn &in, const Fields &f) { v_dnm(in, f), in.rot = f['E']; });
  row("vcvt", "cccc11101D110111dddd101z11M0mmmm", Kind::VCvt, CVT_F2F, v_cvt_f2f);
  row("vcvt", "cccc11101D111000dddd101zo1M0mmmm", Kind::VCvt, CVT_I2F, v_cvt_i2f);
  row("vcvt", "cccc11101D111o1Udddd101zX1k0iiii", Kind::VCvtFixed, 0, v_cvt_fixed);
  row("vcvt", "cccc11101D11110odddd101zr1M0mmmm", Kind::VCvt, CVT_F2I, v_cvt_f2i);
  row("vmov", "cccc11100000nnnntttt1010N0010000", Kind::VMovS, 0,
      [](Insn &in, const Fields &f) { in.vn = sreg(f['n'], f['N']), in.n = -1; });
  row("vmov", "cccc11100001nnnntttt1010N0010000", Kind::VMovS, 1,
      [](Insn &in, const Fields &f) { in.vn = sreg(f['n'], f['N']), in.n = -1; });
  row("vmov", "cccc1100010oTTTTtttt101z00M1mmmm", Kind::VMov2, 0, [](Insn &in, const Fields &f) {
    in.sub = int(f['o']);
    in.dp = f['z'];
    in.vm = in.dp ? dreg(f['M'], f['m']) : sreg(f['m'], f['M']);
    in.m = -1;
  });
  row("vmov", "cccc111000i0ddddtttt1011D0010000", Kind::VMovScalar, 0, [](Insn &in, const Fields &f) {
    in.vd = dreg(f['D'], f['d']), in.esize = 32, in.imm = f['i'], in.d = -1;
  });
  row("vmov", "cccc111000i0ddddtttt1011Di110000", Kind::VMovScalar, 0, [](Insn &in, const Fields &f) {
    in.vd = dreg(f['D'], f['d']), in.esize = 16, in.imm = f['i'], in.d = -1;
  });
  row("vmov", "cccc111001i0ddddtttt1011Dii10000", Kind::VMovScalar, 0, [](Insn &in, const Fields &f) {
    in.vd = dreg(f['D'], f['d']), in.esize = 8, in.imm = f['i'], in.d = -1;
  });
  row("vmov", "cccc111000i1nnnntttt1011N0010000", Kind::VMovScalar, 1, [](Insn &in, const Fields &f) {
    in.vd = dreg(f['N'], f['n']), in.esize = 32, in.imm = f['i'], in.n = -1;
  });
  row("vmov", "cccc1110U0i1nnnntttt1011Ni110000", Kind::VMovScalar, 1, [](Insn &in, const Fields &f) {
    in.vd = dreg(f['N'], f['n']), in.esize = 16, in.imm = f['i'], in.n = -1;
  });
  row("vmov", "cccc1110U1i1nnnntttt1011Nii10000", Kind::VMovScalar, 1, [](Insn &in, const Fields &f) {
    in.vd = dreg(f['N'], f['n']), in.esize = 8, in.imm = f['i'], in.n = -1;
  });
  row("vdup", "cccc11101BQ0ddddtttt1011D0E10000", Kind::VDup, 0, [](Insn &in, const Fields &f) {
    in.vd = dreg(f['D'], f['d']);
    in.Q = f['Q'];
    uint32_t be = f['B'] << 1 | f['E'];
    in.esize = be == 0 ? 32 : be == 1 ? 16 : 8;
    in.d = -1;
    if (be == 3 || (in.Q && (in.vd & 1))) in.kind = Kind::Undef;
  });
  row("vmsr", "cccc111011100001tttt101000010000", Kind::VMsr);
  row("vmrs", "cccc111011110001tttt101000010000", Kind::VMrs);
  row("vldr", "cccc1101UD01nnnndddd101ziiiiiiii", Kind::VLdSt, 1, v_ldst);
  row("vstr", "cccc1101UD00nnnndddd101ziiiiiiii", Kind::VLdSt, 0, v_ldst);
  row("vldm", "cccc110PUDW1nnnndddd101ziiiiiiii", Kind::VLdm, 1, v_ldm);
  row("vstm", "cccc110PUDW0nnnndddd101ziiiiiiii", Kind::VLdm, 0, v_ldm);
  row("mrc", "cccc1110ooo1NNNNttttppppooo1MMMM", Kind::Mrc, 0, a_cp);
  row("mcr", "cccc1110ooo0NNNNttttppppooo1MMMM", Kind::Mcr, 0, a_cp);
}

// ---- Advanced SIMD (ARM form) ---------------------------------------------

// D:Vd, N:Vn, M:Vm as D register numbers; Q (when the row has it).
void n_dnm(Insn &in, const Fields &f) {
  in.vd = dreg(f['D'], f['d']);
  in.vn = dreg(f['N'], f['n']);
  in.vm = dreg(f['M'], f['m']);
  in.Q = f['Q'];
  in.d = in.n = in.m = -1;
  in.esize = uint8_t(8u << f['z']);
  in.dp = true;
  if (in.Q && ((in.vd | in.vn | in.vm) & 1)) in.kind = Kind::Undef;
}
void n_scalar(Insn &in, const Fields &f) { // f32 by scalar: Vm<3:0> is the register, M the index
  in.vd = dreg(f['D'], f['d']);
  in.vn = dreg(f['N'], f['n']);
  in.vm = uint8_t(f['m']);
  in.imm = f['M'];
  in.Q = f['Q'];
  in.d = in.n = in.m = -1;
  in.esize = 32;
  if (in.Q && ((in.vd | in.vn) & 1)) in.kind = Kind::Undef;
}
// AdvSIMDExpandImm: the 64-bit immediate of VMOV/VMVN (immediate).
void n_movi(Insn &in, const Fields &f) {
  uint32_t imm8 = f['i'] << 7 | f['k'] << 4 | f['j'], cmode = f['c'], op = f['o'];
  in.vd = dreg(f['D'], f['d']);
  in.Q = f['Q'];
  in.d = -1;
  if (in.Q && (in.vd & 1)) in.kind = Kind::Undef;
  if ((cmode & 1) && cmode < 12) { // VORR/VBIC (immediate)
    in.sub = -1;
    return;
  }
  uint64_t v = 0;
  auto rep32 = [](uint32_t x) { return uint64_t(x) << 32 | x; };
  switch (cmode >> 1) {
  case 0: v = rep32(imm8); break;
  case 1: v = rep32(imm8 << 8); break;
  case 2: v = rep32(imm8 << 16); break;
  case 3: v = rep32(imm8 << 24); break;
  case 4: v = rep32(imm8 << 16 | imm8); break;
  case 5: v = rep32(imm8 << 24 | imm8 << 8); break;
  case 6: v = rep32((cmode & 1) ? (imm8 << 16 | 0xffff) : (imm8 << 8 | 0xff)); break;
  default:
    if (!(cmode & 1) && !op) {
      v = imm8 * 0x0101010101010101ull;
    } else if (!(cmode & 1) && op) {
      for (int b = 0; b < 8; b++)
        if ((imm8 >> b) & 1) v |= 0xffull << (8 * b);
    } else if (!op) {
      uint32_t b6 = imm8 >> 6 & 1;
      uint32_t x = (imm8 >> 7) << 31 | (b6 ? 0x3e000000u : 0x40000000u) | (imm8 & 0x3f) << 19;
      v = rep32(x);
    } else {
      in.kind = Kind::Undef;
    }
  }
  if (op && cmode != 14) v = ~v; // VMVN
  in.x1 = uint32_t(v);
  in.x2 = uint32_t(v >> 32);
}
void n_ldst(Insn &in, const Fields &f) {
  static const int8_t structs[16] = {4, 4, 1, 2, 3, 3, 1, 1, 2, 2, 1, -1, -1, -1, -1, -1};
  static const int8_t regs[16] = {1, 1, 4, 2, 1, 1, 3, 1, 1, 1, 2, -1, -1, -1, -1, -1};
  static const int8_t inc[16] = {1, 2, 1, 2, 1, 2, 1, 1, 1, 2, 1, -1, -1, -1, -1, -1};
  uint32_t type = f['t'];
  in.vd = dreg(f['D'], f['d']);
  in.width = f['L'];
  in.esize = uint8_t(8u << f['z']);
  in.rot = uint32_t(structs[type] < 0 ? 0 : structs[type]);
  in.x1 = uint32_t(regs[type] < 0 ? 0 : regs[type]);
  in.x2 = uint32_t(inc[type] < 0 ? 0 : inc[type]);
  in.d = -1;
  if (structs[type] < 0) in.kind = Kind::Undef;
  else if (in.rot > 2) in.sub = -1; // VLD3/VLD4: not yet
  if (in.vd + (in.rot == 1 ? in.x1 : in.x2 + in.x1) > 32) in.kind = Kind::Undef;
}

void build_neon() {
  // Float, vector (size bit 20 = 0: f32).
  row("vadd", "111100100D00nnnndddd1101NQM0mmmm", Kind::Neon, N_FADD, n_dnm);
  row("vsub", "111100100D10nnnndddd1101NQM0mmmm", Kind::Neon, N_FSUB, n_dnm);
  row("vmla", "111100100D00nnnndddd1101NQM1mmmm", Kind::Neon, N_FMLA, n_dnm);
  row("vmls", "111100100D10nnnndddd1101NQM1mmmm", Kind::Neon, N_FMLS, n_dnm);
  row("vmul", "111100110D00nnnndddd1101NQM1mmmm", Kind::Neon, N_FMUL, n_dnm);
  // Float by scalar (size 10).
  row("vmla", "1111001Q1D10nnnndddd0001N1M0mmmm", Kind::Neon, N_FMLA_S, n_scalar);
  row("vmls", "1111001Q1D10nnnndddd0101N1M0mmmm", Kind::Neon, N_FMLS_S, n_scalar);
  row("vmul", "1111001Q1D10nnnndddd1001N1M0mmmm", Kind::Neon, N_FMUL_S, n_scalar);
  // Bitwise.
  row("vlogic", "111100100Doonnnndddd0001NQM1mmmm", Kind::Neon, N_LOGIC,
      [](Insn &in, const Fields &f) { n_dnm(in, f), in.x1 = f['o']; });
  row("vlogic", "111100110Doonnnndddd0001NQM1mmmm", Kind::Neon, N_LOGIC,
      [](Insn &in, const Fields &f) { n_dnm(in, f), in.x1 = 4 + f['o']; });
  // Integer add/subtract.
  row("vadd", "111100100Dzznnnndddd1000NQM0mmmm", Kind::Neon, N_IADD, n_dnm);
  row("vsub", "111100110Dzznnnndddd1000NQM0mmmm", Kind::Neon, N_ISUB, n_dnm);
  // VDUP (scalar), VCVT between f32 and integers.
  row("vdup", "111100111D11iiiidddd11000QM0mmmm", Kind::Neon, N_DUP_S, [](Insn &in, const Fields &f) {
    n_dnm(in, f);
    in.kind = in.Q && (in.vd & 1) ? Kind::Undef : Kind::Neon; // Vm is a D register here
    uint32_t imm4 = f['i'];
    if (imm4 & 1) in.esize = 8, in.imm = imm4 >> 1;
    else if (imm4 & 2) in.esize = 16, in.imm = imm4 >> 2;
    else if (imm4 & 4) in.esize = 32, in.imm = imm4 >> 3;
    else in.kind = Kind::Undef;
  });
  row("vcvt", "111100111D111011dddd011ooQM0mmmm", Kind::Neon, N_CVT,
      [](Insn &in, const Fields &f) { n_dnm(in, f), in.x1 = f['o'], in.esize = 32; });
  // VMOV/VMVN (immediate) and the VORR/VBIC (immediate) forms in that space.
  row("vmov", "1111001i1D000kkkddddcccc0Qo1jjjj", Kind::Neon, N_MOVI, n_movi);
  // Element and structure loads/stores, multiple structures.
  row("vldst", "111101000DL0nnnnddddttttzzaammmm", Kind::Neon, N_LDST, n_ldst);
}

std::vector<InsnDef> build(void (*fn)()) {
  std::vector<InsnDef> t;
  g_build = &t;
  fn();
  g_build = nullptr;
  return t;
}

Insn match(const std::vector<InsnDef> &table, uint32_t addr, uint32_t raw, bool thumb, uint8_t size, bool in_it,
           bool arm_cond) {
  Insn in;
  in.addr = addr;
  in.raw = raw;
  in.thumb = thumb;
  in.size = size;
  in.in_it = in_it;
  bool uncond = arm_cond && (raw >> 28) == 0xf;
  for (const InsnDef &d : table) {
    if ((raw & d.mask) != d.value) continue;
    if (d.has_cond && uncond) continue;
    in.def = &d;
    in.kind = d.kind;
    in.sub = d.sub;
    Fields f = extract(d, raw);
    generic(in, f);
    if (d.norm) d.norm(in, f);
    if (in.kind == Kind::Dp && in.sub >= TST && in.sub <= CMN) in.S = true, in.d = -1; // compares set flags
    break;
  }
  return in;
}

} // namespace

const std::vector<InsnDef> &arm_table() {
  static const std::vector<InsnDef> t = build(build_arm);
  return t;
}
const std::vector<InsnDef> &thumb16_table() {
  static const std::vector<InsnDef> t = build(build_thumb16);
  return t;
}
const std::vector<InsnDef> &thumb32_table() {
  static const std::vector<InsnDef> t = build(build_thumb32);
  return t;
}
const std::vector<InsnDef> &vfp_table() {
  static const std::vector<InsnDef> t = build(build_vfp);
  return t;
}

const std::vector<InsnDef> &neon_table() {
  static const std::vector<InsnDef> t = build(build_neon);
  return t;
}

const char *Insn::name() const { return def ? def->name.c_str() : "?"; }

uint32_t thumb_expand_imm(uint32_t imm12, int8_t *carry) {
  uint32_t imm8 = imm12 & 0xff;
  if ((imm12 >> 10) == 0) {
    *carry = -1;
    switch ((imm12 >> 8) & 3) {
    case 0: return imm8;
    case 1: return imm8 << 16 | imm8;
    case 2: return imm8 << 24 | imm8 << 8;
    default: return imm8 * 0x01010101u;
    }
  }
  uint32_t v = ror32(0x80 | (imm12 & 0x7f), imm12 >> 7);
  *carry = int8_t(v >> 31);
  return v;
}

// ARM-form coprocessor / SIMD words go to the VFP or NEON table.
static Insn decode_cp(uint32_t addr, uint32_t armraw, uint32_t raw, bool thumb, uint8_t size, bool in_it) {
  bool neon = (armraw >> 25) == 0x79 || (armraw >> 24) == 0xf4;
  Insn in = match(neon ? neon_table() : vfp_table(), addr, armraw, thumb, size, in_it, !neon);
  in.raw = raw;
  if (thumb && in.def && in.def->has_cond) in.cond = 14; // IT supplies the condition
  return in;
}

Insn decode_arm(uint32_t addr, uint32_t raw) {
  uint32_t op = (raw >> 25) & 7;
  bool cp = (op == 6 || (op == 7 && !((raw >> 24) & 1))) && (raw >> 28) != 0xf;
  bool neon = (raw >> 25) == 0x79 || (raw >> 24) == 0xf4;
  if (cp || neon) {
    Insn in = decode_cp(addr, raw, raw, false, 4, false);
    if (in.def) return in;
  }
  return match(arm_table(), addr, raw, false, 4, false, true);
}

Insn decode_thumb(uint32_t addr, uint16_t hw1, uint16_t hw2, bool in_it) {
  if (!thumb32(hw1)) return match(thumb16_table(), addr, hw1, true, 2, in_it, false);
  uint32_t raw = uint32_t(hw1) << 16 | hw2;
  if ((hw1 & 0xef00) == 0xef00) // Advanced SIMD data processing: 111U1111 -> 1111001U
    return decode_cp(addr, 0xf2000000u | ((raw >> 4) & 0x01000000u) | (raw & 0x00ffffffu), raw, true, 4, in_it);
  if ((hw1 & 0xff10) == 0xf900) // Advanced SIMD element / structure load/store
    return decode_cp(addr, 0xf4000000u | (raw & 0x00ffffffu), raw, true, 4, in_it);
  if ((hw1 & 0xec00) == 0xec00 && (hw1 >> 12) == 0xe) // coprocessor, cond AL in ARM form
    return decode_cp(addr, 0xe0000000u | (raw & 0x0fffffffu), raw, true, 4, in_it);
  return match(thumb32_table(), addr, raw, true, 4, in_it, false);
}

} // namespace recomp
