#include "guest_mem.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#define FULL_SPACE (1ull << 32)
/* Keep the low 1 MiB unmapped so stray null-ish pointers fault. */
#define GUEST_ALLOC_START 0x00100000u
#define MAX_REGIONS 512
/* Host commit granularity: Apple Silicon pages are 16 KiB. */
#define HOST_PAGE 0x4000u

uint8_t *g_mem;
/* How much of the guest space the reservation backs: 4 GiB, or less where
 * the host refuses that much address space (an iPhone app without
 * the extended-virtual-addressing entitlement, which a free team can't get). */
static uint64_t guest_space = FULL_SPACE;

static uint64_t next_free = GUEST_ALLOC_START;
/* gm_alloc stays below this: the lowest gm_map_fixed region, less a guard. */
static uint64_t alloc_limit = FULL_SPACE;
static struct {
  uint32_t start, end;
  const char *tag;
} regions[MAX_REGIONS];
static int n_regions; /* published with release order; readers take no lock */
static pthread_mutex_t alloc_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t gm_space(void) { return guest_space; }

int gm_init(void) {
  /* Largest first. DL_GUEST_MB caps it, to test the fallback on the Mac. */
  static const uint64_t sizes_mib[] = {4096, 3072, 2048, 1536, 1024};
  const size_t n_sizes = sizeof sizes_mib / sizeof *sizes_mib;
  uint64_t cap = FULL_SPACE >> 20;
  const char *env = getenv("DL_GUEST_MB");
  if (env && *env) {
    cap = strtoull(env, NULL, 10);
    if (cap < 256 || cap > (FULL_SPACE >> 20)) {
      fprintf(stderr, "gm_init: DL_GUEST_MB=%s is not 256..4096\n", env);
      return -1;
    }
  }
  /* The cap first, then each listed size below it. */
  for (size_t i = 0; i <= n_sizes; i++) {
    uint64_t mib = i == 0 ? cap : sizes_mib[i - 1];
    if (i > 0 && mib >= cap) continue;
    void *p = mmap(NULL, mib << 20, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) continue;
    g_mem = p;
    guest_space = mib << 20;
    /* The game's own memory all comes from gm_alloc, so it fits; but a wild
     * guest pointer above the reservation no longer faults. */
    if (guest_space != FULL_SPACE)
      fprintf(stderr, "[mem] guest reservation %llu MiB, not 4 GiB: guest memory ends at 0x%llx\n",
              (unsigned long long)mib, (unsigned long long)guest_space);
    return 0;
  }
  perror("gm_init: mmap guest reservation (4 GiB down to 1 GiB)");
  return -1;
}

void gm_free(void) {
  if (g_mem) munmap(g_mem, guest_space);
  g_mem = NULL;
  next_free = GUEST_ALLOC_START;
  alloc_limit = FULL_SPACE;
  n_regions = 0;
}

uint32_t gm_alloc(uint32_t size, uint32_t align, const char *tag) {
  pthread_mutex_lock(&alloc_lock);
  uint32_t r = 0;
  if (align < HOST_PAGE) align = HOST_PAGE;
  uint64_t start = (next_free + align - 1) & ~(uint64_t)(align - 1);
  uint64_t len = ((uint64_t)size + HOST_PAGE - 1) & ~(uint64_t)(HOST_PAGE - 1);
  if (len == 0 || start + len > guest_space || start + len > alloc_limit || n_regions == MAX_REGIONS) {
    fprintf(stderr, "gm_alloc: cannot place %s (0x%x bytes)\n", tag, size);
    goto out;
  }
  if (mprotect(g_mem + start, len, PROT_READ | PROT_WRITE) != 0) {
    perror("gm_alloc: mprotect");
    goto out;
  }
  regions[n_regions].start = (uint32_t)start;
  regions[n_regions].end = (uint32_t)(start + len - 1);
  regions[n_regions].tag = tag;
  __atomic_store_n(&n_regions, n_regions + 1, __ATOMIC_RELEASE);
  /* One guard page between regions. */
  next_free = start + len + HOST_PAGE;
  r = (uint32_t)start;
out:
  pthread_mutex_unlock(&alloc_lock);
  return r;
}

uint32_t gm_map_fixed(uint32_t addr, uint32_t size, const char *tag) {
  pthread_mutex_lock(&alloc_lock);
  uint32_t r = 0;
  uint64_t start = addr & ~(uint64_t)(HOST_PAGE - 1);
  uint64_t len = (((uint64_t)addr + size + HOST_PAGE - 1) & ~(uint64_t)(HOST_PAGE - 1)) - start;
  if (size == 0 || start + len > guest_space || start < next_free || n_regions == MAX_REGIONS) {
    fprintf(stderr, "gm_map_fixed: cannot place %s at 0x%08x\n", tag, addr);
    goto out;
  }
  if (mprotect(g_mem + start, len, PROT_READ | PROT_WRITE) != 0) {
    perror("gm_map_fixed: mprotect");
    goto out;
  }
  regions[n_regions].start = (uint32_t)start;
  regions[n_regions].end = (uint32_t)(start + len - 1);
  regions[n_regions].tag = tag;
  __atomic_store_n(&n_regions, n_regions + 1, __ATOMIC_RELEASE);
  if (start - HOST_PAGE < alloc_limit) alloc_limit = start - HOST_PAGE;
  r = addr;
out:
  pthread_mutex_unlock(&alloc_lock);
  return r;
}

const char *gm_region_at(int i, uint32_t *start, uint32_t *end) {
  int n = __atomic_load_n(&n_regions, __ATOMIC_ACQUIRE);
  if (i < 0 || i >= n) return NULL;
  *start = regions[i].start;
  *end = regions[i].end;
  return regions[i].tag;
}

const char *gm_region_tag(uint32_t addr) {
  int n = __atomic_load_n(&n_regions, __ATOMIC_ACQUIRE);
  for (int i = 0; i < n; i++)
    if (addr >= regions[i].start && addr <= regions[i].end) return regions[i].tag;
  return NULL;
}
