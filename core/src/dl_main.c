/*
 * dl_main: the loader, driven by a game profile (droidlift.h).
 *
 * Loads the profile's libraries from the APK (or an unpacked copy) into the
 * guest address space, relocates them and binds every import. With no test
 * flag that is all (--check makes a missing import an error); the test
 * flags then exercise the toolkit and the game:
 *
 *   --cpu-test     guest code under the CPU backend against the host
 *   --interp-test  the interpreter against dynarmic (plus the profile's
 *                  fn_tests on real game functions)
 *   --fmt-test     the printf/scanf shims against the host libc
 *   --jni-test     the fake JNIEnv (plus the profile's jni_test)
 *   --patch-test   the load-time game patches (plus the profile's patch_test)
 *   --script-test  the --touch parser and its waits on made-up signals
 *   --init-test    the libraries' constructors, the profile's start and
 *                  surface hooks, then --frames N frames (or a --window until
 *                  it closes), with screenshot, touch, audio and pillarbox
 *                  checks for the smoke tests
 *
 * GL goes to ANGLE's Metal backend, loaded from --angle. --cpu picks the
 * backend: dynarmic (JIT), interp (our ARMv7 interpreter) or recomp (the
 * libraries recompiled ahead of time, with the interpreter as fallback).
 *
 * Debug aids: DL_TRACE_INIT=1 logs each constructor, DL_TRACE_PC=1
 * single-steps the guest and logs every PC, DL_TRACE_IMPORTS=1 logs every
 * implemented import and host-thunk call with its arguments and result,
 * DL_TRACE_INPUT=1 logs every touch delivered to the game,
 * DL_TRACE_FILES=1 logs the game's file opens, renames and saves,
 * DL_TRACE_FMT=1 logs every printf- and scanf-family call and its result.
 *
 * When the frames end (or the window closes) the app quits like an Android
 * app that is left and closed: paused, then stopped, which is when Android
 * games save.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "apk.h"
#include "audio.h"
#include "cpu.h"
#include "cpu_selftest.h"
#include "droidlift.h"
#include "dynlib.h"
#include "fake_jni.h"
#include "fmt_selftest.h"
#include "game_patches.h"
#include "gfx.h"
#include "guest_mem.h"
#include "input.h"
#include "interp_selftest.h"
#include "jni_selftest.h"
#include "lifecycle.h"
#include "reimpl/reimpl.h"
#include "so_util.h"
#include "startup.h"
#include "storage.h"

#ifdef DL_IOS
#include "ios_shell.h"
#endif

static const dl_game *game;
static so_module *modules;
static int n_libs;

const dl_game *dl_game_active(void) { return game; }

const so_module *dl_modules(int *count) {
  if (count) *count = n_libs;
  return modules;
}

static void usage(const char *prog) {
  fprintf(stderr, "usage: %s --apk PATH [--work DIR] [--data DIR] [--report FILE] [--check] [--cpu-test]\n"
                  "         [--fmt-test] [--patch-test] [--jni-test] [--init-test] ...\n"
                  "  --apk     the game's APK (read-only input)\n"
                  "  --work    directory to unpack into (default: extracted/apk)\n"
                  "  --unpacked  instead of --apk: --work already holds the APK's lib/ and\n"
                  "            assets/ (the iOS app bundle)\n"
                  "  --data    directory for the game's files, saves and cache (default:\n"
                  "            the app's storage, ~/Library/Application Support/%s)\n"
                  "  --report  write the import binding report (TSV) here\n"
                  "  --check   exit non-zero unless every import is bound and every\n"
                  "            relocation was applied\n"
                  "  --cpu-test  run guest functions under the CPU backend and check them\n"
                  "            against the host\n"
                  "  --cpu NAME  CPU backend: dynarmic, interp (our ARMv7 interpreter) or\n"
                  "            recomp (the recompiled libraries); the default is this\n"
                  "            build's DL_DEFAULT_CPU (dynarmic on the Mac, recomp on the iPhone)\n"
                  "  --interp-test  differential tests of the interpreter against dynarmic\n"
                  "            (DL_INTERP_TESTS=N scales the random tests)\n"
                  "  --fmt-test  run the printf and scanf shims through their guest thunks and\n"
                  "            compare them with the host libc\n"
                  "  --patch-test  check the load-time game patches\n"
                  "  --script-test  check the --touch parser and its waits on made-up signals\n"
                  "            (no game code runs)\n"
                  "  --jni-test  check the fake JNIEnv and call the game's Java_* entry points\n"
                  "  --init-test  run the libraries' constructors like Android's linker, then\n"
                  "            the game's start and surface hooks\n"
                  "  --angle   directory with ANGLE's libEGL/libGLESv2.dylib\n"
                  "            (default: " GFX_DEFAULT_ANGLE_DIR ")\n"
                  "  --frames  with --init-test: render this many frames afterwards\n"
                  "  --screenshot  save the last frame here (PNG); fails if under 1%% of it\n"
                  "            differs from the background\n"
                  "  --window  with --init-test: render into a window at up to 60 fps until\n"
                  "            it is closed (or for --frames N); exits 77 if no window opens\n"
                  "  --pvrtc-check  with --frames: after the last frame, decode the game's PVRTC\n"
                  "            textures in software and on the GPU and require them to match;\n"
                  "            also check each uncompressed format the game uses (LUMINANCE...)\n"
                  "  --audio-check  with --init-test: require output, a quarter of it non-silent,\n"
                  "            at the audio output (DL_AUDIO=off plays nothing and uses a virtual clock)\n"
                  "  --touch   with --frames: scripted touches, \"WHEN:ACTION:X,Y;...\" with\n"
                  "            ACTION down|move|up|tap and X,Y in surface pixels (repeatable);\n"
                  "            WHEN is a frame or a wait for the game, SIGNAL[#K][+N], then\n"
                  "            optionally \"every N[ms] [until SIGNAL] [times K]\" (input.h)\n"
                  "  --frames-after-script N  with --touch: end the run N frames after the\n"
                  "            last scripted touch; fail if the script isn't done by --frames\n"
                  "  --expect-change  with --touch: fail unless the last frame differs from\n"
                  "            the frame before the first scripted touch in >= 10%% of pixels\n"
                  "  --sfx-check  with --touch: fail unless a quarter second of sound\n"
                  "            reaches the output after the last scripted touch\n"
                  "  --shots-every N:DIR  with --frames: also save every Nth frame as\n"
                  "            DIR/frame-NNNNN.png (to watch a scripted run)\n"
                  "  --size WxH  headless surface size in pixels (default: the game's test size)\n"
                  "  --expect-pillarbox  with --frames: fail unless the last frame has black\n"
                  "            bars beside a game that fills its rectangle\n"
                  "  --verbose  log every asset open, including the engine's format probes\n"
                  "            that miss (also DL_VERBOSE=1)\n",
          prog, game->storage);
}

static void print_summary(const so_module *m) {
  const so_stats *s = &m->stats;
  printf("%-16s base 0x%08x size 0x%07x  relocs: %u relative, %u abs32, %u glob_dat, "
         "%u jump_slot\n",
         m->soname, m->base, m->size, s->relative, s->abs32, s->glob_dat, s->jump_slot);
  printf("%-16s imports: %u = %u library + %u stub + %u data + %u weak-null; "
         "%u unresolved, %u unsupported relocs\n",
         "", s->imports, s->imports_lib, s->imports_stub, s->imports_data, s->imports_null,
         s->unresolved, s->unsupported);
}

/* Every relocation entry must have been applied exactly once. */
static int all_relocs_applied(const so_module *m) {
  const so_stats *s = &m->stats;
  uint32_t applied = s->relative + s->abs32 + s->glob_dat + s->jump_slot;
  uint32_t expected = (m->relsz + m->pltrelsz) / 8;
  if (applied != expected) {
    fprintf(stderr, "check: %s applied %u of %u relocations\n", m->soname, applied, expected);
    return 0;
  }
  return 1;
}

