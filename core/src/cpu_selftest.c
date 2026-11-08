/*
 * CPU-backend round-trip test: runs guest code under the backend and checks
 * it against the host.
 *
 * - If the main library has OpenSSL linked in (many games do): SHA256 on
 *   several inputs vs the host's CommonCrypto, SHA1 vs the FIPS 180 test
 *   vectors, and BUF_strdup, which calls the strlen, malloc and memcpy
 *   imports through dynlib thunks into host shims, then CRYPTO_free -> free.
 * - Small hand-assembled ARM and Thumb functions for cpu_call's argument
 *   passing (stack arguments) and Thumb entry.
 * - The printf family over guest varargs and va_lists, strtok_r, strstr,
 *   strtod and gmtime_r/strftime through their thunks.
 * - Runtime glue: setjmp/longjmp through the import thunks,
 *   pthread_once running its guest init routine as a nested callback, a
 *   recursive mutex, and the CP15 barrier and virtual counter.
 * - stdio and scanf: a root-check fopen outside the data
 *   directory fails with ENOENT; a file in it is written, read back through
 *   fgets, fscanf and getc (with the guest EOF flag), and sscanf parses the
 *   build-date strings and the length modifiers.
 * - The game's own isspace/isdigit/isxdigit/isalpha, which read bionic's
 *   _ctype_ table through the data import.
 * - Guest threads: a joinable thread returns its value through
 *   pthread_join, has its own errno and key values, and runs its key
 *   destructor at exit; four threads (one detached) bump a counter under a
 *   guest mutex.
 */
#include "cpu_selftest.h"

#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio.h"
#include "dynlib.h"
#include "guest_mem.h"
#include "reimpl/reimpl.h"

static int failures;

#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      fprintf(stderr, "cpu-test FAIL: " __VA_ARGS__);                                              \
      fputc('\n', stderr);                                                                         \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

/* The game's own copy of a common library function (OpenSSL, bionic's
 * ctype), if the main library exports it: the tests that use one are
 * skipped, with a note, for a game that doesn't. */
static uint32_t export_opt(const so_module *m, const char *name) {
  uint32_t a = so_find_export(m, name);
  if (!a) printf("cpu-test: %s doesn't export %s: skipping its check\n", m->soname, name);
  return a;
}

static uint32_t import_calls(const char *name) {
  const dynlib_entry *e = dynlib_find(name);
  return e ? e->calls : 0;
}

static void test_sha256(cpu *c, const so_module *main_lib, uint32_t buf, uint32_t md) {
  uint32_t fn = export_opt(main_lib, "SHA256");
  if (!fn) return;
  static const uint32_t lengths[] = {0, 3, 55, 56, 64, 1000, 65537};
  for (size_t t = 0; t < sizeof(lengths) / sizeof(lengths[0]); t++) {
    uint32_t len = lengths[t];
    uint8_t *data = g2h(buf);
    for (uint32_t i = 0; i < len; i++) data[i] = (uint8_t)(i * 131u + t * 7u + (i >> 8));

    uint8_t want[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data, len, want);
    uint32_t args[] = {buf, len, md};
    uint32_t r = cpu_call(c, fn, 3, args);
    CHECK(!c->faulted && r == md && memcmp(g2h(md), want, sizeof want) == 0,
          "SHA256 of %u bytes differs from the host", len);
  }
}

static void test_sha1(cpu *c, const so_module *main_lib, uint32_t buf, uint32_t md) {
  static const struct {
    const char *msg;
    uint8_t digest[20];
  } vectors[] = {
      {"abc",
       {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
        0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d}},
      {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       {0x84, 0x98, 0x3e, 0x44, 0x1c, 0x3b, 0xd2, 0x6e, 0xba, 0xae,
        0x4a, 0xa1, 0xf9, 0x51, 0x29, 0xe5, 0xe5, 0x46, 0x70, 0xf1}},
  };
  uint32_t fn = export_opt(main_lib, "SHA1");
  if (!fn) return;
  for (size_t t = 0; t < sizeof(vectors) / sizeof(vectors[0]); t++) {
    uint32_t len = (uint32_t)strlen(vectors[t].msg);
    memcpy(g2h(buf), vectors[t].msg, len);
    uint32_t args[] = {buf, len, md};
    cpu_call(c, fn, 3, args);
    CHECK(!c->faulted && memcmp(g2h(md), vectors[t].digest, 20) == 0, "SHA1(\"%s\") is wrong",
          vectors[t].msg);
  }
}

static void test_import_thunks(cpu *c, const so_module *main_lib, uint32_t buf) {
  uint32_t strdup_fn = export_opt(main_lib, "BUF_strdup");
  uint32_t free_fn = export_opt(main_lib, "CRYPTO_free");
  if (!strdup_fn || !free_fn) return;

  const char *s = "The quick brown droid.";
  strcpy(g2h(buf), s);
  uint32_t strlen0 = import_calls("strlen"), malloc0 = import_calls("malloc"),
           memcpy0 = import_calls("memcpy"), free0 = import_calls("free");

  uint32_t args[] = {buf};
  uint32_t dup = cpu_call(c, strdup_fn, 1, args);
  CHECK(!c->faulted && dup && dup != buf && strcmp(g2h(dup), s) == 0,
        "BUF_strdup returned 0x%08x", dup);
  CHECK(import_calls("strlen") > strlen0, "BUF_strdup did not reach the strlen shim");
  CHECK(import_calls("malloc") > malloc0, "BUF_strdup did not reach the malloc shim");
  CHECK(import_calls("memcpy") > memcpy0, "BUF_strdup did not reach the memcpy shim");

  args[0] = dup;
  cpu_call(c, free_fn, 1, args);
  CHECK(!c->faulted && import_calls("free") == free0 + 1, "CRYPTO_free did not reach free");
}

static void test_calling_convention(cpu *c, uint32_t code) {
  /* ARM: return a0 + a1 + ... + a5 (two arguments on the stack). */
  static const uint32_t sum6[] = {
      0xe0800001, /* add r0, r0, r1 */
      0xe0800002, /* add r0, r0, r2 */
      0xe0800003, /* add r0, r0, r3 */
      0xe59dc000, /* ldr ip, [sp] */
      0xe080000c, /* add r0, r0, ip */
      0xe59dc004, /* ldr ip, [sp, #4] */
      0xe080000c, /* add r0, r0, ip */
      0xe12fff1e, /* bx lr */
  };
  /* Thumb: return a0 - a1 as a 64-bit value with r1 = a1. */
  static const uint16_t sub_thumb[] = {
      0x1a40, /* subs r0, r0, r1 */
      0x4770, /* bx lr */
  };
  memcpy(g2h(code), sum6, sizeof sum6);
  memcpy(g2h(code + 0x100), sub_thumb, sizeof sub_thumb);
  c->ops->invalidate(c, code, 0x200);

  uint32_t args[] = {1, 20, 300, 4000, 50000, 600000};
  uint32_t sp = c->ops->get_reg(c, CPU_REG_SP);
  uint32_t r = cpu_call(c, code, 6, args);
  CHECK(!c->faulted && r == 654321, "6-argument ARM call returned %u", r);
  CHECK(c->ops->get_reg(c, CPU_REG_SP) == sp, "cpu_call did not restore sp");

  uint32_t targs[] = {0x10000000u, 0x00000001u};
  uint64_t r64 = cpu_call64(c, (code + 0x100) | 1, 2, targs);
  CHECK(!c->faulted && r64 == 0x000000010fffffffull, "Thumb call returned 0x%llx",
        (unsigned long long)r64);
}

