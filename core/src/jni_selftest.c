/*
 * Fake-JNI test: drives the guest JNIEnv and JavaVM tables through their
 * thunks the way native code does: class and method lookups (found and
 * missing), pending exceptions, and a host-implemented Java method called
 * through the variadic, va_list and jvalue[] forms with every argument kind.
 * The profile's jni_test then calls some of the game's own entry points.
 */
#include "jni_selftest.h"

#include <stdio.h>
#include <string.h>

#include "fake_jni.h"
#include "guest_mem.h"

static int failures;

#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      fprintf(stderr, "jni-test FAIL: " __VA_ARGS__);                                              \
      fputc('\n', stderr);                                                                         \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

/* JNINativeInterface / JNIInvokeInterface slots used here. */
enum {
  GetVersion = 4,
  FindClass = 6,
  ExceptionClear = 17,
  GetStaticMethodID = 113,
  CallStaticLongMethod = 132,
  CallStaticLongMethodV = 133,
  CallStaticLongMethodA = 134,
  GetStringUTFChars = 169,
  ReleaseStringUTFChars = 170,
  GetJavaVM = 219,
  ExceptionCheck = 228,
  VM_GetEnv = 6,
};

static uint32_t env_fn(int slot) { return gm_r32(gm_r32(jni_env()) + (uint32_t)slot * 4); }
static uint32_t vm_fn(int slot) { return gm_r32(gm_r32(jni_vm()) + (uint32_t)slot * 4); }

static uint32_t env_call(cpu *c, int slot, int nargs, const uint32_t *rest) {
  uint32_t args[12] = {jni_env()};
  for (int i = 0; i < nargs; i++) args[i + 1] = rest[i];
  return cpu_call(c, env_fn(slot), nargs + 1, args);
}

static uint32_t put_str(uint32_t *scratch, const char *s) {
  uint32_t a = *scratch;
  strcpy(g2h(a), s);
  *scratch += ((uint32_t)strlen(s) + 8) & ~7u;
  return a;
}

/* long jniTestSum(int, long, float, double, String, boolean, short) */
#define TEST_SIG "(IJFDLjava/lang/String;ZS)J"
static uint64_t test_sum(const jni_call *call) {
  if (call->nargs != 7) return 0;
  float f;
  double d;
  uint32_t fb = (uint32_t)call->args[2];
  memcpy(&f, &fb, 4);
  memcpy(&d, &call->args[3], 8);
  const char *s = jni_string_utf8((uint32_t)call->args[4]);
  return (uint64_t)((int64_t)(int32_t)call->args[0] + (int64_t)call->args[1] + (int64_t)(f * 4) +
                    (int64_t)(d * 8) + (s ? (int64_t)strlen(s) : -1000) +
                    (int64_t)call->args[5] * 100 + (int64_t)call->args[6]);
}
/* 7 + 0x100000000 + 1.5*4 + 2.25*8 + strlen("rainbow") + 100 + (-3) */
#define TEST_WANT (7ll + 0x100000000ll + 6 + 18 + 7 + 100 - 3)