/* --pvrtc-check tolerance per 8-bit channel. The Apple M4 matches exactly;
 * the slack is for GPUs that round the blend differently. */
#define PVRTC_MAX_DIFF 8
#define PVRTC_MAX_MEAN_DIFF 0.5

/* --expect-change: a pixel counts as changed if a channel moved by more than
 * this, and the screen counts as changed if this share of pixels did. Small
 * animations (a button's shimmer) stay well below it. */
#define CHANGE_MIN_DELTA 48
#define CHANGE_MIN_SHARE 0.10
/* --sfx-check: non-silent samples (all channels) needed after the last
 * touch; a quarter second of 24 kHz stereo. */
#define SFX_MIN_NONSILENT 12000

/* Share of pixels whose RGB differs by more than CHANGE_MIN_DELTA. */
static double frame_change(const uint8_t *a, const uint8_t *b, long pixels) {
  long changed = 0;
  for (long i = 0; i < pixels; i++)
    for (int ch = 0; ch < 3; ch++)
      if (abs(a[4 * i + ch] - b[4 * i + ch]) > CHANGE_MIN_DELTA) {
        changed++;
        break;
      }
  return pixels ? (double)changed / pixels : 0;
}

/* Pillarbox check on a w x h RGBA frame with the game in the centred
 * game_w columns: the bars are black, and each edge column of the game is
 * mostly drawn (not black), so the game reaches its edges. (The caller also
 * checks the viewport: a stretched game passes this.) */