static uint32_t thunk(const char *name) {
  const dynlib_entry *e = dynlib_find(name);
  CHECK(e && e->addr, "no thunk for %s", name);
  return e ? e->addr : 0;
}

static void test_runtime_glue(cpu *c, uint32_t code, uint32_t buf) {
  /* f(buf, setjmp, longjmp): returns setjmp's second return value. */
  static const uint32_t jump[] = {
      0xe92d4070, /* push {r4, r5, r6, lr} */
      0xe1a04000, /* mov r4, r0 */
      0xe1a06002, /* mov r6, r2 */
      0xe12fff31, /* blx r1          ; setjmp(buf) */
      0xe3500000, /* cmp r0, #0 */
      0x1a000002, /* bne done */
      0xe1a00004, /* mov r0, r4 */
      0xe3a01007, /* mov r1, #7 */
      0xe12fff36, /* blx r6          ; longjmp(buf, 7) */
      0xe8bd8070, /* done: pop {r4, r5, r6, pc} */
  };
  /* At +0x40: void init(void) { ++*counter; }, counter address at +0x54. */
  static const uint32_t once_init[] = {
      0xe59f000c, /* ldr r0, [pc, #12] */
      0xe5901000, /* ldr r1, [r0] */
      0xe2811001, /* add r1, r1, #1 */
      0xe5801000, /* str r1, [r0] */
      0xe12fff1e, /* bx lr */
  };
  /* At +0x80: CP15 DMB, then return CNTVCT. */
  static const uint32_t counter[] = {
      0xee070fba, /* mcr p15, 0, r0, c7, c10, 5 */
      0xec510f1e, /* mrrc p15, 1, r0, r1, c14 */
      0xe12fff1e, /* bx lr */
  };
  uint32_t count_addr = buf, once_word = buf + 4, mutex = buf + 8, jb = buf + 0x100;
  memcpy(g2h(code), jump, sizeof jump);
  memcpy(g2h(code + 0x40), once_init, sizeof once_init);
  gm_w32(code + 0x54, count_addr);
  memcpy(g2h(code + 0x80), counter, sizeof counter);
  c->ops->invalidate(c, code, 0x200);

  uint32_t jargs[] = {jb, thunk("setjmp"), thunk("longjmp")};
  uint32_t sp = c->ops->get_reg(c, CPU_REG_SP);
  uint32_t r = cpu_call(c, code, 3, jargs);
  CHECK(!c->faulted && r == 7, "setjmp/longjmp round trip returned %u", r);
  CHECK(c->ops->get_reg(c, CPU_REG_SP) == sp, "longjmp left sp unbalanced");

  gm_w32(count_addr, 0);
  gm_w32(once_word, 0);
  uint32_t oargs[] = {once_word, code + 0x40};
  for (int i = 0; i < 2; i++) {
    r = cpu_call(c, thunk("pthread_once"), 2, oargs);
    CHECK(!c->faulted && r == 0, "pthread_once returned %u", r);
  }
  CHECK(gm_r32(count_addr) == 1 && gm_r32(once_word) == 2,
        "pthread_once ran its routine %u times (once word %u)", gm_r32(count_addr), gm_r32(once_word));

  gm_w32(mutex, 0x4000); /* PTHREAD_RECURSIVE_MUTEX_INITIALIZER */
  uint32_t margs[] = {mutex};
  uint32_t lock = thunk("pthread_mutex_lock"), unlock = thunk("pthread_mutex_unlock");
  r = cpu_call(c, lock, 1, margs) | cpu_call(c, lock, 1, margs);
  r |= cpu_call(c, unlock, 1, margs) | cpu_call(c, unlock, 1, margs);
  CHECK(!c->faulted && r == 0, "recursive mutex lock/unlock failed");

  uint64_t t0 = cpu_call64(c, code + 0x80, 0, NULL);
  uint64_t t1 = cpu_call64(c, code + 0x80, 0, NULL);
  CHECK(!c->faulted && t0 != 0 && t1 >= t0, "CNTVCT read 0x%llx then 0x%llx",
        (unsigned long long)t0, (unsigned long long)t1);
}

/* Copies a host string into guest memory at *at and returns its address. */
static uint32_t gstr(uint32_t *at, const char *s) {
  uint32_t a = *at;
  strcpy(g2h(a), s);
  *at += ((uint32_t)strlen(s) + 8) & ~7u;
  return a;
}

static uint64_t dbits(double d) {
  uint64_t b;
  memcpy(&b, &d, sizeof b);
  return b;
}

/* The printf, string and time shims, called through their thunks with the
 * guest's variadic layout. */
static void test_libc_shims(cpu *c, uint32_t buf) {
  uint32_t out = buf, at = buf + 0x400;
  const char *got = g2h(out);
  uint32_t snp = thunk("snprintf");

  /* r3 and the stack; the long long lands at an even slot. */
  uint64_t big = 1ull << 40, pi = dbits(3.14159);
  uint32_t a1[] = {out, 128, gstr(&at, "%d|%5s|%-3x|%lld|%.2f|%c|%%|%p|%s"), (uint32_t)-42,
                   gstr(&at, "ab"), 0xa, (uint32_t)big, (uint32_t)(big >> 32), (uint32_t)pi,
                   (uint32_t)(pi >> 32), 'Z', 0x1234, 0};
  uint32_t r = cpu_call(c, snp, 13, a1);
  const char *want = "-42|   ab|a  |1099511627776|3.14|Z|%|0x1234|(null)";
  CHECK(!c->faulted && !strcmp(got, want) && r == strlen(want), "snprintf gave \"%s\" (%u)", got, r);

  /* A double after the format skips r3 and starts on the stack. */
  uint64_t half = dbits(2.5);
  uint32_t a2[] = {out, 128, gstr(&at, "%f %*d"), 0xdeadbeef, (uint32_t)half, (uint32_t)(half >> 32),
                   4, 9};
  r = cpu_call(c, snp, 8, a2);
  CHECK(!c->faulted && !strcmp(got, "2.500000    9"), "snprintf alignment gave \"%s\"", got);

  /* Truncation: returns the full length. */
  uint32_t a3[] = {out, 8, gstr(&at, "%u"), 123456789};
  r = cpu_call(c, snp, 4, a3);
  CHECK(r == 9 && !strcmp(got, "1234567"), "snprintf truncation gave \"%s\" (%u)", got, r);

  /* vsnprintf over a guest va_list: an int, a double at the next 8-byte slot,
   * then an int printed as a signed char. */
  uint32_t ap = (at + 7) & ~7u;
  gm_w32(ap, 7);
  gm_w32(ap + 8, (uint32_t)dbits(0.5));
  gm_w32(ap + 12, (uint32_t)(dbits(0.5) >> 32));
  gm_w32(ap + 16, 0x1ff);
  at = ap + 24;
  uint32_t a4[] = {out, 128, gstr(&at, "%d %g %hhd"), ap};
  cpu_call(c, thunk("vsnprintf"), 4, a4);
  CHECK(!c->faulted && !strcmp(got, "7 0.5 -1"), "vsnprintf gave \"%s\"", got);

  /* strtok_r writes NULs into the guest string and keeps state in a guest word. */
  uint32_t text = gstr(&at, "  one,two;;three"), delim = gstr(&at, ",; "), save = at;
  at += 8;
  uint32_t t1[] = {text, delim, save}, t2[] = {0, delim, save};
  uint32_t w1 = cpu_call(c, thunk("strtok_r"), 3, t1);
  uint32_t w2 = cpu_call(c, thunk("strtok_r"), 3, t2);
  uint32_t w3 = cpu_call(c, thunk("strtok_r"), 3, t2);
  uint32_t w4 = cpu_call(c, thunk("strtok_r"), 3, t2);
  CHECK(w1 == text + 2 && w2 == text + 6 && w3 == text + 11 && w4 == 0 &&
            !strcmp(g2h(w3), "three"),
        "strtok_r returned 0x%x 0x%x 0x%x 0x%x", w1, w2, w3, w4);

  uint32_t hay = gstr(&at, "droid android"), s1[] = {hay, gstr(&at, "and")};
  CHECK(cpu_call(c, thunk("strstr"), 2, s1) == hay + 6, "strstr returned the wrong guest address");

  uint32_t num = gstr(&at, "-1.25e2x"), end = at, d1[] = {num, end};
  at += 8;
  uint64_t d = cpu_call64(c, thunk("strtod"), 2, d1);
  CHECK(d == dbits(-125.0) && gm_r32(end) == num + 7, "strtod gave 0x%llx", (unsigned long long)d);

  /* gmtime_r(0) and strftime on the guest struct tm. */
  uint32_t tval = at, tm = at + 8;
  at += 64;
  gm_w32(tval, 86400 + 3661);
  uint32_t g1[] = {tval, tm};
  cpu_call(c, thunk("gmtime_r"), 2, g1);
  uint32_t f1[] = {out, 64, gstr(&at, "%Y-%m-%d %H:%M:%S"), tm};
  r = cpu_call(c, thunk("strftime"), 4, f1);
  CHECK(!c->faulted && r == 19 && !strcmp(got, "1970-01-02 01:01:01"), "strftime gave \"%s\"", got);
}

