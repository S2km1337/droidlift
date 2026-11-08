/*
 * The game's GLES 2.0 imports, forwarded to ANGLE.
 *
 * Each handler reads the guest's AAPCS arguments, turns guest pointers into
 * host ones and calls the host function. Floats arrive as bit patterns in
 * core registers (armeabi-v7a is softfp). Two calls take a pointer that is a
 * byte offset when a buffer is bound (glVertexAttribPointer and
 * glDrawElements); the binding is asked of the host context, which tracks it
 * per vertex array object. glGetString answers are copied into guest memory
 * once per name.
 *
 * Every texture upload is followed by a host glGetError. A failed upload is
 * logged and counted (gles_upload_failures), and its error is kept for the
 * guest's next glGetError, so the check is invisible to the game.
 *
 * PVRTC 4bpp uploads are decoded to RGBA8 in software (pvrtc.c) when the
 * host lacks GL_IMG_texture_compression_pvrtc (the iOS Simulator), or when
 * DL_PVRTC_SOFTWARE=1; the extension is then still listed to the game.
 * gles_pvrtc_check compares that decoder with the GPU on the game's own
 * textures.
 *
 * Pillarbox (gles_set_content): the game draws into a rectangle of the
 * surface. The initial viewport is that rectangle, the game's viewports and
 * pixel reads are offset into it, and a scissor
 * test the game can't see keeps its clears inside (it never sets a scissor or
 * binds a framebuffer of its own); gles_begin_frame paints the rest black.
 *
 * gles_bind() resolves the host functions after gfx_init_*; until then a GL
 * call from the guest is a host crash, so every path that runs game code
 * creates the context first.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cpu.h"
#include "../dynlib.h"
#include "../gfx.h"
#include "../guest_mem.h"
#include "../pvrtc.h"
#include "reimpl.h"

typedef unsigned GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef long GLintptr, GLsizeiptr;

/* X(return type, name, parameter list) for every host function used. */
#define GL_FUNCS(X)                                                                          \
  X(void, glActiveTexture, (GLenum))                                                       \
  X(void, glAttachShader, (GLuint, GLuint))                                                \
  X(void, glBindAttribLocation, (GLuint, GLuint, const char *))                            \
  X(void, glBindBuffer, (GLenum, GLuint))                                                  \
  X(void, glBindTexture, (GLenum, GLuint))                                                 \
  X(void, glBlendEquationSeparate, (GLenum, GLenum))                                       \
  X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                           \
  X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum))                        \
  X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *))                   \
  X(void, glClear, (GLbitfield))                                                           \
  X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                              \
  X(void, glClearDepthf, (GLfloat))                                                        \
  X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                       \
  X(void, glCompileShader, (GLuint))                                                       \
  X(void, glCompressedTexImage2D,                                                          \
    (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *))               \
  X(GLuint, glCreateProgram, (void))                                                       \
  X(GLuint, glCreateShader, (GLenum))                                                      \
  X(void, glCullFace, (GLenum))                                                            \
  X(void, glDeleteBuffers, (GLsizei, const GLuint *))                                      \
  X(void, glDeleteProgram, (GLuint))                                                       \
  X(void, glDeleteShader, (GLuint))                                                        \
  X(void, glDeleteTextures, (GLsizei, const GLuint *))                                     \
  X(void, glDepthFunc, (GLenum))                                                           \
  X(void, glDepthMask, (GLboolean))                                                        \
  X(void, glDisable, (GLenum))                                                             \
  X(void, glDisableVertexAttribArray, (GLuint))                                            \
  X(void, glDrawArrays, (GLenum, GLint, GLsizei))                                          \
  X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *))                         \
  X(void, glEnable, (GLenum))                                                              \
  X(void, glEnableVertexAttribArray, (GLuint))                                             \
  X(void, glFinish, (void))                                                                \
  X(void, glGenBuffers, (GLsizei, GLuint *))                                               \
  X(void, glGenTextures, (GLsizei, GLuint *))                                              \
  X(void, glGenFramebuffers, (GLsizei, GLuint *))                                          \
  X(void, glBindFramebuffer, (GLenum, GLuint))                                             \
  X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                 \
  X(GLenum, glCheckFramebufferStatus, (GLenum))                                            \
  X(void, glDeleteFramebuffers, (GLsizei, const GLuint *))                                 \
  X(void, glGetActiveAttrib, (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *)) \
  X(void, glGetActiveUniform, (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *)) \
  X(GLenum, glGetError, (void))                                                            \
  X(void, glGetIntegerv, (GLenum, GLint *))                                                \
  X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *))                       \
  X(void, glGetProgramiv, (GLuint, GLenum, GLint *))                                       \
  X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, char *))                        \
  X(void, glGetShaderiv, (GLuint, GLenum, GLint *))                                        \
  X(const char *, glGetString, (GLenum))                                                   \
  X(GLint, glGetUniformLocation, (GLuint, const char *))                                   \
  X(void, glLinkProgram, (GLuint))                                                         \
  X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))          \
  X(void, glShaderSource, (GLuint, GLsizei, const char *const *, const GLint *))           \
  X(void, glStencilFuncSeparate, (GLenum, GLenum, GLint, GLuint))                          \
  X(void, glStencilMask, (GLuint))                                                         \
  X(void, glStencilOpSeparate, (GLenum, GLenum, GLenum, GLenum))                           \
  X(void, glTexImage2D,                                                                    \
    (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))         \
  X(void, glTexParameterf, (GLenum, GLenum, GLfloat))                                      \
  X(void, glTexParameteri, (GLenum, GLenum, GLint))                                        \
  X(void, glUniform1fv, (GLint, GLsizei, const GLfloat *))                                 \
  X(void, glUniform1i, (GLint, GLint))                                                     \
  X(void, glUniform2fv, (GLint, GLsizei, const GLfloat *))                                 \
  X(void, glUniform3fv, (GLint, GLsizei, const GLfloat *))                                 \
  X(void, glUniform4fv, (GLint, GLsizei, const GLfloat *))                                 \
  X(void, glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat *))                \
  X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *))                \
  X(void, glUseProgram, (GLuint))                                                          \
  X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
  X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                    \
  X(void, glScissor, (GLint, GLint, GLsizei, GLsizei))                                     \
  /* extensions the game fetches with eglGetProcAddress */                                 \
  X(void, glGenVertexArraysOES, (GLsizei, GLuint *))                                       \
  X(void, glBindVertexArrayOES, (GLuint))                                                  \
  X(void, glDeleteVertexArraysOES, (GLsizei, const GLuint *))                              \
  X(GLboolean, glIsVertexArrayOES, (GLuint))                                               \
  X(void, glInsertEventMarkerEXT, (GLsizei, const char *))                                 \
  X(void, glPushGroupMarkerEXT, (GLsizei, const char *))                                   \
  X(void, glPopGroupMarkerEXT, (void))                                                     \
  X(void, glLabelObjectEXT, (GLenum, GLuint, GLsizei, const char *))                       \
  X(void, glGetObjectLabelEXT, (GLenum, GLuint, GLsizei, GLsizei *, char *))

