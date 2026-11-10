/*
 * The 32-bit guest address space.
 *
 * One 4 GiB host reservation backs the whole guest space, so a guest address
 * is just an offset from g_mem. Regions are committed read-write on
 * allocation; guest page permissions are the CPU backend's business.
 *
 * Where the host refuses 4 GiB of address space (an iPhone app without the
 * extended-virtual-addressing entitlement), gm_init
 * reserves less (3, 2, 1.5 or 1 GiB) and gm_alloc stays below gm_space().
 */
#ifndef GUEST_MEM_H
#define GUEST_MEM_H

#include <stdint.h>
#include <string.h>

#define GUEST_PAGE 0x1000u

extern uint8_t *g_mem;

int gm_init(void);
/* Bytes of guest space the reservation backs: 4 GiB unless gm_init fell back. */
uint64_t gm_space(void);
void gm_free(void);

/* Commits a zeroed region and returns its guest address, or 0 on failure. */
uint32_t gm_alloc(uint32_t size, uint32_t align, const char *tag);

/* Commits a zeroed region covering [addr, addr+size) at that address (rounded
 * out to host pages), for fixed guest addresses such as the kernel's helper
 * page. It must lie above everything gm_alloc may hand out. Returns addr, or 0. */
uint32_t gm_map_fixed(uint32_t addr, uint32_t size, const char *tag);
/* Name of the region that contains addr, or NULL. */
const char *gm_region_tag(uint32_t addr);

/* The committed regions, in allocation order (differential tests snapshot
 * guest memory with these). Returns the tag, or NULL past the last one. */
const char *gm_region_at(int i, uint32_t *start, uint32_t *end);

static inline void *g2h(uint32_t addr) { return g_mem + addr; }
static inline uint32_t h2g(const void *p) { return (uint32_t)((const uint8_t *)p - g_mem); }

static inline uint32_t gm_r32(uint32_t addr) {
  uint32_t v;
  memcpy(&v, g_mem + addr, 4);
  return v;
}

static inline void gm_w32(uint32_t addr, uint32_t v) { memcpy(g_mem + addr, &v, 4); }

#endif