/* stdio and the scanf family through their thunks. */
static void test_stdio(cpu *c, uint32_t buf) {
  uint32_t out = buf, at = buf + 0x400, w = buf + 0x200; /* w: result words */
  const char *got = g2h(out);

  uint32_t o1[] = {gstr(&at, "/system/app/Superuser.apk"), gstr(&at, "r")};
  uint32_t f = cpu_call(c, thunk("fopen"), 2, o1);
  CHECK(f == 0 && gm_r32(guest_errno_addr()) == 2, "root-check fopen gave 0x%x, errno %u", f,
        gm_r32(guest_errno_addr()));
  /* A host file that exists is still out of reach. */
  uint32_t o0[] = {gstr(&at, "/etc/hosts"), gstr(&at, "r")};
  f = cpu_call(c, thunk("fopen"), 2, o0);
  CHECK(f == 0, "fopen(\"/etc/hosts\") escaped the data directory");

  char path[1100];
  snprintf(path, sizeof path, "%s/files/stdio-selftest.txt", stdio_data_root());
  uint32_t gpath = gstr(&at, path);
  uint32_t o2[] = {gpath, gstr(&at, "w")};
  f = cpu_call(c, thunk("fopen"), 2, o2);
  CHECK(f != 0, "fopen(\"%s\", \"w\") failed", path);
  if (!f) return;
  uint32_t p1[] = {f, gstr(&at, "%d %s\n"), 42, gstr(&at, "android")};
  cpu_call(c, thunk("fprintf"), 4, p1);
  uint32_t p2[] = {gstr(&at, "7 2.5 rainbow\n"), f};
  cpu_call(c, thunk("fputs"), 2, p2);
  uint32_t p3[] = {gstr(&at, "xy"), 1, 2, f};
  uint32_t r = cpu_call(c, thunk("fwrite"), 4, p3);
  CHECK(r == 2, "fwrite returned %u", r);
  uint32_t cl[] = {f};
  CHECK(cpu_call(c, thunk("fclose"), 1, cl) == 0, "fclose failed");

  uint32_t o3[] = {gpath, gstr(&at, "r")};
  f = cpu_call(c, thunk("fopen"), 2, o3);
  CHECK(f != 0, "reopening \"%s\" failed", path);
  if (!f) return;
  uint32_t g1[] = {out, 64, f};
  r = cpu_call(c, thunk("fgets"), 3, g1);
  CHECK(r == out && !strcmp(got, "42 android\n"), "fgets gave \"%s\"", got);
  uint32_t s1[] = {f, gstr(&at, "%d %lf %7s"), w, w + 8, out};
  r = cpu_call(c, thunk("fscanf"), 5, s1);
  double d;
  memcpy(&d, g2h(w + 8), 8);
  CHECK(r == 3 && gm_r32(w) == 7 && d == 2.5 && !strcmp(got, "rainbow"),
        "fscanf gave %u: %u %g \"%s\"", r, gm_r32(w), d, got);
  uint32_t gc[] = {f};
  uint32_t c1 = cpu_call(c, thunk("getc"), 1, gc), c2 = cpu_call(c, thunk("getc"), 1, gc);
  uint32_t c3 = cpu_call(c, thunk("__srget"), 1, gc), c4 = cpu_call(c, thunk("getc"), 1, gc);
  uint16_t flags;
  memcpy(&flags, g2h(f + 12), 2);
  CHECK(c1 == '\n' && c2 == 'x' && c3 == 'y' && c4 == 0xffffffffu && (flags & 0x20),
        "getc gave %d %d %d %d, flags 0x%x", c1, c2, c3, c4, flags);
  cpu_call(c, thunk("fclose"), 1, gc);

  /* nativeInitGraphics parses the build date and time like this. */
  uint32_t ss = thunk("sscanf");
  uint32_t a1[] = {gstr(&at, "Nov 20 2018"), gstr(&at, "%s %d %d"), out, w, w + 4};
  r = cpu_call(c, ss, 5, a1);
  CHECK(r == 3 && !strcmp(got, "Nov") && gm_r32(w) == 20 && gm_r32(w + 4) == 2018,
        "sscanf date gave %u: \"%s\" %u %u", r, got, gm_r32(w), gm_r32(w + 4));
  uint32_t a2[] = {gstr(&at, "11:16:25"), gstr(&at, "%d:%d:%d"), w, w + 4, w + 8};
  r = cpu_call(c, ss, 5, a2);
  CHECK(r == 3 && gm_r32(w) == 11 && gm_r32(w + 4) == 16 && gm_r32(w + 8) == 25,
        "sscanf time gave %u: %u %u %u", r, gm_r32(w), gm_r32(w + 4), gm_r32(w + 8));

  /* Widths of the stores: hh, h, ll, float; a skipped field, a scanset, %n. */
  memset(g2h(w), 0xee, 32);
  uint32_t a3[] = {gstr(&at, "300 -2 x 1099511627776 0.25 abc;def"),
                   gstr(&at, "%hhu %hd %*s %lld %f %[a-z];%n"), w, w + 2, w + 8, w + 16, out,
                   w + 20};
  r = cpu_call(c, ss, 8, a3);
  float fl;
  memcpy(&fl, g2h(w + 16), 4);
  uint64_t ll = gm_r32(w + 8) | (uint64_t)gm_r32(w + 12) << 32;
  CHECK(r == 5 && *(uint8_t *)g2h(w) == 44 && *(uint8_t *)g2h(w + 1) == 0xee &&
            *(int16_t *)g2h(w + 2) == -2 && ll == 1ull << 40 && fl == 0.25f &&
            !strcmp(got, "abc") && gm_r32(w + 20) == 32,
        "sscanf modifiers gave %u: %u %d %llu %g \"%s\" n=%u", r, *(uint8_t *)g2h(w),
        *(int16_t *)g2h(w + 2), (unsigned long long)ll, fl, got, gm_r32(w + 20));

  /* A matching failure stops early; empty input is EOF. */
  uint32_t a4[] = {gstr(&at, "5 apples"), gstr(&at, "%d %d"), w, w + 4};
  r = cpu_call(c, ss, 4, a4);
  CHECK(r == 1 && gm_r32(w) == 5, "sscanf partial match returned %u", r);
  uint32_t a5[] = {gstr(&at, "   "), gstr(&at, "%d"), w};
  r = cpu_call(c, ss, 3, a5);
  CHECK(r == 0xffffffffu, "sscanf on empty input returned %d", (int)r);
}

