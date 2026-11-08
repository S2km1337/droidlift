/*
 * Sampled differential test for the static recompiler.
 *
 * droidlift-recomp --sample picks random fully-recompiled functions from each
 * of a game's libraries (Thumb, ARM, VFP and NEON code all represented) and
 * recompiles them with their direct-call closures. This test maps the
 * libraries into guest memory, relocated, with every
 * import bound to a stub that returns 0, then runs each sampled function on
 * random inputs twice: recompiled, and under dynarmic (the reference CPU,
 * through core/src/cpu.h), from identical starting states. Arguments,
 * registers and memory are random but pointer-rich: registers and arena
 * words are often pointers into a 1 MiB arena, so pointer chasing stays in
 * it. The whole 4 GiB guest space is readable and writable (zero), so a wild
 * pointer reads zeros in both runs and its writes are compared too.
 *
 * Compared after each run: r0-r14, CPSR (NZCVQ, GE, T, IT), the 64 VFP/NEON
 * words, FPSCR (NZCV and the control bits), the arena, the libraries'
 * writable segments, and every other page either run touched.
 *
 * An input is skipped (not compared) when the recompiled run leaves the
 * sample (an indirect call or jump to code that isn't in it), makes an SVC,
 * faults, or runs longer than the budget (a runaway loop on random input).
 *
 *   rc-sampletest --dir APKLIBDIR [--iters N] [--seed S] [--only ADDR] [-v]
 *
 * Regression cases (droidlift-recomp --also, listed in <prefix>_also) must also
 * stay inside the sample on every input and be compared on at least half.
 *
 * Prints "SAMPLETEST OK" when every compared input matched.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cpu.h"
#include "elf32.h"
#include "guest_mem.h"
#include "rc_runtime.h"

/* cpu.c routes guest SVCs to the loader's dispatchers. The recompiled run
 * goes first and skips inputs that make an SVC, so dynarmic never should. */
static void unexpected_svc(cpu *c, const char *what, uint32_t i) {
  fprintf(stderr, "sampletest: unexpected %s svc 0x%x under dynarmic\n", what, i);
  c->faulted = 1;
  c->ops->halt(c);
}
void dynlib_dispatch(cpu *c, uint32_t i) { unexpected_svc(c, "dynlib", i); }
void dynlib_host_dispatch(cpu *c, uint32_t i) { unexpected_svc(c, "host", i); }
void jni_dispatch(cpu *c, uint32_t i) { unexpected_svc(c, "jni", i); }

#define LIB(p)                                       \
  extern const rc_module p##_module;                 \
  extern uint32_t p##_base;                          \
  extern const uint32_t p##_sample[], p##_sample_count, p##_also[], p##_also_count;
/* The game's libraries: DL_SAMPLE_LIBS(X) lists X(prefix, soname), from the
 * header droidlift_add_recomp_sampletest generates. */
#include DL_SAMPLE_HEADER
#define DECLARE(p, file) LIB(p)
DL_SAMPLE_LIBS(DECLARE)

typedef struct {
  const char *file;
  const rc_module *mod;
  uint32_t *base;
  const uint32_t *sample;
  const uint32_t *count;
  uint32_t size;        /* image size */
  uint8_t *pristine;    /* the relocated image */
  uint32_t rw_lo, rw_hi; /* writable part, image-relative */
  /* Regression cases (droidlift-recomp --also): each must be compared. */
  const uint32_t *also;
  const uint32_t *also_count;
} lib;

#define ENTRY(p, file) {file, &p##_module, &p##_base, p##_sample, &p##_sample_count, 0, 0, 0, 0, p##_also, &p##_also_count},
static lib libs[] = {DL_SAMPLE_LIBS(ENTRY)};

static int is_also(const lib *L, uint32_t fn) {
  for (uint32_t i = 0; i < *L->also_count; i++)
    if (L->also[i] == fn) return 1;
  return 0;
}
#define NLIBS (sizeof libs / sizeof libs[0])

/* ---- Imports: one stub that returns 0 ---------------------------------- */

#define ARENA_SIZE 0x100000u
#define STACK_TOP (ARENA_SIZE - 0x2000u)
static uint32_t arena; /* guest address of the 1 MiB arena: buffers and stack */

