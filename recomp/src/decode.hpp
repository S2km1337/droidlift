// Table-driven instruction decoder (decoding approach in
// docs/recompiler.md).
//
// Each table row is a bit pattern written like the encoding diagrams in the
// ARM Architecture Reference Manual: '0'/'1' are fixed bits, 'x' is ignored,
// and every other letter names a field ('c' cond, 'n' Rn, 'd' Rd, ...). Bits
// that share a letter are concatenated most-significant first, so split
// immediates need no special code. The first matching row wins, so more
// specific rows go before general ones.
//
// There are three tables: ARM (32-bit), Thumb (16- and 32-bit patterns) and
// VFP/Advanced SIMD. The VFP/SIMD table is written in ARM form; Thumb
// coprocessor and SIMD encodings are rewritten into that form first (the
// ARM ARM defines them that way).
//
// A row's `norm` function turns the raw fields into the normalized Insn below,
// so the emitter never sees encoding details: it gets registers, a final
// immediate (ARM's rotated immediate and ThumbExpandImm already applied), an
// operand-2 description, addressing bits, an absolute branch target and so
// on. Adding an instruction means adding a row (with a norm if the generic
// one doesn't fit) and, for a new Kind, an emitter case.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace recomp {

enum class Kind : uint8_t {
  Undef,     // decodes, but is UNDEFINED / UNPREDICTABLE here
  Dp,        // data processing; sub = DpOp; operand 2 in op2
  Adr,       // Rd = target (PC-relative address, a constant)
  MovW,
  MovT,
  Nop,       // hints and preloads
  Barrier,   // DMB/DSB/ISB
  Clrex,
  Mul,       // sub = MulOp
  MulHalf,   // SMULxy/SMLAxy/SMULWy/SMLAWy/SMLALxy; sub = HalfOp; x/y in rot
  MulDual,   // SMUAD/SMLAD/SMUSD/SMLSD/SMLALD/SMLSLD; sub = DualOp; X in rot
  MulHi,     // SMMUL/SMMLA/SMMLS; sub = 0/1/2; R (round) in rot
  Div,       // sub = 1 signed
  Clz,
  Rev,       // sub = RevOp
  Extend,    // sub = ExtOp; n != 15 is the add form; rotation in rot
  Bfx,       // sub = 1 signed
  Bfc,
  Bfi,
  Sat,       // SSAT/USAT: sub = 1 signed; width-1 or width in width; op2 shift
  Sat16,     // SSAT16/USAT16
  QArith,    // QADD/QSUB/QDADD/QDSUB: sub = QOp
  Parallel,  // xADD16 etc: sub = prefix * 8 + op (see emitter)
  Sel,
  Pkh,       // sub = 1 TB
  Usad8,     // a == 15: USAD8
  LdSt,      // sub = LdStOp; offset imm or op2 register; n == 15 literal
  Ldm,
  Stm,
  Ldrex,     // sub = size 0 byte 1 half 2 word 3 dual
  Strex,     // d = status register
  B,         // target
  Bl,        // target (interworking address)
  Bx,        // m
  BlxReg,    // m
  Cbz,       // sub = 1 CBNZ; n, target
  Tb,        // TBB/TBH: sub = 1 halfword; n, m
  It,        // imm = firstcond:mask
  Svc,       // imm
  Bkpt,
  Mrs,
  Msr,       // imm = mask bits (bit 1: nzcvq, bit 0: g)
  Mrc,       // coprocessor read: imm = raw
  Mcr,       // coprocessor write: imm = raw
  // VFP
  VLdSt,     // VLDR/VSTR: sub = 1 load; dp; vd; n; imm (byte offset); U
  VLdm,      // VLDM/VSTM/VPUSH/VPOP: sub = 1 load; dp; vd; n; imm = count; P U W
  VMovS,     // core <-> single: sub = 1 to core; t; vn
  VMov2,     // two cores <-> two singles / double: sub = 1 to core; t, t2; vm; dp
  VMovScalar,// core <-> scalar: sub = 1 to core; t; vd = D reg; esize; imm = index; U
  VDup,      // core -> all lanes: t; vd; esize; Q
  VMrs,      // t (15: APSR_nzcv)
  VMsr,      // t
  VArith3,   // sub = V3Op; dp; vd vn vm
  VArith2,   // sub = V2Op; dp; vd vm
  VMovImm,   // dp; vd; imm (single bits) / imm64 in x1:x2
  VCmp,      // sub = 1 with zero; rot = E; dp; vd vm
  VCvt,      // sub = CvtOp; dp = source/dest double flag (see emitter); vd vm; rot
  VCvtFixed, // sub = 1 to fixed; dp; U signed/unsigned (P); sx (16/32); vd; imm = frac bits
  Neon,      // Advanced SIMD; sub = NeonOp (see neon section of decode.cpp)
};