static void test_threads(cpu *c, uint32_t code, uint32_t buf) {
  /* start(a): a[1] = __errno() via a[0]; pthread_setspecific(a[2], a) via a[3];
   * return a + 1. */
  static const uint32_t start[] = {
      0xe92d4010, /* push {r4, lr} */
      0xe1a04000, /* mov r4, r0 */
      0xe5941000, /* ldr r1, [r4] */
      0xe12fff31, /* blx r1 */
      0xe5840004, /* str r0, [r4, #4] */
      0xe5940008, /* ldr r0, [r4, #8] */
      0xe1a01004, /* mov r1, r4 */
      0xe594200c, /* ldr r2, [r4, #12] */
      0xe12fff32, /* blx r2 */
      0xe2840001, /* add r0, r4, #1 */
      0xe8bd8010, /* pop {r4, pc} */
  };
  /* dtor(a): ++a[4]. */
  static const uint32_t dtor[] = {
      0xe5901010, /* ldr r1, [r0, #16] */
      0xe2811001, /* add r1, r1, #1 */
      0xe5801010, /* str r1, [r0, #16] */
      0xe12fff1e, /* bx lr */
  };
  /* worker(w): w[4] times { lock(w[2]) via w[0]; ++*w[3]; unlock via w[1] }. */
  static const uint32_t worker[] = {
      0xe92d4070, /* push {r4, r5, r6, lr} */
      0xe1a04000, /* mov r4, r0 */
      0xe5945010, /* ldr r5, [r4, #16] */
      0xe5940008, /* loop: ldr r0, [r4, #8] */
      0xe5941000, /* ldr r1, [r4] */
      0xe12fff31, /* blx r1 */
      0xe594100c, /* ldr r1, [r4, #12] */
      0xe5912000, /* ldr r2, [r1] */
      0xe2822001, /* add r2, r2, #1 */
      0xe5812000, /* str r2, [r1] */
      0xe5940008, /* ldr r0, [r4, #8] */
      0xe5941004, /* ldr r1, [r4, #4] */
      0xe12fff31, /* blx r1 */
      0xe2555001, /* subs r5, r5, #1 */
      0x1afffff3, /* bne loop */
      0xe3a00000, /* mov r0, #0 */
      0xe8bd8070, /* pop {r4, r5, r6, pc} */
  };
  uint32_t f_start = code + 0x200, f_dtor = code + 0x240, f_worker = code + 0x260;
  memcpy(g2h(f_start), start, sizeof start);
  memcpy(g2h(f_dtor), dtor, sizeof dtor);
  memcpy(g2h(f_worker), worker, sizeof worker);
  c->ops->invalidate(c, code + 0x200, 0x100);

  uint32_t a = buf + 0x400, key = buf + 0x480, attr = buf + 0x4a0, tid = buf + 0x4c0,
           ret = buf + 0x4e0;
  uint32_t kargs[] = {key, f_dtor};
  uint32_t r = cpu_call(c, thunk("pthread_key_create"), 2, kargs);
  CHECK(!c->faulted && r == 0, "pthread_key_create returned %u", r);
  uint32_t main_errno = cpu_call(c, thunk("__errno"), 0, NULL);
  memset(g2h(a), 0, 20);
  gm_w32(a, thunk("__errno"));
  gm_w32(a + 8, gm_r32(key));
  gm_w32(a + 12, thunk("pthread_setspecific"));

  uint32_t one[] = {attr};
  cpu_call(c, thunk("pthread_attr_init"), 1, one);
  /* 32-bit bionic accepts stacks down to 8 KiB; FMOD's stream thread asks
   * for exactly that, and fails to start its streams otherwise. */
  uint32_t sargs[] = {attr, 0x1fff};
  r = cpu_call(c, thunk("pthread_attr_setstacksize"), 2, sargs);
  CHECK(r == 22, "pthread_attr_setstacksize(8191) returned %u, not EINVAL", r);
  sargs[1] = 0x2000;
  r = cpu_call(c, thunk("pthread_attr_setstacksize"), 2, sargs);
  CHECK(r == 0, "pthread_attr_setstacksize(8192) returned %u, not 0", r);
  sargs[1] = 0x10000;
  r = cpu_call(c, thunk("pthread_attr_setstacksize"), 2, sargs);
  uint32_t cargs[] = {tid, attr, f_start, a};
  r |= cpu_call(c, thunk("pthread_create"), 4, cargs);
  CHECK(!c->faulted && r == 0 && gm_r32(tid), "pthread_create failed (%u)", r);
  uint32_t jargs[] = {gm_r32(tid), ret};
  r = cpu_call(c, thunk("pthread_join"), 2, jargs);
  CHECK(!c->faulted && r == 0 && gm_r32(ret) == a + 1, "pthread_join returned %u, value 0x%x", r,
        gm_r32(ret));
  CHECK(gm_r32(a + 4) && gm_r32(a + 4) != main_errno,
        "guest thread shares errno 0x%x with the main thread", gm_r32(a + 4));
  CHECK(gm_r32(a + 16) == 1, "key destructor ran %u times", gm_r32(a + 16));
  uint32_t gargs[] = {gm_r32(key)};
  CHECK(cpu_call(c, thunk("pthread_getspecific"), 1, gargs) == 0,
        "a guest thread's key value leaked into the main thread");
  r = cpu_call(c, thunk("pthread_join"), 2, jargs);
  CHECK(r == 3, "joining a reaped thread returned %u, not ESRCH", r);

  uint32_t self = cpu_call(c, thunk("pthread_self"), 0, NULL);
  CHECK(self && self == cpu_call(c, thunk("pthread_self"), 0, NULL),
        "pthread_self in the main thread is 0x%x", self);

  /* Four workers, the last one detached, share a counter under one mutex. */
  /* Enough rounds that the threads overlap: a no-op lock loses updates. */
  const uint32_t rounds = 20000;
  uint32_t w = buf + 0x500, counter = buf + 0x520, mutex = buf + 0x524;
  gm_w32(w, thunk("pthread_mutex_lock"));
  gm_w32(w + 4, thunk("pthread_mutex_unlock"));
  gm_w32(w + 8, mutex);
  gm_w32(w + 12, counter);
  gm_w32(w + 16, rounds);
  gm_w32(counter, 0);
  gm_w32(mutex, 0);
  uint32_t tids[4];
  for (int i = 0; i < 4; i++) {
    if (i == 3) {
      uint32_t dargs[] = {attr, 1};
      cpu_call(c, thunk("pthread_attr_setdetachstate"), 2, dargs);
    }
    uint32_t wargs[] = {tid + 4u * (uint32_t)i, attr, f_worker, w};
    r = cpu_call(c, thunk("pthread_create"), 4, wargs);
    CHECK(!c->faulted && r == 0, "pthread_create of worker %d returned %u", i, r);
    tids[i] = gm_r32(tid + 4u * (uint32_t)i);
  }
  for (int i = 0; i < 3; i++) {
    uint32_t args[] = {tids[i], 0};
    r = cpu_call(c, thunk("pthread_join"), 2, args);
    CHECK(!c->faulted && r == 0, "pthread_join of worker %d returned %u", i, r);
  }
  for (int ms = 0; ms < 5000 && __atomic_load_n((uint32_t *)g2h(counter), __ATOMIC_ACQUIRE) != 4 * rounds;
       ms++)
    nanosleep(&(struct timespec){0, 1000000}, NULL);
  CHECK(gm_r32(counter) == 4 * rounds, "four workers counted to %u, not %u", gm_r32(counter),
        4 * rounds);
  CHECK(thread_fault_count() == 0, "%d guest threads faulted", thread_fault_count());
}