static int pillarbox_ok(const uint8_t *rgba, int w, int h, int game_w) {
  int x0 = (w - game_w) / 2;
  long bar_lit = 0, edge_dark[2] = {0, 0};
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const uint8_t *p = rgba + 4 * ((long)y * w + x);
      int lit = p[0] || p[1] || p[2];
      if (x < x0 || x >= x0 + game_w) bar_lit += lit;
      else if (x == x0 || x == x0 + game_w - 1) edge_dark[x != x0] += !lit;
    }
  if (bar_lit || edge_dark[0] > h / 2 || edge_dark[1] > h / 2) {
    fprintf(stderr, "frames: pillarbox wrong: %ld lit pixels in the bars, %ld/%ld black pixels "
            "in the game's left/right edge columns (of %d)\n", bar_lit, edge_dark[0], edge_dark[1], h);
    return 0;
  }
  return 1;
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* --frames-after-script N: the run ends N frames after the last scripted
 * touch, once every scripted item has finished (--frames stays the limit). */
static int script_over(int after_script, int rendered) {
  return after_script >= 0 && input_script_done() &&
         rendered > input_script_last_touch() + after_script;
}

/* --shots-every: DIR/frame-NNNNN.png of the back buffer. */
static int save_numbered_shot(const char *dir, int frame) {
  char path[1024];
  snprintf(path, sizeof path, "%s/frame-%05d.png", dir, frame);
  if (gfx_screenshot_png(path) < 0) {
    fprintf(stderr, "frames: can't write %s\n", path);
    return 0;
  }
  return 1;
}

/* --patch-test, the toolkit's half: every patch of a loaded library is in
 * place (dl_main applied them; applying again must find them already
 * there). */
static int patches_in_place(void) {
  if (!game_patches_enabled()) return 0; /* the game's own test checks the original bytes */
  int bad = 0, n = 0;
  for (const game_patch *p; (p = game_patch_at(n)); n++) {
    const so_module *m = so_module_find(p->soname);
    if (!m) continue;
    game_patch_result r = game_patch_apply(p, g2h(m->base), m->size);
    if (r != GP_ALREADY) {
      fprintf(stderr, "patch-test: %s (%s) is not in place\n", p->name, p->soname);
      bad++;
    }
  }
  printf("[patch] %d patches checked\n", n);
  return bad ? -1 : 0;
}

/* Runs fn on a fresh cpu; 0 on success. */
static int with_cpu(int (*fn)(cpu *c, const so_module *main)) {
  cpu *c = cpu_create();
  int r = cpu_setup(c, 1u << 20) != 0 || fn(c, &modules[n_libs - 1]) != 0 ? -1 : 0;
  if (dynlib_unimplemented_calls() != 0 || jni_unimplemented_calls() != 0) r = -1;
  c->ops->destroy(c);
  return r;
}

static int jni_checks(cpu *c, const so_module *main) {
  if (jni_selftest(c) != 0) return -1;
  return game->jni_test ? game->jni_test(c, main) : 0;
}

