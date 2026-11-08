/*
 * Differential test for the static recompiler: hand-picked functions.
 *
 * Maps a library's loadable segments into guest memory, then runs each
 * recompiled function and the same guest code under dynarmic (through the
 * CPU-backend interface, core/src/cpu.h) from identical starting states:
 * every register r0-r12 random, random NZCV, random arguments and buffer
 * contents. After each pair of runs it compares r0-r14, the CPSR flags
 * (NZCVQ, GE, T) and a 256 KiB arena holding the stack and all buffers; after
 * each function it checks that the image itself was left alone.
 *
 *   rc-difftest --elf LIB.so [--iters N] [--seed S] [--only NAME]
 *
 * Prints "DIFFTEST OK" when every function matched on every input. A
 * function that runs for over a minute (a mistranslated loop) fails.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cpu.h"
#include "elf32.h"
#include "guest_mem.h"
#include "rc_runtime.h"

/* The recompiled functions' prefix (droidlift_add_recomp_difftest PREFIX). */
#define DL_CAT2(a, b) a##b
#define DL_CAT(a, b) DL_CAT2(a, b)
#define lib_module DL_CAT(DL_DIFFTEST_PREFIX, _module)
#define lib_base DL_CAT(DL_DIFFTEST_PREFIX, _base)
extern const rc_module lib_module;
extern uint32_t lib_base;

/* cpu.c routes guest SVCs to the loader's dispatchers; here only cpu_call's
 * return sentinel may be hit, so anything else is a test failure. */
static void unexpected_svc(cpu *c, const char *what, uint32_t i) {
  fprintf(stderr, "difftest: unexpected %s svc 0x%x\n", what, i);
  c->faulted = 1;
  c->ops->halt(c);
}
void dynlib_dispatch(cpu *c, uint32_t i) { unexpected_svc(c, "dynlib", i); }
void dynlib_host_dispatch(cpu *c, uint32_t i) { unexpected_svc(c, "host", i); }
void jni_dispatch(cpu *c, uint32_t i) { unexpected_svc(c, "jni", i); }

/* ---- Inputs --------------------------------------------------------------- */

static uint64_t rng_state;

