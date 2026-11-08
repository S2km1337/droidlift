/*
 * droidlift: run a native Android game (armeabi-v7a .so libraries plus a thin
 * Java shell) on macOS and iOS, under a JIT, an interpreter, or ahead-of-time
 * recompiled C.
 *
 * A port is a game profile (dl_game, below), registered with DL_MAIN:
 *
 *     #include "droidlift.h"
 *     static const dl_game my_game = {...};
 *     DL_MAIN(&my_game)
 *
 * The toolkit loads and relocates the libraries from the APK, binds their
 * imports to its own bionic/GLES/OpenSL ES/AAsset implementations, runs the
 * constructors, opens a window (or a headless surface for tests), and then
 * calls the profile's hooks where Android's Java side would call into native
 * code. Everything the Java side did for the game is the profile's job:
 * which Java_* entry points to call, and the Java methods the native code
 * calls back (jni_register_method in the init hook). docs/porting.md walks
 * through writing one; docs/limits.md lists what the toolkit does not do.
 */
#ifndef DROIDLIFT_H
#define DROIDLIFT_H

#include <stdint.h>

#include "cpu.h"
#include "fake_jni.h"
#include "input.h"
#include "so_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A real function in one of the game's libraries, run under dynarmic and
 * the interpreter by --interp-test (both must get it right and agree on
 * every register and all of guest memory). buf is a 1 MiB guest buffer. */
typedef struct dl_fn_test {
  const char *name;
  int (*run)(cpu *c, uint32_t buf);
} dl_fn_test;

typedef struct dl_game {
  /* Shown as the window title and in logs. */
  const char *name;
  /* Folder for the game's files, saves and cache under the platform's app
   * storage (~/Library/Application Support/<storage> on the Mac, the app
   * container on iOS), unless --data says otherwise. */
  const char *storage;
  /* The libraries in System.loadLibrary order, as lib/armeabi-v7a/<name> in
   * the APK; NULL-terminated. The last one is the "main" library that the
   * hooks get. */
  const char *const *libs;
  /* Classes FindClass resolves, besides the android/java basics the fake VM
   * knows (fake_jni.c); NULL-terminated. A class missing here fails like a
   * class missing from classes.dex. */
  const char *const *jni_classes;
  /* Widest aspect ratio the game draws for, e.g. 16:9. A wider surface gets
   * the game centred with black bars (pillarbox); 0:0 fills the surface. */
  int max_aspect_w, max_aspect_h;
  /* Default headless surface for tests (--size overrides it); 0 means
   * 2532x1170, an iPhone 14 in landscape. */
  int test_w, test_h;

  /* ---- hooks: return 0, or -1 on failure (a guest fault) ---- */

  /* After the fake VM is up, before any guest code runs: register the Java
   * methods native code calls (jni_register_method). data_dir is where the
   * game's files go. Required. */
  int (*init)(const char *data_dir);
  /* After every library's constructors: the activity's onCreate natives. */
  int (*start)(cpu *c, const so_module *main);
  /* The GL surface exists (width x height pixels, the game's rectangle):
   * onSurfaceCreated/onSurfaceChanged. */
  int (*surface)(cpu *c, const so_module *main, uint32_t width, uint32_t height);
  /* One frame: onDrawFrame. Call input_dispatch() in here where the Java
   * side delivered queued touches. Required. */
  int (*frame)(cpu *c, const so_module *main);
  /* One touch for the game, called by input_dispatch() in arrival order. x
   * and y are pixels of the game's rectangle, time is seconds of uptime. */
  int (*touch)(cpu *c, const so_module *main, touch_kind kind, int id, float x, float y,
               double time);
  /* Activity lifecycle. The toolkit tracks the state, so each hook runs once
   * per change: pause (onPause), stop (onStop; also at quit, which is when
   * Android games save), restart (onRestart, after a stop) and resume
   * (onResume). Any may be NULL. */
  int (*pause)(cpu *c, const so_module *main);
  int (*stop)(cpu *c, const so_module *main);
  int (*restart)(cpu *c, const so_module *main);
  int (*resume)(cpu *c, const so_module *main);

  /* ---- optional game-specific checks for the toolkit's self-tests ---- */

  /* --jni-test, after the fake JNIEnv checks: call some of the game's
   * Java_* entry points. */
  int (*jni_test)(cpu *c, const so_module *main);
  /* --init-test, right after start: e.g. that the assets the game needs open. */
  int (*init_test)(cpu *c, const so_module *main);
  /* --patch-test: that the game's load-time patches (game_patches.h) work. */
  int (*patch_test)(cpu *c, const so_module *main);
  /* --interp-test: real functions to compare (NULL-terminated by name). */
  const dl_fn_test *fn_tests;
} dl_game;

/* Runs the loader with the profile: parses the command line (--help lists
 * it), loads the game and runs it or the requested self-tests. Returns the
 * process exit status. */
int dl_main(const dl_game *game, int argc, char **argv);

/* The profile dl_main is running. */
const dl_game *dl_game_active(void);
/* The libraries as loaded, in profile order. */
const so_module *dl_modules(int *count);

/* Lifecycle steps for the toolkit (lifecycle.c) and the profile: each calls
 * the profile's hook once per state change. */
int dl_app_pause(cpu *c);
int dl_app_stop(cpu *c);
int dl_app_resume(cpu *c);
int dl_app_is_paused(void);

/* A Java class's native method, as Java would find it: registered with
 * RegisterNatives (JNI_OnLoad), else the exported Java_<class>_<method>
 * (JNI's name mangling). cls uses slashes ("com/example/Game"). 0 if
 * neither exists. */
uint32_t dl_native(const so_module *m, const char *cls, const char *name);

/* Finds a hidden (non-exported) function by its offset in a library, after
 * checking its first two halfwords: for --interp-test and game patches.
 * Returns the guest address (bit 0 set for Thumb), or 0 with a message. */
typedef struct dl_known_fn {
  const char *name;
  uint32_t off; /* bit 0: Thumb */
  uint16_t head[2];
} dl_known_fn;
uint32_t dl_known_addr(const so_module *m, const dl_known_fn *table, int count, const char *name);

#ifdef __cplusplus
}
#endif

/* The port's profile: DL_MAIN(&profile) in one of the port's files. The
 * toolkit's own main() (dl_entry.c; on iOS SDL's entry point) runs dl_main
 * with it, so a port never defines main itself. */
extern const dl_game *const dl_the_game;
#define DL_MAIN(game) const dl_game *const dl_the_game = (game);

#endif
