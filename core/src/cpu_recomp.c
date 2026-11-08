/*
 * The statically recompiled backend: guest code runs as the C that
 * droidlift-recomp generated from the game's libraries (droidlift_add_game
 * RECOMP), linked in as librc_<prefix> and listed in dl_recomp_modules. No
 * JIT and no executable memory.
 *
 * - The cpu is an rc_state (what cpu.h's accessors expose) plus TLS and a
 *   halt flag: creating one is a calloc (a game may start hundreds of threads).
 * - run dispatches through rc_lookup (the address tables of the recompiled
 *   modules, plus the loader's thunk areas) until the pc reaches the return
 *   sentinel cpu_call put in LR.
 * - Code with no recompiled function goes to our interpreter
 *   (interp_rc_fallback), counted per guest address for the
 *   fallback report (cpu_recomp_report).
 * - Import, host and JNI thunks (`svc #imm; bx lr`) run natively: their
 *   areas are registered with the dispatcher (rc_register_range), so neither
 *   recompiled nor interpreted callers bounce through the interpreter.
 * - SVCs go to cpu_svc, as from the other backends. A halt (the shims'
 *   faults, an executed return sentinel) can come from deep inside
 *   recompiled C frames; it is made to look like a longjmp past every guest
 *   frame (SP above all of them, pc at the sentinel), so each rc_call and
 *   interpreter loop on the host stack returns on its own (the unwind
 *   check) and no host frame is skipped.
 */
#include <mach/mach_time.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "cpu_interp.h"
#include "guest_mem.h"
#include "rc_runtime.h"
#include "so_util.h"

/* The game's recompiled libraries (<prefix>_table.c), listed by the
 * registry droidlift_add_game generates; NULL-terminated. */
extern const rc_module *const dl_recomp_modules[];

typedef struct rcpu {
  cpu base; /* must stay first: cpu * <-> rcpu * */
  rc_state st;
  volatile int halt_req;
} rcpu;

static rcpu *of_state(rc_state *s) { return (rcpu *)((char *)s - offsetof(rcpu, st)); }
static rcpu *self(cpu *c) { return (rcpu *)c; }

/* TPIDRURO/TPIDRURW per host thread (a guest thread and its nested callback
 * cpus share one). Nothing in the libraries reads them (reimpl/thread.c). */
static __thread uint32_t tls_ro, tls_rw;

/* An SP above every guest stack: rc_call and the interpreter fallback treat
 * it as "the frame was unwound" and return. */
#define UNWOUND_SP 0xFFFFFFF0u

static void unwind(rcpu *r) {
  r->halt_req = 1;
  r->st.r[13] = UNWOUND_SP;
  rc_set_pc(&r->st, r->base.ret_sentinel);
}

/* ---- hooks of the recompiled code ---------------------------------------- */

/* Deepest host stack use seen at an SVC, over all threads (for the report:
 * recompiled guest calls are host calls, and iOS's main thread has a small
 * stack). */
static uintptr_t max_stack_depth;
static __thread uintptr_t stack_top;

