// Read-only view of a 32-bit little-endian ARM shared object: the
// loadable segments, sections, dynamic symbols, dynamic relocations and the
// function starts recorded in .ARM.exidx.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace recomp {

struct Segment {
  uint32_t vaddr, memsz, filesz, offset, flags; // flags: PF_X=1, PF_W=2, PF_R=4
};

struct Section {
  std::string name;
  uint32_t type, addr, offset, size;
};

struct Symbol {
  std::string name;
  uint32_t value; // bit 0 set = Thumb
  uint32_t size;
  bool func;
  bool defined;
};

struct Reloc {
  uint32_t offset; // vaddr of the patched word
  uint32_t type;   // R_ARM_*
  uint32_t sym;    // dynsym index
};

constexpr uint32_t R_ARM_ABS32 = 2, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22, R_ARM_RELATIVE = 23;

class Elf {
public:
  // Returns an error message, or std::nullopt on success.
  std::optional<std::string> load(const std::string &path);

  const std::vector<Segment> &segments() const { return segs_; }
  const std::vector<Section> &sections() const { return secs_; }
  const Section *section(const std::string &name) const;
  // Defined dynamic symbols.
  const std::vector<Symbol> &symbols() const { return syms_; }
  // All dynamic symbols by index (undefined ones are imports).
  const std::vector<Symbol> &dynsyms() const { return dynsyms_; }
  const std::vector<Reloc> &relocs() const { return relocs_; }
  // Function starts from .ARM.exidx, sorted. exidx does not record the
  // instruction set, so these carry no Thumb bit.
  const std::vector<uint32_t> &exidx_starts() const { return exidx_; }
  // GOT slot address -> symbol name (JUMP_SLOT / GLOB_DAT; imports, and own
  // exported symbols called through the PLT).
  const std::map<uint32_t, std::string> &import_slots() const { return import_slots_; }

  const Symbol *find_symbol(const std::string &name) const;

  // Bytes at a virtual address, if they lie in a segment's file image.
  bool read(uint32_t vaddr, void *out, uint32_t n) const;
  std::optional<uint32_t> read32(uint32_t vaddr) const;
  bool is_executable(uint32_t vaddr) const;
  bool is_writable(uint32_t vaddr) const;
  // Inside .text (code, but also literal pools and inline tables).
  bool is_text(uint32_t vaddr) const { return vaddr >= text_lo_ && vaddr < text_hi_; }
  // A dynamic relocation patches the word at vaddr when the image loads.
  bool is_relocated(uint32_t vaddr) const { return relocated_.count(vaddr) != 0; }
  // Highest virtual address any segment occupies, plus one.
  uint32_t image_end() const;

private:
  std::vector<uint8_t> data_;
  std::vector<Segment> segs_;
  std::vector<Section> secs_;
  std::vector<Symbol> syms_, dynsyms_;
  std::vector<Reloc> relocs_;
  std::vector<uint32_t> exidx_;
  std::set<uint32_t> relocated_;
  std::map<uint32_t, std::string> import_slots_;
  uint32_t text_lo_ = 0, text_hi_ = 0;
  const Segment *segment_for(uint32_t vaddr) const;
};

} // namespace recomp
