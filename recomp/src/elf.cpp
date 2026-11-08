#include "elf.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "game_patches.h"

namespace recomp {

namespace {

constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t SHT_REL = 9;
constexpr uint32_t SHT_DYNSYM = 11;
constexpr uint32_t SHT_ARM_EXIDX = 0x70000001;
constexpr uint8_t STT_FUNC = 2;

template <typename T> T get(const std::vector<uint8_t> &d, size_t off) {
  T v{};
  if (off + sizeof v <= d.size()) std::memcpy(&v, d.data() + off, sizeof v);
  return v;
}

} // namespace

std::optional<std::string> Elf::load(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return "cannot open " + path;
  data_.assign(std::istreambuf_iterator<char>(f), {});
  const auto &d = data_;
  if (d.size() < 52 || std::memcmp(d.data(), "\x7f" "ELF", 4) != 0) return path + " is not an ELF file";
  if (d[4] != 1 || d[5] != 1) return path + " is not 32-bit little-endian";
  if (get<uint16_t>(d, 18) != 40) return path + " is not an ARM ELF";

  uint32_t phoff = get<uint32_t>(d, 28), shoff = get<uint32_t>(d, 32);
  uint16_t phentsize = get<uint16_t>(d, 42), phnum = get<uint16_t>(d, 44);
  uint16_t shentsize = get<uint16_t>(d, 46), shnum = get<uint16_t>(d, 48), shstrndx = get<uint16_t>(d, 50);

  for (uint16_t i = 0; i < phnum; i++) {
    size_t p = phoff + size_t(i) * phentsize;
    if (get<uint32_t>(d, p) != PT_LOAD) continue;
    Segment s{};
    s.offset = get<uint32_t>(d, p + 4);
    s.vaddr = get<uint32_t>(d, p + 8);
    s.filesz = get<uint32_t>(d, p + 16);
    s.memsz = get<uint32_t>(d, p + 20);
    s.flags = get<uint32_t>(d, p + 24);
    segs_.push_back(s);
  }

  // The loader's load-time game patches (loader/src/game_patches.h),
  // so the C is generated from the bytes the game runs with: code and
  // literal-pool constants folded here see them too. They patch the first
  // PT_LOAD, which these libraries map from file offset 0 (vaddr == offset).
  if (!segs_.empty() && segs_[0].offset == 0 && segs_[0].vaddr == 0 && segs_[0].filesz <= d.size()) {
    std::string soname = path.substr(path.find_last_of('/') + 1);
    game_patches_apply(soname.c_str(), data_.data(), segs_[0].filesz);
  }

  struct Sec {
    uint32_t name, type, addr, offset, size, link, entsize;
  };
  std::vector<Sec> secs;
  for (uint16_t i = 0; i < shnum; i++) {
    size_t p = shoff + size_t(i) * shentsize;
    secs.push_back({get<uint32_t>(d, p), get<uint32_t>(d, p + 4), get<uint32_t>(d, p + 12), get<uint32_t>(d, p + 16),
                    get<uint32_t>(d, p + 20), get<uint32_t>(d, p + 24), get<uint32_t>(d, p + 36)});
  }
  uint32_t strtab = shstrndx < secs.size() ? secs[shstrndx].offset : 0;
  for (const Sec &s : secs) {
    std::string name = strtab && strtab + s.name < d.size() ? reinterpret_cast<const char *>(d.data() + strtab + s.name) : "";
    secs_.push_back({name, s.type, s.addr, s.offset, s.size});
    if (name == ".text") text_lo_ = s.addr, text_hi_ = s.addr + s.size;
  }
  for (const Sec &s : secs) {
    if (s.type == SHT_DYNSYM && s.link < secs.size()) {
      const Sec &str = secs[s.link];
      for (uint32_t o = s.offset; o + 16 <= s.offset + s.size; o += 16) {
        uint32_t name = get<uint32_t>(d, o), value = get<uint32_t>(d, o + 4), size = get<uint32_t>(d, o + 8);
        uint8_t info = d[o + 12];
        uint16_t shndx = get<uint16_t>(d, o + 14);
        const char *nm = reinterpret_cast<const char *>(d.data() + str.offset + name);
        Symbol sym{nm, value, size, (info & 0xf) == STT_FUNC, shndx != 0};
        dynsyms_.push_back(sym);
        if (sym.defined) syms_.push_back(sym);
      }
    } else if (s.type == SHT_ARM_EXIDX) {
      for (uint32_t i = 0; i + 8 <= s.size; i += 8) {
        uint32_t w = get<uint32_t>(d, s.offset + i);
        int32_t rel = int32_t(w << 1) >> 1; // prel31
        exidx_.push_back(s.addr + i + uint32_t(rel));
      }
    }
  }
  for (const Sec &s : secs) {
    if (s.type != SHT_REL) continue;
    for (uint32_t o = s.offset; o + 8 <= s.offset + s.size; o += 8) {
      uint32_t off = get<uint32_t>(d, o), info = get<uint32_t>(d, o + 4);
      relocs_.push_back({off, info & 0xff, info >> 8});
      relocated_.insert(off);
      uint32_t type = info & 0xff, sym = info >> 8;
      if ((type == R_ARM_JUMP_SLOT || type == R_ARM_GLOB_DAT) && sym && sym < dynsyms_.size())
        import_slots_[off] = dynsyms_[sym].name;
    }
  }
  std::sort(exidx_.begin(), exidx_.end());
  exidx_.erase(std::unique(exidx_.begin(), exidx_.end()), exidx_.end());
  return std::nullopt;
}

const Section *Elf::section(const std::string &name) const {
  for (const Section &s : secs_)
    if (s.name == name) return &s;
  return nullptr;
}

const Symbol *Elf::find_symbol(const std::string &name) const {
  for (const Symbol &s : syms_)
    if (s.name == name) return &s;
  return nullptr;
}

const Segment *Elf::segment_for(uint32_t vaddr) const {
  for (const Segment &s : segs_)
    if (vaddr >= s.vaddr && vaddr - s.vaddr < s.memsz) return &s;
  return nullptr;
}

bool Elf::read(uint32_t vaddr, void *out, uint32_t n) const {
  const Segment *s = segment_for(vaddr);
  if (!s || vaddr - s->vaddr + uint64_t(n) > s->filesz) return false;
  size_t off = s->offset + (vaddr - s->vaddr);
  if (off + n > data_.size()) return false;
  std::memcpy(out, data_.data() + off, n);
  return true;
}

std::optional<uint32_t> Elf::read32(uint32_t vaddr) const {
  uint32_t v;
  if (!read(vaddr, &v, 4)) return std::nullopt;
  return v;
}

bool Elf::is_executable(uint32_t vaddr) const {
  const Segment *s = segment_for(vaddr);
  return s && (s->flags & 1);
}

bool Elf::is_writable(uint32_t vaddr) const {
  const Segment *s = segment_for(vaddr);
  return s && (s->flags & 2);
}

uint32_t Elf::image_end() const {
  uint32_t end = 0;
  for (const Segment &s : segs_) end = std::max(end, s.vaddr + s.memsz);
  return end;
}

} // namespace recomp
