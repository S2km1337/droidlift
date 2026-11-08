#include "gfx.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "input.h"
#include "droidlift.h"

/* The few EGL types and names used here, so the loader builds without
 * ANGLE's headers. Values from the Khronos and ANGLE registries. */
typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int32_t EGLint;
typedef unsigned EGLBoolean, EGLenum;

#define EGL_NONE 0x3038
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_WINDOW_BIT 0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489

#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401

static struct {
  void *(*GetProcAddress)(const char *);
  EGLint (*GetError)(void);
  EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
  EGLBoolean (*BindAPI)(EGLenum);
  EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
  EGLSurface (*CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
  EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *);
  EGLBoolean (*QuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
  EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
  EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
  EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface);
  EGLBoolean (*Terminate)(EGLDisplay);
} egl;

static void *lib_egl, *lib_gles;
static EGLDisplay display;
static EGLSurface surface;
static EGLContext context;
static int surf_w, surf_h;
static SDL_Window *window;
static SDL_MetalView metal_view;
static uint64_t next_frame_ns;

/* dir/NAME.dylib (the Mac build), else dir/NAME.framework/NAME (the iOS
 * build: ANGLE makes frameworks there, kept in the app's
 * Frameworks folder). */
static void *open_angle_lib(const char *dir, const char *name) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s.dylib", dir, name);
  if (access(path, F_OK) != 0) snprintf(path, sizeof path, "%s/%s.framework/%s", dir, name, name);
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static int load_angle(const char *dir) {
  /* libGLESv2 first, so libEGL binds to this copy. */
  if (!(lib_gles = open_angle_lib(dir, "libGLESv2"))) goto fail;
  if (!(lib_egl = open_angle_lib(dir, "libEGL"))) goto fail;
#define SYM(field, name) \
  if (!(*(void **)&egl.field = dlsym(lib_egl, name))) goto fail;
  SYM(GetProcAddress, "eglGetProcAddress")
  SYM(GetError, "eglGetError")
  SYM(Initialize, "eglInitialize")
  SYM(BindAPI, "eglBindAPI")
  SYM(ChooseConfig, "eglChooseConfig")
  SYM(CreatePbufferSurface, "eglCreatePbufferSurface")
  SYM(CreateWindowSurface, "eglCreateWindowSurface")
  SYM(QuerySurface, "eglQuerySurface")
  SYM(CreateContext, "eglCreateContext")
  SYM(MakeCurrent, "eglMakeCurrent")
  SYM(SwapBuffers, "eglSwapBuffers")
  SYM(Terminate, "eglTerminate")
#undef SYM
  return 0;
fail:
  fprintf(stderr, "[gfx] can't load ANGLE from %s: %s (run scripts/fetch-angle.sh)\n", dir,
          dlerror());
  return -1;
}

/* Loads ANGLE, opens its Metal display and picks what a typical
 * GLSurfaceView asks for: RGBA8 with depth and stencil. */
