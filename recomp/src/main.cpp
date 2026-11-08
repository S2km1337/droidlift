// droidlift-recomp: static ARMv7 -> C recompiler, whole-library stage
//.
//
//   droidlift-recomp --elf LIB --out DIR --prefix P [--soname S] --all [--shards N]
//   droidlift-recomp --elf LIB --out DIR --prefix P --func NAME...
//   droidlift-recomp --elf LIB --out DIR --prefix P --sample N [--seed S] [--closure K] [--also ADDR]...
//   droidlift-recomp --elf LIB --census
//
// Every mode first discovers the library's functions (program.hpp). --all
// writes all of them, split over N shard files (DIR/P_funcs_NNN.c), with
// DIR/P_decls.h, DIR/P_table.c and a coverage report DIR/P_coverage.txt.
// --func writes only the named functions (symbol names or ADDR:SIZE[:NAME],
// hex, bit 0 = Thumb). --sample picks N random fully-recompiled functions
// (stratified: Thumb, ARM, VFP, NEON) whose direct-call closure has at most K
// functions, writes them with their closures, and lists the picks in
// DIR/P_sample.c for the differential test; --also adds a given function
// (hex, bit 0 = Thumb: a regression case) to the sample and lists it in
// P_also too, and fails if discovery didn't find it. --census prints the coverage
// report and writes nothing. Output is derived from the game binary and
// belongs in an ignored *.nosync directory.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "decode.hpp"
#include "elf.hpp"
#include "emit.hpp"
#include "program.hpp"

using namespace recomp;

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: droidlift-recomp --elf LIB (--census | --out DIR --prefix P [--soname S]\n"
               "                   (--all [--shards N] | --func F... | --sample N [--seed S] [--closure K]\n"
               "                    [--also ADDR]...))\n");
  return 2;
}

std::string pct(size_t a, size_t b) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f%%", b ? 100.0 * double(a) / double(b) : 0.0);
  return buf;
}

// The coverage report: functions found and recompiled, instructions, and what
// is left to the interpreter, by mnemonic and reason.
std::string coverage(const Elf &prog_elf, const Program &prog, const std::vector<FunctionResult> &res, const std::string &soname,
                     size_t c_bytes) {
  std::ostringstream o;
  size_t found = 0, thumb = 0, arm = 0, guessed = 0, ok = 0, full = 0, partial = 0, failed = 0, entries = 0;
  std::map<uint8_t, size_t> by_src;
  for (const auto &[p, f] : prog.funcs()) {
    if (f.mode < 0) continue;
    found++;
    (f.mode ? thumb : arm)++;
    guessed += f.guessed;
    for (uint8_t b = 1; b; b <<= 1)
      if (f.sources & b) by_src[b]++;
  }
  size_t insns = 0, unsup = 0, after = 0, vfp = 0, neon = 0, tables = 0, computed = 0;
  std::map<uint32_t, uint8_t> covered; // instructions reached (shared code counted once)
  std::map<std::string, size_t> why, why_after, mn;
  for (const FunctionResult &r : res) {
    if (!r.entry_ok) {
      failed++;
      continue;
    }
    ok++;
    (r.n_unsupported ? partial : full)++;
    entries += r.entries.size();
    insns += r.insns;
    covered.insert(r.insn_addrs.begin(), r.insn_addrs.end());
    unsup += r.n_unsupported;
    after += r.n_after_call;
    vfp += r.n_vfp;
    neon += r.n_neon;
    tables += r.jump_tables;
    computed += r.computed_jumps;
    for (auto &[k, v] : r.unsupported) why[k] += v;
    for (auto &[k, v] : r.unsupported_after_call) why_after[k] += v;
    for (auto &[k, v] : r.mnemonics) mn[k] += v;
  }
  o << "coverage of " << soname << "\n";
  o << "functions found: " << found << " (Thumb " << thumb << ", ARM " << arm << "; mode guessed for " << guessed
    << ")\n";
  o << "  from: symbols " << by_src[SRC_SYMBOL] << ", exidx " << by_src[SRC_EXIDX] << ", plt " << by_src[SRC_PLT]
    << ", relocated pointers " << by_src[SRC_RELOC] << ", call targets " << by_src[SRC_CALL] << ", tail targets "
    << by_src[SRC_TAIL] << ", computed pointers " << by_src[SRC_POINTER] << "\n";
  o << "functions recompiled: " << ok << " (" << pct(ok, found) << "): " << full << " completely, " << partial
    << " with instructions left to the interpreter; " << failed << " not at all (first instruction undecodable)\n";
  o << "entries in the dispatch table: " << entries << "\n";
  o << "instructions reachable: " << insns << "; recompiled " << insns - unsup - after << " ("
    << pct(insns - unsup - after, insns) << "), left to the interpreter " << unsup << " (" << pct(unsup, insns)
    << ") + " << after << " after calls (likely data after noreturn calls)\n";
  o << "  VFP instructions recompiled: " << vfp << ", NEON: " << neon << "; jump tables: " << tables
    << "; computed jumps through the dispatcher: " << computed << "\n";
  size_t reached = 0;
  for (auto &[a, sz] : covered) reached += sz;
  if (const Section *t = prog_elf.section(".text"))
    o << ".text reached as code: " << reached << " of " << t->size << " bytes (" << pct(reached, t->size)
      << "; the rest is literal pools, tables, padding and code nothing reaches)\n";
  o << "generated C: " << c_bytes << " bytes\n";
  auto top = [&](const char *title, const std::map<std::string, size_t> &m, size_t n) {
    std::vector<std::pair<size_t, std::string>> v;
    for (auto &[k, c] : m) v.push_back({c, k});
    std::sort(v.rbegin(), v.rend());
    o << title << " (" << v.size() << " kinds):\n";
    for (size_t i = 0; i < v.size() && i < n; i++) o << "  " << v[i].first << "\t" << v[i].second << "\n";
  };
  top("left to the interpreter, by mnemonic: reason", why, 60);
  top("after calls, by mnemonic: reason", why_after, 15);
  top("recompiled, by mnemonic", mn, 400);
  return o.str();
}