/* A read-only guest page with ARM "mov r0, #0; bx lr" (dynarmic runs it; the
 * recompiled side dispatches to stub_ret0). Imported objects point into the
 * arena's top page, so they hold random data restored with the arena. */
static uint32_t stub_base;
#define STUB_DATA (arena + ARENA_SIZE - 0x1000u)

static void stub_ret0(rc_state *s) {
  s->r[0] = 0;
  rc_set_pc(s, s->r[14]);
}
static const rc_func stub_funcs[] = {{0, stub_ret0, "import stub"}};
static const rc_module stub_module = {"imports", &stub_base, stub_funcs, 1};

/* ---- Loading ------------------------------------------------------------- */

static int load_lib(const char *dir, lib *L) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", dir, L->file);
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return -1;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *file = malloc((size_t)n);
  if (!file || fread(file, 1, (size_t)n, f) != (size_t)n) return fclose(f), -1;
  fclose(f);
  const Elf32_Ehdr *eh = (const Elf32_Ehdr *)file;
  const Elf32_Phdr *ph = (const Elf32_Phdr *)(file + eh->e_phoff);
  uint32_t dyn = 0;
  L->rw_lo = UINT32_MAX;
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > L->size) L->size = ph[i].p_vaddr + ph[i].p_memsz;
    if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & 2)) {
      if (ph[i].p_vaddr < L->rw_lo) L->rw_lo = ph[i].p_vaddr;
      if (ph[i].p_vaddr + ph[i].p_memsz > L->rw_hi) L->rw_hi = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (ph[i].p_type == PT_DYNAMIC) dyn = ph[i].p_vaddr;
  }
  uint32_t base = gm_alloc(L->size, 0x10000, L->file);
  if (!base) return -1;
  *L->base = base;
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD) memcpy(g2h(base + ph[i].p_vaddr), file + ph[i].p_offset, ph[i].p_filesz);
  free(file);

  /* Relocate: own symbols to their addresses, imports to the stub. */
  uint32_t rel = 0, relsz = 0, jmprel = 0, pltsz = 0, symtab = 0;
  for (const Elf32_Dyn *d = g2h(base + dyn); d->d_tag != DT_NULL; d++) {
    if (d->d_tag == DT_REL) rel = d->d_val;
    if (d->d_tag == DT_RELSZ) relsz = d->d_val;
    if (d->d_tag == DT_JMPREL) jmprel = d->d_val;
    if (d->d_tag == DT_PLTRELSZ) pltsz = d->d_val;
    if (d->d_tag == DT_SYMTAB) symtab = d->d_val;
  }
  const uint32_t tabs[2][2] = {{rel, relsz}, {jmprel, pltsz}};
  for (int k = 0; k < 2; k++)
    for (uint32_t o = 0; o + 8 <= tabs[k][1]; o += 8) {
      const Elf32_Rel *r = g2h(base + tabs[k][0] + o);
      uint32_t *p = g2h(base + r->r_offset);
      uint32_t type = ELF32_R_TYPE(r->r_info);
      const Elf32_Sym *sym = g2h(base + symtab + 16 * ELF32_R_SYM(r->r_info));
      uint32_t S;
      if (sym->st_shndx != SHN_UNDEF) S = base + sym->st_value;
      else S = ELF32_ST_TYPE(sym->st_info) == 1 ? STUB_DATA : stub_base; /* OBJECT: data */
      if (type == R_ARM_RELATIVE) *p += base;
      else if (type == R_ARM_ABS32) *p += S;
      else if (type == R_ARM_GLOB_DAT || type == R_ARM_JUMP_SLOT) *p = S;
    }
  L->pristine = malloc(L->size);
  memcpy(L->pristine, g2h(base), L->size);
  /* The read-only segment (code, rodata) becomes read-only for the host:
   * dynarmic would run code a random pointer had overwritten, the
   * recompiled code wouldn't, so such a write must fault (and skip the
   * input). Everything from the first page that is partly writable on is
   * restored and compared after each run. */
  uint32_t ro_end = L->rw_lo == UINT32_MAX ? L->size : L->rw_lo;
  uint32_t prot = ro_end & ~0x3fffu; /* host pages are 16 KiB */
  if (prot && mprotect(g2h(base), prot, PROT_READ) != 0) {
    perror("sampletest: mprotect");
    return -1;
  }
  L->rw_lo = prot;
  L->rw_hi = L->size;
  return 0;
}