static void test_env_tables(cpu *c, uint32_t scratch) {
  CHECK(env_call(c, GetVersion, 0, NULL) == JNI_VERSION_1_6, "GetVersion");

  /* JavaVM: GetJavaVM, then vm->GetEnv must hand back the same JNIEnv. */
  uint32_t out = scratch;
  scratch += 8;
  uint32_t a[8];
  a[0] = out;
  CHECK(env_call(c, GetJavaVM, 1, a) == 0 && gm_r32(out) == jni_vm(), "GetJavaVM");
  uint32_t vmargs[] = {jni_vm(), out, JNI_VERSION_1_6};
  CHECK(cpu_call(c, vm_fn(VM_GetEnv), 3, vmargs) == 0 && gm_r32(out) == jni_env(), "vm->GetEnv");

  /* A class the Java shell lacks: NULL plus a pending exception, which clears. */
  a[0] = put_str(&scratch, "droidlift/test/NoSuchClass");
  CHECK(env_call(c, FindClass, 1, a) == 0, "FindClass of a missing class is not NULL");
  CHECK(env_call(c, ExceptionCheck, 0, NULL) == 1, "missing class left no exception");
  env_call(c, ExceptionClear, 0, NULL);
  CHECK(env_call(c, ExceptionCheck, 0, NULL) == 0, "ExceptionClear");

  a[0] = put_str(&scratch, "java/lang/Object");
  uint32_t cls = env_call(c, FindClass, 1, a);
  CHECK(cls, "FindClass(java/lang/Object) is NULL");

  /* A method lookup on a bad class handle: NULL, exception, no crash. */
  a[0] = 0;
  a[1] = put_str(&scratch, "IsTablet");
  a[2] = put_str(&scratch, "()Z");
  CHECK(env_call(c, GetStaticMethodID, 3, a) == 0, "GetStaticMethodID on NULL class");
  env_call(c, ExceptionClear, 0, NULL);

  CHECK(jni_register_method("java/lang/Object", "jniTestSum", TEST_SIG, test_sum) == 0,
        "jni_register_method");
  a[0] = cls;
  a[1] = put_str(&scratch, "jniTestSum");
  a[2] = put_str(&scratch, TEST_SIG);
  uint32_t mid = env_call(c, GetStaticMethodID, 3, a);
  CHECK(mid, "GetStaticMethodID(jniTestSum) is NULL");

  uint32_t str = jni_new_string("rainbow");
  double d = 2.25, fd = 1.5; /* variadic float arguments arrive as double */
  uint32_t dw[2], fw[2];
  memcpy(dw, &d, 8);
  memcpy(fw, &fd, 8);

  /* Variadic: env, cls, mid in r0-r2, the int in r3, then the stack: the long
   * at words 4-5 (8-byte aligned), float-as-double, double, String, boolean,
   * short. */
  uint32_t va[] = {jni_env(), cls,   mid, 7, 0, 1, fw[0], fw[1],
                   dw[0],     dw[1], str, 1, (uint32_t)-3};
  uint64_t r = cpu_call64(c, env_fn(CallStaticLongMethod), 13, va);
  CHECK(!c->faulted && (int64_t)r == TEST_WANT, "CallStaticLongMethod returned %lld, want %lld",
        (long long)r, TEST_WANT);

  /* va_list: a pointer to the same words as they would sit on the stack. */
  uint32_t list = (scratch + 7) & ~7u;
  static const uint32_t words[] = {7, 0, 0, 1};
  memcpy(g2h(list), words, sizeof words);
  memcpy(g2h(list + 16), fw, 8);
  memcpy(g2h(list + 24), dw, 8);
  gm_w32(list + 32, str);
  gm_w32(list + 36, 1);
  gm_w32(list + 40, (uint32_t)-3);
  uint32_t vl[] = {jni_env(), cls, mid, list};
  r = cpu_call64(c, env_fn(CallStaticLongMethodV), 4, vl);
  CHECK(!c->faulted && (int64_t)r == TEST_WANT, "CallStaticLongMethodV returned %lld",
        (long long)r);

  /* jvalue[]: 8 bytes per argument, float stored as float, boolean and short narrow. */
  uint32_t jv = list + 64;
  memset(g2h(jv), 0xAA, 7 * 8); /* garbage above the narrow members must be ignored */
  gm_w32(jv + 0, 7);
  gm_w32(jv + 8, 0);
  gm_w32(jv + 12, 1);
  float f = 1.5f;
  memcpy(g2h(jv + 16), &f, 4);
  memcpy(g2h(jv + 24), &d, 8);
  gm_w32(jv + 32, str);
  *(uint8_t *)g2h(jv + 40) = 1;
  *(int16_t *)g2h(jv + 48) = -3;
  uint32_t ja[] = {jni_env(), cls, mid, jv};
  r = cpu_call64(c, env_fn(CallStaticLongMethodA), 4, ja);
  CHECK(!c->faulted && (int64_t)r == TEST_WANT, "CallStaticLongMethodA returned %lld",
        (long long)r);

  /* GetStringUTFChars hands out a guest copy. */
  a[0] = str;
  a[1] = 0;
  uint32_t chars = env_call(c, GetStringUTFChars, 2, a);
  CHECK(chars && strcmp(g2h(chars), "rainbow") == 0, "GetStringUTFChars");
  a[1] = chars;
  env_call(c, ReleaseStringUTFChars, 2, a);
}

int jni_selftest(cpu *c) {
  failures = 0;
  uint32_t scratch = gm_alloc(0x1000, 0, "jni-test scratch");
  if (!scratch) return -1;
  test_env_tables(c, scratch);
  CHECK(jni_unimplemented_calls() == 0, "%u unimplemented JNI calls", jni_unimplemented_calls());
  printf("jni-test [%s]: %s\n", c->ops->name, failures ? "FAILED" : "OK");
  return failures ? -1 : 0;
}
