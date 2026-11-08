/*
 * The guest heap and the libc memory/string primitives.
 *
 * The heap is a power-of-two size-class allocator in one guest region: every
 * block has a 16-byte header holding its class, and freed blocks go on a
 * per-class free list threaded through guest memory. Simple, and it keeps
 * 16-byte alignment (bionic's malloc guarantees 8). One host lock guards the
 * free lists, since guest threads run on host threads.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cpu.h"
#include "../guest_mem.h"
#include "reimpl.h"

#define HEAP_SIZE (1u << 30)
/* Guest space kept outside the heap: the libraries (~40 MiB), thread stacks
 * and thunks. */
#define HEAP_ROOM (256ull << 20)
#define HDR 16u
#define MIN_CLASS 5 /* 32-byte blocks */
#define MAX_CLASS 30

static uint32_t heap_base, heap_next, heap_end;
static uint32_t free_list[MAX_CLASS + 1];
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;

void guest_heap_stats(void) {
  if (heap_base)
    fprintf(stderr, "[mem] guest heap: %u MiB of %u MiB used\n", (heap_next - heap_base) >> 20,
            (heap_end - heap_base) >> 20);
}

static int heap_init(void) {
  /* 1 GiB, or what a smaller guest reservation leaves after the libraries,
   * stacks and thunks (gm_init's fallback). */
  uint32_t size = HEAP_SIZE;
  if (gm_space() < (uint64_t)HEAP_SIZE + HEAP_ROOM) size = (uint32_t)(gm_space() - HEAP_ROOM);
  heap_base = gm_alloc(size, 0, "guest heap");
  if (!heap_base) return -1;
  heap_next = heap_base;
  heap_end = heap_base + size;
  return 0;
}

static int size_class(uint32_t size) {
  uint64_t need = (uint64_t)size + HDR;
  int k = MIN_CLASS;
  while (k <= MAX_CLASS && (1ull << k) < need) k++;
  return k;
}

uint32_t guest_malloc(uint32_t size) {
  int k = size_class(size);
  if (k > MAX_CLASS) return 0;
  pthread_mutex_lock(&heap_lock);
  uint32_t block = 0;
  if (!heap_base && heap_init() != 0) goto out;
  block = free_list[k];
  if (block) {
    free_list[k] = gm_r32(block + HDR);
  } else if ((uint64_t)heap_next + (1ull << k) <= heap_end) {
    block = heap_next;
    heap_next += 1u << k;
  }
out:
  pthread_mutex_unlock(&heap_lock);
  if (!block) return 0;
  gm_w32(block, (uint32_t)k);
  return block + HDR;
}

/* memalign blocks with alignment above 16 carry this header in place of a
 * size class, followed by the pointer guest_malloc returned. */
#define ALIGNED_MARK 0xA11A11EDu

uint32_t guest_memalign(uint32_t align, uint32_t size) {
  if (align <= HDR) return guest_malloc(size);
  if (align & (align - 1)) return 0;
  uint64_t need = (uint64_t)size + align + HDR;
  if (need > UINT32_MAX) return 0;
  uint32_t raw = guest_malloc((uint32_t)need);
  if (!raw) return 0;
  uint32_t p = (raw + HDR + align - 1) & ~(align - 1);
  gm_w32(p - HDR, ALIGNED_MARK);
  gm_w32(p - HDR + 4, raw);
  gm_w32(p - HDR + 8, size);
  return p;
}

void guest_free(uint32_t ptr) {
  if (!ptr) return;
  if (gm_r32(ptr - HDR) == ALIGNED_MARK) ptr = gm_r32(ptr - HDR + 4);
  uint32_t block = ptr - HDR;
  uint32_t k = gm_r32(block);
  pthread_mutex_lock(&heap_lock);
  gm_w32(block + HDR, free_list[k]);
  free_list[k] = block;
  pthread_mutex_unlock(&heap_lock);
}

static uint32_t usable_size(uint32_t ptr) {
  uint32_t k = gm_r32(ptr - HDR);
  return k == ALIGNED_MARK ? gm_r32(ptr - HDR + 8) : (1u << k) - HDR;
}

_Static_assert(sizeof(((guest_heap_state *)0)->free_list) == sizeof free_list,
               "guest_heap_state matches the allocator");

void guest_heap_save(guest_heap_state *st) {
  pthread_mutex_lock(&heap_lock);
  st->base = heap_base;
  st->next = heap_next;
  memcpy(st->free_list, free_list, sizeof free_list);
  pthread_mutex_unlock(&heap_lock);
}