static int open_display(const char *angle_dir, EGLint surface_type, EGLConfig *config) {
  if (load_angle(angle_dir ? angle_dir : GFX_DEFAULT_ANGLE_DIR) != 0) return -1;
  EGLDisplay (*get_platform_display)(EGLenum, void *, const EGLint *) =
      (EGLDisplay(*)(EGLenum, void *, const EGLint *))egl.GetProcAddress("eglGetPlatformDisplayEXT");
  const EGLint display_attrs[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE,
                                  EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE};
  if (!get_platform_display ||
      !(display = get_platform_display(EGL_PLATFORM_ANGLE_ANGLE, NULL, display_attrs)) ||
      !egl.Initialize(display, NULL, NULL) || !egl.BindAPI(EGL_OPENGL_ES_API)) {
    fprintf(stderr, "[gfx] no ANGLE Metal display (EGL error 0x%x)\n", egl.GetError());
    return -1;
  }
  const EGLint config_attrs[] = {EGL_RED_SIZE,     8, EGL_GREEN_SIZE,      8,
                                 EGL_BLUE_SIZE,    8, EGL_ALPHA_SIZE,      8,
                                 EGL_DEPTH_SIZE,  24, EGL_STENCIL_SIZE,    8,
                                 EGL_SURFACE_TYPE, surface_type,
                                 EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
  EGLint n = 0;
  if (!egl.ChooseConfig(display, config_attrs, config, 1, &n) || n < 1) {
    fprintf(stderr, "[gfx] no RGBA8/D24S8 config (EGL error 0x%x)\n", egl.GetError());
    return -1;
  }
  return 0;
}

static int make_current(EGLConfig config, int width, int height) {
  const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  if (!surface || !(context = egl.CreateContext(display, config, NULL, context_attrs)) ||
      !egl.MakeCurrent(display, surface, surface, context)) {
    fprintf(stderr, "[gfx] can't make a %dx%d GLES 2 context (EGL error 0x%x)\n", width, height,
            egl.GetError());
    return -1;
  }
  surf_w = width;
  surf_h = height;
  return 0;
}

int gfx_init_headless(const char *angle_dir, int width, int height) {
  EGLConfig config;
  if (open_display(angle_dir, EGL_PBUFFER_BIT, &config) != 0) return -1;
  const EGLint surface_attrs[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
  surface = egl.CreatePbufferSurface(display, config, surface_attrs);
  if (make_current(config, width, height) != 0) return -1;
  printf("[gfx] ANGLE Metal, headless %dx%d\n", width, height);
  return 0;
}

int gfx_init_window(const char *angle_dir, const char *title, int *width, int *height) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "[gfx] SDL video init failed: %s\n", SDL_GetError());
    return -2;
  }
#ifdef SDL_PLATFORM_IOS
  /* iOS: the whole screen, landscape (wider than tall), at the
   * screen's native pixel density (3x on the iPhone 14). */
  window = SDL_CreateWindow(title ? title : "droidlift", *width, *height,
                            SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS);
  float scale = window ? SDL_GetWindowPixelDensity(window) : 0;
#else
  /* Ask for the size in pixels: SDL sizes windows in points, and a Retina
   * screen has two pixels per point. */
  window = SDL_CreateWindow(title ? title : "droidlift", *width, *height,
                            SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  float scale = window ? SDL_GetWindowPixelDensity(window) : 0;
  if (window && scale > 1)
    SDL_SetWindowSize(window, (int)(*width / scale), (int)(*height / scale));
#endif
  if (!window || !(metal_view = SDL_Metal_CreateView(window))) {
    fprintf(stderr, "[gfx] can't open a Metal window: %s\n", SDL_GetError());
    return -2;
  }
  EGLConfig config;
  if (open_display(angle_dir, EGL_WINDOW_BIT, &config) != 0) return -1;
  /* ANGLE takes the CAMetalLayer as the native window and renders into it. */
  surface = egl.CreateWindowSurface(display, config, SDL_Metal_GetLayer(metal_view), NULL);
  EGLint w = 0, h = 0;
  if (surface) {
    egl.QuerySurface(display, surface, EGL_WIDTH, &w);
    egl.QuerySurface(display, surface, EGL_HEIGHT, &h);
  }
  if (make_current(config, w, h) != 0) return -1;
  SDL_ShowWindow(window);
  *width = w;
  *height = h;
  printf("[gfx] ANGLE Metal, window %dx%d pixels (%.0fx density)\n", w, h, scale);
  SDL_Rect safe;
  if (SDL_GetWindowSafeArea(window, &safe))
    printf("[gfx] safe area x %d-%d, y %d-%d pixels\n", (int)(safe.x * scale),
           (int)((safe.x + safe.w) * scale), (int)(safe.y * scale), (int)((safe.y + safe.h) * scale));
  return 0;
}

void gfx_safe_columns(int *x, int *w) {
  SDL_Rect safe;
  *x = 0;
  *w = surf_w;
  if (!window || !SDL_GetWindowSafeArea(window, &safe)) return;
  float s = SDL_GetWindowPixelDensity(window);
  int x0 = (int)(safe.x * s + 0.5f), x1 = (int)((safe.x + safe.w) * s + 0.5f);
  if (x0 < 0) x0 = 0;
  if (x1 > surf_w) x1 = surf_w;
  if (x1 - x0 < surf_w / 2) return; /* not believable: use the whole surface */
  *x = x0;
  *w = x1 - x0;
}

