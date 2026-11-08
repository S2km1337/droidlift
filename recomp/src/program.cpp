#include "program.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>

namespace recomp {

namespace {

uint32_t ror32(uint32_t v, uint32_t n) {
  n &= 31;
  return n ? (v >> n) | (v << (32 - n)) : v;
}

bool is_noreturn_name(const std::string &n) {
  static const char *const names[] = {"abort", "exit", "_exit", "_Exit", "__cxa_throw", "__cxa_rethrow",
                                      "_Unwind_Resume", "__cxa_end_cleanup", "longjmp", "siglongjmp", "_longjmp",
                                      "__stack_chk_fail", "__assert", "__assert2", "__cxa_pure_virtual",
                                      "__cxa_bad_cast", "__cxa_bad_typeid", "__cxa_call_unexpected", "pthread_exit",
                                      "_ZSt9terminatev", "__aeabi_unwind_cpp_pr0_noreturn", "__fortify_chk_fail",
                                      "__gnu_cxx::__verbose_terminate_handler", "luaD_throw"};
  for (const char *x : names)
    if (n == x) return true;
  // std::__throw_length_error etc.: _ZSt<len>__throw_...
  return n.rfind("_ZSt", 0) == 0 && n.find("__throw_") != std::string::npos;
}

std::string sub_name(uint32_t a) {
  char b[32];
  std::snprintf(b, sizeof b, "sub_%x", a);
  return b;
}

} // namespace

Program::Program(const Elf &elf, const std::string &prefix) : elf_(elf) {
  ctx_.prefix = prefix;
  ctx_.callee = [this](uint32_t iw) -> std::string {
    const Func *f = find(iw);
    if (!f || !f->res.entry_ok || f->spec.addr != iw) return "";
    return f->res.c_name;
  };
  ctx_.noreturn = [this](uint32_t iw) { return noreturn_.count(iw) != 0; };
}

const Func *Program::find(uint32_t iw) const {
  auto it = funcs_.find(iw & ~1u);
  if (it == funcs_.end() || it->second.mode != int(iw & 1)) return nullptr;
  return &it->second;
}

void Program::add_strong(uint32_t plain, int mode, uint32_t end, const std::string &name, uint8_t src) {
  strong_.insert(plain);
  Func &f = funcs_[plain];
  if (f.spec.name.empty() || (src & (SRC_SYMBOL | SRC_PLT))) f.spec.name = name;
  if (end) f.spec.end = std::max(f.spec.end, end);
  f.sources |= src;
  if (mode >= 0 && f.mode < 0) f.mode = mode;
  f.spec.addr = plain | (f.mode == 1 ? 1u : 0u);
}

void Program::parse_plt() {
  const Section *s = elf_.section(".plt");
  if (!s) return;
  for (uint32_t a = s->addr; a + 12 <= s->addr + s->size; a += 4) {
    auto w1 = elf_.read32(a), w2 = elf_.read32(a + 4), w3 = elf_.read32(a + 8);
    if (!w1 || !w2 || !w3) break;
    // add ip, pc, #X; add ip, ip, #Y; ldr pc, [ip, #Z]!
    if ((*w1 & 0xfffff000u) != 0xe28fc000u || (*w2 & 0xfffff000u) != 0xe28cc000u ||
        (*w3 & 0xfffff000u) != 0xe5bcf000u)
      continue;
    uint32_t x = ror32(*w1 & 0xff, ((*w1 >> 8) & 15) * 2), y = ror32(*w2 & 0xff, ((*w2 >> 8) & 15) * 2);
    uint32_t slot = a + 8 + x + y + (*w3 & 0xfff);
    auto it = elf_.import_slots().find(slot);
    std::string name = (it != elf_.import_slots().end() ? it->second : sub_name(a)) + "@plt";
    plt_[a] = name;
    add_strong(a, 0, a + 12, name, SRC_PLT);
    if (it != elf_.import_slots().end() && is_noreturn_name(it->second)) noreturn_.insert(a);
    a += 8;
  }
}

uint32_t Program::range_end(uint32_t plain) const {
  auto it = funcs_.find(plain);
  if (it != funcs_.end() && it->second.spec.end > plain && (it->second.sources & (SRC_SYMBOL | SRC_PLT)))
    return it->second.spec.end;
  auto nx = strong_.upper_bound(plain);
  uint32_t end = nx == strong_.end() ? 0xffffffffu : *nx;
  // Stay inside the section the start is in.
  for (const Section &s : elf_.sections())
    if (plain >= s.addr && plain < s.addr + s.size && (s.name == ".text" || s.name == ".plt"))
      end = std::min(end, s.addr + s.size);
  return end;
}

Func *Program::containing(uint32_t plain) {
  auto it = funcs_.upper_bound(plain);
  if (it == funcs_.begin()) return nullptr;
  --it;
  if (plain >= it->first && plain < it->second.spec.end) return &it->second;
  return nullptr;
}

void Program::set_mode(Func &f, int mode) {
  f.mode = mode;
  f.spec.addr = (f.spec.addr & ~1u) | uint32_t(mode);
  queue_.push_back(f.spec.addr & ~1u);
}

// p is half of an interworking veneer: Thumb "bx pc" at a word boundary
// (mode 1), or the ARM code right after one (mode 0).
bool Program::veneer_half(uint32_t p, int mode) const {
  uint16_t hw = 0;
  uint32_t at = mode ? p : p - 4;
  return !(at & 3) && elf_.read(at, &hw, 2) && hw == 0x4778;
}

// A code address seen somewhere (interworking: bit 0 = Thumb).
void Program::candidate(uint32_t iw, uint8_t src) {
  uint32_t p = iw & ~1u;
  int mode = int(iw & 1);
  if (!elf_.is_text(p) && !plt_.count(p)) return;
  if (!mode && (p & 3)) return; // ARM code is word aligned
  auto it = funcs_.find(p);
  if (it != funcs_.end()) {
    Func &f = it->second;
    f.sources |= src;
    if (f.mode < 0) set_mode(f, mode);
    return;
  }
  Func *c = containing(p);
  // Call targets and pointers kept in data name function starts, unless they
  // fall inside a function whose symbol gives its exact size.
  bool starts = (src & (SRC_CALL | SRC_RELOC)) && !(c && (c->sources & (SRC_SYMBOL | SRC_PLT)));
  // A pointer computed in code that lands past everything the walk of an
  // inferred range reached is the next function (ranges end at the next
  // symbol or exidx entry, which static functions often lack).
  if (src == SRC_POINTER && c && !(c->sources & (SRC_SYMBOL | SRC_PLT)) && !starts) {
    // That needs the walk: until the range is analyzed, wait.
    if (!c->analyzed) {
      pending_.push_back({iw, src});
      return;
    }
    if (c->res.insn_addrs.empty() || p > c->res.insn_addrs.rbegin()->first) starts = true;
  }
  if (c && !starts) {
    if (c->mode < 0) {
      pending_.push_back({iw, src});
      return;
    }
    if (c->mode != mode) {
      // A mode clash: not a real entry here, except for the linker's
      // Thumb-to-ARM interworking veneers ("bx pc; nop" in Thumb, then ARM
      // code at the next word). They sit side by side with no symbols or
      // exidx entries, often inside a range whose mode was guessed, and are
      // reached by tail branches: each half starts a function.
      if (!(src & SRC_TAIL) || (c->sources & (SRC_SYMBOL | SRC_PLT)) || !veneer_half(p, mode)) {
        // A split of this range by a veneer may yet put it in a function of
        // its mode (an ARM half followed by more ARM code).
        if (!(c->sources & (SRC_SYMBOL | SRC_PLT))) c->clashes.push_back({iw, src});
        return;
      }
      starts = true;
    } else {
      // Pointers computed in code count only where the walk already found an
      // instruction (they may point at data otherwise); tail branches and
      // pointers in tables are trusted.
      if (src == SRC_POINTER && !c->res.insn_addrs.count(p)) return;
      if (c->roots.insert(p).second) {
        c->spec.roots.assign(c->roots.begin(), c->roots.end());
        queue_.push_back(c->spec.addr & ~1u);
      }
      return;
    }
  }
  // A new function: from here to the next strong start.
  if (starts) strong_.insert(p);
  Func &f = funcs_[p];
  f.spec.addr = p;
  f.spec.name = sub_name(p);
  f.sources |= src;
  f.spec.end = range_end(p);
  set_mode(f, mode);
  if (c) {
    // It splits the inferred range of the function it was in.
    c->spec.end = std::min(c->spec.end, p);
    for (auto r = c->roots.lower_bound(p); r != c->roots.end();) {
      if (f.mode == c->mode && *r != p) f.roots.insert(*r);
      // A split by code of the other mode (a veneer): the roots past it are
      // looked at again, as the branch targets they were.
      else if (*r != p) pending_.push_back({*r | uint32_t(c->mode), SRC_TAIL});
      r = c->roots.erase(r);
    }
    for (auto k = c->clashes.begin(); k != c->clashes.end();) {
      if ((k->first & ~1u) >= p) {
        pending_.push_back(*k);
        k = c->clashes.erase(k);
      } else {
        ++k;
      }
    }
    c->spec.roots.assign(c->roots.begin(), c->roots.end());
    f.spec.roots.assign(f.roots.begin(), f.roots.end());
    if (c->mode >= 0) queue_.push_back(c->spec.addr & ~1u);
  }
}

void Program::analyze(Func &f) {
  FunctionEmitter em(elf_, ctx_);
  f.spec.end = f.spec.end ? f.spec.end : range_end(f.spec.addr & ~1u);
  f.res = em.run(f.spec, false);
  f.analyzed = true;
  for (uint32_t t : f.res.calls) candidate(t, SRC_CALL);
  for (uint32_t t : f.res.tails) candidate(t, SRC_TAIL);
  for (uint32_t t : f.res.pointers) candidate(t, SRC_POINTER);
}

void Program::drain() {
  while (!queue_.empty()) {
    uint32_t p = queue_.back();
    queue_.pop_back();
    auto it = funcs_.find(p);
    if (it == funcs_.end() || it->second.mode < 0) continue;
    analyze(it->second);
    // Candidates that waited for this function's mode.
    if (!pending_.empty()) {
      auto pend = std::move(pending_);
      pending_.clear();
      for (auto [iw, src] : pend) candidate(iw, src);
    }
  }
}

// exidx starts nothing reached: decode both ways, keep the cleaner one.
void Program::guess_modes() {
  for (auto &[p, f] : funcs_) {
    if (f.mode >= 0) continue;
    FunctionEmitter em(elf_, ctx_);
    FunctionSpec s = f.spec;
    s.end = range_end(p);
    long score[2];
    for (int mode = 0; mode < 2; mode++) {
      if (mode == 0 && (p & 3)) {
        score[0] = 1L << 40;
        continue;
      }
      s.addr = p | uint32_t(mode);
      FunctionResult r = em.run(s, false);
      // Invalid encodings are the signal; a missing emitter case is not.
      score[mode] = r.entry_ok ? long(r.n_invalid) * 16 + long(r.n_unsupported) : 1L << 30;
    }
    int mode = score[0] == score[1] ? default_mode_ : score[1] < score[0] ? 1 : 0;
    f.guessed = true;
    f.spec.end = s.end;
    set_mode(f, mode);
  }
}

void Program::discover() {
  // Strong starts: sized function symbols, exidx, PLT stubs.
  size_t thumb_syms = 0, arm_syms = 0;
  std::vector<std::pair<uint32_t, uint32_t>> sized;
  for (const Symbol &s : elf_.symbols()) {
    if (!s.func || !s.defined) continue;
    uint32_t p = s.value & ~1u;
    if (!elf_.is_text(p)) continue;
    (s.value & 1 ? thumb_syms : arm_syms)++;
    add_strong(p, int(s.value & 1), s.size ? p + s.size : 0, s.name, SRC_SYMBOL);
    if (is_noreturn_name(s.name)) noreturn_.insert(s.value);
    if (s.size) sized.push_back({p, p + s.size});
  }
  default_mode_ = thumb_syms >= arm_syms ? 1 : 0;
  std::sort(sized.begin(), sized.end());
  for (uint32_t e : elf_.exidx_starts()) {
    if (!elf_.is_text(e)) continue;
    auto it = std::upper_bound(sized.begin(), sized.end(), std::make_pair(e, 0xffffffffu));
    if (it != sized.begin() && e > std::prev(it)->first && e < std::prev(it)->second) continue; // inside a symbol
    add_strong(e, -1, 0, sub_name(e), SRC_EXIDX);
  }
  parse_plt();
  for (auto &[p, f] : funcs_)
    if (!(f.sources & (SRC_SYMBOL | SRC_PLT)) || f.spec.end <= p) f.spec.end = range_end(p);
  for (auto &[p, f] : funcs_)
    if (f.mode >= 0) queue_.push_back(p);

  // Code pointers in relocated data.
  std::vector<std::pair<uint32_t, uint8_t>> ptrs;
  for (const Reloc &r : elf_.relocs()) {
    auto w = elf_.read32(r.offset);
    if (!w) continue;
    uint8_t src = elf_.is_text(r.offset) ? SRC_TABLE : SRC_RELOC;
    if (r.type == R_ARM_RELATIVE) ptrs.push_back({*w, src});
    if (r.type == R_ARM_ABS32 && r.sym < elf_.dynsyms().size() && elf_.dynsyms()[r.sym].defined &&
        elf_.dynsyms()[r.sym].func)
      ptrs.push_back({elf_.dynsyms()[r.sym].value + *w, src});
  }
  drain();
  for (auto [v, src] : ptrs) candidate(v, src);
  drain();
  for (int round = 0; round < 4; round++) {
    bool unknown = false;
    for (auto &[p, f] : funcs_) unknown |= f.mode < 0;
    if (!unknown) break;
    guess_modes();
    drain();
  }
}

std::set<uint32_t> Program::closure(uint32_t iw, size_t limit) const {
  std::set<uint32_t> seen;
  std::vector<uint32_t> work{iw};
  while (!work.empty()) {
    uint32_t a = work.back();
    work.pop_back();
    const Func *f = find(a);
    if (!f) {
      // A branch into another function (one of its roots): that function.
      auto it = funcs_.upper_bound(a & ~1u);
      for (int k = 0; k < 16 && it != funcs_.begin() && !f; k++) {
        --it;
        const std::vector<uint32_t> &e = it->second.res.entries;
        if (std::find(e.begin(), e.end(), a) != e.end()) f = &it->second;
      }
    }
    if (!f || !seen.insert(f->spec.addr).second) continue;
    if (seen.size() > limit) return {};
    for (uint32_t c : f->res.calls) work.push_back(c);
    for (uint32_t c : f->res.tails) work.push_back(c);
  }
  return seen;
}

std::vector<FunctionResult> Program::emit_all(const std::set<uint32_t> *only) {
  std::vector<const Func *> list;
  for (const auto &[p, f] : funcs_)
    if (f.mode >= 0 && (!only || only->count(f.spec.addr))) list.push_back(&f);
  // With a subset, calls leave it through the dispatcher.
  ModuleContext ctx = ctx_;
  if (only)
    ctx.callee = [this, only](uint32_t iw) { return only->count(iw) ? ctx_.callee(iw) : std::string(); };
  std::vector<FunctionResult> out(list.size());
  std::atomic<size_t> next{0};
  unsigned n = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < n; t++)
    threads.emplace_back([&] {
      FunctionEmitter em(elf_, ctx);
      for (size_t i; (i = next++) < list.size();) out[i] = em.run(list[i]->spec, true);
    });
  for (auto &t : threads) t.join();
  return out;
}

} // namespace recomp