void guest_heap_restore(const guest_heap_state *st) {
  pthread_mutex_lock(&heap_lock);
  heap_next = st->next;
  memcpy(free_list, st->free_list, sizeof free_list);
  pthread_mutex_unlock(&heap_lock);
}

static void h_malloc(cpu *c) { cpu_ret(c, guest_malloc(cpu_arg(c, 0))); }

static void h_free(cpu *c) { guest_free(cpu_arg(c, 0)); }

static void h_memalign(cpu *c) { cpu_ret(c, guest_memalign(cpu_arg(c, 0), cpu_arg(c, 1))); }

static void h_calloc(cpu *c) {
  uint64_t n = (uint64_t)cpu_arg(c, 0) * cpu_arg(c, 1);
  uint32_t p = n > UINT32_MAX ? 0 : guest_malloc((uint32_t)n);
  if (p) memset(g2h(p), 0, (size_t)n);
  cpu_ret(c, p);
}

static void h_realloc(cpu *c) {
  uint32_t old = cpu_arg(c, 0), size = cpu_arg(c, 1);
  if (!old) {
    cpu_ret(c, guest_malloc(size));
    return;
  }
  if (size == 0) {
    guest_free(old);
    cpu_ret(c, 0);
    return;
  }
  uint32_t have = usable_size(old);
  if (size <= have) {
    cpu_ret(c, old);
    return;
  }
  uint32_t p = guest_malloc(size);
  if (p) {
    memcpy(g2h(p), g2h(old), have);
    guest_free(old);
  }
  cpu_ret(c, p);
}

static void h_memcpy(cpu *c) {
  memcpy(g2h(cpu_arg(c, 0)), g2h(cpu_arg(c, 1)), cpu_arg(c, 2));
  cpu_ret(c, cpu_arg(c, 0));
}

static void h_memmove(cpu *c) {
  memmove(g2h(cpu_arg(c, 0)), g2h(cpu_arg(c, 1)), cpu_arg(c, 2));
  cpu_ret(c, cpu_arg(c, 0));
}

static void h_memset(cpu *c) {
  memset(g2h(cpu_arg(c, 0)), (int)cpu_arg(c, 1), cpu_arg(c, 2));
  cpu_ret(c, cpu_arg(c, 0));
}

static void h_memcmp(cpu *c) {
  cpu_ret(c, (uint32_t)memcmp(g2h(cpu_arg(c, 0)), g2h(cpu_arg(c, 1)), cpu_arg(c, 2)));
}

/* memchr, memrchr, memmem: results map back to guest addresses. */
static void h_memchr(cpu *c) {
  const uint8_t *p = memchr(g2h(cpu_arg(c, 0)), (int)cpu_arg(c, 1), cpu_arg(c, 2));
  cpu_ret(c, p ? h2g(p) : 0);
}

static void h_memrchr(cpu *c) {
  const uint8_t *s = g2h(cpu_arg(c, 0));
  uint8_t ch = (uint8_t)cpu_arg(c, 1);
  for (uint32_t n = cpu_arg(c, 2); n-- > 0;)
    if (s[n] == ch) {
      cpu_ret(c, h2g(s + n));
      return;
    }
  cpu_ret(c, 0);
}

static void h_memmem(cpu *c) {
  const uint8_t *p = memmem(g2h(cpu_arg(c, 0)), cpu_arg(c, 1), g2h(cpu_arg(c, 2)), cpu_arg(c, 3));
  cpu_ret(c, p ? h2g(p) : 0);
}

static void h_strlen(cpu *c) { cpu_ret(c, (uint32_t)strlen(g2h(cpu_arg(c, 0)))); }

static void h_strcmp(cpu *c) {
  cpu_ret(c, (uint32_t)strcmp(g2h(cpu_arg(c, 0)), g2h(cpu_arg(c, 1))));
}

const dynlib_impl reimpl_mem[] = {
    {"calloc", h_calloc}, {"free", h_free},       {"malloc", h_malloc}, {"memalign", h_memalign}, {"memchr", h_memchr}, {"memcmp", h_memcmp},
    {"memmem", h_memmem}, {"memrchr", h_memrchr},
    {"memcpy", h_memcpy}, {"memmove", h_memmove}, {"memset", h_memset}, {"realloc", h_realloc},
    {"strcmp", h_strcmp}, {"strlen", h_strlen},   {NULL, NULL},
};