/* Fingers on a touch screen (iOS) become the game's touch ids 0..9, in the
 * order they land: Android's pointer ids work the same way. */
#define MAX_FINGERS 10
static SDL_FingerID fingers[MAX_FINGERS];

static int finger_slot(SDL_FingerID id, int add) {
  for (int i = 0; i < MAX_FINGERS; i++)
    if (fingers[i] == id) return i;
  if (add)
    for (int i = 0; i < MAX_FINGERS; i++)
      if (!fingers[i]) {
        fingers[i] = id;
        return i;
      }
  return -1;
}

static void finger_event(const SDL_TouchFingerEvent *f, Uint32 type) {
  /* Only a screen's fingers: a Mac trackpad's touches move the pointer. */
  if (SDL_GetTouchDeviceType(f->touchID) != SDL_TOUCH_DEVICE_DIRECT) return;
  int slot = finger_slot(f->fingerID, type == SDL_EVENT_FINGER_DOWN);
  if (slot < 0) return;
  touch_kind kind = type == SDL_EVENT_FINGER_DOWN     ? TOUCH_DOWN
                    : type == SDL_EVENT_FINGER_MOTION ? TOUCH_MOVE
                    : type == SDL_EVENT_FINGER_UP     ? TOUCH_UP
                                                      : TOUCH_CANCEL;
  /* Finger positions are 0..1 across the window; the surface is in pixels. */
  input_touch(kind, slot, f->x * surf_w, f->y * surf_h);
  if (kind == TOUCH_UP || kind == TOUCH_CANCEL) fingers[slot] = 0;
}

int gfx_poll_events(void) {
  if (!window) return 1;
  /* Mouse positions are in points; the game's surface is in pixels. */
  float scale = SDL_GetWindowPixelDensity(window);
  SDL_Event e;
  for (;;) {
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return 0;
      if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) return 0;
      if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_MOTION ||
          e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) {
        finger_event(&e.tfinger, e.type);
        continue;
      }
      /* A mouse made up from a finger: the finger itself counts (above). */
      if (((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
           e.button.which == SDL_TOUCH_MOUSEID) ||
          (e.type == SDL_EVENT_MOUSE_MOTION && e.motion.which == SDL_TOUCH_MOUSEID))
        continue;
      /* The left button is one finger, touch 0. */
      if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
          e.button.button == SDL_BUTTON_LEFT)
        input_touch(e.button.down ? TOUCH_DOWN : TOUCH_UP, 0, e.button.x * scale,
                    e.button.y * scale);
      else if (e.type == SDL_EVENT_MOUSE_MOTION && (e.motion.state & SDL_BUTTON_LMASK))
        input_touch(TOUCH_MOVE, 0, e.motion.x * scale, e.motion.y * scale);
    }
    /* Paused (iOS: inactive or in the background, lifecycle.h): no frames,
     * since iOS doesn't allow GPU work in the background. Wait for events,
     * which also lets the lifecycle calls run. */
    if (!dl_app_is_paused()) return 1;
    SDL_WaitEventTimeout(NULL, 100);
  }
}

void gfx_emit_touch(touch_kind kind, float x, float y) {
  if (!window) {
    input_touch(kind, 0, x, y);
    return;
  }
  float scale = SDL_GetWindowPixelDensity(window);
  SDL_Event e;
  SDL_zero(e);
  if (kind == TOUCH_MOVE) {
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = SDL_GetWindowID(window);
    e.motion.state = SDL_BUTTON_LMASK;
    e.motion.x = x / scale;
    e.motion.y = y / scale;
  } else {
    /* The window has no cancel; a cancel ends the touch like a release. */
    e.type = kind == TOUCH_DOWN ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = SDL_GetWindowID(window);
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = kind == TOUCH_DOWN;
    e.button.clicks = 1;
    e.button.x = x / scale;
    e.button.y = y / scale;
  }
  SDL_PushEvent(&e);
}

void gfx_pace_frame(void) {
  const uint64_t frame_ns = 1000000000ull / 60;
  uint64_t now = SDL_GetTicksNS();
  /* After a long frame, restart the schedule instead of rushing to catch up. */
  if (next_frame_ns < now || next_frame_ns > now + frame_ns) next_frame_ns = now;
  else SDL_DelayPrecise(next_frame_ns - now);
  next_frame_ns += frame_ns;
}

