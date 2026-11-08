// Whole-library function discovery.
//
// Function starts come from the sized dynamic symbols, .ARM.exidx, the PLT
// stubs, code pointers in relocated data (vtables, callbacks, init_array),
// and then the call graph: every BL/BLX target, constant tail-branch target
// and pc-relative code address found while walking the functions already
// known. A start's instruction set comes from symbol values and pointers (bit
// 0) and from the BL/BLX that reach it; exidx-only starts nothing reaches are
// decoded both ways and the cleaner one wins.
//
// A function's range ends at its symbol size, or else at the next "strong"
// start (symbol, exidx, PLT). Pointers into the middle of a known function
// become extra entry roots of that function instead of new functions.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "elf.hpp"
#include "emit.hpp"

namespace recomp {

enum Source : uint8_t {
  SRC_SYMBOL = 1,
  SRC_EXIDX = 2,
  SRC_PLT = 4,
  SRC_RELOC = 8,   // pointer in relocated data
  SRC_CALL = 16,   // BL/BLX target
  SRC_TAIL = 32,   // constant branch target outside the caller
  SRC_POINTER = 64, // pc-relative address computed in code
  SRC_TABLE = 128   // pointer stored in .text (an ARM jump table)
};

struct Func {
  FunctionSpec spec;  // addr carries the mode once known
  int mode = -1;      // -1 unknown, 0 ARM, 1 Thumb
  bool guessed = false; // mode chosen by trying both decoders
  uint8_t sources = 0;
  std::set<uint32_t> roots;
  // Candidates of the other mode that fell in this (inferred) range: looked
  // at again if the range is split before them.
  std::vector<std::pair<uint32_t, uint8_t>> clashes;
  FunctionResult res; // last analysis (no code)
  bool analyzed = false;
};

class Program {
public:
  Program(const Elf &elf, const std::string &prefix);

  void discover();
  // Analyses and emits every function (in parallel), in address order.
  std::vector<FunctionResult> emit_all(const std::set<uint32_t> *only = nullptr);

  const std::map<uint32_t, Func> &funcs() const { return funcs_; }
  const ModuleContext &context() const { return ctx_; }
  // Names of PLT stubs: plain address -> "symbol@plt".
  const std::map<uint32_t, std::string> &plt() const { return plt_; }
  // Direct-call closure of a function (interworking addresses), or empty if
  // it exceeds `limit` functions.
  std::set<uint32_t> closure(uint32_t iw, size_t limit) const;
  const Func *find(uint32_t iw) const;

private:
  const Elf &elf_;
  ModuleContext ctx_;
  std::map<uint32_t, Func> funcs_;     // by plain start address
  std::set<uint32_t> strong_;          // plain starts that bound ranges
  std::map<uint32_t, std::string> plt_;
  std::set<uint32_t> noreturn_;        // interworking addresses
  std::vector<uint32_t> queue_;        // plain starts to (re)analyse
  std::vector<std::pair<uint32_t, uint8_t>> pending_; // candidates waiting for a mode
  int default_mode_ = 1;

  void add_strong(uint32_t plain, int mode, uint32_t end, const std::string &name, uint8_t src);
  void parse_plt();
  void candidate(uint32_t iw, uint8_t src);
  bool veneer_half(uint32_t p, int mode) const;
  uint32_t range_end(uint32_t plain) const;
  Func *containing(uint32_t plain);
  void set_mode(Func &f, int mode);
  void analyze(Func &f);
  void drain();
  void guess_modes();
};

} // namespace recomp