/* ---- The wild zone: all guest memory above our regions ------------------ */

/* It is one guest region (gm_alloc), so dynarmic's slow path, which handles
 * exclusive accesses, accepts it too. The low 1 MiB (never a region) is
 * mapped as well, so null-ish pointers read zeros instead of ending the run;
 * only exclusive accesses there skip the input (rc_test_exclusive). */
#define HOST_PAGE 0x4000u
#define LOW_ZONE 0x100000u
static uint64_t wild_lo; /* everything from here to 4 GiB */

static void reset_wild(void) {
  if (mmap(g_mem, LOW_ZONE, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED ||
      mmap(g_mem + wild_lo, (1ull << 32) - wild_lo, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1,
           0) == MAP_FAILED) {
    perror("sampletest: mmap");
    exit(1);
  }
}

typedef struct {
  uint32_t addr;
  uint8_t *data;
} page;
typedef struct {
  page *p;
  size_t n, cap;
} pageset;

static unsigned char *mc_vec;
static void snap_range(pageset *ps, uint64_t lo, uint64_t hi) {
  size_t npages = (size_t)((hi - lo) / HOST_PAGE);
  if (mincore(g_mem + lo, (size_t)(hi - lo), (char *)mc_vec) != 0) {
    perror("sampletest: mincore");
    exit(1);
  }
  for (size_t i = 0; i < npages; i++) {
    if (!(mc_vec[i] & 1)) continue;
    uint32_t a = (uint32_t)(lo + i * HOST_PAGE);
    const uint8_t *src = g_mem + a;
    int zero = 1;
    for (uint32_t k = 0; k < HOST_PAGE && zero; k += 8) zero = *(const uint64_t *)(src + k) == 0;
    if (zero) continue;
    if (ps->n == ps->cap) {
      ps->cap = ps->cap ? 2 * ps->cap : 16;
      ps->p = realloc(ps->p, ps->cap * sizeof *ps->p);
    }
    ps->p[ps->n].addr = a;
    ps->p[ps->n].data = malloc(HOST_PAGE);
    memcpy(ps->p[ps->n].data, src, HOST_PAGE);
    ps->n++;
  }
}
static void snap_wild(pageset *ps) {
  for (size_t i = 0; i < ps->n; i++) free(ps->p[i].data);
  ps->n = 0;
  snap_range(ps, 0, LOW_ZONE);
  snap_range(ps, wild_lo, 1ull << 32);
}
static const uint8_t *find_page(const pageset *ps, uint32_t a) {
  for (size_t i = 0; i < ps->n; i++)
    if (ps->p[i].addr == a) return ps->p[i].data;
  return NULL;
}
/* Returns the first differing guest address, or 0. */
static uint32_t diff_wild(const pageset *a, const pageset *b) {
  static uint8_t zero[HOST_PAGE];
  const pageset *sets[2] = {a, b};
  for (int k = 0; k < 2; k++)
    for (size_t i = 0; i < sets[k]->n; i++) {
      uint32_t addr = sets[k]->p[i].addr;
      const uint8_t *x = find_page(a, addr), *y = find_page(b, addr);
      if (!x) x = zero;
      if (!y) y = zero;
      if (memcmp(x, y, HOST_PAGE) != 0) {
        uint32_t j = 0;
        while (x[j] == y[j]) j++;
        return addr + j;
      }
    }
  return 0;
}

/* ---- Inputs ------------------------------------------------------------- */

static uint64_t rng_state;
static uint32_t rnd(void) {
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;
  return (uint32_t)((rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}
static uint32_t rnd_below(uint32_t n) { return n ? rnd() % n : 0; }

static uint8_t *arena_init, *arena_after;

static uint32_t rnd_word(void) {
  static const uint32_t edge[] = {0, 1, 2, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0xfffffffeu, 0x10000u, 0xffu};
  switch (rnd_below(16)) {
  case 0:
  case 1:
  case 2:
  case 3:
  case 4:
  case 5:
  case 6:
  case 7:
  case 8: return arena + (rnd_below(STACK_TOP - 0x10000u) & (rnd_below(8) ? ~3u : ~0u)); /* pointer */
  case 9:
  case 10:
  case 11: return rnd_below(256);
  case 12: return edge[rnd_below(9)];
  default: return rnd() >> rnd_below(32);
  }
}

static uint32_t rnd_fp_word(int hi_of_double) {
  static const uint32_t singles[] = {0, 0x80000000u, 0x3f800000u, 0xbfc00000u, 0x7f800000u, 0xff800000u,
                                     0x7fc00000u, 0x7fa00000u, 0x00000001u, 0x007fffffu, 0x4b000000u, 0xcf000000u};
  if (hi_of_double) {
    static const uint32_t his[] = {0, 0x3ff00000u, 0xc0000000u, 0x7ff00000u, 0x7ff80000u, 0x00000001u, 0x41e00000u};
    return rnd_below(3) ? his[rnd_below(7)] | (rnd() & 0x000fffffu & -(uint32_t)rnd_below(2)) : rnd();
  }
  switch (rnd_below(4)) {
  case 0: return singles[rnd_below(12)];
  case 1: { /* a float of moderate magnitude */
    uint32_t e = 100 + rnd_below(56);
    return (rnd() & 0x80000000u) | e << 23 | (rnd() & 0x7fffffu);
  }
  case 2: return rnd_word();
  default: return rnd();
  }
}

/* ---- Runs ------------------------------------------------------------- */

int64_t rc_test_budget;

/* The recompiled run's basic blocks (guest addresses), for -v reports. */
#define TRACE_MAX 4096
static uint32_t trace_buf[TRACE_MAX];
static int trace_n;
void rc_test_block(uint32_t addr) {
  if (trace_n < TRACE_MAX) trace_buf[trace_n++] = addr;
}
static sigjmp_buf bail;
static volatile int bail_why; /* 1 budget, 2 left the sample, 3 svc, 4 fault */
static volatile int in_recompiled;

void rc_test_bailout(void) {
  bail_why = 1;
  siglongjmp(bail, 1);
}
void rc_test_exclusive(uint32_t addr) {
  if (addr >= LOW_ZONE) return;
  bail_why = 4;
  siglongjmp(bail, 1);
}
static int fallback(rc_state *s) {
  (void)s;
  bail_why = 2;
  siglongjmp(bail, 1);
}
static void svc_hook(rc_state *s, uint32_t imm) {
  (void)s, (void)imm;
  bail_why = 3;
  siglongjmp(bail, 1);
}
static uint32_t cp15_hook(rc_state *s, uint32_t raw) { /* TPIDRURO/TPIDRURW of a fresh dynarmic cpu */
  (void)s, (void)raw;
  return 0;
}
static int faults_low, faults_other; /* host faults in recompiled runs: below 1 MiB, elsewhere */
static void on_fault(int sig, siginfo_t *si, void *uc) {
  (void)uc;
  if (!in_recompiled) {
    signal(sig, SIG_DFL);
    return;
  }
  uint64_t a = (uint64_t)((uint8_t *)si->si_addr - g_mem);
  if (a < 0x100000u) faults_low++;
  else faults_other++;
  bail_why = 4;
  siglongjmp(bail, 1);
}
static void timed_out(int sig) {
  (void)sig;
  static const char msg[] = "sampletest FAIL: a function ran for over a minute (endless loop?)\n";
  write(2, msg, sizeof msg - 1);
  _exit(1);
}

#define CPSR_COMPARE 0xFE0FFC20u /* NZCVQ, IT, GE, T */
#define FPSCR_COMPARE 0xF7FFFF00u /* NZCV and the control bits; not QC or the cumulative flags */

static void restore(void) {
  reset_wild();
  memcpy(g2h(arena), arena_init, ARENA_SIZE);
  for (size_t i = 0; i < NLIBS; i++)
    if (libs[i].rw_hi)
      memcpy(g2h(*libs[i].base + libs[i].rw_lo), libs[i].pristine + libs[i].rw_lo, libs[i].rw_hi - libs[i].rw_lo);
}

typedef struct {
  int compared, skipped[5];
} stats;

static int verbose;
static pageset wild_a, wild_b;
static uint8_t *rw_after[NLIBS];

static int run_function(cpu *c, const lib *L, uint32_t fn, int iters, stats *st) {
  uint32_t entry = *L->base + fn;
  const char *name = "?";
  for (uint32_t i = 0; i < L->mod->count; i++)
    if (L->mod->funcs[i].addr == fn && L->mod->funcs[i].name) name = L->mod->funcs[i].name;
  for (int it = 0; it < iters; it++) {
    rc_state init;
    memset(&init, 0, sizeof init);
    for (uint32_t i = 0; i < ARENA_SIZE; i += 4) {
      uint32_t w = rnd_below(8) ? rnd_word() : rnd();
      memcpy(arena_init + i, &w, 4);
    }
    for (int i = 0; i < 13; i++) init.r[i] = rnd_word();
    init.r[13] = arena + STACK_TOP;
    init.r[14] = c->ret_sentinel;
    init.cpsr = 0x10u | (rnd() & 0xF80F0000u);
    for (int i = 0; i < 64; i++) init.ext[i] = rnd_fp_word(i & 1 && rnd_below(2));
    init.fpscr = rnd() & 0xF0000000u;

    /* Recompiled first: it decides whether the input is usable. */
    restore();
    rc_state s = init;
    rc_set_pc(&s, entry);
    rc_test_budget = 200000;
    trace_n = 0;
    bail_why = 0;
    in_recompiled = 1;
    if (sigsetjmp(bail, 1) == 0) {
      if (rc_run(&s, c->ret_sentinel) != 0) bail_why = 2;
    }
    in_recompiled = 0;
    if (bail_why) {
      st->skipped[bail_why]++;
      continue;
    }
    memcpy(arena_after, g2h(arena), ARENA_SIZE);
    for (size_t i = 0; i < NLIBS; i++)
      if (libs[i].rw_hi) memcpy(rw_after[i], g2h(*libs[i].base + libs[i].rw_lo), libs[i].rw_hi - libs[i].rw_lo);
    snap_wild(&wild_a);

    /* dynarmic */
    restore();
    for (int i = 0; i < 15; i++) c->ops->set_reg(c, i, init.r[i]);
    c->ops->set_reg(c, 15, entry & ~1u);
    c->ops->set_cpsr(c, (init.cpsr & ~CPU_CPSR_T) | ((entry & 1) ? CPU_CPSR_T : 0));
    for (int i = 0; i < 64; i++) c->ops->set_ext(c, i, init.ext[i]);
    c->ops->set_fpscr(c, init.fpscr);
    c->faulted = 0;
    int drc = c->ops->run(c);
    uint32_t pc = c->ops->get_reg(c, 15);
    if (drc != 0 || c->faulted || (pc != c->ret_sentinel && pc != c->ret_sentinel + 4)) {
      fprintf(stderr, "sampletest FAIL: %s %s (0x%x) input %d: dynarmic faulted at 0x%08x\n", L->file, name, fn, it,
              pc);
      return 1;
    }

    int bad = 0;
    for (int i = 0; i < 15; i++) {
      uint32_t d = c->ops->get_reg(c, i);
      if (s.r[i] != d) {
        fprintf(stderr, "sampletest FAIL: %s %s (0x%x) input %d: r%d = 0x%08x, dynarmic 0x%08x\n", L->file, name, fn,
                it, i, s.r[i], d);
        bad = 1;
      }
    }
    uint32_t dc = c->ops->get_cpsr(c);
    if ((s.cpsr & CPSR_COMPARE) != (dc & CPSR_COMPARE)) {
      fprintf(stderr, "sampletest FAIL: %s %s (0x%x) input %d: cpsr 0x%08x, dynarmic 0x%08x\n", L->file, name, fn, it,
              s.cpsr & CPSR_COMPARE, dc & CPSR_COMPARE);
      bad = 1;
    }
    for (int i = 0; i < 64; i++) {
      uint32_t d = c->ops->get_ext(c, i);
      if (s.ext[i] != d) {
        fprintf(stderr, "sampletest FAIL: %s %s (0x%x) input %d: s%d = 0x%08x, dynarmic 0x%08x (input 0x%08x)\n",
                L->file, name, fn, it, i, s.ext[i], d, init.ext[i]);
        bad = 1;
        break;
      }
    }
    uint32_t df = c->ops->get_fpscr(c);
    if ((s.fpscr & FPSCR_COMPARE) != (df & FPSCR_COMPARE)) {
      fprintf(stderr, "sampletest FAIL: %s %s (0x%x) input %d: fpscr 0x%08x, dynarmic 0x%08x\n", L->file, name, fn,
              it, s.fpscr, df);
      bad = 1;
    }
    if (memcmp(arena_after, g2h(arena), ARENA_SIZE) != 0) {
      uint32_t i = 0;
      const uint8_t *d = g2h(arena);
      while (arena_after[i] == d[i]) i++;
      uint32_t wi = i & ~3u, wa, wd, w0;
      memcpy(&wa, arena_after + wi, 4);
      memcpy(&wd, d + wi, 4);
      memcpy(&w0, arena_init + wi, 4);
      fprintf(stderr,
              "sampletest FAIL: %s %s (0x%x) input %d: arena+0x%x = 0x%02x, dynarmic 0x%02x (word 0x%08x, dynarmic "
              "0x%08x, before 0x%08x)\n",
              L->file, name, fn, it, i, arena_after[i], d[i], wa, wd, w0);
      bad = 1;
    }
    for (size_t k = 0; k < NLIBS; k++) {
      if (!libs[k].rw_hi) continue;
      const uint8_t *d = g2h(*libs[k].base + libs[k].rw_lo);
      uint32_t n = libs[k].rw_hi - libs[k].rw_lo;
      if (memcmp(rw_after[k], d, n) != 0) {
        uint32_t i = 0;
        while (rw_after[k][i] == d[i]) i++;
        fprintf(stderr, "sampletest FAIL: %s %s (0x%x) input %d: %s data at 0x%x differs\n", L->file, name, fn, it,
                libs[k].file, libs[k].rw_lo + i);
        bad = 1;
      }
    }
    snap_wild(&wild_b);
    uint32_t w = diff_wild(&wild_a, &wild_b);
    if (w) {
      fprintf(stderr, "sampletest FAIL: %s %s (0x%x) input %d: memory at 0x%08x differs\n", L->file, name, fn, it, w);
      bad = 1;
    }
    if (bad) {
      if (verbose) {
        fprintf(stderr, "  recompiled blocks:");
        for (int k = 0; k < trace_n; k++) fprintf(stderr, " %x", trace_buf[k]);
        fprintf(stderr, "\n");
      }
      fprintf(stderr, "  inputs: r0-r3 = 0x%08x 0x%08x 0x%08x 0x%08x, cpsr 0x%08x, fpscr 0x%08x\n", init.r[0],
              init.r[1], init.r[2], init.r[3], init.cpsr, init.fpscr);
      return 1;
    }
    st->compared++;
  }
  return 0;
}

int main(int argc, char **argv) {
  const char *dir = NULL;
  int iters = 30;
  uint32_t only = 0, from = 0;
  uint64_t seed = 0x5eed11;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
    else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = (uint32_t)strtoul(argv[++i], NULL, 16);
    else if (!strcmp(argv[i], "--from") && i + 1 < argc) from = (uint32_t)strtoul(argv[++i], NULL, 16);
    else if (!strcmp(argv[i], "-v")) verbose = 1;
    else {
      fprintf(stderr, "usage: rc-sampletest --dir APKLIBDIR [--iters N] [--seed S] [--only ADDR] [-v]\n");
      return 2;
    }
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (!dir || gm_init() != 0) return 1;
  /* Every guest region the runs don't restore is read-only: a write there by
   * one run would otherwise reach the next. */
  stub_base = gm_alloc(HOST_PAGE, 0, "import stubs");
  static const uint32_t ret0[] = {0xe3a00000u, 0xe12fff1eu};
  memcpy(g2h(stub_base), ret0, sizeof ret0);
  arena = gm_alloc(ARENA_SIZE, 0, "sampletest arena");
  if (mprotect(g2h(stub_base), HOST_PAGE, PROT_READ) != 0) return 1;
  for (size_t i = 0; i < NLIBS; i++)
    if (load_lib(dir, &libs[i]) != 0) return 1;
  for (size_t i = 0; i < NLIBS; i++) {
    rc_register_module(libs[i].mod);
    rw_after[i] = malloc(libs[i].rw_hi - libs[i].rw_lo + 1);
  }
  rc_register_module(&stub_module);
  rc_fallback = fallback;
  rc_svc = svc_hook;
  rc_cp15_read = cp15_hook;

  uint32_t cpu_lo = gm_alloc(HOST_PAGE, 0, "start of the cpu's regions");
  cpu *c = cpu_dynarmic_create();
  if (cpu_setup(c, 0x10000) != 0) return 1; /* its stack (unused: SP is in the arena) and return sentinel */
  if (!arena || !cpu_lo) return 1;
  wild_lo = gm_alloc(HOST_PAGE, 0, "end of the regions");
  if (!wild_lo || mprotect(g2h(cpu_lo), wild_lo + HOST_PAGE - cpu_lo, PROT_READ) != 0) return 1;
  wild_lo += 2 * HOST_PAGE; /* gm_alloc leaves a guard page */
  if (gm_alloc((uint32_t)((1ull << 32) - wild_lo - HOST_PAGE), 0, "wild zone") != wild_lo) {
    fprintf(stderr, "sampletest: cannot place the wild zone\n");
    return 1;
  }
  mc_vec = malloc((size_t)(((1ull << 32) - wild_lo) / HOST_PAGE) + 1);
  arena_init = malloc(ARENA_SIZE);
  arena_after = malloc(ARENA_SIZE);
  signal(SIGALRM, timed_out);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);

  int failed = 0, functions = 0, compared = 0, untested = 0;
  stats total = {0, {0}};
  for (size_t i = 0; i < NLIBS; i++) {
    const lib *L = &libs[i];
    int lib_fns = 0, lib_cmp = 0;
    for (uint32_t k = 0; k < *L->count; k++) {
      uint32_t fn = L->sample[k];
      if (from && fn == from) from = 0;
      if ((only && fn != only) || from) continue;
      rng_state = seed ^ (0x9E3779B97F4A7C15ull * (fn + 1));
      stats st = {0, {0}};
      alarm(60);
      int bad = run_function(c, L, fn, iters, &st);
      alarm(0);
      failed += bad;
      functions++;
      lib_fns++;
      lib_cmp += st.compared;
      compared += st.compared;
      untested += !bad && st.compared == 0;
      for (int j = 0; j < 5; j++) total.skipped[j] += st.skipped[j];
      int also = is_also(L, fn);
      if (verbose || also)
        printf("sampletest: %s 0x%x%s: %d compared, skipped %d budget, %d left sample, %d svc, %d fault\n", L->file,
               fn, also ? " (regression case)" : "", st.compared, st.skipped[1], st.skipped[2], st.skipped[3],
               st.skipped[4]);
      /* A regression case must stay inside the sample (everything it reaches
       * was found by discovery and recompiled) and be compared on most
       * inputs. */
      if (also && !bad && (st.skipped[2] || st.compared < iters / 2)) {
        fprintf(stderr, "sampletest FAIL: regression case %s 0x%x compared on %d of %d inputs, %d left the sample\n",
                L->file, fn, st.compared, iters, st.skipped[2]);
        bad = 1;
        failed++;
      }
      if (bad && !verbose) break;
    }
    printf("sampletest: %-16s %3d functions, %5d inputs compared\n", L->file, lib_fns, lib_cmp);
  }
  c->ops->destroy(c);
  printf("sampletest: skipped inputs: %d budget, %d left the sample, %d svc, %d host fault (%d below 1 MiB); "
         "%d functions never compared\n",
         total.skipped[1], total.skipped[2], total.skipped[3], total.skipped[4], faults_low, untested);
  if (failed) {
    fprintf(stderr, "sampletest: %d of %d functions differ\n", failed, functions);
    return 1;
  }
  if (!functions || compared < functions * iters / 4) {
    fprintf(stderr, "sampletest FAIL: too few inputs compared (%d for %d functions)\n", compared, functions);
    return 1;
  }
  printf("SAMPLETEST OK (%d functions, %d inputs compared)\n", functions, compared);
  return 0;
}