bool parse_func(const Elf &elf, const Program &prog, const std::string &arg, std::set<uint32_t> *out) {
  if (const Symbol *s = elf.find_symbol(arg)) {
    if (!prog.find(s->value)) {
      std::fprintf(stderr, "droidlift-recomp: %s was not discovered as a function\n", arg.c_str());
      return false;
    }
    out->insert(s->value);
    return true;
  }
  unsigned addr = 0;
  if (std::sscanf(arg.c_str(), "%x", &addr) != 1 || !prog.find(addr)) {
    std::fprintf(stderr, "droidlift-recomp: no function %s\n", arg.c_str());
    return false;
  }
  out->insert(addr);
  return true;
}

bool write_text(const std::string &path, const std::string &text) {
  std::ofstream o(path, std::ios::binary);
  o << text;
  return bool(o);
}

} // namespace

int main(int argc, char **argv) {
  std::string elf_path, out_dir, prefix = "lib", soname;
  std::vector<std::string> funcs;
  bool census = false, all = false, dump = false;
  int shards = 0, sample = 0;
  unsigned seed = 1;
  size_t closure_limit = 24;
  std::vector<uint32_t> also;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--elf") elf_path = next();
    else if (a == "--out") out_dir = next();
    else if (a == "--prefix") prefix = next();
    else if (a == "--soname") soname = next();
    else if (a == "--func") funcs.push_back(next());
    else if (a == "--census") census = true;
    else if (a == "--dump") dump = true, census = true;
    else if (a == "--all") all = true;
    else if (a == "--shards") shards = std::atoi(next().c_str());
    else if (a == "--sample") sample = std::atoi(next().c_str());
    else if (a == "--seed") seed = unsigned(std::strtoul(next().c_str(), nullptr, 0));
    else if (a == "--closure") closure_limit = size_t(std::atoi(next().c_str()));
    else if (a == "--also") also.push_back(uint32_t(std::strtoul(next().c_str(), nullptr, 16)));
    else return usage();
  }
  if (elf_path.empty() || (!census && (out_dir.empty() || (!all && funcs.empty() && !sample)))) return usage();
  if (soname.empty()) soname = elf_path.substr(elf_path.find_last_of('/') + 1);
  Elf elf;
  if (auto e = elf.load(elf_path)) {
    std::fprintf(stderr, "droidlift-recomp: %s\n", e->c_str());
    return 1;
  }
  Program prog(elf, prefix);
  prog.discover();
  if (dump) { // debugging aid: the discovered functions
    for (const auto &[p, f] : prog.funcs()) {
      std::printf("%08x..%08x %s src=%02x roots=%zu insns=%u unsup=%u entry_ok=%d %s tails", p, f.spec.end,
                  f.mode < 0 ? "?" : f.mode ? "T" : "A", f.sources, f.roots.size(), f.res.insns, f.res.n_unsupported,
                  f.res.entry_ok, f.spec.name.c_str());
      for (uint32_t t : f.res.tails) std::printf(" %x", t);
      std::printf("\n");
    }
    return 0;
  }

  if (census || all) {
    std::vector<FunctionResult> res = prog.emit_all();
    size_t bytes = 0;
    if (all) {
      std::string err;
      bytes = write_module(out_dir, prefix, soname, res, shards, &err);
      if (!bytes) {
        std::fprintf(stderr, "droidlift-recomp: %s\n", err.c_str());
        return 1;
      }
    } else {
      for (const FunctionResult &r : res) bytes += r.code.size();
    }
    if (getenv("RC_GAPS")) { // debugging aid: the largest stretches of .text no function reaches
      std::map<uint32_t, uint8_t> cov;
      for (const FunctionResult &r : res) cov.insert(r.insn_addrs.begin(), r.insn_addrs.end());
      const Section *t = elf.section(".text");
      std::vector<std::pair<uint32_t, uint32_t>> gaps;
      uint32_t at = t->addr;
      for (auto &[a, sz] : cov) {
        if (a > at) gaps.push_back({a - at, at});
        at = std::max(at, a + sz);
      }
      std::sort(gaps.rbegin(), gaps.rend());
      for (size_t i = 0; i < gaps.size() && i < 25; i++) {
        const Func *f = nullptr;
        for (auto &[p, fn] : prog.funcs())
          if (p <= gaps[i].second) f = &fn;
        std::printf("gap %08x +%u (after %s)\n", gaps[i].second, gaps[i].first, f ? f->spec.name.c_str() : "-");
      }
    }
    std::string rep = coverage(elf, prog, res, soname, bytes);
    if (all && !write_text(out_dir + "/" + prefix + "_coverage.txt", rep)) return 1;
    // The summary lines (up to the per-reason lists) on stdout.
    std::printf("%s", rep.substr(0, rep.find("left to the interpreter, by")).c_str());
    if (census) std::printf("%s", rep.substr(rep.find("left to the interpreter, by")).c_str());
    return 0;
  }

  std::set<uint32_t> only;
  std::vector<uint32_t> picks;
  if (!funcs.empty()) {
    for (const std::string &f : funcs)
      if (!parse_func(elf, prog, f, &only)) return 1;
    for (uint32_t a : only) {
      std::set<uint32_t> c = prog.closure(a, 1000);
      only.insert(c.begin(), c.end());
    }
  } else {
    // A random sample, stratified so Thumb, ARM, VFP and NEON code all show up.
    // Discovery's analyses already count what each function leaves out.
    auto clean = [&](uint32_t a) {
      const Func *f = prog.find(a);
      return f && f->res.entry_ok && f->res.n_unsupported == 0;
    };
    std::vector<uint32_t> cand[4]; // thumb, arm, vfp, neon
    for (const auto &[p, f] : prog.funcs()) {
      if (f.mode < 0 || !clean(f.spec.addr) || prog.plt().count(p)) continue;
      int k = f.res.n_neon ? 3 : f.res.n_vfp ? 2 : f.mode ? 0 : 1;
      cand[k].push_back(f.spec.addr);
    }
    std::mt19937 rng(seed);
    for (auto &c : cand) std::shuffle(c.begin(), c.end(), rng);
    size_t idx[4] = {0, 0, 0, 0};
    for (int got = 0, k = 0, misses = 0; got < sample && misses < 4; k = (k + 1) % 4) {
      bool took = false;
      while (idx[k] < cand[k].size()) {
        uint32_t a = cand[k][idx[k]++];
        std::set<uint32_t> c = prog.closure(a, closure_limit);
        bool ok = !c.empty();
        for (uint32_t x : c) ok = ok && (clean(x) || prog.plt().count(x & ~1u));
        if (!ok) continue;
        picks.push_back(a);
        only.insert(c.begin(), c.end());
        took = true;
        got++;
        break;
      }
      misses = took ? 0 : misses + 1;
    }
    for (uint32_t a : also) {
      const Func *f = prog.find(a);
      if (!f || !f->res.entry_ok || f->spec.addr != a) {
        std::fprintf(stderr, "droidlift-recomp: --also 0x%x: discovery found no function there\n", a);
        return 1;
      }
      std::set<uint32_t> c = prog.closure(a, 1000);
      if (c.empty()) {
        std::fprintf(stderr, "droidlift-recomp: --also 0x%x: its closure has over 1000 functions\n", a);
        return 1;
      }
      only.insert(c.begin(), c.end());
      if (std::find(picks.begin(), picks.end(), a) == picks.end()) picks.push_back(a);
    }
  }
  std::vector<FunctionResult> res = prog.emit_all(&only);
  std::string err;
  size_t bytes = write_module(out_dir, prefix, soname, res, 0, &err);
  if (!bytes) {
    std::fprintf(stderr, "droidlift-recomp: %s\n", err.c_str());
    return 1;
  }
  for (const FunctionResult &r : res)
    if (r.entry_ok && only.count(r.spec.addr) && !funcs.empty())
      std::printf("droidlift-recomp: %-28s 0x%08x %5u insns%s\n", r.spec.name.c_str(), r.spec.addr, r.insns,
                  r.n_unsupported ? " (some left to the interpreter)" : "");
  if (sample) {
    std::string s = "/* Generated by droidlift-recomp: the differential test's sample from " + soname + ". */\n" +
                    "#include <stdint.h>\nconst uint32_t " + prefix + "_sample[] = {";
    for (uint32_t a : picks) {
      char b[24];
      std::snprintf(b, sizeof b, "0x%xu, ", a);
      s += b;
    }
    s += "0};\nconst uint32_t " + prefix + "_sample_count = " + std::to_string(picks.size()) + ";\n";
    s += "const uint32_t " + prefix + "_also[] = {";
    for (uint32_t a : also) {
      char b[24];
      std::snprintf(b, sizeof b, "0x%xu, ", a);
      s += b;
    }
    s += "0};\nconst uint32_t " + prefix + "_also_count = " + std::to_string(also.size()) + ";\n";
    if (!write_text(out_dir + "/" + prefix + "_sample.c", s)) return 1;
    std::printf("droidlift-recomp: sampled %zu functions from %s (%zu with closures)\n", picks.size(), soname.c_str(),
                only.size());
  }
  return 0;
}