static uint32_t rnd(void) {
  /* xorshift64* */
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;
  return (uint32_t)((rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}
static uint32_t rnd_below(uint32_t n) { return n ? rnd() % n : 0; }

/* A word whose value spans every bit length, with the edge values often. */
static uint32_t rnd_word(void) {
  static const uint32_t edge[] = {0, 1, 2, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0xfffffffeu, 0x10000u};
  if (rnd_below(4) == 0) return edge[rnd_below(8)];
  uint32_t sh = rnd_below(33);
  return sh == 32 ? 0 : rnd() >> sh;
}

#define ARENA_SIZE 0x40000u
#define STACK_TOP (ARENA_SIZE - 0x1000u) /* offset of the initial SP */

static uint32_t arena;      /* guest address */
static uint8_t *arena_host; /* g2h(arena) */

/* Guest pointer to a random spot in one of 3 slots of 64 KiB below the
 * stack, so the buffers of one call never overlap. */
static uint32_t buf(int slot, uint32_t align) {
  return arena + (uint32_t)slot * 0x10000u + (rnd_below(64) & ~(align - 1));
}

static void fill_words(uint32_t g, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) gm_w32(g + 4 * i, rnd_word());
}

/* Each setup sets the call's arguments in r[0..3] (the rest of the state is
 * already random) and fills the arena. */
typedef void (*setup_fn)(uint32_t *r);

static void setup_memcmp(uint32_t *r) {
  uint32_t len = rnd_below(5) == 0 ? rnd_below(4) : rnd_below(300);
  r[0] = buf(0, 1);
  r[1] = buf(1, 1);
  if (rnd_below(2)) {
    memcpy(g2h(r[1]), g2h(r[0]), len);
    if (len && rnd_below(2)) ((uint8_t *)g2h(r[1]))[rnd_below(len)] ^= (uint8_t)(1u << rnd_below(8));
  }
  r[2] = len;
}

static void setup_num_bits_word(uint32_t *r) { r[0] = rnd_word(); }

/* BIGNUM { BN_ULONG *d; int top; int dmax; int neg; int flags; } */
static void setup_num_bits(uint32_t *r) {
  uint32_t bn = buf(0, 4), d = buf(1, 4), top = rnd_below(9);
  fill_words(d, 8);
  gm_w32(bn, d);
  gm_w32(bn + 4, top);
  r[0] = bn;
}

/* bn_mul_add_words / bn_mul_words (rp, ap, num, w); num may be <= 0. */
static void setup_mul_words(uint32_t *r) {
  int32_t num = (int32_t)rnd_below(40) - 3;
  r[0] = buf(0, 4);
  r[1] = buf(1, 4);
  fill_words(r[0], 40);
  fill_words(r[1], 40);
  r[2] = (uint32_t)num;
  r[3] = rnd_word();
}

/* bn_sqr_words(r, a, n): r holds 2n words. */
static void setup_sqr_words(uint32_t *r) {
  r[0] = buf(0, 4);
  r[1] = buf(1, 4);
  fill_words(r[1], 40);
  r[2] = (uint32_t)((int32_t)rnd_below(40) - 3);
}

/* bn_add_words / bn_sub_words (r, a, b, n) and bn_cmp_words (a, b, n). */
static void setup_add_words(uint32_t *r) {
  r[0] = buf(0, 4);
  r[1] = buf(1, 4);
  r[2] = buf(2, 4);
  fill_words(r[1], 40);
  fill_words(r[2], 40);
  if (rnd_below(4) == 0) memcpy(g2h(r[2]), g2h(r[1]), 160);
  r[3] = (uint32_t)((int32_t)rnd_below(40) - 3);
}
static void setup_cmp_words(uint32_t *r) {
  setup_add_words(r);
  r[0] = r[1];
  r[1] = r[2];
  r[2] = r[3];
}

static void setup_cleanse(uint32_t *r) {
  r[0] = buf(0, 1);
  r[1] = rnd_below(4) == 0 ? rnd_below(8) : rnd_below(500);
}

/* md5/sha1/sha256_block_data_order(ctx, data, num): num 64-byte blocks. */
static void setup_block(uint32_t *r) {
  r[0] = buf(0, 4);
  r[1] = buf(1, rnd_below(2) ? 4 : 1); /* misaligned input too */
  fill_words(r[0], 8);
  r[2] = 1 + rnd_below(6);
  for (uint32_t i = 0; i < r[2] * 64; i++) ((uint8_t *)g2h(r[1]))[i] = (uint8_t)rnd();
}

/* RC4(RC4_KEY *key, size_t len, const uint8_t *in, uint8_t *out); this build
 * uses the byte-sized key (x, y, data[256]). */
static void setup_rc4(uint32_t *r) {
  r[0] = buf(0, 1);
  for (uint32_t i = 0; i < 258; i++) ((uint8_t *)g2h(r[0]))[i] = (uint8_t)rnd();
  r[1] = rnd_below(4) == 0 ? rnd_below(8) : rnd_below(400);
  r[2] = buf(1, rnd_below(2) ? 4 : 1);
  r[3] = rnd_below(4) == 0 ? r[2] : buf(2, rnd_below(2) ? 4 : 1); /* in place too */
  for (uint32_t i = 0; i < r[1]; i++) ((uint8_t *)g2h(r[2]))[i] = (uint8_t)rnd();
}

static const struct {
  const char *name;
  setup_fn setup;
} cases[] = {
    {"CRYPTO_memcmp", setup_memcmp},
    {"BN_num_bits_word", setup_num_bits_word},
    {"BN_num_bits", setup_num_bits}, /* calls BN_num_bits_word (bl) */
    {"bn_mul_add_words", setup_mul_words},
    {"bn_mul_words", setup_mul_words},
    {"bn_sqr_words", setup_sqr_words},
    {"bn_add_words", setup_add_words},
    {"bn_sub_words", setup_add_words},
    {"bn_cmp_words", setup_cmp_words},
    {"OPENSSL_cleanse", setup_cleanse},
    {"md5_block_data_order", setup_block},
    {"sha1_block_data_order", setup_block},
    {"sha256_block_data_order", setup_block},
    {"RC4", setup_rc4},
};

/* ---- Image ------------------------------------------------------------------ */

static uint32_t image_size;

static int map_image(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return -1;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *file = malloc((size_t)n);
  if (!file || fread(file, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    return -1;
  }
  fclose(f);
  const Elf32_Ehdr *eh = (const Elf32_Ehdr *)file;
  const Elf32_Phdr *ph = (const Elf32_Phdr *)(file + eh->e_phoff);
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > image_size)
      image_size = ph[i].p_vaddr + ph[i].p_memsz;
  lib_base = gm_alloc(image_size, 0x10000, "library (unrelocated)");
  if (!lib_base) return -1;
  /* Segments as in the file; relocations aren't applied (the functions under
   * test read only position-independent data). */
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD) memcpy(g2h(lib_base + ph[i].p_vaddr), file + ph[i].p_offset, ph[i].p_filesz);
  free(file);
  return 0;
}

