#include "rc_runtime.h"

#include <stdio.h>
#include <stdlib.h>

#define RC_MAX_MODULES 8

static const rc_module *modules[RC_MAX_MODULES];
static int n_modules;

int (*rc_fallback)(rc_state *s);

#define RC_MAX_RANGES 8

static struct {
  uint32_t lo, size, stride;
  rc_fn fn;
} ranges[RC_MAX_RANGES];
static int n_ranges; /* published with release order: ranges may be added while guest code runs */

void rc_register_module(const rc_module *m) {
  if (n_modules == RC_MAX_MODULES) {
    fprintf(stderr, "[recomp] too many modules (%s)\n", m->soname);
    abort();
  }
  modules[n_modules++] = m;
}

void rc_register_range(uint32_t lo, uint32_t hi, uint32_t stride, rc_fn fn) {
  if (n_ranges == RC_MAX_RANGES || !stride || hi <= lo) {
    fprintf(stderr, "[recomp] can't register range 0x%08x-0x%08x\n", lo, hi);
    abort();
  }
  int n = n_ranges;
  ranges[n].lo = lo;
  ranges[n].size = hi - lo;
  ranges[n].stride = stride;
  ranges[n].fn = fn;
  __atomic_store_n(&n_ranges, n + 1, __ATOMIC_RELEASE);
}

static rc_fn lookup_slow(uint32_t addr);

/* A small direct-mapped cache per host thread in front of the binary
 * searches (indirect calls and returns to the dispatcher are frequent).
 * Misses aren't cached: a range registered later may cover them. */
#define RC_CACHE 1024
static __thread struct {
  uint32_t addr;
  rc_fn fn;
} cache[RC_CACHE];

rc_fn rc_lookup(uint32_t addr) {
  uint32_t h = (addr >> 1) & (RC_CACHE - 1);
  if (cache[h].addr == addr && cache[h].fn) return cache[h].fn;
  rc_fn fn = lookup_slow(addr);
  if (fn) {
    cache[h].addr = addr;
    cache[h].fn = fn;
  }
  return fn;
}

static rc_fn lookup_slow(uint32_t addr) {
  int nr = __atomic_load_n(&n_ranges, __ATOMIC_ACQUIRE);
  for (int i = 0; i < nr; i++) {
    uint32_t off = addr - ranges[i].lo;
    if (off < ranges[i].size && off % ranges[i].stride == 0) return ranges[i].fn;
  }
  for (int i = 0; i < n_modules; i++) {
    const rc_module *m = modules[i];
    uint32_t off = addr - *m->base;
    if (!m->count || off > m->funcs[m->count - 1].addr) continue;
    uint32_t lo = 0, hi = m->count;
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      if (m->funcs[mid].addr < off)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo < m->count && m->funcs[lo].addr == off) return m->funcs[lo].fn;
  }
  return NULL;
}

int rc_run(rc_state *s, uint32_t stop) {
  for (;;) {
    uint32_t pc = rc_get_pc(s);
    if (pc == stop) return 0;
    rc_fn fn = rc_lookup(pc);
    if (fn) {
      fn(s);
    } else if (!rc_fallback || rc_fallback(s) != 0) {
      return -1;
    }
  }
}

int rc_call(rc_state *s, uint32_t ret, uint32_t sp) {
  for (;;) {
    uint32_t pc = rc_get_pc(s);
    if (pc == ret && s->r[13] == sp) return 0;
    if (s->r[13] > sp) return 1;
    rc_fn fn = rc_lookup(pc);
    if (fn) {
      fn(s);
    } else if (!rc_fallback || rc_fallback(s) != 0) {
      fprintf(stderr, "[recomp] no code for guest pc 0x%08x (call returning to 0x%08x)\n", s->r[15], ret);
      abort();
    }
  }
}

static void default_svc(rc_state *s, uint32_t imm) {
  fprintf(stderr, "[recomp] svc 0x%x before 0x%08x with no handler\n", imm, s->r[15]);
  abort();
}
static uint32_t default_cp15_read(rc_state *s, uint32_t raw) {
  fprintf(stderr, "[recomp] mrc 0x%08x before 0x%08x with no handler\n", raw, s->r[15]);
  abort();
}
void (*rc_svc)(rc_state *s, uint32_t imm) = default_svc;
uint32_t (*rc_cp15_read)(rc_state *s, uint32_t raw) = default_cp15_read;

void rc_bad_entry(rc_state *s) {
  fprintf(stderr, "[recomp] function entered at 0x%08x, which is not one of its entries\n", s->r[15]);
  abort();
}