static void test_ctype(cpu *c, const so_module *main_lib) {
  static const struct {
    const char *fn;
    const char *yes, *no;
  } cases[] = {
      {"isspace", " \t\n\v\f\r", "&a0_\x7f"},
      {"isdigit", "0189", "a/:"},
      {"isxdigit", "09afAF", "gG/:"},
      {"isalpha", "azAZ", "09@[`{"},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    uint32_t fn = export_opt(main_lib, cases[i].fn);
    if (!fn) continue;
    for (const char *p = cases[i].yes; *p; p++) {
      uint32_t ch = (uint8_t)*p;
      CHECK(cpu_call(c, fn, 1, &ch) != 0 && !c->faulted, "%s(0x%02x) is false", cases[i].fn, ch);
    }
    for (const char *p = cases[i].no; *p; p++) {
      uint32_t ch = (uint8_t)*p;
      CHECK(cpu_call(c, fn, 1, &ch) == 0 && !c->faulted, "%s(0x%02x) is true", cases[i].fn, ch);
    }
  }
  const int16_t *lo = g2h(gm_r32(dynlib_find("_tolower_tab_")->addr));
  const int16_t *up = g2h(gm_r32(dynlib_find("_toupper_tab_")->addr));
  CHECK(lo[0] == -1 && lo[1 + 'Q'] == 'q' && lo[1 + 'q'] == 'q' && up[1 + 'q'] == 'Q' &&
            up[1 + '1'] == '1',
        "tolower/toupper tables are wrong");
}

static uint32_t call_n(cpu *c, const char *name, int n, const uint32_t *args) {
  return cpu_call(c, thunk(name), n, args);
}

/* fd-level files, directories, sockets, poll, process ids (fileio.c, net.c). */
static void test_fileio(cpu *c, uint32_t buf) {
  uint32_t at = buf + 0x400, st = buf, w = buf + 0x100, io = buf + 0x200;
  uint32_t err = guest_errno_addr();
  char path[1100], dir[1100], moved[1100];
  snprintf(path, sizeof path, "%s/files/fileio-selftest.bin", stdio_data_root());
  snprintf(dir, sizeof dir, "%s/files/fileio-dir", stdio_data_root());
  snprintf(moved, sizeof moved, "%s/x", dir);
  uint32_t gpath = gstr(&at, path), gdir = gstr(&at, dir), gmoved = gstr(&at, moved);

  /* Linux O_WRONLY|O_CREAT|O_TRUNC */
  uint32_t o1[] = {gpath, 01101, 0644};
  int fd = (int)call_n(c, "open", 3, o1);
  CHECK(fd > 2, "open for writing gave %d (errno %u)", fd, gm_r32(err));
  uint32_t wr[] = {(uint32_t)fd, gstr(&at, "hello world"), 11};
  CHECK(call_n(c, "write", 3, wr) == 11, "write failed");
  uint32_t ls[] = {(uint32_t)fd, 0, 1 /* SEEK_CUR */};
  CHECK(call_n(c, "lseek", 3, ls) == 11, "lseek(SEEK_CUR) is not at 11");
  uint32_t fl[] = {(uint32_t)fd, 3 /* F_GETFL */};
  CHECK(call_n(c, "fcntl", 2, fl) == 1, "F_GETFL is not Linux O_WRONLY");
  uint32_t fs[] = {(uint32_t)fd, st};
  memset(g2h(st), 0xee, 104);
  uint32_t r = call_n(c, "fstat", 2, fs);
  CHECK(r == 0 && gm_r32(st + 48) == 11 && gm_r32(st + 52) == 0 &&
            (gm_r32(st + 16) & 0170000) == 0100000 && gm_r32(st + 24) != 0,
        "fstat gave size %u, mode 0%o, uid %u", gm_r32(st + 48), gm_r32(st + 16), gm_r32(st + 24));
  uint32_t cl[] = {(uint32_t)fd};
  CHECK(call_n(c, "close", 1, cl) == 0, "close failed");

  uint32_t o2[] = {gpath, 0};
  fd = (int)call_n(c, "open", 2, o2);
  uint32_t rd[] = {(uint32_t)fd, io, 5};
  r = call_n(c, "read", 3, rd);
  CHECK(r == 5 && !memcmp(g2h(io), "hello", 5), "read gave %d", (int)r);
  uint32_t ls2[] = {(uint32_t)fd, 6, 0 /* SEEK_SET */};
  call_n(c, "lseek", 3, ls2);
  r = call_n(c, "read", 3, rd);
  CHECK(r == 5 && !memcmp(g2h(io), "world", 5), "read after lseek gave %d", (int)r);
  cl[0] = (uint32_t)fd;
  call_n(c, "close", 1, cl);

  uint32_t s1[] = {gpath, st};
  CHECK(call_n(c, "stat", 2, s1) == 0 && gm_r32(st + 48) == 11, "stat size %u", gm_r32(st + 48));
  uint32_t s2[] = {gstr(&at, "/etc/hosts"), st};
  CHECK(call_n(c, "stat", 2, s2) == 0xffffffffu && gm_r32(err) == 2, "stat escaped the data dir");
  uint32_t o3[] = {s2[0], 0};
  CHECK(call_n(c, "open", 2, o3) == 0xffffffffu && gm_r32(err) == 2, "open escaped the data dir");

  /* OpenSSL's seeding: /dev/urandom, polled, then read; never writable. */
  uint32_t o4[] = {gstr(&at, "/dev/urandom"), 0x900 /* O_NONBLOCK|O_NOCTTY */};
  int rfd = (int)call_n(c, "open", 2, o4);
  uint32_t sargs[] = {2 /* AF_INET */, 1, 0};
  int sock = (int)call_n(c, "socket", 3, sargs);
  gm_w32(w, (uint32_t)rfd);
  gm_w32(w + 4, 1 /* POLLIN */);
  gm_w32(w + 8, (uint32_t)sock);
  gm_w32(w + 12, 1);
  uint32_t pl[] = {w, 2, 10};
  r = call_n(c, "poll", 3, pl);
  CHECK(rfd > 2 && r == 2 && (gm_r32(w + 4) >> 16) == 1 && (gm_r32(w + 12) >> 16) == 0x10,
        "poll gave %u, revents 0x%x 0x%x", r, gm_r32(w + 4) >> 16, gm_r32(w + 12) >> 16);
  uint32_t rr[] = {(uint32_t)rfd, io, 16};
  CHECK(call_n(c, "read", 3, rr) == 16, "reading /dev/urandom failed");
  cl[0] = (uint32_t)rfd;
  call_n(c, "close", 1, cl);
  o4[1] = 1;
  CHECK(call_n(c, "open", 2, o4) == 0xffffffffu, "/dev/urandom opened for writing");

  /* Offline sockets: local names are refused, the network is unreachable. */
  uint32_t cn[] = {(uint32_t)sock, w, 16};
  r = call_n(c, "connect", 3, cn);
  CHECK(sock >= 1008 && r == 0xffffffffu && gm_r32(err) == 101,
        "network connect gave %d, errno %u", (int)r, gm_r32(err));
  sargs[0] = 1 /* AF_UNIX */;
  int usock = (int)call_n(c, "socket", 3, sargs);
  cn[0] = (uint32_t)usock;
  r = call_n(c, "connect", 3, cn);
  CHECK(r == 0xffffffffu && gm_r32(err) == 111, "local connect gave errno %u", gm_r32(err));
  cl[0] = (uint32_t)sock;
  r = call_n(c, "close", 1, cl);
  cl[0] = (uint32_t)usock;
  r |= call_n(c, "close", 1, cl);
  CHECK(r == 0 && call_n(c, "close", 1, cl) == 0xffffffffu && gm_r32(err) == 9,
        "closing fake sockets misbehaved");
  cl[0] = 1;
  CHECK(call_n(c, "close", 1, cl) == 0xffffffffu && gm_r32(err) == 9, "the guest closed host fd 1");
  gm_w32(w, 0xdeadbeef);
  uint32_t ga[] = {gstr(&at, "cdn.example.com"), 0, 0, w};
  CHECK(call_n(c, "getaddrinfo", 4, ga) == 8 && gm_r32(w) == 0, "getaddrinfo resolved offline");

  /* Directories: mkdir, rename into it, list it, clean up. */
  uint32_t md[] = {gdir, 0755};
  call_n(c, "rmdir", 1, md);
  CHECK(call_n(c, "mkdir", 2, md) == 0, "mkdir failed (errno %u)", gm_r32(err));
  uint32_t rn[] = {gpath, gmoved};
  CHECK(call_n(c, "rename", 2, rn) == 0, "rename failed");
  uint32_t od[] = {gdir};
  uint32_t d = call_n(c, "opendir", 1, od), e, found = 0;
  uint32_t rdd[] = {d};
  while (d && (e = call_n(c, "readdir", 1, rdd)) != 0)
    if (!strcmp(g2h(e + 19), "x") && *(uint8_t *)g2h(e + 18) == 8 /* DT_REG */) found = 1;
  CHECK(found, "readdir did not list the renamed file");
  call_n(c, "closedir", 1, rdd);
  uint32_t ul[] = {gmoved};
  CHECK(call_n(c, "unlink", 1, ul) == 0 && call_n(c, "rmdir", 1, md) == 0, "cleanup failed");
  uint32_t ac[] = {gdir, 0};
  CHECK(call_n(c, "access", 2, ac) == 0xffffffffu && gm_r32(err) == 2, "access after rmdir");

  uint32_t sc[] = {224 /* __NR_gettid */};
  CHECK(call_n(c, "syscall", 1, sc) == call_n(c, "getpid", 0, NULL), "gettid on the main thread");
  CHECK(call_n(c, "getuid", 0, NULL) != 0, "the app runs as root");
}