/* ---- Runs ------------------------------------------------------------------- */

static uint32_t find(const char *name) {
  for (uint32_t i = 0; i < lib_module.count; i++)
    if (lib_module.funcs[i].name && strcmp(lib_module.funcs[i].name, name) == 0) return lib_module.funcs[i].addr;
  return UINT32_MAX;
}

#define CPSR_COMPARE 0xF80F0020u /* N Z C V Q, GE[3:0], T */
#define CASE_TIMEOUT_S 60

static void timed_out(int sig) {
  (void)sig;
  static const char msg[] = "difftest FAIL: a function ran for over a minute (endless loop?)\n";
  write(2, msg, sizeof msg - 1);
  _exit(1);
}

static int run_case(cpu *c, const char *name, setup_fn setup, int iters, uint8_t *before, uint8_t *after_dyn,
                    const uint8_t *image0) {
  uint32_t off = find(name);
  if (off == UINT32_MAX) {
    fprintf(stderr, "difftest FAIL: %s was not recompiled\n", name);
    return 1;
  }
  uint32_t entry = lib_base + off;
  for (int it = 0; it < iters; it++) {
    rc_state init;
    memset(&init, 0, sizeof init);
    for (int i = 0; i < 13; i++) init.r[i] = rnd();
    for (uint32_t i = 0; i < ARENA_SIZE; i += 4) gm_w32(arena + i, rnd());
    setup(init.r);
    init.r[13] = arena + STACK_TOP;
    init.r[14] = c->ret_sentinel;
    init.cpsr = 0x10u | (rnd() & 0xF0000000u);
    memcpy(before, arena_host, ARENA_SIZE);

    /* dynarmic */
    for (int i = 0; i < 15; i++) c->ops->set_reg(c, i, init.r[i]);
    c->ops->set_reg(c, 15, entry & ~1u);
    c->ops->set_cpsr(c, (init.cpsr & ~CPU_CPSR_T) | ((entry & 1) ? CPU_CPSR_T : 0));
    c->faulted = 0;
    int drc = c->ops->run(c);
    uint32_t dyn[16], dyn_cpsr = c->ops->get_cpsr(c);
    for (int i = 0; i < 16; i++) dyn[i] = c->ops->get_reg(c, i);
    if (drc != 0 || c->faulted || (dyn[15] != c->ret_sentinel && dyn[15] != c->ret_sentinel + 4)) {
      fprintf(stderr, "difftest FAIL: %s input %d: dynarmic faulted (pc 0x%08x)\n", name, it, dyn[15]);
      return 1;
    }
    memcpy(after_dyn, arena_host, ARENA_SIZE);
    memcpy(arena_host, before, ARENA_SIZE);

    /* recompiled */
    rc_state s = init;
    rc_set_pc(&s, entry);
    if (rc_run(&s, c->ret_sentinel) != 0) {
      fprintf(stderr, "difftest FAIL: %s input %d: no recompiled code at 0x%08x\n", name, it, s.r[15]);
      return 1;
    }

    int bad = 0;
    for (int i = 0; i < 15; i++)
      if (s.r[i] != dyn[i]) {
        fprintf(stderr, "difftest FAIL: %s input %d: r%d = 0x%08x, dynarmic 0x%08x\n", name, it, i, s.r[i], dyn[i]);
        bad = 1;
      }
    if ((s.cpsr & CPSR_COMPARE) != (dyn_cpsr & CPSR_COMPARE)) {
      fprintf(stderr, "difftest FAIL: %s input %d: cpsr 0x%08x, dynarmic 0x%08x\n", name, it,
              s.cpsr & CPSR_COMPARE, dyn_cpsr & CPSR_COMPARE);
      bad = 1;
    }
    if (memcmp(arena_host, after_dyn, ARENA_SIZE) != 0) {
      uint32_t i = 0;
      while (arena_host[i] == after_dyn[i]) i++;
      fprintf(stderr, "difftest FAIL: %s input %d: memory differs at arena+0x%x (0x%02x, dynarmic 0x%02x)\n", name,
              it, i, arena_host[i], after_dyn[i]);
      bad = 1;
    }
    if (bad) {
      fprintf(stderr, "  inputs: r0-r3 = 0x%08x 0x%08x 0x%08x 0x%08x, cpsr 0x%08x\n", init.r[0], init.r[1],
              init.r[2], init.r[3], init.cpsr);
      return 1;
    }
  }
  if (memcmp(g2h(lib_base), image0, image_size) != 0) {
    fprintf(stderr, "difftest FAIL: %s wrote into the library image\n", name);
    return 1;
  }
  printf("difftest: %-24s %d inputs match\n", name, iters);
  return 0;
}

