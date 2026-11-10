/*
 * The guest heap and the libc memory/string primitives.
 *
 * The heap is a power-of-two size-class allocator in one guest region: every
 * block has a 16-byte header holding its class, and freed blocks go on a
 * per-class free list threaded through guest memory. Simple, and it keeps
 * 16-byte alignment (bionic's malloc guarantees 8). One host lock guards the
 * free lists, since guest threads run on host threads.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* --- mmap family ---
 *
 * The guest space is one committed reservation, so a mapping is page-aligned
 * heap memory: zeroed for anonymous maps, filled from the file for file maps
 * (guest fds are host fds). Writes to a MAP_SHARED file mapping are not
 * written back; the game is warned about once. Protections are ignored, as
 * for the rest of guest memory. */

#define G_MAP_SHARED 0x01
#define G_MAP_FIXED 0x10
#define G_MAP_ANONYMOUS 0x20
#define G_MREMAP_MAYMOVE 1
#define G_PROT_WRITE 0x2
#define MAP_FAILED_G 0xFFFFFFFFu
/* LuaJIT grows its heap in 128 KiB mmaps: the whole 1 GiB heap's worth. */
#define MAX_MAPS 16384

static struct {
  uint32_t addr, len;
} maps[MAX_MAPS];
/* Slots at and above this were never used: lookups stop here. */
static int maps_used;
static pthread_mutex_t maps_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t page_round(uint32_t n) { return (n + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1); }

static int map_find(uint32_t addr) {
  for (int i = 0; i < maps_used; i++)
    if (maps[i].addr == addr && maps[i].len) return i;
  return -1;
}

static int map_add(uint32_t addr, uint32_t len) {
  for (int i = 0; i < MAX_MAPS; i++)
    if (!maps[i].len) {
      maps[i].addr = addr, maps[i].len = len;
      if (i >= maps_used) maps_used = i + 1;
      return 0;
    }
  return -1;
}

static int trace_mmap(void) {
  static int on = -1;
  if (on < 0) on = getenv("DL_TRACE_MMAP") != NULL;
  return on;
}

static int maps_live(void) {
  int n = 0;
  for (int i = 0; i < maps_used; i++) n += maps[i].len != 0;
  return n;
}

static void mmap_fail(cpu *c, int host_errno) {
  guest_set_errno(host_errno);
  cpu_ret(c, MAP_FAILED_G);
}

/* mmap(addr, len, prot, flags, fd, off_t offset): offset is 32-bit. */
static void h_mmap(cpu *c) {
  uint32_t len = cpu_arg(c, 1), prot = cpu_arg(c, 2), flags = cpu_arg(c, 3);
  int fd = (int32_t)cpu_arg(c, 4);
  uint32_t off = cpu_arg(c, 5);
  if (!len || (flags & G_MAP_FIXED)) {
    if (flags & G_MAP_FIXED) fprintf(stderr, "[mem] mmap with MAP_FIXED is not supported\n");
    mmap_fail(c, EINVAL);
    return;
  }
  uint32_t size = page_round(len);
  uint32_t a = guest_memalign(GUEST_PAGE, size);
  if (!a) {
    mmap_fail(c, ENOMEM);
    return;
  }
  memset(g2h(a), 0, size);
  if (!(flags & G_MAP_ANONYMOUS)) {
    if ((flags & G_MAP_SHARED) && (prot & G_PROT_WRITE)) {
      static int warned;
      if (!warned++) fprintf(stderr, "[mem] writable MAP_SHARED file mapping: writes stay in memory\n");
    }
    ssize_t got = pread(fd, g2h(a), len, (off_t)off);
    if (got < 0) {
      int e = errno;
      guest_free(a);
      mmap_fail(c, e);
      return;
    }
  }
  pthread_mutex_lock(&maps_lock);
  int ok = map_add(a, size) == 0;
  pthread_mutex_unlock(&maps_lock);
  if (!ok) {
    fprintf(stderr, "[mem] mmap table full\n");
    guest_free(a);
    mmap_fail(c, ENOMEM);
    return;
  }
  if (trace_mmap()) fprintf(stderr, "[mmap] map %08x %u fd %d lr %08x live %d\n", a, size, fd, c->ops->get_reg(c, CPU_REG_LR), maps_live());
  cpu_ret(c, a);
}

/* munmap(addr, len): frees a whole mapping. A partial unmap keeps the memory
 * (the guest can't tell, short of mapping something new at that address). */
static void h_munmap(cpu *c) {
  uint32_t addr = cpu_arg(c, 0), len = page_round(cpu_arg(c, 1));
  pthread_mutex_lock(&maps_lock);
  int i = map_find(addr);
  int whole = i >= 0 && len >= maps[i].len;
  if (whole) maps[i].len = 0;
  if (trace_mmap())
    fprintf(stderr, "[mmap] unmap %08x %u %s lr %08x\n", addr, len, whole ? "whole" : i >= 0 ? "PART" : "UNKNOWN", c->ops->get_reg(c, CPU_REG_LR));
  pthread_mutex_unlock(&maps_lock);
  if (whole) guest_free(addr);
  cpu_ret(c, 0);
}

static void h_mprotect(cpu *c) { cpu_ret(c, 0); }

/* mremap(old, old_len, new_len, flags, ...) */
static void h_mremap(cpu *c) {
  uint32_t old = cpu_arg(c, 0), new_len = page_round(cpu_arg(c, 2)), flags = cpu_arg(c, 3);
  pthread_mutex_lock(&maps_lock);
  int i = map_find(old);
  uint32_t have = i >= 0 ? maps[i].len : 0;
  if (trace_mmap()) fprintf(stderr, "[mmap] remap %08x %u -> %u flags %u %s\n", old, have, new_len, flags, i < 0 ? "UNKNOWN" : "");
  if (i >= 0 && new_len <= have) {
    pthread_mutex_unlock(&maps_lock);
    cpu_ret(c, old); /* shrinking: keep the block */
    return;
  }
  pthread_mutex_unlock(&maps_lock);
  if (i < 0 || !(flags & G_MREMAP_MAYMOVE)) {
    mmap_fail(c, i < 0 ? EINVAL : ENOMEM);
    return;
  }
  uint32_t a = guest_memalign(GUEST_PAGE, new_len);
  if (!a) {
    mmap_fail(c, ENOMEM);
    return;
  }
  memcpy(g2h(a), g2h(old), have);
  memset((uint8_t *)g2h(a) + have, 0, new_len - have);
  pthread_mutex_lock(&maps_lock);
  maps[i].addr = a, maps[i].len = new_len;
  pthread_mutex_unlock(&maps_lock);
  guest_free(old);
  cpu_ret(c, a);
}

const dynlib_impl reimpl_mem[] = {
    {"mmap", h_mmap},     {"munmap", h_munmap},   {"mprotect", h_mprotect}, {"mremap", h_mremap},
    {"calloc", h_calloc}, {"free", h_free},       {"malloc", h_malloc}, {"memalign", h_memalign}, {"memchr", h_memchr}, {"memcmp", h_memcmp},
    {"memmem", h_memmem}, {"memrchr", h_memrchr},
    {"memcpy", h_memcpy}, {"memmove", h_memmove}, {"memset", h_memset}, {"realloc", h_realloc},
    {"strcmp", h_strcmp}, {"strlen", h_strlen},   {NULL, NULL},
};
