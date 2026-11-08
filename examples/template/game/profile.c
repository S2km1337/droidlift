/*
 * The game profile: what the game's Java side did, in C. Start with libs and
 * classes, run --check, then fill in the hooks one at a time
 * (droidlift/docs/porting.md).
 */
#include <stdio.h>

#include "droidlift.h"

/* System.loadLibrary order; the game itself last. */
static const char *const libs[] = {"libgame.so", NULL};

/* The FindClass strings in the libraries that exist in classes.dex. */
static const char *const classes[] = {
    "com/example/game/GameActivity",
    "com/example/game/GameLib",
    NULL,
};

#define LIB "com/example/game/GameLib"

/* Calls a static native of LIB: (JNIEnv *, jclass, args...). */
static int call_lib(cpu *c, const so_module *m, const char *name, int nargs, const uint32_t *extra) {
  uint32_t fn = dl_native(m, LIB, name);
  if (!fn) {
    fprintf(stderr, "[game] %s.%s not found\n", LIB, name);
    return -1;
  }
  uint32_t args[8] = {jni_env(), jni_find_class(LIB)};
  for (int i = 0; i < nargs && i < 6; i++) args[2 + i] = extra[i];
  cpu_call(c, fn, 2 + nargs, args);
  return c->faulted ? -1 : 0;
}

/* The Java methods native code calls: jni_register_method(class, name,
 * signature, host function) for each. Unimplemented ones are logged as
 * "[jni] ... no host implementation". */
static int init(const char *data_dir) {
  printf("[game] data in %s\n", data_dir);
  return 0;
}

static int start(cpu *c, const so_module *m) { return call_lib(c, m, "init", 0, NULL); }

static int surface(cpu *c, const so_module *m, uint32_t w, uint32_t h) {
  uint32_t size[2] = {w, h};
  return call_lib(c, m, "resize", 2, size);
}

static int frame(cpu *c, const so_module *m) {
  if (input_dispatch(c, m) != 0) return -1;
  return call_lib(c, m, "step", 0, NULL);
}

static uint32_t f32_bits(float f) {
  union {
    float f;
    uint32_t u;
  } v = {f};
  return v.u;
}

/* e.g. GameLib.touch(int action, int id, float x, float y). */
static int touch(cpu *c, const so_module *m, touch_kind kind, int id, float x, float y, double time) {
  (void)time;
  uint32_t a[4] = {(uint32_t)kind, (uint32_t)id, f32_bits(x), f32_bits(y)};
  return call_lib(c, m, "touch", 4, a);
}

static int on_pause(cpu *c, const so_module *m) { return call_lib(c, m, "pause", 0, NULL); }
static int on_resume(cpu *c, const so_module *m) { return call_lib(c, m, "resume", 0, NULL); }

static const dl_game game = {
    .name = "My Game",
    .storage = "My Game",
    .libs = libs,
    .jni_classes = classes,
    .max_aspect_w = 16,
    .max_aspect_h = 9,
    .init = init,
    .start = start,
    .surface = surface,
    .frame = frame,
    .touch = touch,
    .pause = on_pause,
    .resume = on_resume,
};

DL_MAIN(&game)