/* qsort with a guest comparator, the mem* searches, libm, the unwinder's
 * exidx lookup and the GL queries. */
static void test_misc_shims(cpu *c, const so_module *main_lib, uint32_t buf) {
  static const uint32_t cmp[] = {
      0xe5900000, /* ldr r0, [r0] */
      0xe5911000, /* ldr r1, [r1] */
      0xe0400001, /* sub r0, r0, r1 */
      0xe12fff1e, /* bx lr */
  };
  uint32_t code = gm_alloc(0x100, 0, "misc-test code"), arr = buf, at = buf + 0x400;
  memcpy(g2h(code), cmp, sizeof cmp);
  c->ops->invalidate(c, code, sizeof cmp);
  uint32_t seed = 12345;
  for (uint32_t i = 0; i < 37; i++) {
    seed = seed * 1103515245u + 12345u;
    gm_w32(arr + 4 * i, (seed >> 16) % 1000);
  }
  uint32_t qa[] = {arr, 37, 4, code};
  call_n(c, "qsort", 4, qa);
  int sorted = !c->faulted;
  for (uint32_t i = 1; i < 37; i++) sorted &= gm_r32(arr + 4 * i - 4) <= gm_r32(arr + 4 * i);
  CHECK(sorted, "qsort did not sort");

  uint32_t hay = gstr(&at, "pak/droid/android.pak");
  uint32_t m1[] = {hay, '/', 21}, m2[] = {hay, gstr(&at, "and"), 3};
  CHECK(call_n(c, "memchr", 3, m1) == hay + 3 && call_n(c, "memrchr", 3, m1) == hay + 9,
        "memchr/memrchr gave the wrong guest address");
  uint32_t mm[] = {hay, 21, m2[1], 3};
  CHECK(call_n(c, "memmem", 4, mm) == hay + 10, "memmem gave the wrong guest address");

  uint64_t two = dbits(2.0), ten = dbits(10.0), eight = dbits(8.0);
  uint32_t pw[] = {(uint32_t)two, (uint32_t)(two >> 32), (uint32_t)ten, (uint32_t)(ten >> 32)};
  CHECK(cpu_call64(c, thunk("pow"), 4, pw) == dbits(1024.0), "pow(2, 10) is not 1024");
  uint32_t fx[] = {(uint32_t)eight, (uint32_t)(eight >> 32), buf + 0x300};
  CHECK(cpu_call64(c, thunk("frexp"), 3, fx) == dbits(0.5) && gm_r32(buf + 0x300) == 4,
        "frexp(8) wrong");
  float f25 = 2.5f;
  uint32_t fb;
  memcpy(&fb, &f25, 4);
  uint32_t lr[] = {fb};
  CHECK(call_n(c, "lrintf", 1, lr) == 2, "lrintf(2.5) is not 2");

  uint32_t ex[] = {main_lib->base + 0x1000, buf + 0x300};
  CHECK(call_n(c, "__gnu_Unwind_Find_exidx", 2, ex) == main_lib->exidx &&
            gm_r32(buf + 0x300) == main_lib->exidx_count,
        "__gnu_Unwind_Find_exidx missed the main library's table");
  ex[0] = buf;
  CHECK(call_n(c, "__gnu_Unwind_Find_exidx", 2, ex) == 0, "exidx found for a non-code address");

  uint32_t gs[] = {0x1F02 /* GL_VERSION */};
  uint32_t v = call_n(c, "glGetString", 1, gs);
  CHECK(v && !strncmp(g2h(v), "OpenGL ES ", 10), "glGetString(GL_VERSION) is %s", v ? (char *)g2h(v) : "NULL");
  uint32_t gi[] = {0x0D33 /* GL_MAX_TEXTURE_SIZE */, buf + 0x300};
  call_n(c, "glGetIntegerv", 2, gi);
  CHECK(gm_r32(buf + 0x300) >= 2048, "GL_MAX_TEXTURE_SIZE is %u", gm_r32(buf + 0x300));
  /* Softfp floats in core registers, then a read back through a guest
   * pointer passed on the stack (glReadPixels' 7th argument). */
  float rgba[4] = {1.0f, 0.5f, 0.0f, 1.0f};
  uint32_t cc[4];
  memcpy(cc, rgba, sizeof cc);
  call_n(c, "glClearColor", 4, cc);
  uint32_t cl[] = {0x4000 /* GL_COLOR_BUFFER_BIT */};
  call_n(c, "glClear", 1, cl);
  gm_w32(buf + 0x300, 0);
  uint32_t rp[] = {0, 0, 1, 1, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, buf + 0x300};
  call_n(c, "glReadPixels", 7, rp);
  uint8_t *px = g2h(buf + 0x300);
  CHECK(px[0] == 255 && px[1] >= 127 && px[1] <= 128 && px[2] == 0 && px[3] == 255,
        "glClearColor/glReadPixels read back %u %u %u %u", px[0], px[1], px[2], px[3]);
}