#define X(ret, name, params) ret(*name) params;
static struct {
  GL_FUNCS(X)
} gl;
#undef X

#define GL_EXTENSIONS 0x1F03
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_PVRTC_EXTENSION "GL_IMG_texture_compression_pvrtc"

static int host_pvrtc, pvrtc_software;

int gles_bind(void) {
  int missing = 0;
#define X(ret, name, params)                                       \
  if (!(*(void **)&gl.name = gfx_gl_proc(#name))) {                \
    fprintf(stderr, "[gles] host GL has no %s\n", #name);          \
    missing++;                                                     \
  }
  GL_FUNCS(X)
#undef X
  if (missing) return -1;
  /* The game picks its texture folders from these (iOS PVRTC, ETC1, ...). */
  const char *ext = gl.glGetString(GL_EXTENSIONS);
  host_pvrtc = ext && strstr(ext, GL_PVRTC_EXTENSION);
  const char *env = getenv("DL_PVRTC_SOFTWARE");
  pvrtc_software = !host_pvrtc || (env && *env == '1');
  if (pvrtc_software) fprintf(stderr, "[gles] PVRTC textures are decoded in software\n");
  static const char *const compressed[] = {"GL_IMG_texture_compression_pvrtc",
                                           "GL_OES_compressed_ETC1_RGB8_texture",
                                           "GL_AMD_compressed_ATC_texture"};
  for (unsigned i = 0; i < sizeof compressed / sizeof *compressed; i++)
    fprintf(stderr, "[gles] host %s %s\n", ext && strstr(ext, compressed[i]) ? "has" : "lacks",
            compressed[i]);
  return 0;
}

#define A(i) cpu_arg(c, i)
#define I(i) ((GLint)cpu_arg(c, i))
#define P(i) gptr(cpu_arg(c, i))

static void *gptr(uint32_t a) { return a ? g2h(a) : NULL; }

#define GL_SCISSOR_TEST 0x0C11
#define GL_VIEWPORT 0x0BA2
#define GL_COLOR_BUFFER_BIT 0x4000

/* The game's rectangle of the surface (gles_set_content); unset: all of it. */
static struct {
  int on;
  GLint x, y;
  GLsizei w, h;
  GLfloat clear[4]; /* the game's clear colour, restored after the bars */
} content;

void gles_set_content(int x, int y, int w, int h) {
  content.on = 1;
  content.x = x, content.y = y, content.w = w, content.h = h;
  /* The game never sets a viewport: it relies on the initial one, the whole
   * surface on Android. */
  gl.glViewport(x, y, w, h);
  gl.glScissor(x, y, w, h);
  gl.glEnable(GL_SCISSOR_TEST);
}

int gles_viewport_in_content(void) {
  GLint v[4];
  gl.glGetIntegerv(GL_VIEWPORT, v);
  return v[0] >= content.x && v[1] >= content.y && v[0] + v[2] <= content.x + content.w &&
         v[1] + v[3] <= content.y + content.h;
}

void gles_begin_frame(void) {
  if (!content.on) return;
  gl.glDisable(GL_SCISSOR_TEST);
  gl.glClearColor(0, 0, 0, 1);
  gl.glClear(GL_COLOR_BUFFER_BIT);
  gl.glClearColor(content.clear[0], content.clear[1], content.clear[2], content.clear[3]);
  gl.glEnable(GL_SCISSOR_TEST);
}

static GLfloat F(cpu *c, int i) {
  uint32_t bits = cpu_arg(c, i);
  GLfloat f;
  memcpy(&f, &bits, 4);
  return f;
}

#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895

/* A guest pointer argument that is a buffer offset while `binding` is bound. */
static const void *buffer_or_client(GLenum binding, uint32_t a) {
  GLint bound = 0;
  gl.glGetIntegerv(binding, &bound);
  return bound ? (const void *)(uintptr_t)a : gptr(a);
}

static GLenum pending_error; /* GL_NO_ERROR */
static int upload_failures;

int gles_upload_failures(void) { return upload_failures; }

/* After a texture upload: logs and counts an error, keeps it for the guest. */
static void check_upload(const char *call, GLenum fmt, GLint w, GLint h) {
  GLenum err = gl.glGetError();
  if (!err) return;
  if (!pending_error) pending_error = err;
  if (upload_failures++ < 16)
    fprintf(stderr, "[gles] %s: format 0x%04x %dx%d failed with GL error 0x%04x\n", call, fmt,
            w, h, err);
}

/* Level-0 PVRTC uploads kept for gles_pvrtc_check, one per size. */
#define PVRTC_KEEP 8
static struct {
  GLenum fmt;
  GLint w, h;
  size_t size;
  uint8_t *data;
} kept[PVRTC_KEEP];
static int n_kept, keep_pvrtc;

void gles_pvrtc_keep(void) { keep_pvrtc = 1; }

static void pvrtc_keep(GLint level, GLenum fmt, GLint w, GLint h, const void *data, size_t size) {
  if (!keep_pvrtc || level != 0 || n_kept == PVRTC_KEEP) return;
  for (int i = 0; i < n_kept; i++)
    if (kept[i].w == w && kept[i].h == h) return;
  if (!(kept[n_kept].data = malloc(size))) return;
  memcpy(kept[n_kept].data, data, size);
  kept[n_kept].fmt = fmt;
  kept[n_kept].w = w;
  kept[n_kept].h = h;
  kept[n_kept++].size = size;
}

/* Logs each texture format the game uploads, once. */
static void note_format(const char *call, GLenum fmt, GLenum type) {
  static struct { GLenum fmt, type; } seen[64];
  static int n;
  for (int i = 0; i < n; i++)
    if (seen[i].fmt == fmt && seen[i].type == type) return;
  if (n < 64) seen[n++] = (typeof(seen[0])){fmt, type};
  fprintf(stderr, "[gles] %s: format 0x%04x type 0x%04x\n", call, fmt, type);
}

static void h_glActiveTexture(cpu *c) { gl.glActiveTexture(A(0)); }
static void h_glAttachShader(cpu *c) { gl.glAttachShader(A(0), A(1)); }
static void h_glBindAttribLocation(cpu *c) { gl.glBindAttribLocation(A(0), A(1), P(2)); }
static void h_glBindBuffer(cpu *c) { gl.glBindBuffer(A(0), A(1)); }
static void h_glBindTexture(cpu *c) { gl.glBindTexture(A(0), A(1)); }
static void h_glBlendEquationSeparate(cpu *c) { gl.glBlendEquationSeparate(A(0), A(1)); }
static void h_glBlendFuncSeparate(cpu *c) { gl.glBlendFuncSeparate(A(0), A(1), A(2), A(3)); }
static void h_glBufferData(cpu *c) { gl.glBufferData(A(0), (GLsizeiptr)I(1), P(2), A(3)); }
static void h_glBufferSubData(cpu *c) {
  gl.glBufferSubData(A(0), (GLintptr)I(1), (GLsizeiptr)I(2), P(3));
}
static void h_glClear(cpu *c) { gl.glClear(A(0)); }
static void h_glClearColor(cpu *c) {
  for (int i = 0; i < 4; i++) content.clear[i] = F(c, i);
  gl.glClearColor(F(c, 0), F(c, 1), F(c, 2), F(c, 3));
}
static void h_glClearDepthf(cpu *c) { gl.glClearDepthf(F(c, 0)); }
static void h_glColorMask(cpu *c) {
  gl.glColorMask((GLboolean)A(0), (GLboolean)A(1), (GLboolean)A(2), (GLboolean)A(3));
}
static void h_glCompileShader(cpu *c) { gl.glCompileShader(A(0)); }
static void h_glCompressedTexImage2D(cpu *c) {
  GLenum fmt = A(2);
  GLint w = I(3), h = I(4);
  note_format("glCompressedTexImage2D", fmt, 0);
  if (fmt == GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG || fmt == GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG) {
    pvrtc_keep(I(1), fmt, w, h, P(7), (size_t)I(6));
    if (pvrtc_software) {
      uint8_t *rgba = malloc((size_t)w * h * 4);
      if (rgba && pvrtc4_decode(P(7), (size_t)I(6), w, h, rgba) == 0) {
        gl.glTexImage2D(A(0), I(1), GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        check_upload("glCompressedTexImage2D (software PVRTC)", fmt, w, h);
      } else {
        fprintf(stderr, "[gles] can't decode PVRTC %dx%d (%d bytes)\n", w, h, I(6));
        upload_failures++;
      }
      free(rgba);
      return;
    }
  }
  gl.glCompressedTexImage2D(A(0), I(1), fmt, w, h, I(5), I(6), P(7));
  check_upload("glCompressedTexImage2D", fmt, w, h);
}
static void h_glCreateProgram(cpu *c) { cpu_ret(c, gl.glCreateProgram()); }
static void h_glCreateShader(cpu *c) { cpu_ret(c, gl.glCreateShader(A(0))); }
static void h_glCullFace(cpu *c) { gl.glCullFace(A(0)); }
static void h_glDeleteBuffers(cpu *c) { gl.glDeleteBuffers(I(0), P(1)); }
static void h_glDeleteProgram(cpu *c) { gl.glDeleteProgram(A(0)); }
static void h_glDeleteShader(cpu *c) { gl.glDeleteShader(A(0)); }
static void h_glDeleteTextures(cpu *c) { gl.glDeleteTextures(I(0), P(1)); }
static void h_glDepthFunc(cpu *c) { gl.glDepthFunc(A(0)); }
static void h_glDepthMask(cpu *c) { gl.glDepthMask((GLboolean)A(0)); }
static void h_glDisable(cpu *c) {
  if (!(content.on && A(0) == GL_SCISSOR_TEST)) gl.glDisable(A(0));
}
static void h_glDisableVertexAttribArray(cpu *c) { gl.glDisableVertexAttribArray(A(0)); }
static void h_glDrawArrays(cpu *c) { gl.glDrawArrays(A(0), I(1), I(2)); }
static void h_glDrawElements(cpu *c) {
  gl.glDrawElements(A(0), I(1), A(2), buffer_or_client(GL_ELEMENT_ARRAY_BUFFER_BINDING, A(3)));
}
static void h_glEnable(cpu *c) {
  if (!(content.on && A(0) == GL_SCISSOR_TEST)) gl.glEnable(A(0));
}
static void h_glEnableVertexAttribArray(cpu *c) { gl.glEnableVertexAttribArray(A(0)); }
static void h_glFinish(cpu *c) {
  (void)c;
  gl.glFinish();
}
static void h_glGenBuffers(cpu *c) { gl.glGenBuffers(I(0), P(1)); }
static void h_glGenTextures(cpu *c) { gl.glGenTextures(I(0), P(1)); }
static void h_glGetActiveAttrib(cpu *c) {
  gl.glGetActiveAttrib(A(0), A(1), I(2), P(3), P(4), P(5), P(6));
}
static void h_glGetActiveUniform(cpu *c) {
  gl.glGetActiveUniform(A(0), A(1), I(2), P(3), P(4), P(5), P(6));
}
static void h_glGetError(cpu *c) {
  GLenum err = pending_error ? pending_error : gl.glGetError();
  pending_error = 0;
  cpu_ret(c, err);
}
static void h_glGetIntegerv(cpu *c) {
  GLint *v = P(1);
  gl.glGetIntegerv(A(0), v);
  if (content.on && A(0) == GL_VIEWPORT && v) v[0] -= content.x, v[1] -= content.y;
}
static void h_glGetProgramInfoLog(cpu *c) { gl.glGetProgramInfoLog(A(0), I(1), P(2), P(3)); }
static void h_glGetProgramiv(cpu *c) { gl.glGetProgramiv(A(0), A(1), P(2)); }
static void h_glGetShaderInfoLog(cpu *c) { gl.glGetShaderInfoLog(A(0), I(1), P(2), P(3)); }
static void h_glGetShaderiv(cpu *c) { gl.glGetShaderiv(A(0), A(1), P(2)); }

/* glGetString: the host string, copied into guest memory once per name. */
static void h_glGetString(cpu *c) {
  static struct { GLenum name; uint32_t addr; } cache[8];
  static int n;
  GLenum name = A(0);
  for (int i = 0; i < n; i++)
    if (cache[i].name == name) {
      cpu_ret(c, cache[i].addr);
      return;
    }
  const char *s = gl.glGetString(name);
  uint32_t g = 0;
  if (s) {
    /* Decoded in software, PVRTC still reaches the game's texture choice. */
    int add = name == GL_EXTENSIONS && !host_pvrtc;
    uint32_t len = (uint32_t)strlen(s) + 1;
    g = guest_malloc(len + (add ? (uint32_t)strlen(" " GL_PVRTC_EXTENSION) : 0));
    memcpy(g2h(g), s, len);
    if (add) strcat(g2h(g), " " GL_PVRTC_EXTENSION);
    if (n < 8) cache[n++] = (typeof(cache[0])){name, g};
  }
  cpu_ret(c, g);
}

static void h_glGetUniformLocation(cpu *c) { cpu_ret(c, (uint32_t)gl.glGetUniformLocation(A(0), P(1))); }
static void h_glLinkProgram(cpu *c) { gl.glLinkProgram(A(0)); }
static void h_glReadPixels(cpu *c) {
  gl.glReadPixels(I(0) + content.x, I(1) + content.y, I(2), I(3), A(4), A(5), P(6));
}

/* glShaderSource(shader, count, strings, lengths): strings is a guest array
 * of guest pointers. */
static void h_glShaderSource(cpu *c) {
  GLsizei count = I(1);
  const char *strings[64];
  if (count < 0 || count > 64) {
    fprintf(stderr, "[gles] glShaderSource with %d strings\n", count);
    return;
  }
  for (GLsizei i = 0; i < count; i++) strings[i] = gptr(gm_r32(A(2) + 4 * (uint32_t)i));
  gl.glShaderSource(A(0), count, strings, P(3));
}

static void h_glStencilFuncSeparate(cpu *c) { gl.glStencilFuncSeparate(A(0), A(1), I(2), A(3)); }
static void h_glStencilMask(cpu *c) { gl.glStencilMask(A(0)); }
static void h_glStencilOpSeparate(cpu *c) { gl.glStencilOpSeparate(A(0), A(1), A(2), A(3)); }
static void h_glTexImage2D(cpu *c) {
  note_format("glTexImage2D", A(6), A(7));
  gl.glTexImage2D(A(0), I(1), I(2), I(3), I(4), I(5), A(6), A(7), P(8));
  check_upload("glTexImage2D", A(6), I(3), I(4));
}
static void h_glTexParameterf(cpu *c) { gl.glTexParameterf(A(0), A(1), F(c, 2)); }
static void h_glTexParameteri(cpu *c) { gl.glTexParameteri(A(0), A(1), I(2)); }
static void h_glUniform1fv(cpu *c) { gl.glUniform1fv(I(0), I(1), P(2)); }
static void h_glUniform1i(cpu *c) { gl.glUniform1i(I(0), I(1)); }
static void h_glUniform2fv(cpu *c) { gl.glUniform2fv(I(0), I(1), P(2)); }
static void h_glUniform3fv(cpu *c) { gl.glUniform3fv(I(0), I(1), P(2)); }
static void h_glUniform4fv(cpu *c) { gl.glUniform4fv(I(0), I(1), P(2)); }
static void h_glUniformMatrix3fv(cpu *c) { gl.glUniformMatrix3fv(I(0), I(1), (GLboolean)A(2), P(3)); }
static void h_glUniformMatrix4fv(cpu *c) { gl.glUniformMatrix4fv(I(0), I(1), (GLboolean)A(2), P(3)); }
static void h_glUseProgram(cpu *c) { gl.glUseProgram(A(0)); }
static void h_glVertexAttribPointer(cpu *c) {
  gl.glVertexAttribPointer(A(0), I(1), A(2), (GLboolean)A(3), I(4),
                           buffer_or_client(GL_ARRAY_BUFFER_BINDING, A(5)));
}
static void h_glViewport(cpu *c) { gl.glViewport(I(0) + content.x, I(1) + content.y, I(2), I(3)); }

static void h_glGenVertexArraysOES(cpu *c) { gl.glGenVertexArraysOES(I(0), P(1)); }
static void h_glBindVertexArrayOES(cpu *c) { gl.glBindVertexArrayOES(A(0)); }
static void h_glDeleteVertexArraysOES(cpu *c) { gl.glDeleteVertexArraysOES(I(0), P(1)); }
static void h_glIsVertexArrayOES(cpu *c) { cpu_ret(c, gl.glIsVertexArrayOES(A(0))); }
static void h_glInsertEventMarkerEXT(cpu *c) { gl.glInsertEventMarkerEXT(I(0), P(1)); }
static void h_glPushGroupMarkerEXT(cpu *c) { gl.glPushGroupMarkerEXT(I(0), P(1)); }
static void h_glPopGroupMarkerEXT(cpu *c) {
  (void)c;
  gl.glPopGroupMarkerEXT();
}
static void h_glLabelObjectEXT(cpu *c) { gl.glLabelObjectEXT(A(0), A(1), I(2), P(3)); }
static void h_glGetObjectLabelEXT(cpu *c) { gl.glGetObjectLabelEXT(A(0), A(1), I(2), P(3), P(4)); }

static const struct {
  const char *name;
  dl_handler fn;
} gl_procs[] = {
    {"glGenVertexArraysOES", h_glGenVertexArraysOES},
    {"glBindVertexArrayOES", h_glBindVertexArrayOES},
    {"glDeleteVertexArraysOES", h_glDeleteVertexArraysOES},
    {"glIsVertexArrayOES", h_glIsVertexArrayOES},
    {"glInsertEventMarkerEXT", h_glInsertEventMarkerEXT},
    {"glPushGroupMarkerEXT", h_glPushGroupMarkerEXT},
    {"glPopGroupMarkerEXT", h_glPopGroupMarkerEXT},
    {"glLabelObjectEXT", h_glLabelObjectEXT},
    {"glGetObjectLabelEXT", h_glGetObjectLabelEXT},
};

/* eglGetProcAddress(name): a host thunk per known extension call, made on
 * first request; NULL for anything else, as for a missing extension. */
static void h_eglGetProcAddress(cpu *c) {
  static uint32_t thunks[sizeof gl_procs / sizeof gl_procs[0]];
  const char *name = g2h(cpu_arg(c, 0));
  for (size_t i = 0; i < sizeof gl_procs / sizeof gl_procs[0]; i++) {
    if (strcmp(name, gl_procs[i].name) != 0) continue;
    if (!thunks[i]) thunks[i] = dynlib_host_thunk(gl_procs[i].name, gl_procs[i].fn);
    cpu_ret(c, thunks[i]);
    return;
  }
  fprintf(stderr, "[egl] eglGetProcAddress(\"%s\"): unknown, NULL\n", name);
  cpu_ret(c, 0);
}

#define E(name) {#name, h_##name}
const dynlib_impl reimpl_gles[] = {
    E(glActiveTexture), E(glAttachShader), E(glBindAttribLocation), E(glBindBuffer),
    E(glBindTexture), E(glBlendEquationSeparate), E(glBlendFuncSeparate), E(glBufferData),
    E(glBufferSubData), E(glClear), E(glClearColor), E(glClearDepthf), E(glColorMask),
    E(glCompileShader), E(glCompressedTexImage2D), E(glCreateProgram), E(glCreateShader),
    E(glCullFace), E(glDeleteBuffers), E(glDeleteProgram), E(glDeleteShader),
    E(glDeleteTextures), E(glDepthFunc), E(glDepthMask), E(glDisable),
    E(glDisableVertexAttribArray), E(glDrawArrays), E(glDrawElements), E(glEnable),
    E(glEnableVertexAttribArray), E(glFinish), E(glGenBuffers), E(glGenTextures),
    E(glGetActiveAttrib), E(glGetActiveUniform), E(glGetError), E(glGetIntegerv),
    E(glGetProgramInfoLog), E(glGetProgramiv), E(glGetShaderInfoLog), E(glGetShaderiv),
    E(glGetString), E(glGetUniformLocation), E(glLinkProgram), E(glReadPixels),
    E(glShaderSource), E(glStencilFuncSeparate), E(glStencilMask), E(glStencilOpSeparate),
    E(glTexImage2D), E(glTexParameterf), E(glTexParameteri), E(glUniform1fv), E(glUniform1i),
    E(glUniform2fv), E(glUniform3fv), E(glUniform4fv), E(glUniformMatrix3fv),
    E(glUniformMatrix4fv), E(glUseProgram), E(glVertexAttribPointer), E(glViewport),
    E(eglGetProcAddress),
    {NULL, NULL},
};

/* gles_pvrtc_check: the GPU's decode of each kept texture, drawn 1:1 into an
 * RGBA8 framebuffer with nearest sampling, against pvrtc4_decode. Runs after
 * the game's last frame, so it doesn't restore the game's GL state. */
#define GL_TEXTURE_2D 0x0DE1
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_NEAREST 0x2600
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_FLOAT 0x1406
#define GL_TRIANGLE_STRIP 0x0005
#define GL_ARRAY_BUFFER 0x8892
#define GL_TEXTURE0 0x84C0

static GLuint shader(GLenum type, const char *src) {
  GLuint sh = gl.glCreateShader(type);
  gl.glShaderSource(sh, 1, &src, NULL);
  gl.glCompileShader(sh);
  return sh;
}

/* GL state for drawing texture unit 0 1:1 over the bound framebuffer with a
 * full-screen quad and no blending (gles_pvrtc_check, gles_format_check). */
static void blit_setup(void) {
  static const char *vs = "attribute vec2 p; varying vec2 t;\n"
                          "void main() { t = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
  static const char *fs = "precision highp float; varying vec2 t; uniform sampler2D s;\n"
                          "void main() { gl_FragColor = texture2D(s, t); }\n";
  static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
  GLuint prog = gl.glCreateProgram();
  gl.glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
  gl.glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
  gl.glBindAttribLocation(prog, 0, "p");
  gl.glLinkProgram(prog);
  gl.glUseProgram(prog);
  gl.glUniform1i(gl.glGetUniformLocation(prog, "s"), 0);
  gl.glBindVertexArrayOES(0);
  gl.glBindBuffer(GL_ARRAY_BUFFER, 0);
  for (GLuint i = 0; i < 16; i++) gl.glDisableVertexAttribArray(i);
  gl.glEnableVertexAttribArray(0);
  gl.glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, quad);
  static const GLenum off[] = {0x0BE2 /* BLEND */, 0x0B71 /* DEPTH_TEST */,
                               0x0C11 /* SCISSOR_TEST */, 0x0B44 /* CULL_FACE */,
                               0x0B90 /* STENCIL_TEST */};
  for (unsigned i = 0; i < sizeof off / sizeof *off; i++) gl.glDisable(off[i]);
  gl.glColorMask(1, 1, 1, 1);
  gl.glActiveTexture(GL_TEXTURE0);
}

int gles_pvrtc_check(int *max_diff, double *mean_diff) {
  if (!host_pvrtc) return -1;
  blit_setup();

  long long total = 0, count = 0;
  int worst = 0, rc = n_kept ? 0 : -1;
  for (int k = 0; k < n_kept && rc == 0; k++) {
    GLint w = kept[k].w, h = kept[k].h;
    uint8_t *sw = malloc((size_t)w * h * 4), *hw = malloc((size_t)w * h * 4);
    GLuint tex[2], fbo;
    gl.glGenTextures(2, tex);
    gl.glBindTexture(GL_TEXTURE_2D, tex[1]);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl.glGenFramebuffers(1, &fbo);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[1], 0);
    gl.glBindTexture(GL_TEXTURE_2D, tex[0]);
    gl.glCompressedTexImage2D(GL_TEXTURE_2D, 0, kept[k].fmt, w, h, 0, (GLsizei)kept[k].size,
                              kept[k].data);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!sw || !hw || gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        pvrtc4_decode(kept[k].data, kept[k].size, w, h, sw) != 0) {
      rc = -1;
    } else {
      gl.glViewport(0, 0, w, h);
      gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      gl.glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, hw);
      if (gl.glGetError()) rc = -1;
      int tex_worst = 0;
      long long tex_total = 0;
      for (size_t i = 0; i < (size_t)w * h * 4; i++) {
        int d = abs((int)sw[i] - (int)hw[i]);
        tex_total += d;
        if (d > tex_worst) tex_worst = d;
      }
      fprintf(stderr, "[gles] PVRTC check %dx%d (0x%04x): max diff %d, mean %.3f\n", w, h,
              kept[k].fmt, tex_worst, (double)tex_total / ((double)w * h * 4));
      total += tex_total;
      count += (long long)w * h * 4;
      if (tex_worst > worst) worst = tex_worst;
    }
    gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.glDeleteFramebuffers(1, &fbo);
    gl.glDeleteTextures(2, tex);
    free(sw);
    free(hw);
  }
  *max_diff = worst;
  *mean_diff = count ? (double)total / (double)count : 0;
  return rc;
}