void *gfx_gl_proc(const char *name) {
  void *p = lib_gles ? dlsym(lib_gles, name) : NULL;
  return p ? p : (egl.GetProcAddress ? egl.GetProcAddress(name) : NULL);
}

void gfx_swap(void) {
  if (display) egl.SwapBuffers(display, surface);
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
  uint8_t head[8];
  put_be32(head, len);
  memcpy(head + 4, type, 4);
  uLong crc = crc32(crc32(0, NULL, 0), head + 4, 4);
  if (len) crc = crc32(crc, data, len);
  uint8_t tail[4];
  put_be32(tail, (uint32_t)crc);
  fwrite(head, 1, 8, f);
  if (len) fwrite(data, 1, len, f);
  fwrite(tail, 1, 4, f);
}

long gfx_screenshot_png(const char *path) {
  void (*read_pixels)(int, int, int, int, unsigned, unsigned, void *) =
      (void (*)(int, int, int, int, unsigned, unsigned, void *))gfx_gl_proc("glReadPixels");
  if (!read_pixels || surf_w <= 0) return -1;
  size_t row = (size_t)surf_w * 4;
  uint8_t *px = malloc(row * (size_t)surf_h);
  /* PNG rows: a filter byte (0) then RGBA, top row first; GL's row 0 is the bottom. */
  size_t raw_len = (row + 1) * (size_t)surf_h;
  uint8_t *raw = malloc(raw_len);
  uLongf z_len = compressBound(raw_len);
  uint8_t *z = malloc(z_len);
  long lit = -1;
  FILE *f = NULL;
  if (!px || !raw || !z) goto out;
  read_pixels(0, 0, surf_w, surf_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  lit = 0;
  /* GL's top-left pixel is the start of its top row. */
  const uint8_t *bg = px + row * (size_t)(surf_h - 1);
  for (int y = 0; y < surf_h; y++) {
    uint8_t *src = px + row * (size_t)(surf_h - 1 - y), *dst = raw + (row + 1) * (size_t)y;
    dst[0] = 0;
    memcpy(dst + 1, src, row);
    for (int x = 0; x < surf_w; x++)
      if (memcmp(src + 4 * x, bg, 3) != 0) lit++;
  }
  if (compress2(z, &z_len, raw, raw_len, 6) != Z_OK || !(f = fopen(path, "wb"))) {
    lit = -1;
    goto out;
  }
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  uint8_t ihdr[13];
  put_be32(ihdr, (uint32_t)surf_w);
  put_be32(ihdr + 4, (uint32_t)surf_h);
  ihdr[8] = 8; /* bit depth */
  ihdr[9] = 6; /* RGBA */
  ihdr[10] = ihdr[11] = ihdr[12] = 0;
  fwrite(sig, 1, 8, f);
  png_chunk(f, "IHDR", ihdr, 13);
  png_chunk(f, "IDAT", z, (uint32_t)z_len);
  png_chunk(f, "IEND", NULL, 0);
out:
  if (f) fclose(f);
  free(px);
  free(raw);
  free(z);
  return lit;
}

uint8_t *gfx_read_rgba(void) {
  void (*read_pixels)(int, int, int, int, unsigned, unsigned, void *) =
      (void (*)(int, int, int, int, unsigned, unsigned, void *))gfx_gl_proc("glReadPixels");
  uint8_t *px = read_pixels && surf_w > 0 ? malloc((size_t)surf_w * surf_h * 4) : NULL;
  if (px) read_pixels(0, 0, surf_w, surf_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  return px;
}

void gfx_shutdown(void) {
  if (display) {
    egl.MakeCurrent(display, NULL, NULL, NULL);
    egl.Terminate(display);
  }
  display = surface = context = NULL;
  if (metal_view) SDL_Metal_DestroyView(metal_view);
  if (window) SDL_DestroyWindow(window);
  if (window) SDL_QuitSubSystem(SDL_INIT_VIDEO); /* audio may still be playing */
  metal_view = NULL;
  window = NULL;
}