enum DpOp { AND, EOR, SUB, RSB, ADD, ADC, SBC, RSC, TST, TEQ, CMP, CMN, ORR, MOV, BIC, MVN, ORN };
enum MulOp { MUL, MLA, MLS, UMULL, UMLAL, SMULL, SMLAL, UMAAL };
enum HalfOp { SMULXY, SMLAXY, SMULWY, SMLAWY, SMLALXY };
enum DualOp { SMUAD, SMLAD, SMUSD, SMLSD, SMLALD, SMLSLD };
enum RevOp { REV, REV16, REVSH, RBIT };
enum ExtOp { SXTB, SXTH, UXTB, UXTH, SXTB16, UXTB16 };
enum QOp { QADD, QSUB, QDADD, QDSUB };
enum LdStOp { STR, LDR, STRB, LDRB, STRH, LDRH, LDRSB, LDRSH, LDRD, STRD };
enum V3Op { VMLA, VMLS, VNMLS, VNMLA, VMUL, VNMUL, VADD, VSUB, VDIV, VFMA, VFMS, VFNMA, VFNMS };
enum V2Op { VMOV, VABS, VNEG, VSQRT };
enum CvtOp { CVT_F2F, CVT_I2F, CVT_F2I };
// Advanced SIMD (Kind::Neon). Registers are D numbers; Q = two (or, for the
// loads/stores, x1) of them.
enum NeonOp {
  N_FADD, N_FSUB, N_FMUL, N_FMLA, N_FMLS, // float, vector
  N_FMUL_S, N_FMLA_S, N_FMLS_S,            // float, by scalar: vm D reg, imm = index
  N_LOGIC,                                 // x1: 0 AND 1 BIC 2 ORR 3 ORN 4 EOR 5 BSL 6 BIT 7 BIF
  N_IADD, N_ISUB,                          // integer, esize
  N_DUP_S,                                 // VDUP from scalar: vm, imm = index, esize
  N_CVT,                                   // x1: 0 s32->f32 1 u32->f32 2 f32->s32 3 f32->u32
  N_MOVI,                                  // x1:x2 = the 64-bit immediate
  N_LDST,                                  // VLDn/VSTn multiple: rot = n (1, 2), x1 = registers, x2 = spacing; sub2 in width: 1 load
};

enum class Op2 : uint8_t { None, Imm, RegImm, RegReg };

struct InsnDef;

struct Insn {
  uint32_t addr = 0;
  uint32_t raw = 0;  // Thumb 32-bit: first halfword in the top half
  uint8_t size = 4;  // bytes
  bool thumb = false;
  const InsnDef *def = nullptr; // nullptr: no decoder row

  Kind kind = Kind::Undef;
  int sub = 0;
  uint32_t cond = 14;  // 14 = AL
  bool S = false;      // sets flags
  int8_t d = -1, n = -1, m = -1, s = -1, t = -1, t2 = -1, a = -1;

  // Operand 2 (data processing, register offsets): Imm (imm, carry-out
  // imm_c: -1 = C unchanged), RegImm (m shifted by DecodeImmShift(sht,
  // sha)), RegReg (m shifted by s).
  Op2 op2 = Op2::None;
  uint32_t imm = 0;
  int8_t imm_c = -1;
  uint8_t sht = 0, sha = 0;

  bool P = true, U = true, W = false; // addressing: pre-index, add, writeback
  uint32_t list = 0;                  // LDM/STM register list
  uint32_t target = 0;                // branch target, bit 0 = Thumb
  uint32_t lsb = 0, width = 0, rot = 0;

  // Thumb IT: the ITSTATE this instruction executes under (0: none) and
  // whether it is inside a block (16-bit instructions then don't set flags).
  uint8_t itstate = 0;
  bool in_it = false;

  // VFP / Advanced SIMD.
  uint8_t vd = 0, vn = 0, vm = 0; // register numbers (S or D as dp says; Q as 2*q)
  bool dp = false;                // double precision / D registers
  bool Q = false;                 // quadword
  uint8_t esize = 0;              // element size in bits
  uint32_t x1 = 0, x2 = 0;        // extra immediates

  uint32_t pcval() const { return addr + (thumb ? 4 : 8); }
  bool decoded() const { return def != nullptr; }
  const char *name() const;
};

struct Fields {
  std::array<uint32_t, 128> v{};
  uint32_t operator[](char c) const { return v[uint8_t(c)]; }
};

using NormFn = void (*)(Insn &, const Fields &);

struct InsnDef {
  std::string name;
  std::string pattern;
  int bits = 32; // pattern width (16 for Thumb 16-bit rows)
  Kind kind;
  int sub;
  NormFn norm;
  uint32_t mask = 0, value = 0;
  bool has_cond = false; // ARM 'cccc' at 31:28: cond 1111 is excluded
  std::vector<std::pair<char, std::vector<uint8_t>>> fields;
};

// Decodes one ARM-mode instruction word at `addr`.
Insn decode_arm(uint32_t addr, uint32_t raw);

// Decodes one Thumb instruction at `addr`. hw1 is the first halfword, hw2 the
// next one (used only when hw1 starts a 32-bit encoding). `in_it` gives the
// IT context (16-bit data processing sets flags only outside IT blocks).
Insn decode_thumb(uint32_t addr, uint16_t hw1, uint16_t hw2, bool in_it);

// True if hw1 is the first halfword of a 32-bit Thumb instruction.
inline bool thumb32(uint16_t hw1) { return (hw1 >> 11) >= 0x1d; }

// ThumbExpandImm_C: value and carry (-1 = unchanged).
uint32_t thumb_expand_imm(uint32_t imm12, int8_t *carry);
// Advances an ITSTATE by one instruction.
inline uint8_t it_advance(uint8_t it) { return (it & 7) == 0 ? 0 : uint8_t((it & 0xe0) | ((it << 1) & 0x1f)); }

const std::vector<InsnDef> &arm_table();
const std::vector<InsnDef> &thumb16_table();
const std::vector<InsnDef> &thumb32_table();
const std::vector<InsnDef> &vfp_table();

} // namespace recomp
