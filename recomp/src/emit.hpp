// C emitter: one guest function in, one C function out.
//
// A function is a start address (bit 0 = Thumb), an end (control that runs
// past it leaves through the dispatcher) and extra roots: other addresses in
// the range it can be entered at (pointers taken in data, exception landing
// pads). The emitter walks the code reachable from those roots, so literal
// pools are never decoded, and emits one C function. Instructions it can't
// translate become an exit to the dispatcher (and so to the interpreter
// fallback) at that address, with the next instruction as a resume entry; the
// rest of the function is still recompiled.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "decode.hpp"
#include "elf.hpp"

namespace recomp {

struct FunctionSpec {
  std::string name;
  uint32_t addr = 0;              // ELF virtual address, bit 0 = Thumb
  uint32_t end = 0;               // first address past the function
  std::vector<uint32_t> roots;    // extra entry addresses (no Thumb bit; same mode)
};

// What the emitter needs to know about the rest of the module.
struct ModuleContext {
  std::string prefix;
  // C name of the recompiled function whose primary entry is the interworking
  // address iw, or "" if there is none (calls then go through the dispatcher).
  std::function<std::string(uint32_t iw)> callee;
  // True if a call to iw never returns (abort, __cxa_throw, ...).
  std::function<bool(uint32_t iw)> noreturn;
};

struct FunctionResult {
  FunctionSpec spec;
  std::string c_name;
  std::string code;   // C definition (when emitted)
  bool entry_ok = false; // the first instruction decodes
  uint32_t insns = 0;    // reachable instructions
  uint32_t bytes = 0;    // guest bytes they cover

  std::vector<uint32_t> entries; // interworking addresses the function can be entered at
  std::set<uint32_t> calls;      // direct call targets (interworking)
  std::set<uint32_t> tails;      // constant jump targets outside the function (interworking)
  std::set<uint32_t> pointers;   // code addresses computed in the function (interworking)
  std::map<uint32_t, uint8_t> insn_addrs; // reachable instructions: address -> size

  // Instructions left to the interpreter: "mnemonic: reason" -> count. Ones
  // reached only by falling through a call (likely after a noreturn call, so
  // likely data) are counted apart.
  std::map<std::string, uint32_t> unsupported, unsupported_after_call;
  std::map<std::string, uint32_t> mnemonics; // recompiled instructions by mnemonic
  uint32_t n_unsupported = 0, n_after_call = 0, n_vfp = 0, n_neon = 0;
  uint32_t n_invalid = 0; // undecodable or UNDEFINED encodings among them
  uint32_t jump_tables = 0, computed_jumps = 0;
};

class FunctionEmitter {
public:
  FunctionEmitter(const Elf &elf, const ModuleContext &ctx) : elf_(elf), ctx_(ctx) {}
  // Analyses the function and, if `emit` is set, generates its C.
  FunctionResult run(const FunctionSpec &spec, bool emit);

private:
  const Elf &elf_;
  const ModuleContext &ctx_;

  // Per function.
  FunctionSpec spec_;
  bool thumb_ = false;
  std::map<uint32_t, Insn> insns_;
  std::set<uint32_t> labels_;
  std::set<uint32_t> merges_; // labels where control flow merges (not call returns)
  std::set<uint32_t> after_call_; // reached only by falling through a call
  std::set<uint32_t> entries_;    // plain addresses
  std::map<uint32_t, std::vector<uint32_t>> tables_; // jump table insn -> targets by index
  std::string out_;
  std::string why_; // reason the current instruction is unsupported
  FunctionResult *res_ = nullptr;

  uint32_t start() const { return spec_.addr & ~1u; }
  uint32_t tbit() const { return thumb_ ? 1u : 0u; }
  bool in_range(uint32_t a) const { return a >= start() && a < spec_.end; }

  bool fetch(uint32_t a, bool in_it, Insn *out) const;
  void analyze();
  void successors(const Insn &in, bool after_call, std::vector<std::pair<uint32_t, bool>> &work);
  bool jump_table(const Insn &in, std::vector<uint32_t> *targets) const;
  bool pic_table(const Insn &in, std::vector<uint32_t> *targets) const;
  int table_bound(const Insn &in, int reg) const;
  void find_pointers();
  void emit_function();

  // Code builders; see emit.cpp. Each returns false (with why_) when the
  // instruction can't be translated.
  bool emit_insn(const Insn &in);
  std::string R(const Insn &in, int r) const;
  std::string addr(uint32_t plain_or_iw) const;
  std::string branch_to(const Insn &in, uint32_t target_iw) const;
  bool fail(const std::string &why);
  bool call_idiom(const Insn &in) const;
  std::string jump(const Insn &in, const std::string &v);
  bool emit_dp(const Insn &in);
  bool emit_mul(const Insn &in);
  bool emit_mul_misc(const Insn &in);
  bool emit_ldst(const Insn &in);
  bool emit_ldm_stm(const Insn &in);
  bool emit_exclusive(const Insn &in);
  bool emit_misc(const Insn &in);
  bool emit_table(const Insn &in);
  bool emit_vfp(const Insn &in);
  bool emit_neon(const Insn &in);
};

// Writes a module: shards <prefix>_funcs_NNN.c (functions spread evenly by
// size), <prefix>_decls.h (prototypes) and <prefix>_table.c (the address
// table rc_runtime dispatches through, one row per entry). With shards = 0,
// one <prefix>_funcs.c. Returns the bytes written.
size_t write_module(const std::string &out_dir, const std::string &prefix, const std::string &soname,
                    const std::vector<FunctionResult> &funcs, int shards, std::string *err);

} // namespace recomp