/* gles_format_check: each uncompressed format/type pair the game uploads
 * (the [gles] format lines), as a 4x4 texture of known texels, drawn 1:1 and
 * read back as RGBA8. GL_LUMINANCE must come back as (L, L, L, 1) and
 * GL_LUMINANCE_ALPHA as (L, L, L, A), as on the phone's GLES2; ANGLE's Metal
 * backend has no native luminance and emulates it. */
#define GL_RGB 0x1907
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
int gles_format_check(void) {
  static const struct {
    const char *name;
    GLenum fmt, type;
    int bytes; /* per texel */
  } formats[] = {
      {"RGB", GL_RGB, GL_UNSIGNED_BYTE, 3},
      {"RGBA", GL_RGBA, GL_UNSIGNED_BYTE, 4},
      {"RGBA4444", GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 2},
      {"LUMINANCE", GL_LUMINANCE, GL_UNSIGNED_BYTE, 1},
      {"LUMINANCE_ALPHA", GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 2},
  };
  enum { N = 4 };
  blit_setup();
  int rc = 0;
  for (unsigned f = 0; f < sizeof formats / sizeof *formats; f++) {
    uint8_t src[N * N * 4], want[N * N * 4], got[N * N * 4];
    for (int i = 0; i < N * N; i++) {
      /* Distinct texel values; 4444 uses whole nibbles. */
      uint8_t v[4];
      for (int ch = 0; ch < 4; ch++) v[ch] = (uint8_t)((i * 37 + ch * 71 + 13) * 7);
      uint8_t *s = src + i * formats[f].bytes, *w = want + 4 * i;
      switch (f) {
      case 0: memcpy(s, v, 3); w[0] = v[0], w[1] = v[1], w[2] = v[2], w[3] = 255; break;
      case 1: memcpy(s, v, 4); memcpy(w, v, 4); break;
      case 2: {
        uint16_t px = 0;
        for (int ch = 0; ch < 4; ch++) {
          px |= (uint16_t)((v[ch] >> 4) << (12 - 4 * ch));
          w[ch] = (uint8_t)((v[ch] >> 4) * 17);
        }
        memcpy(s, &px, 2);
        break;
      }
      case 3: s[0] = v[0]; w[0] = w[1] = w[2] = v[0], w[3] = 255; break;
      case 4: s[0] = v[0], s[1] = v[3]; w[0] = w[1] = w[2] = v[0], w[3] = v[3]; break;
      }
    }
    GLuint tex[2], fbo;
    gl.glGenTextures(2, tex);
    gl.glBindTexture(GL_TEXTURE_2D, tex[1]);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl.glGenFramebuffers(1, &fbo);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[1], 0);
    gl.glBindTexture(GL_TEXTURE_2D, tex[0]);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, (GLint)formats[f].fmt, N, N, 0, formats[f].fmt,
                    formats[f].type, src);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    int worst = 255;
    if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
      gl.glViewport(0, 0, N, N);
      gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      gl.glReadPixels(0, 0, N, N, GL_RGBA, GL_UNSIGNED_BYTE, got);
      if (!gl.glGetError()) {
        worst = 0;
        for (int i = 0; i < N * N * 4; i++) {
          int d = abs((int)got[i] - (int)want[i]);
          if (d > worst) worst = d;
        }
      }
    }
    if (worst > 1) {
      fprintf(stderr, "[gles] format check %s (0x%04x/0x%04x): max diff %d; first texel "
              "%u,%u,%u,%u, want %u,%u,%u,%u\n", formats[f].name, formats[f].fmt,
              formats[f].type, worst, got[0], got[1], got[2], got[3], want[0], want[1], want[2],
              want[3]);
      rc = -1;
    }
    gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.glDeleteFramebuffers(1, &fbo);
    gl.glDeleteTextures(2, tex);
  }
  return rc;
}
