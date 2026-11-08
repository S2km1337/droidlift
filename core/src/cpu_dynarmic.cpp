/*
 * dynarmic (A32 JIT) implementation of the CPU backend: the bring-up and
 * reference backend on the Mac.
 *
 * Memory goes through fastmem on the 4 GiB guest reservation (g_mem); accesses
 * that fault fall back to the callbacks below.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <mach/mach_time.h>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

extern "C" {
#include "cpu.h"
#include "guest_mem.h"
}

namespace {

struct DynarmicCpu;

struct Env final : Dynarmic::A32::UserCallbacks {
  DynarmicCpu *owner = nullptr;

  // Slow path: fastmem faulted, so the address may be outside every committed
  // region. Fault the guest instead of the host.
  bool Mapped(std::uint32_t a, std::size_t n, const char *what);
  template <typename T> T Read(std::uint32_t a) {
    T v{};
    if (Mapped(a, sizeof v, "read")) std::memcpy(&v, g_mem + a, sizeof v);
    return v;
  }
  template <typename T> void Write(std::uint32_t a, T v) {
    if (Mapped(a, sizeof v, "write")) std::memcpy(g_mem + a, &v, sizeof v);
  }

  std::uint8_t MemoryRead8(std::uint32_t a) override { return Read<std::uint8_t>(a); }
  std::uint16_t MemoryRead16(std::uint32_t a) override { return Read<std::uint16_t>(a); }
  std::uint32_t MemoryRead32(std::uint32_t a) override { return Read<std::uint32_t>(a); }
  std::uint64_t MemoryRead64(std::uint32_t a) override { return Read<std::uint64_t>(a); }
  void MemoryWrite8(std::uint32_t a, std::uint8_t v) override { Write(a, v); }
  void MemoryWrite16(std::uint32_t a, std::uint16_t v) override { Write(a, v); }
  void MemoryWrite32(std::uint32_t a, std::uint32_t v) override { Write(a, v); }
  void MemoryWrite64(std::uint32_t a, std::uint64_t v) override { Write(a, v); }

  // Exclusive stores (strex) succeed only if memory still holds the value the
  // matching ldrex saw: a compare-and-swap, so they stay correct once guest
  // threads run on host threads.
  template <typename T> bool WriteExclusive(std::uint32_t a, T v, T expected) {
    if (!Mapped(a, sizeof v, "exclusive write")) return false;
    return __atomic_compare_exchange_n(reinterpret_cast<T *>(g_mem + a), &expected, v, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  }
  bool MemoryWriteExclusive8(std::uint32_t a, std::uint8_t v, std::uint8_t e) override {
    return WriteExclusive(a, v, e);
  }
  bool MemoryWriteExclusive16(std::uint32_t a, std::uint16_t v, std::uint16_t e) override {
    return WriteExclusive(a, v, e);
  }
  bool MemoryWriteExclusive32(std::uint32_t a, std::uint32_t v, std::uint32_t e) override {
    return WriteExclusive(a, v, e);
  }
  bool MemoryWriteExclusive64(std::uint32_t a, std::uint64_t v, std::uint64_t e) override {
    return WriteExclusive(a, v, e);
  }

  void InterpreterFallback(std::uint32_t pc, std::size_t n) override;
  void CallSVC(std::uint32_t swi) override;
  void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception e) override;
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return UINT64_MAX; }
};

// CP15 as user-mode Android code sees it: the TLS registers (bionic's
// __get_tls() reads TPIDRURO), the legacy CP15 barrier operations, and the
// virtual counter CNTVCT (OpenSSL's _armv7_tick). Everything else traps.
struct Cp15 final : Dynarmic::A32::Coprocessor {
  using Reg = Dynarmic::A32::CoprocReg;
  std::uint32_t tpidruro = 0, tpidrurw = 0;

  static std::uint64_t Nop(void *, std::uint32_t, std::uint32_t) { return 0; }
  static std::uint64_t Counter(void *, std::uint32_t, std::uint32_t) { return mach_absolute_time(); }

  std::optional<Callback> CompileInternalOperation(bool, unsigned, Reg, Reg, Reg, unsigned) override {
    return std::nullopt;
  }
  CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, Reg CRn, Reg CRm,
                                             unsigned opc2) override {
    if (two || opc1 != 0) return {};
    if (CRn == Reg::C13 && CRm == Reg::C0 && opc2 == 2) return &tpidrurw;
    // c7,c10,4 DSB; c7,c10,5 DMB; c7,c5,4 ISB: the JIT is already ordered.
    if (CRn == Reg::C7 && ((CRm == Reg::C10 && (opc2 == 4 || opc2 == 5)) || (CRm == Reg::C5 && opc2 == 4)))
      return Callback{Nop, std::nullopt};
    return {};
  }
  CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, Reg) override { return {}; }
  CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, Reg CRn, Reg CRm,
                                            unsigned opc2) override {
    if (two || opc1 != 0 || CRn != Reg::C13 || CRm != Reg::C0) return {};
    if (opc2 == 3) return &tpidruro;
    if (opc2 == 2) return &tpidrurw;
    return {};
  }
  CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, Reg CRm) override {
    if (!two && opc == 1 && CRm == Reg::C14) return Callback{Counter, std::nullopt};
    return {};
  }
  std::optional<Callback> CompileLoadWords(bool, bool, Reg, std::optional<std::uint8_t>) override {
    return std::nullopt;
  }
  std::optional<Callback> CompileStoreWords(bool, bool, Reg, std::optional<std::uint8_t>) override {
    return std::nullopt;
  }
};

struct DynarmicCpu {
  cpu base; // must stay first: cpu * <-> DynarmicCpu *
  Env env;
  std::shared_ptr<Cp15> cp15 = std::make_shared<Cp15>();
  std::unique_ptr<Dynarmic::A32::Jit> jit;
  bool checked = false; // no fastmem (a test cpu): never pooled
};

// One exclusive monitor (ldrex/strex reservations) shared by every guest CPU,
// one processor slot per cpu: one per guest thread.
constexpr std::size_t kMaxCpus = 64;
Dynarmic::ExclusiveMonitor g_monitor{kMaxCpus};
std::size_t g_next_cpu = 0;

DynarmicCpu *self(cpu *c) { return reinterpret_cast<DynarmicCpu *>(c); }

void Env::InterpreterFallback(std::uint32_t pc, std::size_t n) {
  std::fprintf(stderr, "[dynarmic] no interpreter fallback (pc 0x%08x, %zu instructions)\n", pc, n);
  owner->base.faulted = 1;
  owner->jit->HaltExecution();
}

bool Env::Mapped(std::uint32_t a, std::size_t n, const char *what) {
  if (gm_region_tag(a) && gm_region_tag(a + static_cast<std::uint32_t>(n) - 1)) return true;
  if (!owner->base.faulted)
    std::fprintf(stderr, "[dynarmic] guest %s of %zu bytes at unmapped 0x%08x (pc 0x%08x, lr 0x%08x)\n",
                 what, n, a, owner->jit->Regs()[15], owner->jit->Regs()[14]);
  owner->base.faulted = 1;
  owner->jit->HaltExecution();
  return false;
}

void Env::CallSVC(std::uint32_t swi) { cpu_svc(&owner->base, swi); }

void Env::ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception e) {
  std::fprintf(stderr, "[dynarmic] exception %d at pc 0x%08x\n", static_cast<int>(e), pc);
  owner->base.faulted = 1;
  owner->jit->HaltExecution();
}

// Retired cpus, kept with their JIT and code cache for the next guest thread.
// The game starts hundreds of short-lived loader threads. A fresh Jit for each
// cost ~55 ms (its constructor invalidates the icache over the whole 128 MiB
// code cache) and then translated the same loader code again from cold, so
// the render thread, waiting on those threads, stalled for seconds (ticket
// 08). Guest code never changes after relocation (nothing calls invalidate),
// so translated blocks stay valid for any guest thread. LIFO: the warmest cpu
// comes back first. A cpu's nested callback cpu stays attached and warm too.
constexpr std::size_t kMaxPooled = 32;
std::mutex g_pool_lock;
std::vector<DynarmicCpu *> g_pool;

// DL_JIT_POOL=0 turns the pool off (to measure what it saves).
bool pool_enabled() {
  static const bool on = [] {
    const char *e = std::getenv("DL_JIT_POOL");
    return !(e && e[0] == '0');
  }();
  return on;
}

void op_destroy(cpu *c) {
  DynarmicCpu *d = self(c);
  if (pool_enabled() && !d->checked && !d->jit->IsExecuting()) {
    std::lock_guard<std::mutex> lock(g_pool_lock);
    if (g_pool.size() < kMaxPooled) {
      g_pool.push_back(d);
      return;
    }
  }
  if (c->nested) c->nested->ops->destroy(c->nested);
  delete d;
}

// A pooled cpu in the state a new one starts in: registers, flags, halt
// requests, TLS and the exclusive reservation cleared; the code cache kept.
DynarmicCpu *pool_take() {
  DynarmicCpu *d = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_pool_lock);
    if (g_pool.empty()) return nullptr;
    d = g_pool.back();
    g_pool.pop_back();
  }
  d->jit->Reset();
  d->jit->ClearHalt(static_cast<Dynarmic::HaltReason>(~0u));
  d->jit->ClearExclusiveState();
  d->cp15->tpidruro = d->cp15->tpidrurw = 0;
  d->base.ret_sentinel = 0;
  d->base.faulted = 0;
  return d;
}
std::uint32_t op_get_reg(cpu *c, int r) { return self(c)->jit->Regs()[r]; }
void op_set_reg(cpu *c, int r, std::uint32_t v) { self(c)->jit->Regs()[r] = v; }
std::uint32_t op_get_cpsr(cpu *c) { return self(c)->jit->Cpsr(); }
void op_set_cpsr(cpu *c, std::uint32_t v) { self(c)->jit->SetCpsr(v); }
std::uint32_t op_get_ext(cpu *c, int i) { return self(c)->jit->ExtRegs()[i]; }
void op_set_ext(cpu *c, int i, std::uint32_t v) { self(c)->jit->ExtRegs()[i] = v; }
std::uint32_t op_get_fpscr(cpu *c) { return self(c)->jit->Fpscr(); }
void op_set_fpscr(cpu *c, std::uint32_t v) { self(c)->jit->SetFpscr(v); }

// DL_TRACE_PC=1 single-steps and logs every guest PC to stderr (debug aid).
bool trace_pc() {
  static const bool on = std::getenv("DL_TRACE_PC") != nullptr;
  return on;
}

int op_run(cpu *c) {
  if (trace_pc()) {
    for (;;) {
      std::fprintf(stderr, "pc %08x\n", self(c)->jit->Regs()[15]);
      auto hr = self(c)->jit->Step();
      if (static_cast<std::uint32_t>(hr) & ~static_cast<std::uint32_t>(Dynarmic::HaltReason::Step)) {
        self(c)->jit->ClearHalt(hr);
        return (static_cast<std::uint32_t>(hr) &
                static_cast<std::uint32_t>(Dynarmic::HaltReason::MemoryAbort))
                   ? -1
                   : 0;
      }
    }
  }
  auto hr = self(c)->jit->Run();
  self(c)->jit->ClearHalt(hr);
  return (static_cast<std::uint32_t>(hr) & static_cast<std::uint32_t>(Dynarmic::HaltReason::MemoryAbort))
             ? -1
             : 0;
}

void op_halt(cpu *c) { self(c)->jit->HaltExecution(); }

void op_invalidate(cpu *c, std::uint32_t addr, std::uint32_t size) {
  self(c)->jit->InvalidateCacheRange(addr, size);
  if (c->nested) c->nested->ops->invalidate(c->nested, addr, size);
}

const cpu_ops dynarmic_ops = {
    "dynarmic",   op_destroy,   op_get_reg, op_set_reg, op_get_cpsr,   op_set_cpsr, op_get_ext,
    op_set_ext,   op_get_fpscr, op_set_fpscr, op_run,   op_halt,       op_invalidate,
    cpu_dynarmic_create,
};

} // namespace

extern "C" int cpu_dynarmic_step(cpu *c) {
  auto hr = self(c)->jit->Step();
  self(c)->jit->ClearHalt(hr);
  auto bits = static_cast<std::uint32_t>(hr);
  if ((bits & static_cast<std::uint32_t>(Dynarmic::HaltReason::MemoryAbort)) || c->faulted) {
    c->faulted = 1;
    return -1;
  }
  return (bits & ~static_cast<std::uint32_t>(Dynarmic::HaltReason::Step)) ? 1 : 0;
}

namespace {
int g_jits_created = 0; // dynarmic Jit instances (cpu_dynarmic_jits)

cpu *create(bool fastmem) {
  // fastmem maps every 32-bit address, so it needs the whole 4 GiB.
  if (gm_space() < (1ull << 32)) fastmem = false;
  if (fastmem && pool_enabled())
    if (DynarmicCpu *pooled = pool_take()) return &pooled->base;
  __atomic_fetch_add(&g_jits_created, 1, __ATOMIC_RELAXED);
  auto *d = new DynarmicCpu{};
  d->base.ops = &dynarmic_ops;
  d->env.owner = d;
  d->checked = !fastmem;

  Dynarmic::A32::UserConfig cfg;
  cfg.callbacks = &d->env;
  cfg.global_monitor = &g_monitor;
  cfg.coprocessors[15] = d->cp15;
  cfg.processor_id = __atomic_fetch_add(&g_next_cpu, 1, __ATOMIC_RELAXED) % kMaxCpus;
  cfg.arch_version = Dynarmic::A32::ArchVersion::v7;
  if (fastmem) cfg.fastmem_pointer = reinterpret_cast<std::uintptr_t>(g_mem);
  cfg.enable_cycle_counting = false;
  d->jit = std::make_unique<Dynarmic::A32::Jit>(cfg);
  return &d->base;
}
} // namespace

extern "C" cpu *cpu_dynarmic_create(void) { return create(true); }

extern "C" int cpu_dynarmic_jits(void) { return __atomic_load_n(&g_jits_created, __ATOMIC_RELAXED); }

extern "C" cpu *cpu_dynarmic_create_checked(void) { return create(false); }