static int patch_checks(cpu *c, const so_module *main) {
  if (patches_in_place() != 0) return -1;
  return game->patch_test ? game->patch_test(c, main) : 0;
}

int dl_main(const dl_game *g, int argc, char **argv) {
  game = g;
  for (n_libs = 0; g->libs[n_libs]; n_libs++) {}
  if (n_libs == 0 || !g->init || !g->frame) {
    fprintf(stderr, "droidlift: the game profile needs libs, init and frame\n");
    return 2;
  }
  modules = calloc((size_t)n_libs, sizeof *modules);
  const char *prog = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];

  const char *apk = NULL, *work = "extracted/apk", *data = NULL;
  const char *report_path = NULL, *angle_dir = GFX_DEFAULT_ANGLE_DIR, *shot_path = NULL;
  int frames = 0, shots_every = 0;
  int size_w = g->test_w > 0 ? g->test_w : 2532, size_h = g->test_h > 0 ? g->test_h : 1170;
  const char *shots_dir = NULL;
  int interp_test = 0, patch_test = 0;
  int check = 0, cpu_test = 0, jni_test = 0, fmt_test = 0, init_test = 0, windowed = 0, pvrtc_check = 0,
      audio_check = 0, expect_change = 0, sfx_check = 0, expect_pillarbox = 0, unpacked = 0;
  int after_script = -1, script_test = 0;
#ifdef DL_IOS
  ios_shell_args(&argc, &argv);