int main(int argc, char **argv) {
  const char *elf = NULL, *only = NULL;
  int iters = 500;
  uint64_t seed = 0x5eed10;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--elf") && i + 1 < argc) elf = argv[++i];
    else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
    else {
      fprintf(stderr, "usage: rc-difftest --elf LIB.so [--iters N] [--seed S] [--only NAME]\n");
      return 2;
    }
  }
  if (!elf || gm_init() != 0 || map_image(elf) != 0) return 1;
  rc_register_module(&lib_module);
  signal(SIGALRM, timed_out);

  cpu *c = cpu_dynarmic_create();
  if (cpu_setup(c, 0x10000) != 0) return 1;
  arena = gm_alloc(ARENA_SIZE, 0, "difftest arena");
  arena_host = g2h(arena);
  uint8_t *before = malloc(ARENA_SIZE), *after_dyn = malloc(ARENA_SIZE), *image0 = malloc(image_size);
  memcpy(image0, g2h(lib_base), image_size);

  int failed = 0, ran = 0;
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    if (only && strcmp(only, cases[i].name) != 0) continue;
    rng_state = seed ^ (0x9E3779B97F4A7C15ull * (i + 1));
    fprintf(stderr, "difftest: running %s\n", cases[i].name);
    alarm(CASE_TIMEOUT_S);
    failed += run_case(c, cases[i].name, cases[i].setup, iters, before, after_dyn, image0);
    alarm(0);
    ran++;
  }
  c->ops->destroy(c);
  if (!ran) {
    fprintf(stderr, "difftest: no case named %s\n", only);
    return 1;
  }
  if (failed) {
    fprintf(stderr, "difftest: %d of %d functions differ\n", failed, ran);
    return 1;
  }
  printf("DIFFTEST OK (%d functions, %d inputs each)\n", ran, iters);
  return 0;
}