static void note_stack_depth(void) {
  if (!stack_top) stack_top = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
  uintptr_t depth = stack_top - (uintptr_t)__builtin_frame_address(0);
  uintptr_t seen = __atomic_load_n(&max_stack_depth, __ATOMIC_RELAXED);
  while (depth > seen &&
         !__atomic_compare_exchange_n(&max_stack_depth, &seen, depth, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
}

static void svc_hook(rc_state *s, uint32_t imm) {
  rcpu *r = of_state(s);
  note_stack_depth();
  cpu_svc(&r->base, imm);
  if (r->halt_req) unwind(r);
}

static uint32_t cp15_hook(rc_state *s, uint32_t raw) {
  (void)s;
  return ((raw >> 5) & 7) == 3 ? tls_ro : tls_rw; /* c13, c0, 3 / 2 */
}

/* A thunk: `svc #imm; bx lr` in ARM state, at s->r[15]. */
#define THUNK_BX_LR 0xE12FFF1Eu
static int is_thunk(uint32_t pc) {
  if ((pc & 3) || !gm_region_tag(pc) || !gm_region_tag(pc + 4)) return 0;
  return (gm_r32(pc) & 0xFF000000u) == 0xEF000000u && gm_r32(pc + 4) == THUNK_BX_LR;
}

static uint64_t thunk_calls;

static void thunk_fn(rc_state *s) {
  uint32_t pc = s->r[15];
  /* The registered areas are mapped: no region lookup on this hot path. */
  if ((s->cpsr & RC_CPSR_T) || (gm_r32(pc) & 0xFF000000u) != 0xEF000000u || gm_r32(pc + 4) != THUNK_BX_LR) {
    /* A registered thunk area, but no thunk here: let the interpreter fault. */
    if (interp_rc_fallback(s) != 0) {
      of_state(s)->base.faulted = 1;
      unwind(of_state(s));
    }
    return;
  }
  __atomic_fetch_add(&thunk_calls, 1, __ATOMIC_RELAXED);
  s->r[15] = pc + 4;
  svc_hook(s, gm_r32(pc) & 0x00FFFFFFu);
  if (s->r[15] == pc + 4 && !(s->cpsr & RC_CPSR_T)) rc_set_pc(s, s->r[14]); /* bx lr */
}

/* Thunk areas are found by their guest-memory tags; "host thunks" is
 * allocated lazily (the first dlsym), so it is registered when first hit. */
static const char *const thunk_tags[] = {"dynlib thunks", "jni thunks", "host thunks"};
#define N_THUNK_TAGS 3
static int thunk_registered[N_THUNK_TAGS];
static pthread_mutex_t thunk_lock = PTHREAD_MUTEX_INITIALIZER;

static int register_thunk_area(uint32_t addr) {
  const char *tag = gm_region_tag(addr);
  int done = 0;
  pthread_mutex_lock(&thunk_lock);
  for (int t = 0; tag && t < N_THUNK_TAGS; t++) {
    if (thunk_registered[t] || strcmp(tag, thunk_tags[t]) != 0) continue;
    uint32_t lo, hi;
    for (int i = 0; gm_region_at(i, &lo, &hi); i++) {
      if (addr < lo || addr > hi) continue;
      rc_register_range(lo, hi + 1, 8, thunk_fn);
      thunk_registered[t] = done = 1;
      break;
    }
  }
  pthread_mutex_unlock(&thunk_lock);
  return done;
}

static void register_thunk_areas(void) {
  uint32_t lo, hi;
  for (int i = 0; gm_region_at(i, &lo, &hi); i++) {
    const char *tag = gm_region_at(i, &lo, &hi);
    for (int t = 0; t < N_THUNK_TAGS; t++)
      if (!strcmp(tag, thunk_tags[t])) register_thunk_area(lo);
  }
}

/* ---- the interpreter fallback and its report ----------------------------- */

/* Fallback entries by guest address (bit 0 = Thumb): hits and host time.
 * Open addressing, lock-free: a slot's key is claimed with a CAS. */
#define HIT_SLOTS 8192
static struct {
  uint32_t pc;
  uint64_t hits, ticks;
} hit_table[HIT_SLOTS];
static uint64_t hit_overflow, fallback_calls, fallback_ticks;

/* DL_RECOMP_TRACE=1 logs each guest address the first time the fallback
 * is entered there, with LR and SP (debug aid). */
static int trace_new(void) {
  static int on = -1;
  if (on < 0) on = getenv("DL_RECOMP_TRACE") != NULL;
  return on;
}

static void describe(uint32_t pc, char *buf, size_t n);

static void count_hit(uint32_t pc, uint64_t ticks, uint32_t lr, uint32_t sp) {
  __atomic_fetch_add(&fallback_calls, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&fallback_ticks, ticks, __ATOMIC_RELAXED);
  uint32_t h = (pc * 2654435761u) % HIT_SLOTS;
  for (int probe = 0; probe < 64; probe++, h = (h + 1) % HIT_SLOTS) {
    uint32_t k = __atomic_load_n(&hit_table[h].pc, __ATOMIC_ACQUIRE);
    if (k == 0) {
      uint32_t zero = 0;
      if (!__atomic_compare_exchange_n(&hit_table[h].pc, &zero, pc, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        k = zero;
      } else {
        k = pc;
        if (trace_new()) {
          char w[512], wl[512];
          describe(pc, w, sizeof w);
          describe(lr, wl, sizeof wl);
          fprintf(stderr, "[recomp] fallback first entered at 0x%08x %s (lr 0x%08x %s, sp 0x%08x)\n", pc, w, lr, wl, sp);
        }
      }
    }
    if (k == pc) {
      __atomic_fetch_add(&hit_table[h].hits, 1, __ATOMIC_RELAXED);
      __atomic_fetch_add(&hit_table[h].ticks, ticks, __ATOMIC_RELAXED);
      return;
    }
  }
  __atomic_fetch_add(&hit_overflow, 1, __ATOMIC_RELAXED);
}

static int fallback(rc_state *s) {
  uint32_t pc = rc_get_pc(s);
  if (!(pc & 1) && is_thunk(pc) && register_thunk_area(pc)) return 0; /* dispatched natively next */
  uint32_t lr = s->r[14], sp = s->r[13];
  uint64_t t0 = mach_absolute_time();
  int r = interp_rc_fallback(s);
  count_hit(pc, mach_absolute_time() - t0, lr, sp);
  if (r != 0) {
    rcpu *c = of_state(s);
    fprintf(stderr, "[recomp] guest fault in interpreted code entered at 0x%08x (pc 0x%08x)\n", pc,
            s->r[15]);
    c->base.faulted = 1;
    unwind(c);
  }
  return 0;
}

/* Which module and nearest named function an address is in. */
static void describe(uint32_t pc, char *buf, size_t n) {
  for (int i = 0; dl_recomp_modules[i]; i++) {
    const rc_module *m = dl_recomp_modules[i];
    uint32_t off = (pc & ~1u) - *m->base;
    const so_module *so = so_module_find(m->soname);
    if (!so || off >= so->size) continue;
    const char *name = NULL;
    uint32_t at = 0;
    for (uint32_t j = 0; j < m->count; j++) {
      if ((m->funcs[j].addr & ~1u) > off) break;
      if (m->funcs[j].name) name = m->funcs[j].name, at = m->funcs[j].addr & ~1u;
    }
    if (name) snprintf(buf, n, "%s+0x%x  %s+0x%x", m->soname, off, name, off - at);
    else snprintf(buf, n, "%s+0x%x", m->soname, off);
    return;
  }
  const char *tag = gm_region_tag(pc & ~1u);
  snprintf(buf, n, "%s", tag ? tag : "unmapped");
}

static int by_hits(const void *a, const void *b) {
  uint64_t x = ((const uint64_t *)a)[1], y = ((const uint64_t *)b)[1];
  return x < y ? 1 : x > y ? -1 : 0;
}

uint64_t cpu_recomp_report(FILE *out, int limit) {
  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);
  uint64_t (*rows)[3] = calloc(HIT_SLOTS, sizeof *rows);
  int n = 0;
  for (int i = 0; i < HIT_SLOTS; i++) {
    if (!hit_table[i].pc) continue;
    rows[n][0] = hit_table[i].pc;
    rows[n][1] = hit_table[i].hits;
    rows[n][2] = hit_table[i].ticks;
    n++;
  }
  qsort(rows, (size_t)n, sizeof *rows, by_hits);
  double ms = (double)fallback_ticks * tb.numer / tb.denom / 1e6;
  fprintf(out, "[recomp] interpreter fallback: %llu entries at %d guest addresses, %.1f ms; %llu native thunk calls\n",
          (unsigned long long)fallback_calls, n, ms, (unsigned long long)thunk_calls);
  if (hit_overflow) fprintf(out, "[recomp] (%llu entries not tabulated)\n", (unsigned long long)hit_overflow);
  fprintf(out, "[recomp] deepest host stack at an SVC: %lu KiB\n", (unsigned long)(max_stack_depth >> 10));
  for (int i = 0; i < n && (limit <= 0 || i < limit); i++) {
    char where[512];
    describe((uint32_t)rows[i][0], where, sizeof where);
    fprintf(out, "[recomp]   %10llu  %8.2f ms  0x%08x %s  %s\n", (unsigned long long)rows[i][1],
            (double)rows[i][2] * tb.numer / tb.denom / 1e6, (uint32_t)rows[i][0] & ~1u,
            (rows[i][0] & 1) ? "T" : "A", where);
  }
  if (limit > 0 && n > limit) fprintf(out, "[recomp]   ... %d more\n", n - limit);
  free(rows);
  return fallback_calls;
}

/* ---- one-time set-up ------------------------------------------------------ */

static int attach_status = -1;

static void attach(void) {
  for (int i = 0; dl_recomp_modules[i]; i++) {
    const rc_module *m = dl_recomp_modules[i];
    const so_module *so = so_module_find(m->soname);
    if (!so) {
      fprintf(stderr, "[recomp] %s isn't loaded: load the libraries before creating a cpu\n", m->soname);
      attach_status = -1;
      return;
    }
    /* The recompiled code addresses its library as base + ELF address;
     * so_relocate already patched the data the code loads at run time. */
    *m->base = so->base;
    rc_register_module(m);
  }
  rc_svc = svc_hook;
  rc_cp15_read = cp15_hook;
  rc_fallback = fallback;
  register_thunk_areas();
  attach_status = 0;
}

/* ---- cpu_ops ------------------------------------------------------------- */

static int op_run(cpu *c) {
  rcpu *r = self(c);
  rc_state *s = &r->st;
  r->halt_req = 0;
  for (;;) {
    if (r->halt_req) break;
    uint32_t pc = rc_get_pc(s);
    if (pc == c->ret_sentinel) break;
    rc_fn fn = rc_lookup(pc);
    if (fn) fn(s);
    else fallback(s);
  }
  return c->faulted ? -1 : 0;
}

static void op_halt(cpu *c) { self(c)->halt_req = 1; }

static void op_invalidate(cpu *c, uint32_t addr, uint32_t size) {
  /* Recompiled code is fixed; the interpreter caches nothing. */
  (void)c;
  (void)addr;
  (void)size;
}

static void op_destroy(cpu *c) {
  if (c->nested) c->nested->ops->destroy(c->nested);
  free(c);
}

static uint32_t op_get_reg(cpu *c, int r) { return self(c)->st.r[r]; }
static void op_set_reg(cpu *c, int r, uint32_t v) { self(c)->st.r[r] = v; }
static uint32_t op_get_cpsr(cpu *c) { return self(c)->st.cpsr; }
static void op_set_cpsr(cpu *c, uint32_t v) { self(c)->st.cpsr = v; }
static uint32_t op_get_ext(cpu *c, int i) { return self(c)->st.ext[i]; }
static void op_set_ext(cpu *c, int i, uint32_t v) { self(c)->st.ext[i] = v; }
static uint32_t op_get_fpscr(cpu *c) { return self(c)->st.fpscr; }
static void op_set_fpscr(cpu *c, uint32_t v) { self(c)->st.fpscr = v; }

static const cpu_ops recomp_ops = {
    "recomp",     op_destroy,   op_get_reg, op_set_reg, op_get_cpsr, op_set_cpsr, op_get_ext,
    op_set_ext,   op_get_fpscr, op_set_fpscr, op_run,   op_halt,     op_invalidate,
    cpu_recomp_create,
};

static pthread_once_t attach_once = PTHREAD_ONCE_INIT;

cpu *cpu_recomp_create(void) {
  pthread_once(&attach_once, attach);
  if (attach_status != 0) return NULL;
  rcpu *r = calloc(1, sizeof *r);
  if (!r) return NULL;
  r->base.ops = &recomp_ops;
  return &r->base;
}