#endif
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      usage(prog);
      return 0;
    }
    if (!strcmp(argv[i], "--apk") && i + 1 < argc) apk = argv[++i];
    else if (!strcmp(argv[i], "--work") && i + 1 < argc) work = argv[++i];
    else if (!strcmp(argv[i], "--unpacked")) unpacked = 1;
    else if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
    else if (!strcmp(argv[i], "--report") && i + 1 < argc) report_path = argv[++i];
    else if (!strcmp(argv[i], "--check")) check = 1;
    else if (!strcmp(argv[i], "--cpu-test")) cpu_test = 1;
    else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) {
      if (cpu_select_backend(argv[++i]) != 0) {
        usage(prog);
        return 2;
      }
    } else if (!strcmp(argv[i], "--interp-test")) interp_test = 1;
    else if (!strcmp(argv[i], "--jni-test")) jni_test = 1;
    else if (!strcmp(argv[i], "--fmt-test")) fmt_test = 1;
    else if (!strcmp(argv[i], "--patch-test")) patch_test = 1;
    else if (!strcmp(argv[i], "--script-test")) script_test = 1;
    else if (!strcmp(argv[i], "--init-test")) init_test = 1;
    else if (!strcmp(argv[i], "--angle") && i + 1 < argc) angle_dir = argv[++i];
    else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot_path = argv[++i];
    else if (!strcmp(argv[i], "--window")) windowed = 1;
    else if (!strcmp(argv[i], "--pvrtc-check")) pvrtc_check = 1;
    else if (!strcmp(argv[i], "--audio-check")) audio_check = 1;
    else if (!strcmp(argv[i], "--touch") && i + 1 < argc) {
      if (input_script_add(argv[++i]) != 0) return 2;
    } else if (!strcmp(argv[i], "--frames-after-script") && i + 1 < argc) {
      after_script = atoi(argv[++i]);
      if (after_script < 0) {
        usage(prog);
        return 2;
      }
    } else if (!strcmp(argv[i], "--expect-change")) expect_change = 1;
    else if (!strcmp(argv[i], "--sfx-check")) sfx_check = 1;
    else if (!strcmp(argv[i], "--expect-pillarbox")) expect_pillarbox = 1;
    else if (!strcmp(argv[i], "--verbose")) android_asset_set_verbose(1);
    else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
      if (sscanf(argv[++i], "%dx%d", &size_w, &size_h) != 2 || size_w <= 0 || size_h <= 0) {
        usage(prog);
        return 2;
      }
    } else if (!strcmp(argv[i], "--shots-every") && i + 1 < argc) {
      char *colon;
      shots_every = (int)strtol(argv[++i], &colon, 10);
      if (*colon != ':' || shots_every <= 0) {
        usage(prog);
        return 2;
      }
      shots_dir = colon + 1;
    }
    else {
      usage(prog);
      return 2;
    }
  }
  if (!apk && !unpacked) {
    usage(prog);
    return 2;
  }
  if (!data && !(data = dl_storage_dir(g->storage))) return 1;
  if (dl_storage_prepare(data) != 0) return 1;

  /* --unpacked: --work already holds the APK's libraries and assets (the
   * iOS app bundle, filled at build time). */
  if (apk && (apk_unpack_libs(apk, work) != 0 || apk_unpack_assets(apk, work) != 0)) return 1;
  char asset_dir[1024];
  snprintf(asset_dir, sizeof(asset_dir), "%s/" APK_ASSET_DIR, work);
  android_asset_set_root(asset_dir);
  if (gm_init() != 0 || dynlib_init() != 0 || jni_init() != 0) return 1;
  if (g->jni_classes) jni_add_known_classes(g->jni_classes);
  if (g->init(data) != 0) return 1;
  stdio_set_data_root(data);

  so_module **loaded = calloc((size_t)n_libs, sizeof *loaded);
  for (int i = 0; i < n_libs; i++) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/" APK_LIB_DIR "/%s", work, g->libs[i]);
    if (so_load(&modules[i], path) != 0) return 1;
    loaded[i] = &modules[i];
  }

  FILE *report = NULL;
  if (report_path) {
    report = fopen(report_path, "w");
    if (!report) {
      perror(report_path);
      return 1;
    }
    fprintf(report, "# importer\tsymbol\tbound to\taddress\n");
  }

  int ok = 1;
  for (int i = 0; i < n_libs; i++) {
    if (so_relocate(&modules[i], loaded, n_libs, report) != 0) ok = 0;
    print_summary(&modules[i]);
    ok &= all_relocs_applied(&modules[i]);
  }
  free(loaded);
  if (report) fclose(report);
  /* The game's own bugs, fixed in guest memory before any guest code runs.
   * The APK stays as it is. */
  if (game_patches_enabled())
    for (int i = 0; i < n_libs; i++)
      game_patches_apply(modules[i].soname, g2h(modules[i].base), modules[i].size);
  else
    printf("[patch] DL_GAME_PATCHES=off: the game runs unpatched\n");

  if (dynlib_unimplemented_calls() != 0) ok = 0;
  printf("M1 %s\n", ok ? "OK: all libraries loaded, relocated and bound" : "FAILED");

  const so_module *main_lib = &modules[n_libs - 1];
  if (ok && cpu_test) {
    cpu *c = cpu_create();
    /* The GL shims need a context. */
    if (gfx_init_headless(angle_dir, 16, 16) != 0 || gles_bind() != 0) ok = 0;
    if (!ok || cpu_setup(c, 1u << 20) != 0 || cpu_selftest(c, main_lib) != 0) ok = 0;
    if (dynlib_unimplemented_calls() != 0) ok = 0;
    c->ops->destroy(c);
    printf("CPU %s\n", ok ? "OK: guest code ran under the backend and matched the host" : "FAILED");
  }

  if (ok && interp_test) {
    if (interp_selftest(modules, n_libs, g->fn_tests) != 0) ok = 0;
    printf("INTERP %s\n", ok ? "OK: the interpreter matched dynarmic" : "FAILED");
  }

  if (ok && fmt_test) {
    cpu *c = cpu_create();
    if (cpu_setup(c, 1u << 20) != 0 || fmt_selftest(c) != 0) ok = 0;
    if (dynlib_unimplemented_calls() != 0) ok = 0;
    c->ops->destroy(c);
    printf("FMT %s\n", ok ? "OK: the printf and scanf shims matched the host libc" : "FAILED");
  }

  if (ok && patch_test) {
    if (with_cpu(patch_checks) != 0) ok = 0;
    printf("PATCH %s\n", ok ? "OK: the load-time game patches are in place and work" : "FAILED");
  }
  if (ok && script_test) {
    if (input_script_selftest() != 0) ok = 0;
    printf("SCRIPT %s\n", ok ? "OK: scripted touches ran at their frames and waits" : "FAILED");
  }

  if (ok && jni_test) {
    if (with_cpu(jni_checks) != 0) ok = 0;
    printf("JNI %s\n", ok ? "OK: Java entry points returned through the fake JNIEnv" : "FAILED");
  }
  if (ok && init_test) {
    cpu *c = cpu_create();
    if (cpu_setup(c, 1u << 20) != 0) ok = 0;
    /* System.loadLibrary order: each library's constructors, then its
     * JNI_OnLoad. */
    for (int i = 0; ok && i < n_libs; i++)
      if (so_run_init_array(c, &modules[i]) != 0 || so_run_jni_onload(c, &modules[i]) != 0) ok = 0;
    if (ok && g->start && g->start(c, main_lib) != 0) ok = 0;
    if (ok && g->init_test && g->init_test(c, main_lib) != 0) ok = 0;
    /* Headless: the profile's test size (--size overrides it). A window
     * reports its real drawable size. */
    int w = size_w, h = size_h;
    int win_rc = ok && windowed ? gfx_init_window(angle_dir, g->name, &w, &h) : 0;
    if (win_rc == -2) {
      /* No window server (e.g. a non-GUI session): ctest's skip code. */
      fflush(stdout);
      _exit(77);
    }
    if (ok && (win_rc != 0 || (!windowed && gfx_init_headless(angle_dir, w, h) != 0) ||
               gles_bind() != 0))
      ok = 0;
    if (ok && pvrtc_check) gles_pvrtc_keep();
    /* A surface wider than the game's widest aspect ratio gets the game
     * centred with black bars. The game also stays out of a notch or camera
     * cutout at the sides (the iOS safe area). */
    int safe_x = 0, safe_w = w;
    if (ok && windowed) gfx_safe_columns(&safe_x, &safe_w);
    int game_w = safe_w;
    if (g->max_aspect_w > 0 && g->max_aspect_h > 0 &&
        (long)safe_w * g->max_aspect_h > (long)h * g->max_aspect_w)
      game_w = h * g->max_aspect_w / g->max_aspect_h;
    int game_x = safe_x + (safe_w - game_w) / 2;
    if (ok && game_w < w) {
      gles_set_content(game_x, 0, game_w, h);
      input_set_content(game_x, 0, game_w, h);
      printf("[gfx] pillarbox: game %dx%d at x %d in %dx%d (safe columns %d-%d)\n", game_w, h,
             game_x, w, h, safe_x, safe_x + safe_w);
    }
    if (ok && g->surface && g->surface(c, main_lib, (uint32_t)game_w, (uint32_t)h) != 0) ok = 0;
    /* From here on, leaving the app (iOS) pauses and saves the game. */
    if (ok) lifecycle_attach(c);
    if (dynlib_unimplemented_calls() != 0 || jni_unimplemented_calls() != 0) ok = 0;
    int rendered = 0;
    uint8_t *before_touch = NULL;
    audio_stats at_last_touch = {0};
    /* Scripted touches due in the frame about to render (input.h). */
    int due = input_script_plan(0);
    if (expect_change && due) {
      fprintf(stderr, "frames: --expect-change needs a --touch after frame 0\n");
      ok = 0;
    }
    /* Frame times: the game's own work in the frame hook (budget 16.7 ms at
     * 60 fps), and the wall clock including swap and pacing. */
    double loop_start = now_ms(), work_ms = 0, slowest_ms = 0;
    int over_budget = 0;
    /* The same per 600 frames (10 s at 60 fps), to tell loading from play. */
    enum { WINDOW = 600 };
    double win_start = loop_start, win_work = 0, win_slowest = 0;
    int win_over = 0;
    /* A window without --frames runs until it is closed. */
    while (ok && !script_over(after_script, rendered) &&
           (rendered < frames || (windowed && !frames))) {
      if (sfx_check && due) audio_get_stats(&at_last_touch);
      input_script_frame(rendered, gfx_emit_touch);
      if (!gfx_poll_events()) break;
      gles_begin_frame();
      double t0 = now_ms();
      int frame_rc = g->frame(c, main_lib);
      double t = now_ms() - t0;
      work_ms += t;
      if (t > slowest_ms) slowest_ms = t;
      if (t > 1000.0 / 60) over_budget++;
      win_work += t;
      if (t > win_slowest) win_slowest = t;
      if (t > 250) printf("[frame] frame %d took %.0f ms\n", rendered, t);
      if (t > 1000.0 / 60) win_over++;
      if ((rendered + 1) % WINDOW == 0) {
        double now = now_ms();
        printf("[frame] %d-%d: %.1f fps; frame avg %.1f ms, slowest %.0f ms, %d over 16.7 ms\n",
               rendered + 1 - WINDOW, rendered, WINDOW * 1e3 / (now - win_start), win_work / WINDOW,
               win_slowest, win_over);
        win_start = now, win_work = 0, win_slowest = 0, win_over = 0;
      }
      if (frame_rc != 0) ok = 0;
      /* The next frame's touches, from the signals so far; --expect-change
       * keeps the frame before the first one. */
      int touched = input_script_last_touch() >= 0;
      due = input_script_plan(rendered + 1);
      if (ok && expect_change && !touched && due && !(before_touch = gfx_read_rgba())) ok = 0;
      if (ok && shots_dir && rendered % shots_every == 0 && !save_numbered_shot(shots_dir, rendered))
        ok = 0;
      if (!ok) break;
      /* The screenshot, --expect-change and --expect-pillarbox read the back
       * buffer, so their frame isn't swapped yet. */
      rendered++;
      int last = rendered >= frames || script_over(after_script, rendered);
      if (!last || !(shot_path || expect_change || expect_pillarbox)) {
        gfx_swap();
        gfx_pace_frame();
      }
    }
    /* Waits that never came: the game didn't reach the state they target. */
    if (input_script_report_pending() != 0 && after_script >= 0) {
      fprintf(stderr, "frames: the scripted touches didn't finish within %d frames\n", frames);
      ok = 0;
    }
    if (expect_change && !before_touch) {
      fprintf(stderr, "frames: --expect-change, but no scripted touch ran\n");
      ok = 0;
    }
    if (sfx_check && input_script_last_touch() < 0) {
      fprintf(stderr, "frames: --sfx-check, but no scripted touch ran\n");
      ok = 0;
    }
    if (rendered > 0)
      printf("[frame] %d frames in %.1f s (%.1f fps); frame avg %.1f ms, slowest %.0f ms, "
             "%d over 16.7 ms\n",
             rendered, (now_ms() - loop_start) / 1e3, rendered * 1e3 / (now_ms() - loop_start),
             work_ms / rendered, slowest_ms, over_budget);
    if (dynlib_unimplemented_calls() != 0 || jni_unimplemented_calls() != 0) ok = 0;
    if (gles_upload_failures() != 0) {
      fprintf(stderr, "frames: %d texture uploads failed\n", gles_upload_failures());
      ok = 0;
    }
    if (ok && (frames > 0 || windowed)) {
      /* At least 1% of the frame must be drawn over the background. */
      long drawn = shot_path ? gfx_screenshot_png(shot_path) : 0;
      if (shot_path && drawn < (long)w * h / 100) {
        fprintf(stderr, "frames: screenshot %s is %s\n", shot_path,
                drawn < 0 ? "unwritable" : "blank (background only)");
        ok = 0;
      }
      if (ok && expect_pillarbox) {
        uint8_t *last = game_w < w ? gfx_read_rgba() : NULL;
        if (game_w == w) fprintf(stderr, "frames: --expect-pillarbox, but the surface isn't wider than the game\n");
        if (game_w < w && !gles_viewport_in_content())
          fprintf(stderr, "frames: pillarbox wrong: the viewport leaves the game's rectangle\n");
        if (!last || !pillarbox_ok(last, w, h, game_w) || !gles_viewport_in_content()) ok = 0;
        else printf("PILLARBOX OK: black bars beside a %dx%d game\n", game_w, h);
        free(last);
      }
      /* Drawn pixels are counted only in a screenshot. */
      if (ok && shot_path)
        printf("FRAMES OK: %d frames rendered, screenshot %s (%ld drawn pixels)\n", rendered,
               shot_path, drawn);
      else if (ok)
        printf("FRAMES OK: %d frames rendered\n", rendered);
      if (ok && expect_change) {
        uint8_t *last = gfx_read_rgba();
        double share = before_touch && last ? frame_change(before_touch, last, (long)w * h) : 0;
        if (share < CHANGE_MIN_SHARE) {
          fprintf(stderr, "frames: %.1f%% of the screen changed after the touches (need %.0f%%)\n",
                  share * 100, CHANGE_MIN_SHARE * 100);
          ok = 0;
        } else {
          printf("TOUCH OK: %d touches delivered, %.1f%% of the screen changed\n",
                 input_delivered(), share * 100);
        }
        free(last);
      }
      if (windowed && (shot_path || expect_change)) gfx_swap();
    }
    free(before_touch);
    if (ok && pvrtc_check) {
      /* The GPU is the reference; the software decoder may round differently. */
      int max_diff;
      double mean_diff;
      if (gles_pvrtc_check(&max_diff, &mean_diff) != 0 || max_diff > PVRTC_MAX_DIFF ||
          mean_diff > PVRTC_MAX_MEAN_DIFF) {
        fprintf(stderr, "pvrtc: software decode doesn't match the GPU\n");
        ok = 0;
      } else {
        printf("PVRTC OK: software decode matches the GPU (max diff %d, mean %.3f)\n", max_diff,
               mean_diff);
      }
      /* The uncompressed formats too, GL_LUMINANCE(_ALPHA) included. */
      if (ok && gles_format_check() != 0) ok = 0;
      else if (ok) printf("FORMATS OK: RGB, RGBA, RGBA4444, LUMINANCE, LUMINANCE_ALPHA\n");
    }
    if (ok && audio_check) {
      audio_stats st;
      audio_get_stats(&st);
      if (st.samples == 0) {
        fprintf(stderr, "audio: nothing reached the output\n");
        ok = 0;
      } else if (st.nonsilent * 4 < st.samples) {
        fprintf(stderr, "audio: only %llu of %llu output samples non-silent\n",
                (unsigned long long)st.nonsilent, (unsigned long long)st.samples);
        ok = 0;
      } else {
        printf("AUDIO OK: %llu of %llu output samples non-silent (%s), %llu underruns\n",
               (unsigned long long)st.nonsilent, (unsigned long long)st.samples,
               st.device ? "SDL device" : "virtual clock", (unsigned long long)st.underruns);
      }
    }
    if (ok && sfx_check) {
      audio_stats st;
      audio_get_stats(&st);
      uint64_t loud = st.nonsilent - at_last_touch.nonsilent;
      if (loud < SFX_MIN_NONSILENT) {
        fprintf(stderr, "audio: only %llu non-silent samples after the last touch (need %d)\n",
                (unsigned long long)loud, SFX_MIN_NONSILENT);
        ok = 0;
      } else {
        printf("SFX OK: %llu non-silent samples after the last touch\n", (unsigned long long)loud);
      }
    }
    if (rendered > 0 && dl_app_stop(c) != 0) ok = 0;
    c->ops->destroy(c);
    printf("INIT %s\n", ok ? "OK: native startup completed" : "FAILED");
  }
  cpu_backend_report();
  int rc = (check || cpu_test || interp_test || jni_test || fmt_test || patch_test || script_test ||
            init_test) && !ok
               ? 1
               : 0;
  const char *mem_stats = getenv("DL_MEM_STATS");
#ifdef DL_IOS
  mem_stats = "1";
#endif
  if (mem_stats && *mem_stats == '1') guest_heap_stats();
#ifdef DL_IOS
  /* The exit below skips atexit: copy the rest of the log out now. */
  ios_shell_exit();
#endif
  /* Guest threads that never end (audio mixers, the game's workers) are
   * still running on guest memory and the JIT, so neither guest memory nor
   * the C++ statics may be torn down under them: leave without exit()'s
   * cleanup. */
  if (thread_live_count() != 0) {
    fflush(stdout);
    fflush(stderr);
    _exit(rc);
  }
  gm_free();
  return rc;
}