/* Calls slot of the interface itf (a guest pointer to a vtable pointer). */
static uint32_t sl_call(cpu *c, uint32_t itf, int slot, int n, const uint32_t *rest) {
  uint32_t args[8] = {itf};
  for (int i = 0; i < n; i++) args[1 + i] = rest[i];
  return cpu_call(c, gm_r32(gm_r32(itf) + 4 * (uint32_t)slot), n + 1, args);
}

/* The buffer-queue callback of the playback test: records its arguments. */
static uint32_t bq_cb_calls, bq_cb_caller, bq_cb_ctx;
static void bq_test_callback(cpu *c) {
  bq_cb_caller = cpu_arg(c, 0);
  bq_cb_ctx = cpu_arg(c, 1);
  __atomic_fetch_add(&bq_cb_calls, 1, __ATOMIC_RELEASE);
}

/* /proc/cpuinfo, libdl and the OpenSL ES objects FMOD uses (fileio.c,
 * opensles.c), and eglGetProcAddress (gles.c). */
static void test_dl_opensl(cpu *c, uint32_t buf) {
  uint32_t at = buf + 0x800, out = buf, io = buf + 0x100;

  uint32_t cpath = gstr(&at, "/proc/cpuinfo");
  uint32_t op[] = {cpath, 0 /* O_RDONLY */};
  int fd = (int)call_n(c, "open", 2, op);
  CHECK(fd > 2, "open(/proc/cpuinfo) failed");
  uint32_t rd[] = {(uint32_t)fd, io, 0x600};
  uint32_t n = call_n(c, "read", 3, rd);
  char text[0x601] = {0};
  if (n <= 0x600) memcpy(text, g2h(io), n);
  CHECK(strstr(text, "CPU architecture: 7") && strstr(text, " neon ") && strstr(text, "(v7l)"),
        "/proc/cpuinfo lacks the ARMv7/NEON lines FMOD parses");
  uint32_t cl[] = {(uint32_t)fd};
  call_n(c, "close", 1, cl);
  op[1] = 1; /* O_WRONLY */
  CHECK((int32_t)call_n(c, "open", 2, op) == -1, "/proc/cpuinfo opened for writing");
  uint32_t fo[] = {cpath, gstr(&at, "r")};
  uint32_t f = call_n(c, "fopen", 2, fo);
  uint32_t fg[] = {io, 64, f};
  CHECK(f && call_n(c, "fgets", 3, fg) == io && !strncmp(g2h(io), "Processor", 9),
        "fopen(/proc/cpuinfo) gave no Processor line");
  if (f) call_n(c, "fclose", 1, &f);

  uint32_t dm[] = {gstr(&at, "libmissing.so"), 0};
  CHECK(call_n(c, "dlopen", 2, dm) == 0, "dlopen of an unknown library succeeded");
  CHECK(call_n(c, "dlerror", 0, NULL) != 0 && call_n(c, "dlerror", 0, NULL) == 0,
        "dlerror should report once, then clear");
  uint32_t dl[] = {gstr(&at, "libOpenSLES.so"), 0};
  uint32_t lib = call_n(c, "dlopen", 2, dl);
  CHECK(lib != 0, "dlopen(libOpenSLES.so) failed");
  uint32_t sy[] = {lib, 0};
  sy[1] = gstr(&at, "slCreateEngine");
  uint32_t create = call_n(c, "dlsym", 2, sy);
  static const char *const names[] = {"SL_IID_ENGINE", "SL_IID_PLAY",
                                      "SL_IID_ANDROIDSIMPLEBUFFERQUEUE",
                                      "SL_IID_ANDROIDCONFIGURATION", "SL_IID_RECORD"};
  uint32_t iid[5];
  for (int i = 0; i < 5; i++) {
    sy[1] = gstr(&at, names[i]);
    uint32_t var = call_n(c, "dlsym", 2, sy);
    iid[i] = var ? gm_r32(var) : 0;
    CHECK(iid[i] != 0, "dlsym(%s) failed", names[i]);
  }
  sy[1] = gstr(&at, "slCreateFoo");
  CHECK(call_n(c, "dlsym", 2, sy) == 0, "dlsym of an unknown symbol succeeded");
  if (!create || !iid[0]) return;

  uint32_t ce[] = {out, 0, 0, 0, 0, 0};
  CHECK(cpu_call(c, create, 6, ce) == 0, "slCreateEngine failed");
  uint32_t eng = gm_r32(out), z[] = {0, 0, 0, 0, 0, 0};
  CHECK(sl_call(c, eng, 0 /* Realize */, 1, z) == 0, "engine Realize failed");
  CHECK(sl_call(c, eng, 0, 1, z) == 1, "a second Realize should be PRECONDITIONS_VIOLATED");
  uint32_t gs[] = {out + 4};
  CHECK(sl_call(c, eng, 2 /* GetState */, 1, gs) == 0 && gm_r32(out + 4) == 2,
        "engine is not REALIZED");
  uint32_t gi[] = {iid[0], out + 8};
  CHECK(sl_call(c, eng, 3 /* GetInterface */, 2, gi) == 0, "GetInterface(ENGINE) failed");
  uint32_t engine = gm_r32(out + 8);

  uint32_t om[] = {out + 12, 0, 0, 0};
  CHECK(sl_call(c, engine, 7 /* CreateOutputMix */, 4, om) == 0, "CreateOutputMix failed");
  uint32_t mix = gm_r32(out + 12);
  sl_call(c, mix, 0, 1, z);
  uint32_t gp[] = {iid[1], out + 16};
  CHECK(sl_call(c, mix, 3, 2, gp) == 12, "an output mix should have no Play interface");

  /* A 2-buffer PCM queue, as FMOD builds it (with 4). */
  uint32_t loc = buf + 0x200, fmt = buf + 0x210, src = buf + 0x230, ids = buf + 0x240,
           req = buf + 0x250;
  uint32_t locv[] = {0x800007BD, 2}, fmtv[] = {2, 2, 24000000, 16, 16, 3, 2};
  memcpy(g2h(loc), locv, sizeof locv);
  memcpy(g2h(fmt), fmtv, sizeof fmtv);
  gm_w32(src, loc);
  gm_w32(src + 4, fmt);
  uint32_t idv[] = {iid[1], iid[2], iid[3]}, reqv[] = {1, 1, 1};
  memcpy(g2h(ids), idv, sizeof idv);
  memcpy(g2h(req), reqv, sizeof reqv);
  gm_w32(ids + 12, iid[4]);
  uint32_t rec[] = {out + 20, src, buf + 0x260, 1, ids + 12, req};
  CHECK(sl_call(c, engine, 2 /* CreateAudioPlayer */, 6, rec) == 12,
        "a player requiring SL_IID_RECORD should be unsupported");
  uint32_t cp[] = {out + 20, src, buf + 0x260, 3, ids, req};
  CHECK(sl_call(c, engine, 2, 6, cp) == 0, "CreateAudioPlayer failed");
  uint32_t player = gm_r32(out + 20);

  uint32_t gc[] = {iid[3], out + 24};
  CHECK(sl_call(c, player, 3, 2, gc) == 0, "configuration interface refused before Realize");
  gp[1] = out + 28;
  CHECK(sl_call(c, player, 3, 2, gp) == 1, "Play interface handed out before Realize");
  CHECK(sl_call(c, player, 0, 1, z) == 0, "player Realize failed");
  CHECK(sl_call(c, player, 3, 2, gp) == 0, "GetInterface(PLAY) failed");
  uint32_t gb[] = {iid[2], out + 32};
  CHECK(sl_call(c, player, 3, 2, gb) == 0, "GetInterface(BUFFERQUEUE) failed");
  uint32_t play = gm_r32(out + 28), bq = gm_r32(out + 32);

  uint32_t rc[] = {0x1235, 0x77};
  CHECK(sl_call(c, bq, 3 /* RegisterCallback */, 2, rc) == 0, "RegisterCallback failed");
  uint32_t eq[] = {io, 0x800};
  CHECK(sl_call(c, bq, 0 /* Enqueue */, 2, eq) == 0 && sl_call(c, bq, 0, 2, eq) == 0,
        "Enqueue failed");
  CHECK(sl_call(c, bq, 0, 2, eq) == 7, "a third Enqueue on 2 buffers should be insufficient");
  uint32_t st[] = {out + 36};
  CHECK(sl_call(c, bq, 2 /* GetState */, 1, st) == 0 && gm_r32(out + 36) == 2,
        "buffer queue count is %u, not 2", gm_r32(out + 36));
  sl_call(c, bq, 1 /* Clear */, 0, z);
  sl_call(c, bq, 2, 1, st);
  CHECK(gm_r32(out + 36) == 0, "Clear left %u buffers", gm_r32(out + 36));

  uint32_t ps[] = {3 /* PLAYING */}, gps[] = {out + 40};
  CHECK(sl_call(c, play, 0, 1, ps) == 0 && sl_call(c, play, 1, 1, gps) == 0 &&
            gm_r32(out + 40) == 3,
        "play state did not become PLAYING");
  CHECK(sl_call(c, bq, 3, 2, rc) == 1, "RegisterCallback while playing should be refused");

  /* The freed block can't be called through, so a stale handle is a word of
   * our own holding the object vtable. */
  uint32_t stale = out + 44;
  gm_w32(stale, gm_r32(player));
  sl_call(c, player, 6 /* Destroy */, 0, z);
  CHECK(sl_call(c, stale, 2, 1, gs) == 2, "an unknown object still answers");

  /* Playback: a fresh player plays two enqueued buffers of a quiet square
   * wave (-54 dBFS) through the host output and calls the callback once per
   * buffer with its buffer-queue interface and context, as Android does. */
  CHECK(sl_call(c, engine, 2, 6, cp) == 0, "second CreateAudioPlayer failed");
  player = gm_r32(out + 20);
  sl_call(c, player, 0, 1, z);
  sl_call(c, player, 3, 2, gp);
  sl_call(c, player, 3, 2, gb);
  play = gm_r32(out + 28);
  bq = gm_r32(out + 32);
  uint32_t cb[] = {dynlib_host_thunk("bq_test_callback", bq_test_callback), 0x5151};
  CHECK(sl_call(c, bq, 3, 2, cb) == 0, "RegisterCallback with a real callback failed");
  for (uint32_t i = 0; i < 0x800 / 2; i++) /* stereo, period 32 frames */
    ((int16_t *)g2h(io))[i] = (i / 2) % 32 < 16 ? 64 : -64;
  CHECK(sl_call(c, bq, 0, 2, eq) == 0 && sl_call(c, bq, 0, 2, eq) == 0, "Enqueue failed");
  audio_stats before, after;
  audio_get_stats(&before);
  CHECK(sl_call(c, play, 0, 1, ps) == 0, "SetPlayState(PLAYING) failed");
  for (int ms = 0; ms < 2000 && __atomic_load_n(&bq_cb_calls, __ATOMIC_ACQUIRE) < 2; ms++)
    usleep(1000);
  CHECK(bq_cb_calls == 2, "buffer-queue callback ran %u times for 2 buffers", bq_cb_calls);
  CHECK(bq_cb_caller == bq && bq_cb_ctx == 0x5151,
        "callback got (0x%08x, 0x%x), not its buffer queue and context", bq_cb_caller, bq_cb_ctx);
  sl_call(c, bq, 2, 1, st);
  CHECK(gm_r32(out + 36) == 0 && gm_r32(out + 40) == 2,
        "after playback the queue state is {%u, %u}, not {0, 2}", gm_r32(out + 36),
        gm_r32(out + 40));
  uint32_t pos[] = {out + 44};
  CHECK(sl_call(c, play, 3 /* GetPosition */, 1, pos) == 0 && gm_r32(out + 44) == 42,
        "position after 1024 frames at 24 kHz is %u ms, not 42", gm_r32(out + 44));
  /* A device mixes asynchronously; give its callback time to see the tone. */
  for (int ms = 0; ms < 1000; ms++) {
    audio_get_stats(&after);
    if (after.nonsilent > before.nonsilent) break;
    usleep(1000);
  }
  CHECK(after.nonsilent > before.nonsilent, "the tone never reached the audio output (%s)",
        after.device ? "SDL device" : "virtual clock");
  sl_call(c, player, 6, 0, z);
  sl_call(c, mix, 6, 0, z);
  sl_call(c, eng, 6, 0, z);

  uint32_t gpa[] = {gstr(&at, "glGenVertexArraysOES")};
  uint32_t gen = call_n(c, "eglGetProcAddress", 1, gpa);
  uint32_t ga[] = {2, out};
  memset(g2h(out), 0, 8);
  CHECK(gen && (cpu_call(c, gen, 2, ga), gm_r32(out) && gm_r32(out + 4) &&
                                             gm_r32(out) != gm_r32(out + 4)),
        "eglGetProcAddress(glGenVertexArraysOES) gave no working function");
  gpa[0] = gstr(&at, "glFooBarEXT");
  CHECK(call_n(c, "eglGetProcAddress", 1, gpa) == 0, "an unknown GL proc was not NULL");
}

int cpu_selftest(cpu *c, const so_module *main_lib) {
  failures = 0;
  uint32_t buf = gm_alloc(0x20000, 0, "cpu-test buffer");
  uint32_t md = gm_alloc(64, 0, "cpu-test digest");
  uint32_t code = gm_alloc(0x400, 0, "cpu-test code");
  if (!buf || !md || !code) return -1;

  test_calling_convention(c, code);
  test_sha256(c, main_lib, buf, md);
  test_sha1(c, main_lib, buf, md);
  test_import_thunks(c, main_lib, buf);
  test_runtime_glue(c, code, buf);
  test_libc_shims(c, buf);
  test_stdio(c, buf);
  test_ctype(c, main_lib);
  test_threads(c, code, buf);
  test_fileio(c, buf);
  test_misc_shims(c, main_lib, buf);
  test_dl_opensl(c, buf);

  printf("cpu-test [%s]: %s\n", c->ops->name, failures ? "FAILED" : "OK");
  return failures ? -1 : 0;
}
