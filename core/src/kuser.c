/*
 * The ARM Linux kernel's user helpers ("kuser helpers"): a page at 0xffff0000
 * the kernel maps into every 32-bit process, holding small routines at fixed
 * addresses. Code built for armeabi (ARMv5TE, no ldrex/strex) calls them for
 * atomics; armeabi-v7a code doesn't, which is why the first port never needed
 * them. The routines here are the kernel's ARMv7 versions (assembled with
 * clang and checked with llvm-objdump), so every CPU backend runs them as
 * ordinary guest code:
 *
 *   0xffff0fa0 __kuser_memory_barrier   dmb ish
 *   0xffff0fc0 __kuser_cmpxchg          r0 old, r1 new, r2 ptr -> r0 0 (and C)
 *                                       on success, non-zero otherwise
 *   0xffff0fe0 __kuser_get_tls          r0 = the word at 0xffff0fec (no
 *                                       guest TLS is set up: 0)
 *   0xffff0ffc __kuser_helper_version   3 (the three above)
 *
 * The page needs the full 4 GiB guest reservation; where gm_init fell back to
 * less (an iPhone without the extended address space entitlement), it can't
 * be mapped and kuser_init says so.
 */
#include <stdio.h>

#include "guest_mem.h"
#include "kuser.h"

#define KUSER_PAGE 0xffff0000u

static const uint32_t barrier[] = {
    0xf57ff05b, /* dmb ish */
    0xe12fff1e, /* bx lr */
};

static const uint32_t cmpxchg[] = {
    0xe1923f9f, /* 1: ldrex r3, [r2] */
    0xe0533000, /* subs r3, r3, r0 */
    0x01823f91, /* strexeq r3, r1, [r2] */
    0x03330001, /* teqeq r3, #1 */
    0x0afffffa, /* beq 1b */
    0xe2730000, /* rsbs r0, r3, #0 */
    0xe12fff1e, /* bx lr */
};

static const uint32_t get_tls[] = {
    0xe59f0004, /* ldr r0, [pc, #4]: the word at 0xffff0fec */
    0xe12fff1e, /* bx lr */
};

static void put(uint32_t addr, const uint32_t *code, unsigned n) {
  for (unsigned i = 0; i < n; i++) gm_w32(addr + 4 * i, code[i]);
}

int kuser_init(void) {
  if (gm_space() < (1ull << 32)) {
    fprintf(stderr, "[kuser] guest space below 4 GiB: no kernel helper page (armeabi atomics will fault)\n");
    return 0;
  }
  if (gm_map_fixed(KUSER_PAGE, 0x1000, "kuser helpers") != KUSER_PAGE) return -1;
  put(0xffff0fa0, barrier, sizeof barrier / 4);
  put(0xffff0fc0, cmpxchg, sizeof cmpxchg / 4);
  put(0xffff0fe0, get_tls, sizeof get_tls / 4);
  gm_w32(0xffff0fec, 0);
  gm_w32(0xffff0ffc, 3);
  return 0;
}
