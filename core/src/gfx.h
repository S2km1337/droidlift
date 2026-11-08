/*
 * Host graphics: an OpenGL ES 2.0 context from ANGLE's Metal backend.
 *
 * ANGLE's libEGL and libGLESv2 are loaded with dlopen from a directory (the
 * output of scripts/fetch-angle.sh by default), so the loader builds without
 * them. Headless mode renders into an EGL pbuffer of the game's size; window
 * mode renders into an SDL3 window's CAMetalLayer.
 *
 * The game's GL imports are forwarded to the functions gfx_gl_proc finds
 * (reimpl/gles.c).
 */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

#include "input.h"

/* Default ANGLE build directory, relative to the repo root. */
#define GFX_DEFAULT_ANGLE_DIR "third_party/angle/out/mac-metal"

/* Loads ANGLE from dir and makes a GLES 2.0 context with an offscreen
 * width x height RGBA8/D24S8 surface current on the calling thread.
 * Returns 0, or -1 with the reason on stderr. */
int gfx_init_headless(const char *angle_dir, int width, int height);

/* Like gfx_init_headless, but renders into a new SDL3 window of
 * width x height pixels (half that in points on a Retina screen). *width and
 * *height are updated to the drawable size ANGLE reports. Returns 0, -2 if
 * no window can be opened (no window server), or -1 if ANGLE can't render
 * into it, with the reason on stderr. */
int gfx_init_window(const char *angle_dir, const char *title, int *width, int *height);

/* The columns of the surface (in pixels, *x the first, *w how many) that the
 * window's safe area leaves clear of a notch or camera cutout at the sides
 * (iOS, in landscape). The whole surface when there is none, and headless.
 * The home indicator's strip at the bottom isn't left out: the game draws
 * under it, as full-screen games do. */
void gfx_safe_columns(int *x, int *w);

/* Window mode: handles pending window events, queueing the left mouse
 * button as touch 0 (input.h) in surface pixels, and a touch screen's
 * fingers as touches 0..9. While the game is paused (lifecycle.h) it waits
 * for events and doesn't return. Returns 0 once the window was closed (or
 * Escape pressed), else 1. Headless: always 1. */
int gfx_poll_events(void);

/* A scripted touch at surface pixel (x, y). Window mode posts it as a mouse
 * event in points, so it takes the real mouse's path through
 * gfx_poll_events; headless queues it directly. */
void gfx_emit_touch(touch_kind kind, float x, float y);

/* Sleeps until the next 1/60 s frame boundary (the original's 60 fps cap).
 * Headless too: an uncapped render loop starves the game's loading threads,
 * so the game never gets past its loading screen. */
void gfx_pace_frame(void);

/* Host GL entry point (ANGLE's libGLESv2 export, else eglGetProcAddress), or
 * NULL. Valid after gfx_init_*. */
void *gfx_gl_proc(const char *name);

/* Ends a frame (eglSwapBuffers). */
void gfx_swap(void);

/* Reads the current surface's colour buffer and writes it as a PNG, top row
 * first. Returns the number of drawn pixels (RGB differs from the top-left
 * pixel, taken as the background), or -1. A cleared-only frame has none. */
long gfx_screenshot_png(const char *path);

/* The current surface's colour buffer as RGBA8, GL's row order (bottom row
 * first), in a malloc'ed buffer the caller frees; NULL on failure. */
uint8_t *gfx_read_rgba(void);

void gfx_shutdown(void);

#endif
