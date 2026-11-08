#include "emit.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>

namespace recomp {

namespace {

std::string fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char *f, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

std::string hex(uint32_t v) { return fmt("0x%xu", v); }

const char *const kCond[15] = {"fz",         "!fz",        "fc",                "!fc",
                               "fn",         "!fn",        "fv",                "!fv",
                               "(fc && !fz)", "(!fc || fz)", "(fn == fv)",       "(fn != fv)",
                               "(!fz && fn == fv)", "(fz || fn != fv)", "1"};

// DecodeImmShift + Shift_C for an immediate amount: the shifted value of `rm`
// and (if carry) an expression for the shifter carry-out ("fc" = unchanged).
std::string shift_imm(const std::string &rm, uint32_t type, uint32_t imm5, std::string *carry) {
  switch (type) {
  case 0: // LSL
    if (imm5 == 0) {
      if (carry) *carry = "fc";
      return rm;
    }
    if (carry) *carry = fmt("((%s >> %u) & 1)", rm.c_str(), 32 - imm5);
    return fmt("(%s << %u)", rm.c_str(), imm5);
  case 1: // LSR; #0 encodes #32
    if (imm5 == 0) {
      if (carry) *carry = fmt("(%s >> 31)", rm.c_str());
      return "0u";
    }
    if (carry) *carry = fmt("((%s >> %u) & 1)", rm.c_str(), imm5 - 1);
    return fmt("(%s >> %u)", rm.c_str(), imm5);
  case 2: { // ASR; #0 encodes #32
    uint32_t n = imm5 ? imm5 : 32;
    if (carry) *carry = fmt("((%s >> %u) & 1)", rm.c_str(), n - 1);
    return fmt("(uint32_t)((int32_t)%s >> %u)", rm.c_str(), n == 32 ? 31 : n);
  }
  default: // ROR; #0 encodes RRX
    if (imm5 == 0) {
      if (carry) *carry = fmt("(%s & 1)", rm.c_str());
      return fmt("((fc << 31) | (%s >> 1))", rm.c_str());
    }
    if (carry) *carry = fmt("((%s >> %u) & 1)", rm.c_str(), imm5 - 1);
    return fmt("rc_ror(%s, %u)", rm.c_str(), imm5);
  }
}

bool is_compare(int op) { return op >= TST && op <= CMN; }

// True if the instruction writes the pc (always, when it executes).
bool writes_pc(const Insn &in) {
  switch (in.kind) {
  case Kind::Dp: return !is_compare(in.sub) && in.d == 15;
  case Kind::LdSt: return in.sub == LDR && in.t == 15;
  case Kind::Ldm: return (in.list >> 15) & 1;
  case Kind::B:
  case Kind::Bx:
  case Kind::Tb: return true;
  default: return false;
  }
}

bool is_vfp(Kind k) { return k >= Kind::VLdSt && k <= Kind::VCvtFixed; }

} // namespace

// ---- Analysis ----------------------------------------------------------------

bool FunctionEmitter::fetch(uint32_t a, bool in_it, Insn *out) const {
  if (thumb_) {
    uint16_t hw[2] = {0, 0};
    if (!elf_.read(a, &hw[0], 2)) return false;
    if (thumb32(hw[0]) && !elf_.read(a + 2, &hw[1], 2)) return false;
    *out = decode_thumb(a, hw[0], hw[1], in_it);
    return true;
  }
  auto w = elf_.read32(a);
  if (!w) return false;
  *out = decode_arm(a, *w);
  return true;
}

// Entries in a jump table: from a "cmp Rm, #imm" just before it (imm + 1
// entries), or -1 if there is none.
int FunctionEmitter::table_bound(const Insn &in, int reg) const {
  auto it = insns_.find(in.addr);
  for (int k = 0; k < 4 && it != insns_.begin(); k++) {
    --it;
    const Insn &p = it->second;
    if (!p.decoded()) break;
    if (p.kind == Kind::Dp && p.sub == CMP && p.n == reg && p.op2 == Op2::Imm) return int(p.imm) + 1;
    // Anything else that writes the index register ends the search.
    if (p.d == reg || p.t == reg) break;
  }
  return -1;
}

// Targets of a jump table instruction (TBB/TBH; ARM add pc, pc, Rm, lsl #2;
// ldr pc, [pc, Rm, lsl #2]), by index.
bool FunctionEmitter::jump_table(const Insn &in, std::vector<uint32_t> *targets) const {
  targets->clear();
  if (in.kind == Kind::Tb) {
    if (in.n != 15 || in.m == 15 || in.m == 13) return false;
    uint32_t base = in.addr + 4, esz = in.sub ? 2 : 1;
    int bound = table_bound(in, in.m);
    uint32_t lowest = spec_.end;
    for (int i = 0; i < 1024; i++) {
      if (bound >= 0 && i >= bound) break;
      uint32_t e = base + uint32_t(i) * esz;
      if (bound < 0 && e >= lowest) break;
      uint32_t v = 0;
      if (!elf_.read(e, &v, esz)) return false;
      uint32_t tgt = base + 2 * v;
      if (!in_range(tgt) || tgt < e + esz) return false;
      lowest = std::min(lowest, tgt);
      targets->push_back(tgt);
    }
    return !targets->empty();
  }
  if (in.kind == Kind::Bx) return pic_table(in, targets);
  if (thumb_) return false;
  bool add_form = in.kind == Kind::Dp && in.sub == ADD && in.d == 15 && in.n == 15 && in.op2 == Op2::RegImm &&
                  in.sht == 0 && in.sha == 2 && !in.S && in.m != 15;
  bool ldr_form = in.kind == Kind::LdSt && in.sub == LDR && in.t == 15 && in.n == 15 &&
                  in.op2 == Op2::RegImm && in.sht == 0 && in.sha == 2 && in.P && in.U && !in.W && in.m != 15;
  if (!add_form && !ldr_form) return false;
  int bound = table_bound(in, in.m);
  if (bound <= 0 || bound > 4096) return false;
  for (int i = 0; i < bound; i++) {
    uint32_t tgt;
    if (add_form) {
      tgt = in.addr + 8 + 4 * uint32_t(i);
    } else {
      auto w = elf_.read32(in.addr + 8 + 4 * uint32_t(i));
      if (!w || (*w & 3) || !elf_.is_relocated(in.addr + 8 + 4 * uint32_t(i))) return false;
      tgt = *w;
    }
    if (!in_range(tgt)) return false;
    targets->push_back(tgt);
  }
  return true;
}

// A position-independent jump table (GCC's switch in -fPIC code):
//     cmp   Ri, #n-1 ; bhi default
//     adr   Ra, T
//     ldr   Rb, [Ra, Ri, lsl #2]
//     add   Rx, Ra, Rb        (16-bit Thumb: add Ra, Rb)
//     bx    Rx
// where T holds n offsets from T to the cases (bit 0 = Thumb). The branch
// becomes a switch on the computed target.
bool FunctionEmitter::pic_table(const Insn &in, std::vector<uint32_t> *targets) const {
  if (in.m < 0 || in.m >= 13 || in.cond != 14) return false;
  auto it = insns_.find(in.addr);
  if (it == insns_.end()) return false;
  const Insn *seq[3];
  uint32_t next = in.addr;
  for (int k = 2; k >= 0; k--) {
    if (it == insns_.begin()) return false;
    --it;
    const Insn &p = it->second;
    if (!p.decoded() || p.cond != 14 || p.addr + p.size != next) return false;
    seq[k] = &p;
    next = p.addr;
  }
  const Insn &adr = *seq[0], &ld = *seq[1], &add = *seq[2];
  if (adr.kind != Kind::Adr || adr.d < 0 || adr.d >= 13) return false;
  int ra = adr.d;
  if (ld.kind != Kind::LdSt || ld.sub != LDR || ld.n != ra || ld.op2 != Op2::RegImm || ld.sht != 0 ||
      ld.sha != 2 || !ld.P || !ld.U || ld.W || ld.t < 0 || ld.t >= 13 || ld.t == ra || ld.m == ra || ld.m < 0 ||
      ld.m >= 13)
    return false;
  int rb = ld.t;
  if (add.kind != Kind::Dp || add.sub != ADD || add.S || add.op2 != Op2::RegImm || add.sht != 0 || add.sha != 0 ||
      add.d != in.m || !((add.n == ra && add.m == rb) || (add.n == rb && add.m == ra)))
    return false;
  int bound = table_bound(ld, ld.m);
  if (bound <= 0 || bound > 4096) return false;
  uint32_t base = adr.target & ~1u;
  for (int i = 0; i < bound; i++) {
    uint32_t e = base + 4 * uint32_t(i), v = 0;
    if (!elf_.read(e, &v, 4) || elf_.is_relocated(e)) return false;
    uint32_t tgt = base + v;
    if ((tgt & 1) != tbit() || !in_range(tgt & ~1u)) return false;
    targets->push_back(tgt & ~1u);
  }
  return true;
}

// The pre-BLX call idiom: "mov lr, pc" then a pc write (ldr pc, [rX, #n] for
// virtual calls, bx rX, mov pc, rX). LR then holds the address after the
// branch, so it is a call that returns there.
bool FunctionEmitter::call_idiom(const Insn &in) const {
  if (thumb_ || !writes_pc(in) || in.kind == Kind::B || tables_.count(in.addr)) return false;
  if (in.kind == Kind::Ldm || (in.kind == Kind::Dp && in.n == 15)) return false;
  auto it = insns_.find(in.addr - 4);
  if (it == insns_.end() || !it->second.decoded()) return false;
  const Insn &p = it->second;
  return p.kind == Kind::Dp && p.sub == MOV && p.d == 14 && p.op2 == Op2::RegImm && p.m == 15 && p.sha == 0 &&
         p.sht == 0 && !p.S && (p.cond == 14 || p.cond == in.cond);
}

// A branch to the computed interworking address v (a call for the idiom).
std::string FunctionEmitter::jump(const Insn &in, const std::string &v) {
  if (call_idiom(in)) return fmt("CALLX(%s, B + 0x%xu);", v.c_str(), in.addr + 4);
  res_->computed_jumps += !(in.kind == Kind::Bx && in.m == 14);
  return "EXIT(" + v + ");";
}

void FunctionEmitter::successors(const Insn &in, bool after_call, std::vector<std::pair<uint32_t, bool>> &work) {
  uint32_t a = in.addr, next = a + in.size;
  if (!in.decoded() || in.kind == Kind::Undef || in.kind == Kind::Bkpt) return;
  bool cond = in.cond != 14;
  bool falls = !(writes_pc(in) && !cond);
  bool call = false;
  auto local = [&](uint32_t tgt_iw) {
    uint32_t p = tgt_iw & ~1u;
    if ((tgt_iw & 1) == tbit() && in_range(p)) {
      labels_.insert(p);
      work.push_back({p, false});
    } else {
      res_->tails.insert(tgt_iw);
    }
  };
  switch (in.kind) {
  case Kind::B:
  case Kind::Cbz: local(in.target); break;
  case Kind::Bl:
    res_->calls.insert(in.target);
    call = true;
    if (ctx_.noreturn && ctx_.noreturn(in.target)) falls = false;
    break;
  case Kind::BlxReg: call = true; break;
  case Kind::Bx:
    // bx pc: a constant switch to ARM at the next word (interworking
    // veneers). In Thumb the target is ARM code past this function.
    if (in.m == 15 && !cond) local(in.pcval());
    break;
  default: break;
  }
  if (call_idiom(in)) call = falls = true;
  std::vector<uint32_t> tbl;
  if (jump_table(in, &tbl)) {
    tables_[a] = tbl;
    for (uint32_t t : tbl) local(t | tbit());
  }
  if (call && falls && in.itstate == 0 && in_range(next)) entries_.insert(next);
  if (falls && in_range(next)) work.push_back({next, after_call || call});
}

void FunctionEmitter::analyze() {
  std::vector<std::pair<uint32_t, bool>> work; // address, reached only through a call's fall-through
  work.push_back({start(), false});
  entries_.insert(start());
  for (uint32_t r : spec_.roots)
    if (in_range(r)) {
      work.push_back({r, false});
      labels_.insert(r);
      entries_.insert(r);
    }
  while (!work.empty()) {
    auto [a, after_call] = work.back();
    work.pop_back();
    if (insns_.count(a)) {
      if (!after_call && after_call_.erase(a)) {
        // Now reached directly too: so is the straight-line code after it.
        for (auto it = insns_.find(a); it != insns_.end() && after_call_.erase(it->first); ++it) {
        }
      }
      continue;
    }
    Insn in;
    if (!fetch(a, false, &in)) {
      in = Insn{};
      in.addr = a;
      in.thumb = thumb_;
      in.size = thumb_ ? 2 : 4;
      insns_[a] = in; // undecodable: an exit to the dispatcher
      if (after_call) after_call_.insert(a);
      continue;
    }
    if (thumb_ && in.decoded() && in.kind == Kind::It) {
      // Decode the whole block now: its instructions depend on ITSTATE.
      insns_[a] = in;
      if (after_call) after_call_.insert(a);
      std::vector<Insn> block;
      uint8_t st = uint8_t(in.imm);
      uint32_t p = a + 2;
      while (st && in_range(p)) {
        Insn x;
        if (!fetch(p, true, &x)) break;
        x.itstate = st;
        x.in_it = true;
        x.cond = st >> 4;
        if (x.cond == 15) x.cond = 14;
        if (insns_.count(p)) break; // overlaps decoded code (bad code or data)
        insns_[p] = x;
        if (after_call) after_call_.insert(p);
        block.push_back(x);
        st = it_advance(st);
        p += x.size;
        if (!x.decoded()) break;
      }
      for (const Insn &x : block) successors(x, after_call, work);
      if (block.empty() || !block.back().decoded()) continue;
      continue;
    }
    insns_[a] = in;
    if (after_call) after_call_.insert(a);
    successors(in, after_call, work);
  }
  // Entries must be instruction boundaries outside IT blocks.
  for (auto it = entries_.begin(); it != entries_.end();) {
    auto f = insns_.find(*it);
    if (f == insns_.end() || f->second.itstate != 0) it = entries_.erase(it);
    else ++it;
  }
  // Control-flow merges: branch targets, roots and the start. Call return
  // sites become labels too (entries), but data flows into them from the call.
  merges_ = labels_;
  merges_.insert(start());
  for (uint32_t e : entries_) labels_.insert(e);
}

// Code addresses computed pc-relative (ldr rX, =off; add rX, pc): candidate
// function pointers for discovery. A simple per-block constant tracker.
void FunctionEmitter::find_pointers() {
  uint32_t known[16];
  bool ok[16] = {};
  for (const auto &[a, in] : insns_) {
    if (merges_.count(a)) std::fill(ok, ok + 16, false);
    if (!in.decoded()) continue;
    int set = -1;
    uint32_t val = 0;
    if (in.kind == Kind::LdSt && in.sub == LDR && in.n == 15 && in.op2 == Op2::Imm && in.t != 15) {
      uint32_t va = ((in.pcval()) & ~3u) + (in.U ? in.imm : -in.imm);
      auto w = elf_.read32(va);
      if (w && !elf_.is_writable(va) && !elf_.is_relocated(va)) set = in.t, val = *w;
    } else if (in.kind == Kind::MovW) {
      set = in.d, val = in.imm;
    } else if (in.kind == Kind::MovT && in.d >= 0 && ok[in.d]) {
      set = in.d, val = (known[in.d] & 0xffff) | in.imm << 16;
    } else if (in.kind == Kind::Dp && in.sub == ADD && !in.S && in.op2 == Op2::RegImm && in.sha == 0 &&
               in.sht == 0 && in.d >= 0) {
      int other = in.n == 15 ? in.m : in.m == 15 ? in.n : -1;
      if (other >= 0 && other != 15 && ok[other]) {
        val = in.pcval() + known[other];
        if (in.d != 15) {
          set = in.d;
          if (elf_.is_text(val & ~1u)) res_->pointers.insert(val);
        } else if (in.cond == 14 && elf_.is_text(val & ~1u)) {
          // add pc, rX, pc: a constant jump (the linker's long-branch
          // veneers). ARM interworks on it, Thumb doesn't.
          res_->tails.insert(thumb_ ? val | 1u : val);
        }
      }
    } else if (in.kind == Kind::Bx && in.cond == 14 && in.m >= 0 && in.m < 15 && ok[in.m] &&
               elf_.is_text(known[in.m] & ~1u)) {
      res_->tails.insert(known[in.m]); // ldr ip, =X; add ip, pc; bx ip (veneers)
    }
    // Everything this instruction writes is unknown now.
    for (int r : {int(in.d), int(in.t), int(in.t2), int(in.a)})
      if (r >= 0 && r < 16) ok[r] = false;
    if (in.kind == Kind::Ldm)
      for (int r = 0; r < 16; r++)
        if ((in.list >> r) & 1) ok[r] = false;
    if (in.kind == Kind::Bl || in.kind == Kind::BlxReg || in.kind == Kind::Svc)
      for (int r : {0, 1, 2, 3, 12, 14}) ok[r] = false;
    if ((in.kind == Kind::LdSt || in.kind == Kind::Ldm || in.kind == Kind::Stm) && (!in.P || in.W) && in.n >= 0)
      ok[in.n] = false;
    if (set >= 0 && set < 15) known[set] = val, ok[set] = true;
  }
}

FunctionResult FunctionEmitter::run(const FunctionSpec &spec, bool emit) {
  spec_ = spec;
  thumb_ = spec.addr & 1;
  insns_.clear();
  labels_.clear();
  merges_.clear();
  after_call_.clear();
  entries_.clear();
  tables_.clear();
  out_.clear();

  FunctionResult res;
  res_ = &res;
  res.spec = spec;
  res.c_name = fmt("%s_f_%08x", ctx_.prefix.c_str(), start());
  analyze();
  auto first = insns_.find(start());
  res.entry_ok = first != insns_.end() && first->second.decoded();
  res.insns = uint32_t(insns_.size());
  for (const auto &[a, in] : insns_) {
    res.bytes += in.size;
    res.insn_addrs[a] = in.size;
  }
  for (uint32_t e : entries_) res.entries.push_back(e | tbit());
  res.jump_tables = uint32_t(tables_.size());
  find_pointers();
  if (res.entry_ok) emit_function(); // also counts what is unsupported
  if (emit && res.entry_ok) res.code = std::move(out_);
  res_ = nullptr;
  return res;
}

// ---- Emission ----------------------------------------------------------------

std::string FunctionEmitter::R(const Insn &in, int r) const {
  if (r == 15) return fmt("(B + 0x%xu)", in.pcval());
  return fmt("r%d", r);
}

std::string FunctionEmitter::addr(uint32_t a) const { return fmt("(B + 0x%xu)", a); }

std::string FunctionEmitter::branch_to(const Insn &in, uint32_t target_iw) const {
  uint32_t p = target_iw & ~1u;
  if ((target_iw & 1) == tbit() && in_range(p) && insns_.count(p)) {
    std::string g = fmt("goto L_%x;", p);
    if (p <= in.addr) g = "{ RC_TICK(); " + g + " }";
    return g;
  }
  return "EXIT(" + addr(target_iw) + ");";
}

bool FunctionEmitter::fail(const std::string &why) {
  why_ = why;
  return false;
}

void FunctionEmitter::emit_function() {
  FunctionResult &res = *res_;
  size_t entries_before = entries_.size();
  out_ += fmt("/* %s */\nvoid %s(rc_state *s) {\n  RC_LOCALS(%s_base);\n  RC_SYNC_IN();\n  RC_TICK();\n",
              spec_.name.c_str(), res.c_name.c_str(), ctx_.prefix.c_str());
  if (entries_.size() > 1) {
    out_ += fmt("  if (s->r[15] != B + 0x%xu) switch (s->r[15] - B) {\n", start());
    for (uint32_t e : entries_)
      if (e != start()) out_ += fmt("    case 0x%xu: goto L_%x;\n", e, e);
    out_ += "    default: rc_bad_entry(s); return;\n  }\n";
  }
  for (auto it = insns_.begin(); it != insns_.end(); ++it) {
    const auto &[a, in] = *it;
    if (labels_.count(a)) out_ += fmt("L_%x: RC_BLOCK(0x%xu);\n", a, a);
    out_ += fmt("  /*%x*/ ", a);
    bool after_call = after_call_.count(a) != 0;
    bool ok = false;
    why_.clear();
    if (!in.decoded()) {
      why_ = "no decoder row";
    } else {
      size_t mark = out_.size();
      if (in.cond != 14 && in.kind != Kind::It) out_ += fmt("if (%s) { ", kCond[in.cond]);
      ok = emit_insn(in);
      if (ok && in.cond != 14 && in.kind != Kind::It) out_ += " }";
      if (!ok) out_.resize(mark);
    }
    if (ok) {
      res.mnemonics[in.name()]++;
      if (is_vfp(in.kind)) res.n_vfp++;
      if (in.kind == Kind::Neon) res.n_neon++;
    } else {
      // Leave this instruction to the dispatcher (interpreter); resume after it.
      std::string key = std::string(in.decoded() ? in.name() : thumb_ ? "?thumb" : "?arm") + ": " + why_;
      if (!in.decoded()) {
        uint32_t raw = in.raw;
        if (!thumb_) {
          if (auto w = elf_.read32(a)) raw = *w;
          key = fmt("?arm %02x/%x: no decoder row", (raw >> 20) & 0xff, (raw >> 4) & 0xf);
        } else {
          uint16_t hw[2] = {0, 0};
          elf_.read(a, &hw[0], 2);
          elf_.read(a + 2, &hw[1], 2);
          key = thumb32(hw[0]) ? fmt("?t32 %03x/%x: no decoder row", hw[0] >> 4, (hw[1] >> 4) & 0xf)
                               : fmt("?t16 %02x: no decoder row", hw[0] >> 8);
        }
      }
      bool invalid = !in.decoded() || in.kind == Kind::Undef || in.kind == Kind::Bkpt;
      res.n_invalid += invalid;
      if (after_call && invalid) {
        res.unsupported_after_call[key]++;
        res.n_after_call++;
      } else {
        res.unsupported[key]++;
        res.n_unsupported++;
      }
      out_ += fmt("UNSUP(B + 0x%xu, 0x%x); /* %s */", a | tbit(), in.itstate, key.c_str());
      auto nx = insns_.find(a + in.size);
      if (in.decoded() && nx != insns_.end() && nx->second.itstate == 0 && !entries_.count(a + in.size)) {
        // The next instruction becomes an entry (resuming inside an IT block
        // would need ITSTATE, so not there).
        entries_.insert(a + in.size);
        labels_.insert(a + in.size);
        res.entries.push_back((a + in.size) | tbit());
      }
      out_ += "\n";
      continue;
    }
    out_ += "\n";
    // Falling through to an address that wasn't walked: leave for it.
    bool falls = (!(writes_pc(in) && in.cond == 14) || call_idiom(in)) && in.kind != Kind::Undef;
    if (in.kind == Kind::Bl && ctx_.noreturn && ctx_.noreturn(in.target)) falls = true; // EXIT emitted below
    auto nx = std::next(it);
    if (falls && (nx == insns_.end() || nx->first != a + in.size)) out_ += fmt("  EXIT(B + 0x%xu);\n", (a + in.size) | tbit());
  }
  out_ += "L_exit:\n  RC_SYNC_OUT();\n  rc_set_pc(s, npc);\n}\n";
  // Resume entries found while emitting need labels and switch cases that
  // were already printed: emit again, now with the complete sets (resume
  // points are only ever added, so the second pass adds none).
  if (entries_.size() != entries_before) {
    out_.clear();
    res.unsupported.clear();
    res.unsupported_after_call.clear();
    res.mnemonics.clear();
    res.n_unsupported = res.n_after_call = res.n_vfp = res.n_neon = res.n_invalid = 0;
    emit_function();
  }
}

bool FunctionEmitter::emit_insn(const Insn &in) {
  switch (in.kind) {
  case Kind::Dp:
    if (tables_.count(in.addr)) return emit_table(in);
    return emit_dp(in);
  case Kind::Adr:
    if (in.d == 15) return fail("pc destination");
    out_ += fmt("r%d = ", in.d) + addr(in.target) + ";";
    return true;
  case Kind::MovW:
    if (in.d == 15 || (thumb_ && in.d == 13)) return fail("sp/pc destination");
    out_ += fmt("r%d = 0x%xu;", in.d, in.imm);
    return true;
  case Kind::MovT:
    if (in.d == 15 || (thumb_ && in.d == 13)) return fail("sp/pc destination");
    out_ += fmt("r%d = (r%d & 0xffffu) | 0x%xu;", in.d, in.d, in.imm << 16);
    return true;
  case Kind::Nop:
  case Kind::It: out_ += ";"; return true;
  case Kind::Barrier: out_ += "__atomic_thread_fence(__ATOMIC_SEQ_CST);"; return true;
  case Kind::Clrex: out_ += "ex_on = 0;"; return true;
  case Kind::Mul: return emit_mul(in);
  case Kind::MulHalf:
  case Kind::MulDual:
  case Kind::MulHi:
  case Kind::Div:
  case Kind::Usad8: return emit_mul_misc(in);
  case Kind::LdSt:
    if (tables_.count(in.addr)) return emit_table(in);
    return emit_ldst(in);
  case Kind::Ldm:
  case Kind::Stm: return emit_ldm_stm(in);
  case Kind::Ldrex:
  case Kind::Strex: return emit_exclusive(in);
  case Kind::B: out_ += branch_to(in, in.target); return true;
  case Kind::Cbz:
    out_ += fmt("if (r%d %s 0) ", in.n, in.sub ? "!=" : "==") + branch_to(in, in.target);
    return true;
  case Kind::Bl: {
    uint32_t ret = (in.addr + in.size) | tbit();
    std::string f = ctx_.callee ? ctx_.callee(in.target) : "";
    out_ += fmt("r14 = B + 0x%xu; ", ret);
    if (!f.empty())
      out_ += fmt("CALL(%s, B + 0x%xu, B + 0x%xu);", f.c_str(), in.target, ret);
    else
      out_ += fmt("CALLX(B + 0x%xu, B + 0x%xu);", in.target, ret);
    if (ctx_.noreturn && ctx_.noreturn(in.target)) out_ += fmt(" EXIT(B + 0x%xu);", ret);
    return true;
  }
  case Kind::Bx:
    if (tables_.count(in.addr)) return emit_table(in);
    out_ += jump(in, R(in, in.m));
    return true;
  case Kind::BlxReg: {
    if (in.m == 15) return fail("pc target");
    uint32_t ret = (in.addr + in.size) | tbit();
    out_ += fmt("{ uint32_t t_ = r%d; r14 = B + 0x%xu; CALLX(t_, B + 0x%xu); }", in.m, ret, ret);
    return true;
  }
  case Kind::Tb: return emit_table(in);
  case Kind::Svc:
    out_ += fmt("SVC(0x%xu, B + 0x%xu);", in.imm, (in.addr + in.size) | tbit());
    return true;
  default: break;
  }
  if (in.kind >= Kind::VLdSt && in.kind <= Kind::VCvtFixed) return emit_vfp(in);
  if (in.kind == Kind::Neon) return emit_neon(in);
  return emit_misc(in);
}

bool FunctionEmitter::emit_dp(const Insn &in) {
  int op = in.sub;
  bool cmp = is_compare(op);
  int d = in.d, n = in.n;
  if (d == 15 && in.S) return fail("flag-setting write to pc (exception return)");
  if (d == 15 && thumb_ && !(op == ADD || op == MOV)) return fail("pc destination");
  std::string pre, val, carry;
  bool imm = false;
  switch (in.op2) {
  case Op2::Imm:
    val = hex(in.imm);
    imm = true;
    carry = in.imm_c < 0 ? "fc" : in.imm_c ? "1u" : "0u";
    break;
  case Op2::RegImm:
    val = shift_imm(R(in, in.m), in.sht, in.sha, &carry);
    break;
  case Op2::RegReg:
    if (d == 15 || n == 15 || in.m == 15 || in.s == 15) return fail("pc in a register-shifted operand");
    pre = fmt("uint32_t sc_, o_ = rc_shift_reg(r%d, %u, r%d, fc, &sc_); ", in.m, in.sht, in.s);
    val = "o_";
    carry = "sc_";
    break;
  default: return fail("no operand 2");
  }
  std::string rn = n >= 0 ? R(in, n) : "";
  auto inv = [&](const std::string &v) { return imm ? hex(~in.imm) : "~" + v; };
  std::string e;
  bool arith_flags = false;
  switch (op) {
  case AND: case TST: e = rn + " & " + val; break;
  case EOR: case TEQ: e = rn + " ^ " + val; break;
  case ORR: e = rn + " | " + val; break;
  case ORN: e = rn + " | " + inv(val); break;
  case BIC: e = rn + " & " + inv(val); break;
  case MOV: e = val; break;
  case MVN: e = inv(val); break;
  default: {
    std::string a = rn, b = val, cin = "0";
    switch (op) {
    case ADD: case CMN: break;
    case ADC: cin = "fc"; break;
    case SUB: case CMP: b = inv(val); cin = "1"; break;
    case SBC: b = inv(val); cin = "fc"; break;
    case RSB: a = "~" + rn; cin = "1"; break;
    case RSC: a = "~" + rn; cin = "fc"; break;
    }
    if (in.S) {
      e = "ADDS(" + a + ", " + b + ", " + cin + ")";
      arith_flags = true;
    } else if (op == ADD) {
      e = rn + " + " + val;
    } else if (op == SUB) {
      e = rn + " - " + val;
    } else if (op == RSB) {
      e = val + " - " + rn;
    } else {
      e = a + " + " + b + " + " + cin;
    }
  }
  }
  std::string code;
  if (!in.S) {
    if (d == 15) {
      // ARM: ALUWritePC is an interworking branch; Thumb (ADD/MOV pc): stays Thumb.
      code = jump(in, thumb_ ? "((" + e + ") | 1u)" : "(" + e + ")");
    } else {
      code = fmt("r%d = ", d) + e + ";";
    }
  } else if (arith_flags) {
    code = cmp ? "(void)" + e + ";" : fmt("r%d = ", d) + e + ";";
  } else { // logical with flags
    bool c = carry != "fc";
    code = (c ? "uint32_t c_ = " + carry + "; " : "") + "uint32_t r_ = " + e + "; NZ(r_); " + (c ? "fc = c_; " : "") +
           (cmp ? "" : fmt("r%d = r_; ", d));
    code = "{ " + code + "}";
  }
  if (!pre.empty()) code = "{ " + pre + code + " }";
  out_ += code;
  return true;
}

bool FunctionEmitter::emit_mul(const Insn &in) {
  int op = in.sub;
  bool flags = in.S;
  int n = in.n, m = in.m;
  if (n == 15 || m == 15) return fail("pc operand");
  std::string rn = fmt("r%d", n), rm = fmt("r%d", m);
  if (op == MUL || op == MLA || op == MLS) {
    int d = in.d, a = in.a;
    if (d == 15 || (op != MUL && a == 15)) return fail("pc operand");
    std::string e = rn + " * " + rm;
    if (op == MLA) e = e + fmt(" + r%d", a);
    if (op == MLS) e = fmt("r%d - ", a) + e;
    if (flags) out_ += "{ uint32_t r_ = " + e + fmt("; NZ(r_); r%d = r_; }", d);
    else out_ += fmt("r%d = ", d) + e + ";";
    return true;
  }
  int h = in.d, l = in.a;
  if (h == 15 || l == 15 || h == l) return fail("unpredictable registers");
  std::string acc = fmt("((uint64_t)r%d << 32 | r%d)", h, l);
  std::string p;
  switch (op) {
  case UMULL: p = "(uint64_t)" + rn + " * " + rm; break;
  case UMLAL: p = "(uint64_t)" + rn + " * " + rm + " + " + acc; break;
  case SMULL: p = "(uint64_t)((int64_t)(int32_t)" + rn + " * (int32_t)" + rm + ")"; break;
  case SMLAL: p = "(uint64_t)((int64_t)(int32_t)" + rn + " * (int32_t)" + rm + ") + " + acc; break;
  case UMAAL: p = "(uint64_t)" + rn + " * " + rm + fmt(" + r%d + r%d", h, l); break;
  }
  out_ += "{ uint64_t p_ = " + p + "; ";
  if (flags) out_ += "fn = (uint32_t)(p_ >> 63); fz = p_ == 0; ";
  out_ += fmt("r%d = (uint32_t)p_; r%d = (uint32_t)(p_ >> 32); }", l, h);
  return true;
}

// Halfword, dual, most-significant-word multiplies; divide; USAD8.
bool FunctionEmitter::emit_mul_misc(const Insn &in) {
  for (int r : {int(in.n), int(in.m), int(in.d)})
    if (r == 15) return fail("pc operand");
  if (in.a == 15 && in.kind != Kind::MulDual && in.kind != Kind::MulHi && in.kind != Kind::Usad8 &&
      !(in.kind == Kind::MulHalf && (in.sub == SMULXY || in.sub == SMULWY)))
    return fail("pc operand");
  std::string rn = fmt("r%d", in.n), rm = fmt("r%d", in.m);
  auto half = [](const std::string &r, bool top) {
    return top ? "(int32_t)(int16_t)(" + r + " >> 16)" : "(int32_t)(int16_t)" + r;
  };
  switch (in.kind) {
  case Kind::MulHalf: {
    bool xt = in.rot >> 1, yt = in.rot & 1;
    switch (in.sub) {
    case SMULXY: out_ += fmt("r%d = (uint32_t)(", in.d) + half(rn, xt) + " * " + half(rm, yt) + ");"; return true;
    case SMLAXY:
      out_ += "{ int64_t p_ = (int64_t)(" + half(rn, xt) + " * " + half(rm, yt) + fmt(") + (int32_t)r%d; ", in.a) +
              fmt("r%d = (uint32_t)p_; SETQ(p_ != (int32_t)p_); }", in.d);
      return true;
    case SMULWY:
      out_ += fmt("r%d = (uint32_t)(((int64_t)(int32_t)", in.d) + rn + " * " + half(rm, yt) + ") >> 16);";
      return true;
    case SMLAWY:
      out_ += "{ int64_t p_ = (((int64_t)(int32_t)" + rn + " * " + half(rm, yt) + ") >> 16) + " +
              fmt("(int32_t)r%d; r%d = (uint32_t)p_; SETQ(p_ != (int32_t)p_); }", in.a, in.d);
      return true;
    case SMLALXY: {
      int h = in.d, l = in.a;
      if (h == l) return fail("unpredictable registers");
      out_ += fmt("{ uint64_t p_ = ((uint64_t)r%d << 32 | r%d) + (uint64_t)(int64_t)(", h, l) + half(rn, xt) + " * " +
              half(rm, yt) + fmt("); r%d = (uint32_t)p_; r%d = (uint32_t)(p_ >> 32); }", l, h);
      return true;
    }
    }
    return fail("no emitter");
  }
  case Kind::MulDual: {
    std::string mm = in.rot ? "rc_ror(" + rm + ", 16)" : rm;
    std::string p1 = "(int64_t)(" + half(rn, false) + " * " + half("m_", false) + ")";
    std::string p2 = "(int64_t)(" + half(rn, true) + " * " + half("m_", true) + ")";
    bool sub = in.sub == SMUSD || in.sub == SMLSD || in.sub == SMLSLD;
    std::string sum = p1 + (sub ? " - " : " + ") + p2;
    std::string head = "{ uint32_t m_ = " + mm + "; ";
    switch (in.sub) {
    case SMUAD:
    case SMUSD:
      out_ += head + "int64_t p_ = " + sum + fmt("; r%d = (uint32_t)p_; ", in.d) +
              (in.sub == SMUAD ? "SETQ(p_ != (int32_t)p_); }" : "}");
      return true;
    case SMLAD:
    case SMLSD:
      out_ += head + "int64_t p_ = " + sum + fmt(" + (int32_t)r%d; r%d = (uint32_t)p_; SETQ(p_ != (int32_t)p_); }", in.a, in.d);
      return true;
    default: {
      int h = in.d, l = in.a;
      if (h == l) return fail("unpredictable registers");
      out_ += head + fmt("uint64_t p_ = ((uint64_t)r%d << 32 | r%d) + (uint64_t)(", h, l) + sum +
              fmt("); r%d = (uint32_t)p_; r%d = (uint32_t)(p_ >> 32); }", l, h);
      return true;
    }
    }
  }
  case Kind::MulHi: {
    std::string p = "(int64_t)(int32_t)" + rn + " * (int32_t)" + rm;
    std::string e;
    if (in.sub == 0) e = p;
    else if (in.sub == 1) e = fmt("((int64_t)r%d << 32) + ", in.a) + p;
    else e = fmt("((int64_t)r%d << 32) - ", in.a) + p;
    out_ += fmt("r%d = (uint32_t)((uint64_t)(", in.d) + e + (in.rot ? " + 0x80000000ll" : "") + ") >> 32);";
    return true;
  }
  case Kind::Div:
    if (in.sub)
      out_ += fmt("r%d = r%d == 0 ? 0u : (r%d == 0x80000000u && r%d == 0xffffffffu) ? 0x80000000u : "
                  "(uint32_t)((int32_t)r%d / (int32_t)r%d);",
                  in.d, in.m, in.n, in.m, in.n, in.m);
    else
      out_ += fmt("r%d = r%d == 0 ? 0u : r%d / r%d;", in.d, in.m, in.n, in.m);
    return true;
  case Kind::Usad8: {
    std::string e = "{ uint32_t a_ = " + rn + ", b_ = " + rm + ", r_ = 0; for (int i_ = 0; i_ < 32; i_ += 8) { int " +
                    "x_ = (int)((a_ >> i_) & 0xff) - (int)((b_ >> i_) & 0xff); r_ += (uint32_t)(x_ < 0 ? -x_ : x_); } ";
    if (in.a >= 0 && in.a != 15) e += fmt("r_ += r%d; ", in.a);
    out_ += e + fmt("r%d = r_; }", in.d);
    return true;
  }
  default: return fail("no emitter");
  }
}

bool FunctionEmitter::emit_ldst(const Insn &in) {
  int op = in.sub;
  int n = in.n, t = in.t;
  bool load = op == LDR || op == LDRB || op == LDRH || op == LDRSB || op == LDRSH || op == LDRD;
  bool dual = op == LDRD || op == STRD;
  bool wb = !in.P || in.W;
  int t2 = dual ? (thumb_ ? in.t2 : t + 1) : -1;
  if (dual) {
    if (!thumb_ && ((t & 1) || t == 14)) return fail("Rt odd or lr");
    if (!thumb_ && !in.P && in.W) return fail("unpredictable addressing");
    if (t == 15 || t2 == 15 || (thumb_ && (t == 13 || t2 == 13))) return fail("pc/sp as Rt");
    if (wb && (n == 15 || n == t || n == t2)) return fail("writeback overlaps");
    if (load && t == t2) return fail("Rt == Rt2");
  } else {
    if (t == 15 && op != LDR && op != STR) return fail("pc as Rt");
    if (t == 15 && op == STR && thumb_) return fail("pc as Rt");
    if (wb && (n == 15 || n == t)) return fail("writeback with Rn == pc or Rn == Rt");
  }
  std::string off;
  if (in.op2 == Op2::Imm) {
    off = hex(in.imm);
  } else {
    if (in.m == 15) return fail("pc offset register");
    if (!thumb_ && wb && in.m == n) return fail("writeback with Rm == Rn");
    if (op == LDRD && (in.m == t || in.m == t2)) return fail("unpredictable Rm");
    off = shift_imm(fmt("r%d", in.m), in.sht, in.sha, nullptr);
  }

  std::string a, wbv;
  if (n == 15 && in.op2 == Op2::Imm) {
    // Literal: a constant address. Values in read-only, unrelocated parts of
    // the image (literal pools) are folded.
    uint32_t va = (in.pcval() & ~3u) + (in.U ? in.imm : -in.imm);
    if (load) {
      uint32_t sz = op == LDR ? 4 : op == LDRD ? 8 : (op == LDRH || op == LDRSH) ? 2 : 1;
      uint32_t v[2] = {0, 0};
      bool fold = elf_.read(va, v, sz) && !elf_.is_writable(va) && !elf_.is_relocated(va) &&
                  !(sz == 8 && elf_.is_relocated(va + 4));
      if (fold && t != 15) {
        uint32_t x = v[0];
        if (op == LDRSB) x = uint32_t(int32_t(int8_t(x)));
        if (op == LDRSH) x = uint32_t(int32_t(int16_t(x)));
        out_ += fmt("/* =%x */ r%d = 0x%xu;", va, t, x);
        if (dual) out_ += fmt(" r%d = 0x%xu;", t2, v[1]);
        return true;
      }
    }
    a = addr(va);
  } else {
    std::string rn = R(in, n);
    std::string offaddr = "(" + rn + (in.U ? " + " : " - ") + off + ")";
    a = in.P ? offaddr : rn;
    if (wb) wbv = offaddr;
  }
  std::string ld, st;
  switch (op) {
  case LDR: ld = "LD32"; break;
  case LDRB: ld = "LD8"; break;
  case LDRH: ld = "LD16"; break;
  case LDRSB: ld = "LDS8"; break;
  case LDRSH: ld = "LDS16"; break;
  case STR: st = "ST32"; break;
  case STRB: st = "ST8"; break;
  case STRH: st = "ST16"; break;
  default: break;
  }
  std::string wbs = wb ? fmt("r%d = w_; ", n) : "";
  if (!wb && !dual && t != 15) {
    if (load) out_ += fmt("r%d = %s(", t, ld.c_str()) + a + ");";
    else out_ += st + "(" + a + ", " + R(in, t) + ");";
    return true;
  }
  out_ += "{ uint32_t a_ = " + a + "; ";
  if (wb) out_ += "uint32_t w_ = " + wbv + "; ";
  if (op == LDRD) {
    out_ += "uint32_t v0_ = LD32(a_), v1_ = LD32(a_ + 4); " + wbs + fmt("r%d = v0_; r%d = v1_; }", t, t2);
  } else if (op == STRD) {
    out_ += fmt("ST32(a_, r%d); ST32(a_ + 4, r%d); ", t, t2) + wbs + "}";
  } else if (load) {
    out_ += "uint32_t v_ = " + ld + "(a_); " + wbs;
    if (t == 15) {
      out_ += jump(in, "v_") + " }";
    } else {
      out_ += fmt("r%d = v_; }", t);
    }
  } else {
    out_ += st + "(a_, " + R(in, t) + "); " + wbs + "}";
  }
  return true;
}

bool FunctionEmitter::emit_ldm_stm(const Insn &in) {
  bool load = in.kind == Kind::Ldm;
  int n = in.n;
  uint32_t list = in.list;
  bool P = in.P, U = in.U, W = in.W;
  if (n == 15 || list == 0) return fail("pc base or empty list");
  if (thumb_ && ((list >> 13) & 1)) return fail("sp in list");
  if (thumb_ && !load && ((list >> 15) & 1)) return fail("pc stored");
  if (thumb_ && load && ((list >> 14) & 1) && ((list >> 15) & 1)) return fail("lr and pc loaded");
  uint32_t count = uint32_t(__builtin_popcount(list));
  bool n_in_list = (list >> n) & 1;
  if (W && n_in_list) {
    if (load || thumb_) return fail("writeback with the base in the list");
    if ((list & ((1u << n) - 1)) != 0) return fail("stored base is UNKNOWN");
  }
  std::string rn = fmt("r%d", n);
  std::string start, wbv;
  if (U) {
    start = P ? "(" + rn + " + 4u)" : rn;
    wbv = fmt("(%s + %uu)", rn.c_str(), 4 * count);
  } else {
    start = P ? fmt("(%s - %uu)", rn.c_str(), 4 * count) : fmt("(%s - %uu)", rn.c_str(), 4 * count - 4);
    wbv = fmt("(%s - %uu)", rn.c_str(), 4 * count);
  }
  out_ += "{ uint32_t a_ = " + start + "; ";
  if (W) out_ += "uint32_t w_ = " + wbv + "; ";
  uint32_t off = 0;
  if (load) {
    for (int r = 0; r < 16; r++)
      if ((list >> r) & 1) {
        out_ += fmt("uint32_t v%d_ = LD32(a_ + %uu); ", r, off);
        off += 4;
      }
    if (W) out_ += fmt("r%d = w_; ", n);
    for (int r = 0; r < 15; r++)
      if ((list >> r) & 1) out_ += fmt("r%d = v%d_; ", r, r);
    if ((list >> 15) & 1) out_ += "EXIT(v15_); ";
  } else {
    for (int r = 0; r < 16; r++)
      if ((list >> r) & 1) {
        out_ += fmt("ST32(a_ + %uu, ", off) + R(in, r) + "); ";
        off += 4;
      }
    if (W) out_ += fmt("r%d = w_; ", n);
  }
  out_ += "}";
  return true;
}

bool FunctionEmitter::emit_exclusive(const Insn &in) {
  int n = in.n, t = in.t;
  int sz = in.sub; // 0 byte, 1 half, 2 word, 3 dual
  int t2 = sz == 3 ? (thumb_ ? in.t2 : t + 1) : -1;
  if (n == 15 || t == 15 || t2 == 15 || (sz == 3 && !thumb_ && (t & 1))) return fail("unpredictable registers");
  std::string a = fmt("r%d", n);
  if (in.imm) a = fmt("(r%d + 0x%xu)", n, in.imm);
  if (in.kind == Kind::Ldrex) {
    if (sz == 3 && t == t2) return fail("Rt == Rt2");
    out_ += "{ uint32_t a_ = " + a + "; RC_EXCL(a_); ";
    switch (sz) {
    case 0: out_ += fmt("uint32_t v_ = LD8(a_); ex_v = v_; r%d = v_; ", t); break;
    case 1: out_ += fmt("uint32_t v_ = LD16(a_); ex_v = v_; r%d = v_; ", t); break;
    case 2: out_ += fmt("uint32_t v_ = LD32(a_); ex_v = v_; r%d = v_; ", t); break;
    default: out_ += fmt("uint64_t v_ = rc_ld64(m, a_); ex_v = v_; r%d = (uint32_t)v_; r%d = (uint32_t)(v_ >> 32); ", t, t2);
    }
    out_ += "ex_a = a_; ex_on = 1; }";
    return true;
  }
  int d = in.d;
  if (d == 15 || d == n || d == t || d == t2) return fail("unpredictable status register");
  static const char *const ty[4] = {"uint8_t", "uint16_t", "uint32_t", "uint64_t"};
  std::string v = sz == 3 ? fmt("((uint64_t)r%d << 32 | r%d)", t2, t) : fmt("r%d", t);
  out_ += "{ uint32_t a_ = " + a + "; RC_EXCL(a_); " + fmt("r%d = STREX_(%s, a_, ", d, ty[sz]) + v + "); ex_on = 0; }";
  return true;
}

// Jump tables: a switch over the known targets, with the general computed
// branch as the default.
bool FunctionEmitter::emit_table(const Insn &in) {
  const std::vector<uint32_t> *tbl = nullptr;
  auto it = tables_.find(in.addr);
  if (it != tables_.end()) tbl = &it->second;
  std::string idx = fmt("r%d", in.m);
  std::string dflt;
  if (in.kind == Kind::Bx) { // pic_table: a switch on the computed target itself
    out_ += fmt("switch (r%d - B) { ", in.m);
    for (uint32_t t : std::set<uint32_t>(tbl->begin(), tbl->end()))
      out_ += fmt("case 0x%xu: goto L_%x; ", t | tbit(), t);
    out_ += fmt("default: EXIT(r%d); }", in.m);
    return true;
  }
  if (in.kind == Kind::Tb) {
    if (in.m == 15 || in.m == 13) return fail("unpredictable index");
    std::string base = R(in, in.n);
    std::string e = in.sub ? "LD16(" + base + " + 2u * " + idx + ")" : "LD8(" + base + " + " + idx + ")";
    dflt = fmt("EXIT((B + 0x%xu + 2u * ", in.addr + 4) + e + ") | 1u);";
  } else if (in.kind == Kind::Dp) {
    dflt = fmt("EXIT(B + 0x%xu + (r%d << 2));", in.addr + 8, in.m);
  } else {
    dflt = fmt("EXIT(LD32(B + 0x%xu + (r%d << 2)));", in.addr + 8, in.m);
  }
  if (!tbl) {
    res_->computed_jumps++;
    out_ += dflt;
    return true;
  }
  out_ += "switch (" + idx + ") { ";
  for (size_t i = 0; i < tbl->size(); i++) out_ += fmt("case %zu: goto L_%x; ", i, (*tbl)[i]);
  out_ += "default: " + dflt + " }";
  return true;
}

bool FunctionEmitter::emit_misc(const Insn &in) {
  for (int r : {int(in.d), int(in.n), int(in.m)})
    if (r == 15 && in.kind != Kind::Mrc && in.kind != Kind::Mcr && !(in.kind == Kind::Extend && r == in.n))
      return fail("pc operand");
  switch (in.kind) {
  case Kind::Clz:
    out_ += fmt("r%d = r%d ? (uint32_t)__builtin_clz(r%d) : 32u;", in.d, in.m, in.m);
    return true;
  case Kind::Rev: {
    int d = in.d, m = in.m;
    switch (in.sub) {
    case REV: out_ += fmt("r%d = __builtin_bswap32(r%d);", d, m); break;
    case REV16: out_ += fmt("r%d = ((r%d & 0xff00ff00u) >> 8) | ((r%d & 0x00ff00ffu) << 8);", d, m, m); break;
    case REVSH: out_ += fmt("r%d = (uint32_t)(int32_t)(int16_t)__builtin_bswap16((uint16_t)r%d);", d, m); break;
    case RBIT: out_ += fmt("r%d = rc_rbit(r%d);", d, m); break;
    }
    return true;
  }
  case Kind::Extend: {
    std::string v = in.rot ? fmt("rc_ror(r%d, %u)", in.m, in.rot) : fmt("r%d", in.m);
    bool add = in.n >= 0 && in.n != 15;
    switch (in.sub) {
    case SXTB16:
    case UXTB16: {
      std::string lo, hi;
      if (in.sub == SXTB16) {
        lo = "(uint32_t)(int32_t)(int8_t)v_";
        hi = "(uint32_t)(int32_t)(int8_t)(v_ >> 16)";
      } else {
        lo = "(v_ & 0xffu)";
        hi = "((v_ >> 16) & 0xffu)";
      }
      if (add) {
        lo = fmt("(r%d + ", in.n) + lo + ")";
        hi = fmt("((r%d >> 16) + ", in.n) + hi + ")";
      }
      out_ += "{ uint32_t v_ = " + v + fmt("; r%d = (", in.d) + lo + " & 0xffffu) | (" + hi + " << 16); }";
      return true;
    }
    default: {
      static const char *const cast[] = {"(uint32_t)(int32_t)(int8_t)", "(uint32_t)(int32_t)(int16_t)",
                                         "(uint32_t)(uint8_t)", "(uint32_t)(uint16_t)"};
      std::string e = std::string(cast[in.sub]) + v;
      if (add) e = fmt("r%d + ", in.n) + e;
      out_ += fmt("r%d = ", in.d) + e + ";";
      return true;
    }
    }
  }
  case Kind::Bfx: {
    uint32_t lsb = in.lsb, width = in.width;
    if (lsb + width > 32) return fail("unpredictable");
    if (in.sub)
      out_ += fmt("r%d = (uint32_t)((int32_t)(r%d << %u) >> %u);", in.d, in.n, 32 - lsb - width, 32 - width);
    else
      out_ += fmt("r%d = (r%d >> %u) & 0x%xu;", in.d, in.n, lsb, width == 32 ? 0xffffffffu : (1u << width) - 1);
    return true;
  }
  case Kind::Bfc:
  case Kind::Bfi: {
    uint32_t msb = in.width, lsb = in.lsb;
    if (msb < lsb) return fail("unpredictable");
    uint32_t mask = (msb == 31 ? 0xffffffffu : (1u << (msb + 1)) - 1) & ~((1u << lsb) - 1);
    if (in.kind == Kind::Bfc)
      out_ += fmt("r%d &= 0x%xu;", in.d, ~mask);
    else
      out_ += fmt("r%d = (r%d & 0x%xu) | ((r%d << %u) & 0x%xu);", in.d, in.d, ~mask, in.n, lsb, mask);
    return true;
  }
  case Kind::Sat: {
    std::string v = shift_imm(fmt("r%d", in.m), in.sht, in.sha, nullptr);
    if (in.sub)
      out_ += "{ uint32_t q_ = 0; " + fmt("r%d = (uint32_t)rc_ssat((int32_t)", in.d) + v +
              fmt(", %u, &q_); SETQ(q_); }", in.width + 1);
    else
      out_ += "{ uint32_t q_ = 0; " + fmt("r%d = rc_usat((int32_t)", in.d) + v + fmt(", %u, &q_); SETQ(q_); }", in.width);
    return true;
  }
  case Kind::Sat16: {
    std::string f = in.sub ? fmt("(uint32_t)rc_ssat((int16_t)(x_), %u, &q_)", in.width + 1)
                           : fmt("rc_usat((int16_t)(x_), %u, &q_)", in.width);
    std::string lo = f, hi = f;
    lo.replace(lo.find("x_"), 2, fmt("r%d", in.n));
    hi.replace(hi.find("x_"), 2, fmt("r%d >> 16", in.n));
    out_ += "{ uint32_t q_ = 0; " + fmt("r%d = (", in.d) + lo + " & 0xffffu) | (" + hi + " << 16); SETQ(q_); }";
    return true;
  }
  case Kind::QArith: {
    std::string nn = fmt("(int64_t)(int32_t)r%d", in.n);
    if (in.sub == QDADD || in.sub == QDSUB) nn = "(int64_t)rc_ssat(2 * " + nn + ", 32, &q_)";
    std::string op = (in.sub == QADD || in.sub == QDADD) ? " + " : " - ";
    out_ += "{ uint32_t q_ = 0; " + fmt("r%d = (uint32_t)rc_ssat((int64_t)(int32_t)r%d", in.d, in.m) + op + nn +
            ", 32, &q_); SETQ(q_); }";
    return true;
  }
  case Kind::Parallel:
    out_ += fmt("r%d = rc_parallel(%d, %d, r%d, r%d, &s->cpsr);", in.d, in.sub / 8, in.sub % 8, in.n, in.m);
    return true;
  case Kind::Sel:
    out_ += fmt("{ uint32_t ge_ = s->cpsr >> 16, k_ = 0; for (int i_ = 0; i_ < 4; i_++) k_ |= ((ge_ >> i_) & 1) ? "
                "0xffu << (8 * i_) : 0; r%d = (r%d & k_) | (r%d & ~k_); }",
                in.d, in.n, in.m);
    return true;
  case Kind::Pkh: {
    std::string v = shift_imm(fmt("r%d", in.m), in.sht, in.sha, nullptr);
    if (in.sub) out_ += fmt("r%d = (r%d & 0xffff0000u) | (", in.d, in.n) + v + " & 0xffffu);";
    else out_ += fmt("r%d = (r%d & 0xffffu) | (", in.d, in.n) + v + " & 0xffff0000u);";
    return true;
  }
  case Kind::Mrs:
    out_ += fmt("r%d = (s->cpsr & 0x0fffffffu) | fn << 31 | fz << 30 | fc << 29 | fv << 28;", in.d);
    return true;
  case Kind::Msr: { // APSR: lsb bit 1 = write NZCVQ, bit 0 = write GE
    std::string c = "{ uint32_t v_ = " + (in.op2 == Op2::Imm ? hex(in.imm) : fmt("r%d", in.n)) + "; ";
    if (in.lsb & 2)
      c += "fn = v_ >> 31; fz = (v_ >> 30) & 1; fc = (v_ >> 29) & 1; fv = (v_ >> 28) & 1; "
           "s->cpsr = (s->cpsr & ~0x08000000u) | (v_ & 0x08000000u); ";
    if (in.lsb & 1) c += "s->cpsr = (s->cpsr & ~0x000f0000u) | (v_ & 0x000f0000u); ";
    out_ += c + "}";
    return true;
  }
  case Kind::Mrc: {
    uint32_t raw = in.imm;
    uint32_t cp = (raw >> 8) & 15, opc1 = (raw >> 21) & 7, crn = (raw >> 16) & 15, crm = raw & 15,
             opc2 = (raw >> 5) & 7;
    if (cp != 15) return fail("coprocessor");
    if (in.t == 15) return fail("pc destination");
    if (opc1 == 0 && crn == 13 && crm == 0 && (opc2 == 2 || opc2 == 3)) {
      out_ += fmt("{ RC_SYNC_OUT(); r%d = rc_cp15_read(s, 0x%xu); }", in.t, raw & 0x0fffffffu);
      return true;
    }
    return fail("cp15 register");
  }
  case Kind::Mcr: {
    uint32_t raw = in.imm;
    uint32_t cp = (raw >> 8) & 15, opc1 = (raw >> 21) & 7, crn = (raw >> 16) & 15, crm = raw & 15,
             opc2 = (raw >> 5) & 7;
    if (cp == 15 && opc1 == 0 && crn == 7 &&
        ((crm == 10 && (opc2 == 4 || opc2 == 5)) || (crm == 5 && opc2 == 4))) {
      out_ += "__atomic_thread_fence(__ATOMIC_SEQ_CST);";
      return true;
    }
    return fail("coprocessor write");
  }
  case Kind::Undef: return fail("undefined");
  case Kind::Bkpt: return fail("bkpt");
  default: return fail("no emitter");
  }
}

// VFP: registers live in s->ext (FS/FD read them as float/double, SFS/SFD
// write them); C float arithmetic on arm64 rounds like VFP with FPSCR = 0
// (round to nearest, no flush-to-zero, NaNs propagated), which is what
// Android runs with. FPSCR's cumulative exception flags aren't tracked.
bool FunctionEmitter::emit_vfp(const Insn &in) {
  bool dp = in.dp;
  const char *ty = dp ? "double" : "float";
  auto get = [&](int i) { return fmt(dp ? "FD(%d)" : "FS(%d)", i); };
  auto set = [&](int i, const std::string &v) { return fmt(dp ? "SFD(%d, " : "SFS(%d, ", i) + v + ");"; };
  auto neg = [&](const std::string &v) { return std::string(dp ? "rc_negd(" : "rc_negf(") + v + ")"; };
  auto word = [&](int i) { return dp ? 2 * i : i; }; // first ext word of register i
  switch (in.kind) {
  case Kind::VLdSt: {
    bool load = in.sub;
    int w0 = word(in.vd), nw = dp ? 2 : 1;
    std::string a;
    if (in.n == 15) {
      uint32_t va = (in.pcval() & ~3u) + (in.U ? in.imm : -in.imm);
      uint32_t v[2] = {0, 0};
      if (load && elf_.read(va, v, 4u * uint32_t(nw)) && !elf_.is_writable(va) && !elf_.is_relocated(va) &&
          !(dp && elf_.is_relocated(va + 4))) {
        out_ += fmt("/* =%x */ s->ext[%d] = 0x%xu;", va, w0, v[0]);
        if (dp) out_ += fmt(" s->ext[%d] = 0x%xu;", w0 + 1, v[1]);
        return true;
      }
      a = addr(va);
    } else {
      a = fmt("(r%d %c 0x%xu)", in.n, in.U ? '+' : '-', in.imm);
    }
    out_ += "{ uint32_t a_ = " + a + "; ";
    for (int k = 0; k < nw; k++)
      out_ += load ? fmt("s->ext[%d] = LD32(a_ + %du); ", w0 + k, 4 * k) : fmt("ST32(a_ + %du, s->ext[%d]); ", 4 * k, w0 + k);
    out_ += "}";
    return true;
  }
  case Kind::VLdm: {
    if (in.n == 15) return fail("pc base");
    uint32_t words = in.imm * (dp ? 2 : 1);
    int w0 = word(in.vd);
    std::string start = in.U ? fmt("r%d", in.n) : fmt("(r%d - %uu)", in.n, 4 * words);
    out_ += "{ uint32_t a_ = " + start + "; ";
    for (uint32_t k = 0; k < words; k++)
      out_ += in.sub ? fmt("s->ext[%u] = LD32(a_ + %uu); ", w0 + k, 4 * k) : fmt("ST32(a_ + %uu, s->ext[%u]); ", 4 * k, w0 + k);
    if (in.W) out_ += fmt("r%d = r%d %c %uu; ", in.n, in.n, in.U ? '+' : '-', 4 * words);
    out_ += "}";
    return true;
  }
  case Kind::VMovS:
    if (in.t == 15) return fail("pc operand");
    out_ += in.sub ? fmt("r%d = s->ext[%d];", in.t, in.vn) : fmt("s->ext[%d] = r%d;", in.vn, in.t);
    return true;
  case Kind::VMov2: {
    int w = dp ? 2 * in.vm : in.vm;
    if (in.t == 15 || in.t2 == 15 || (!dp && in.vm == 31)) return fail("unpredictable registers");
    if (in.sub) {
      if (in.t == in.t2) return fail("Rt == Rt2");
      out_ += fmt("r%d = s->ext[%d]; r%d = s->ext[%d];", in.t, w, in.t2, w + 1);
    } else {
      out_ += fmt("s->ext[%d] = r%d; s->ext[%d] = r%d;", w, in.t, w + 1, in.t2);
    }
    return true;
  }
  case Kind::VMovScalar: {
    if (in.t == 15) return fail("pc operand");
    uint32_t bit = in.esize * in.imm, w = 2u * in.vd + bit / 32, sh = bit % 32;
    uint32_t mask = in.esize == 32 ? 0xffffffffu : (1u << in.esize) - 1;
    if (!in.sub) {
      out_ += fmt("s->ext[%u] = (s->ext[%u] & 0x%xu) | ((r%d & 0x%xu) << %u);", w, w, ~(mask << sh), in.t, mask, sh);
    } else if (in.esize == 32 || in.U) {
      out_ += fmt("r%d = (s->ext[%u] >> %u) & 0x%xu;", in.t, w, sh, mask);
    } else {
      out_ += fmt("r%d = (uint32_t)((int32_t)(s->ext[%u] << %u) >> %u);", in.t, w, 32 - sh - in.esize, 32 - in.esize);
    }
    return true;
  }
  case Kind::VDup: {
    if (in.t == 15) return fail("pc operand");
    std::string v = in.esize == 8 ? fmt("(r%d & 0xffu) * 0x01010101u", in.t)
                    : in.esize == 16 ? fmt("(r%d & 0xffffu) * 0x00010001u", in.t)
                                     : fmt("r%d", in.t);
    out_ += "{ uint32_t v_ = " + v + "; ";
    for (int k = 0; k < (in.Q ? 4 : 2); k++) out_ += fmt("s->ext[%d] = v_; ", 2 * in.vd + k);
    out_ += "}";
    return true;
  }
  case Kind::VMrs:
    if (in.t == 15)
      out_ += "fn = s->fpscr >> 31; fz = (s->fpscr >> 30) & 1; fc = (s->fpscr >> 29) & 1; fv = (s->fpscr >> 28) & 1;";
    else
      out_ += fmt("r%d = s->fpscr;", in.t);
    return true;
  case Kind::VMsr:
    if (in.t == 15) return fail("pc operand");
    out_ += fmt("s->fpscr = r%d;", in.t);
    return true;
  case Kind::VArith3: {
    std::string n = get(in.vn), m = get(in.vm), d = get(in.vd), e;
    auto op = [&](const char *o, const std::string &a, const std::string &b) {
      return std::string(dp ? "rc_d" : "rc_f") + o + "(" + a + ", " + b + ")";
    };
    switch (in.sub) {
    case VMUL: e = op("mul", n, m); break;
    case VNMUL: e = neg(op("mul", n, m)); break;
    case VADD: e = op("add", n, m); break;
    case VSUB: e = op("sub", n, m); break;
    case VDIV: e = op("div", n, m); break;
    case VMLA: e = op("add", d, "p_"); break;
    case VMLS: e = op("add", d, neg("p_")); break;
    case VNMLA: e = op("add", neg(d), neg("p_")); break;
    case VNMLS: e = op("add", neg(d), "p_"); break;
    case VFMA: e = (dp ? "__builtin_fma(" : "__builtin_fmaf(") + n + ", " + m + ", " + d + ")"; break;
    case VFMS: e = (dp ? "__builtin_fma(" : "__builtin_fmaf(") + neg(n) + ", " + m + ", " + d + ")"; break;
    case VFNMA: e = (dp ? "__builtin_fma(" : "__builtin_fmaf(") + neg(n) + ", " + m + ", " + neg(d) + ")"; break;
    case VFNMS: e = (dp ? "__builtin_fma(" : "__builtin_fmaf(") + n + ", " + m + ", " + neg(d) + ")"; break;
    default: return fail("no emitter");
    }
    bool mac = in.sub == VMLA || in.sub == VMLS || in.sub == VNMLA || in.sub == VNMLS;
    // Multiply-accumulate rounds the product first (-ffp-contract=off keeps it).
    if (mac) out_ += fmt("{ %s p_ = ", ty) + op("mul", n, m) + "; " + set(in.vd, e) + " }";
    else out_ += set(in.vd, e);
    return true;
  }
  case Kind::VArith2: {
    int d = word(in.vd), m = word(in.vm);
    switch (in.sub) {
    case VMOV:
      out_ += fmt("s->ext[%d] = s->ext[%d];", d, m);
      if (dp) out_ += fmt(" s->ext[%d] = s->ext[%d];", d + 1, m + 1);
      return true;
    case VABS:
    case VNEG: {
      const char *op = in.sub == VABS ? "& 0x7fffffffu" : "^ 0x80000000u";
      if (dp) out_ += fmt("s->ext[%d] = s->ext[%d]; s->ext[%d] = s->ext[%d] %s;", d, m, d + 1, m + 1, op);
      else out_ += fmt("s->ext[%d] = s->ext[%d] %s;", d, m, op);
      return true;
    }
    case VSQRT: out_ += set(in.vd, std::string(dp ? "__builtin_sqrt(" : "__builtin_sqrtf(") + get(in.vm) + ")"); return true;
    }
    return fail("no emitter");
  }
  case Kind::VMovImm:
    if (dp) out_ += fmt("s->ext[%d] = 0x%xu; s->ext[%d] = 0x%xu;", 2 * in.vd, in.x1, 2 * in.vd + 1, in.x2);
    else out_ += fmt("s->ext[%d] = 0x%xu;", in.vd, in.imm);
    return true;
  case Kind::VCmp:
    out_ += "FCMP(" + get(in.vd) + ", " + (in.sub ? std::string("0.0") : get(in.vm)) + ");";
    return true;
  case Kind::VCvt:
    switch (in.sub) {
    case CVT_F2F: // dp: double -> single
      out_ += dp ? fmt("SFS(%d, (float)FD(%d));", in.vd, in.vm) : fmt("SFD(%d, (double)FS(%d));", in.vd, in.vm);
      return true;
    case CVT_I2F: {
      std::string v = fmt(in.rot ? "(int32_t)s->ext[%d]" : "s->ext[%d]", in.vm);
      out_ += set(in.vd, fmt("(%s)", ty) + v);
      return true;
    }
    case CVT_F2I: {
      std::string v = dp ? fmt("FD(%d)", in.vm) : fmt("(double)FS(%d)", in.vm);
      out_ += fmt("s->ext[%d] = %s(", in.vd, in.rot ? "rc_f2s" : "rc_f2u") + v + fmt(", %u, 32);", in.width);
      return true;
    }
    }
    return fail("no emitter");
  case Kind::VCvtFixed: {
    int w = word(in.vd);
    bool sgn = in.rot;
    uint32_t size = in.esize, frac = in.imm;
    if (in.sub) { // to fixed: round toward zero, saturate, extend
      std::string v = (dp ? fmt("FD(%d)", in.vd) : fmt("(double)FS(%d)", in.vd)) + fmt(" * 0x1p%u", frac);
      out_ += fmt("{ uint32_t v_ = %s(", sgn ? "rc_f2s" : "rc_f2u") + v + fmt(", 1, %u); s->ext[%d] = v_; ", size, w);
      if (dp) out_ += fmt("s->ext[%d] = %s; ", w + 1, sgn ? "(uint32_t)((int32_t)v_ >> 31)" : "0u");
      out_ += "}";
    } else { // from fixed: the low size bits, scaled; one rounding
      std::string v = size == 16 ? fmt(sgn ? "(int16_t)s->ext[%d]" : "(uint16_t)s->ext[%d]", w)
                                 : fmt(sgn ? "(int32_t)s->ext[%d]" : "s->ext[%d]", w);
      out_ += set(in.vd, fmt("(%s)((double)", ty) + v + fmt(" * 0x1p-%u)", frac));
    }
    return true;
  }
  default: return fail("no emitter");
  }
}
// Advanced SIMD: lane by lane on s->ext (D register i is words 2i, 2i+1,
// lanes little-endian). Float ops use the standard-FPSCR helpers.
bool FunctionEmitter::emit_neon(const Insn &in) {
  int words = in.Q ? 4 : 2;
  int d = 2 * in.vd, n = 2 * in.vn, m = 2 * in.vm;
  std::string c;
  switch (in.sub) {
  case N_FADD:
  case N_FSUB:
  case N_FMUL:
  case N_FMLA:
  case N_FMLS: {
    static const char *const fn[] = {"rc_nadd", "rc_nsub", "rc_nmul", "rc_nmla", "rc_nmls"};
    bool acc = in.sub == N_FMLA || in.sub == N_FMLS;
    for (int w = 0; w < words; w++)
      c += acc ? fmt("s->ext[%d] = %s(s->ext[%d], s->ext[%d], s->ext[%d]); ", d + w, fn[in.sub], d + w, n + w, m + w)
               : fmt("s->ext[%d] = %s(s->ext[%d], s->ext[%d]); ", d + w, fn[in.sub], n + w, m + w);
    break;
  }
  case N_FMUL_S:
  case N_FMLA_S:
  case N_FMLS_S: {
    c = fmt("uint32_t k_ = s->ext[%d]; ", 2 * in.vm + int(in.imm));
    for (int w = 0; w < words; w++)
      c += in.sub == N_FMUL_S ? fmt("s->ext[%d] = rc_nmul(s->ext[%d], k_); ", d + w, n + w)
                              : fmt("s->ext[%d] = %s(s->ext[%d], s->ext[%d], k_); ", d + w,
                                    in.sub == N_FMLA_S ? "rc_nmla" : "rc_nmls", d + w, n + w);
    break;
  }
  case N_LOGIC: {
    static const char *const ex[8] = {"a_ & b_",          "a_ & ~b_",          "a_ | b_",
                                      "a_ | ~b_",         "a_ ^ b_",           "(a_ & d_) | (b_ & ~d_)",
                                      "(d_ & ~b_) | (a_ & b_)", "(d_ & b_) | (a_ & ~b_)"};
    for (int w = 0; w < words; w++)
      c += fmt("{ uint32_t a_ = s->ext[%d], b_ = s->ext[%d], d_ = s->ext[%d]; s->ext[%d] = %s; } ", n + w, m + w, d + w,
               d + w, ex[in.x1]);
    break;
  }
  case N_IADD:
  case N_ISUB: {
    bool add = in.sub == N_IADD;
    if (in.esize == 64) {
      for (int w = 0; w < words; w += 2)
        c += fmt("{ uint64_t r_ = DW(%d) %c DW(%d); SDW(%d, r_); } ", (n + w) / 2, add ? '+' : '-', (m + w) / 2,
                 (d + w) / 2);
    } else {
      for (int w = 0; w < words; w++)
        c += fmt("s->ext[%d] = %s(s->ext[%d], s->ext[%d], %d); ", d + w, add ? "rc_vadd" : "rc_vsub", n + w, m + w,
                 in.esize);
    }
    break;
  }
  case N_DUP_S: {
    uint32_t bit = in.esize * in.imm;
    std::string v = fmt("(s->ext[%d] >> %u)", 2 * in.vm + int(bit / 32), bit % 32);
    v = in.esize == 8 ? "(" + v + " & 0xffu) * 0x01010101u" : in.esize == 16 ? "(" + v + " & 0xffffu) * 0x00010001u" : v;
    c = "uint32_t v_ = " + v + "; ";
    for (int w = 0; w < words; w++) c += fmt("s->ext[%d] = v_; ", d + w);
    break;
  }
  case N_CVT: {
    static const char *const ex[4] = {"rc_fb((float)(int32_t)x_)", "rc_fb((float)x_)",
                                      "rc_f2s((double)rc_bf(rc_nfz(x_)), 1, 32)", "rc_f2u((double)rc_bf(rc_nfz(x_)), 1, 32)"};
    for (int w = 0; w < words; w++) c += fmt("{ uint32_t x_ = s->ext[%d]; s->ext[%d] = %s; } ", m + w, d + w, ex[in.x1]);
    break;
  }
  case N_MOVI:
    for (int w = 0; w < words; w++) c += fmt("s->ext[%d] = 0x%xu; ", d + w, (w & 1) ? in.x2 : in.x1);
    break;
  case N_LDST: {
    if (in.n == 15) return fail("pc base");
    bool load = in.width;
    uint32_t regs = in.x1, inc = in.x2, eb = in.esize / 8u;
    uint32_t total = 8 * regs * in.rot;
    c = fmt("uint32_t a_ = r%d; ", in.n);
    if (in.rot == 1) {
      for (uint32_t r = 0; r < regs; r++)
        for (int k = 0; k < 2; k++) {
          int wd = 2 * (in.vd + int(r)) + k;
          uint32_t off = 8 * r + 4 * uint32_t(k);
          c += load ? fmt("s->ext[%d] = LD32(a_ + %uu); ", wd, off) : fmt("ST32(a_ + %uu, s->ext[%d]); ", off, wd);
        }
    } else { // VLD2/VST2: element pairs, de-interleaved into registers d+r and d+inc+r
      uint32_t per = 8 / eb;
      c += fmt("for (int r_ = 0; r_ < %u; r_++) for (int e_ = 0; e_ < %u; e_++, a_ += %u) { ", regs, per, 2 * eb);
      if (load)
        c += fmt("memcpy(LANE(%d + r_, e_, %u), m + a_, %u); memcpy(LANE(%u + r_, e_, %u), m + a_ + %u, %u); } ", in.vd, eb,
                 eb, in.vd + inc, eb, eb, eb);
      else
        c += fmt("memcpy(m + a_, LANE(%d + r_, e_, %u), %u); memcpy(m + a_ + %u, LANE(%u + r_, e_, %u), %u); } ", in.vd, eb,
                 eb, eb, in.vd + inc, eb, eb);
    }
    if (in.m == 13) c += fmt("r%d += %uu; ", in.n, total);
    else if (in.m != 15) c += fmt("r%d += r%d; ", in.n, in.m);
    break;
  }
  default: return fail("neon (not yet)");
  }
  out_ += "{ " + c + "}";
  return true;
}

// ---- Output ----------------------------------------------------------------

size_t write_module(const std::string &out_dir, const std::string &prefix, const std::string &soname,
                    const std::vector<FunctionResult> &funcs, int shards, std::string *err) {
  std::vector<const FunctionResult *> ok;
  for (const FunctionResult &f : funcs)
    if (!f.code.empty()) ok.push_back(&f);
  std::sort(ok.begin(), ok.end(), [](auto *a, auto *b) { return a->spec.addr < b->spec.addr; });

  std::string head = "/* Generated by droidlift-recomp from " + soname +
                     ". Derived from the game binary: never commit. */\n";
  std::vector<std::pair<std::string, std::string>> files;

  std::string decls = head + "#ifndef " + prefix + "_DECLS_H\n#define " + prefix + "_DECLS_H\n#include \"rc_gen.h\"\n\n";
  decls += "extern uint32_t " + prefix + "_base;\n";
  for (auto *f : ok) decls += "void " + f->c_name + "(rc_state *s);\n";
  decls += "#endif\n";
  files.push_back({prefix + "_decls.h", decls});

  size_t total = 0;
  for (auto *f : ok) total += f->code.size();
  int n = shards > 0 ? shards : 1;
  size_t per = total / size_t(n) + 1;
  std::vector<std::string> bodies(static_cast<size_t>(n));
  size_t cur = 0, acc = 0;
  for (auto *f : ok) {
    if (acc >= per * (cur + 1) && cur + 1 < size_t(n)) cur++;
    bodies[cur] += f->code + "\n";
    acc += f->code.size();
  }
  for (int i = 0; i < n; i++) {
    std::string name = shards > 0 ? fmt("%s_funcs_%03d.c", prefix.c_str(), i) : prefix + "_funcs.c";
    files.push_back({name, head + "#include \"" + prefix + "_decls.h\"\n\n" + bodies[size_t(i)]});
  }

  // One table row per entry, sorted by address.
  std::vector<std::tuple<uint32_t, const FunctionResult *>> rows;
  for (auto *f : ok)
    for (uint32_t e : f->entries) rows.push_back({e, f});
  std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return std::get<0>(a) < std::get<0>(b); });
  rows.erase(std::unique(rows.begin(), rows.end(), [](auto &a, auto &b) { return std::get<0>(a) == std::get<0>(b); }),
             rows.end());
  std::string tc = head + "#include \"" + prefix + "_decls.h\"\n\nuint32_t " + prefix + "_base;\n\nstatic const rc_func funcs[] = {\n";
  for (auto &[e, f] : rows) {
    bool primary = e == f->spec.addr;
    tc += fmt("  {0x%08xu, %s, %s},\n", e, f->c_name.c_str(),
              primary ? ("\"" + f->spec.name + "\"").c_str() : "0");
  }
  if (rows.empty()) tc += "  {0, 0, 0},\n";
  tc += "};\n\nconst rc_module " + prefix + "_module = {\"" + soname + "\", &" + prefix + "_base, funcs, " +
        fmt("%zuu", rows.size()) + "};\n";
  files.push_back({prefix + "_table.c", tc});

  size_t bytes = 0;
  for (auto &[name, text] : files) {
    std::string path = out_dir + "/" + name;
    // Leave unchanged files alone so the build doesn't recompile them.
    std::ifstream in(path, std::ios::binary);
    std::string old((std::istreambuf_iterator<char>(in)), {});
    bytes += text.size();
    if (in && old == text) continue;
    std::ofstream o(path, std::ios::binary);
    o << text;
    if (!o) {
      *err = "cannot write " + path;
      return 0;
    }
  }
  return bytes;
}

} // namespace recomp
