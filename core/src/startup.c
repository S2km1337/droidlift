#include "startup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "droidlift.h"
#include "fake_jni.h"
#include "guest_mem.h"
#include "reimpl/reimpl.h"

int so_run_init_array(cpu *c, const so_module *m) {
  for (uint32_t i = 0; i < m->init_array_count; i++) {
    uint32_t fn = gm_r32(m->init_array + i * 4);
    if (fn == 0 || fn == 0xFFFFFFFFu) continue;
    if (getenv("DL_TRACE_INIT")) fprintf(stderr, "[init] %s ctor %u at 0x%08x\n", m->soname, i, fn);
    cpu_call(c, fn, 0, NULL);
    if (c->faulted) {
      fprintf(stderr, "[init] %s constructor %u (0x%08x) faulted\n", m->soname, i, fn);
      return -1;
    }
  }
  printf("[init] %s: %u constructors ran\n", m->soname, m->init_array_count);
  return 0;
}

int so_run_jni_onload(cpu *c, const so_module *m) {
  uint32_t fn = so_find_export(m, "JNI_OnLoad");
  if (!fn) return 0;
  uint32_t args[2] = {jni_vm(), 0};
  uint32_t version = cpu_call(c, fn, 2, args);
  if (c->faulted) {
    fprintf(stderr, "[init] %s: JNI_OnLoad faulted\n", m->soname);
    return -1;
  }
  printf("[init] %s: JNI_OnLoad returned 0x%x\n", m->soname, version);
  return 0;
}

/* Java_<class>_<method> with JNI's escapes: '/' is '_', '_' is "_1", ';'
 * "_2", '[' "_3", anything else outside [A-Za-z0-9] "_0xxxx". */
static void mangle(char *out, size_t n, const char *s) {
  size_t len = strlen(out);
  for (; *s && len + 7 < n; s++) {
    unsigned char ch = (unsigned char)*s;
    if (ch == '/') out[len++] = '_';
    else if (ch == '_') len += (size_t)snprintf(out + len, n - len, "_1");
    else if (ch == ';') len += (size_t)snprintf(out + len, n - len, "_2");
    else if (ch == '[') len += (size_t)snprintf(out + len, n - len, "_3");
    else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) out[len++] = (char)ch;
    else len += (size_t)snprintf(out + len, n - len, "_0%04x", ch);
  }
  out[len] = 0;
}

uint32_t dl_native(const so_module *m, const char *cls, const char *name) {
  uint32_t fn = jni_native(cls, name, NULL);
  if (fn) return fn;
  char sym[512] = "Java_";
  mangle(sym, sizeof sym, cls);
  strncat(sym, "_", sizeof sym - strlen(sym) - 1);
  mangle(sym, sizeof sym, name);
  return so_find_export(m, sym);
}

/* Where the app is in Android's activity lifecycle: paused between onPause
 * and onResume, stopped between onStop and onRestart. Each step runs the
 * profile's hook once per change, so iOS reporting a change twice, or a quit
 * after a stop, does nothing more. */
static int app_paused, app_stopped;

static const so_module *main_lib(void) {
  int n;
  const so_module *m = dl_modules(&n);
  return &m[n - 1];
}

static int hook(int (*fn)(cpu *, const so_module *), cpu *c) { return fn ? fn(c, main_lib()) : 0; }

int dl_app_pause(cpu *c) {
  if (app_paused) return 0;
  app_paused = 1;
  return hook(dl_game_active()->pause, c);
}

int dl_app_stop(cpu *c) {
  int r = dl_app_pause(c);
  if (r == 0 && !app_stopped) {
    app_stopped = 1;
    r = hook(dl_game_active()->stop, c);
  }
  /* Android games save in onStop: the files go to disk now. */
  stdio_commit_all();
  fflush(stdout);
  return r;
}

int dl_app_resume(cpu *c) {
  int r = 0;
  if (app_stopped) {
    app_stopped = 0;
    r = hook(dl_game_active()->restart, c);
  }
  if (r == 0 && app_paused) {
    app_paused = 0;
    r = hook(dl_game_active()->resume, c);
  }
  return r;
}

int dl_app_is_paused(void) { return app_paused; }

uint32_t dl_known_addr(const so_module *m, const dl_known_fn *table, int count, const char *name) {
  for (int i = 0; i < count; i++) {
    if (strcmp(table[i].name, name)) continue;
    uint32_t a = m->base + table[i].off;
    const uint16_t *p = g2h(a & ~1u);
    if (p[0] != table[i].head[0] || p[1] != table[i].head[1]) {
      fprintf(stderr, "droidlift: %s isn't at %s+0x%x in this binary\n", name, m->soname, table[i].off);
      return 0;
    }
    return a;
  }
  fprintf(stderr, "droidlift: no known function %s\n", name);
  return 0;
}
