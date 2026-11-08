#include "input.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "droidlift.h"
#include "fake_jni.h"

typedef struct {
  touch_kind kind;
  int id;
  float x, y;
  double time;
} touch;

#define QUEUE_MAX 256
static touch queue[QUEUE_MAX];
static int queued, delivered;

/* The game's rectangle of the surface (input_set_content); unset: all of it. */
static struct {
  float x, y, w, h;
} content;

void input_set_content(int x, int y, int w, int h) {
  content.x = x, content.y = y, content.w = w, content.h = h;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void input_touch(touch_kind kind, int id, float x, float y) {
  if (queued == QUEUE_MAX) {
    /* A stalled render thread: keep the newest touches. */
    memmove(queue, queue + 1, sizeof(touch) * (QUEUE_MAX - 1));
    queued--;
  }
  /* The guest's CLOCK_MONOTONIC (reimpl/time.c) is the host's, so touch times
   * and the game's own clock agree, like uptimeMillis on Android. */
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  if (content.w > 0) {
    /* A touch on a black bar counts at the nearest edge of the game. */
    x = clampf(x - content.x, 0, content.w - 1);
    y = clampf(y - content.y, 0, content.h - 1);
  }
  queue[queued++] = (touch){kind, id, x, y, (double)ts.tv_sec + ts.tv_nsec / 1e9};
}

int input_dispatch(cpu *c, const so_module *main) {
  const dl_game *g = dl_game_active();
  static const char *const names[] = {"down", "move", "up", "cancel"};
  for (int i = 0; i < queued; i++) {
    const touch *t = &queue[i];
    if (g && g->touch && g->touch(c, main, t->kind, t->id, t->x, t->y, t->time) != 0) {
      fprintf(stderr, "[input] the game faulted on touch %s\n", names[t->kind]);
      return -1;
    }
    if (getenv("DL_TRACE_INPUT"))
      fprintf(stderr, "[input] %s(%d, %.1f, %.1f, %.3f)\n", names[t->kind], t->id, t->x, t->y, t->time);
    delivered++;
  }
  queued = 0;
  return 0;
}

/* --- Scripted touches and the game's signals (input.h) ------- */

#define NAME_LEN 48
#define SIGNALS_MAX 128
#define FRAMES_KEPT 64

/* How often each signal came, and in which frames (the first FRAMES_KEPT).
 * A signal sent before input_script_plan(f) counts for frame f - 1. All of
 * this is under signal_lock: the game sends signals from any thread. */
static struct {
  char name[NAME_LEN];
  int count;
  int frames[FRAMES_KEPT];
} signals[SIGNALS_MAX];
static int n_signals, signal_frame = -1;
static pthread_mutex_t signal_lock = PTHREAD_MUTEX_INITIALIZER;

static int signal_find(const char *name) {
  for (int i = 0; i < n_signals; i++)
    if (!strcmp(signals[i].name, name)) return i;
  return -1;
}

static int signal_count(const char *name) {
  int i = signal_find(name);
  return i < 0 ? 0 : signals[i].count;
}

void input_signal(const char *name) {
  if (!name || !*name) return;
  pthread_mutex_lock(&signal_lock);
  int i = signal_find(name);
  if (i < 0 && n_signals < SIGNALS_MAX) {
    i = n_signals++;
    snprintf(signals[i].name, sizeof signals[i].name, "%s", name);
    signals[i].count = 0;
  }
  if (i >= 0) {
    if (signals[i].count < FRAMES_KEPT) signals[i].frames[signals[i].count] = signal_frame;
    signals[i].count++;
  }
  int frame = signal_frame;
  pthread_mutex_unlock(&signal_lock);
  printf("[signal] frame %d: %s\n", frame, name);
}

typedef struct {
  touch_kind kind;
  int tap; /* down, then up two frames later */
  float x, y;
  /* Start: frame, or (signal[0] set) offset frames after the nth signal. */
  int frame;
  char signal[NAME_LEN];
  int nth, offset;
  /* Repeat every frames, or every_ms of wall-clock time (both 0: once),
   * until a signal comes, at most times (0: no limit). */
  int every, every_ms, times;
  char until[NAME_LEN];
  /* State. */
  int next;       /* frame of the next occurrence; -1 while the start waits.
                   * With every_ms, the earliest frame for it */
  double next_ms; /* with every_ms: the wall-clock time of the next occurrence */
  int until_base; /* count of `until` when the item started */
  int count;      /* occurrences so far */
  int up_at;      /* frame of a tap's pending release, or -1 */
  int fire;       /* an occurrence is due in the planned frame */
  int done;       /* no more occurrences (a tap's release may still be due) */
} scripted;

#define SCRIPT_MAX 1024
static scripted script[SCRIPT_MAX];
static int n_script, planned_frame = -1, last_touch_frame = -1;

static int valid_name(const char *s) {
  if (!*s || strlen(s) >= NAME_LEN || !(isalpha((unsigned char)*s) || *s == '_')) return 0;
  for (; *s; s++)
    if (!isalnum((unsigned char)*s) && *s != '_' && *s != '.' && *s != '-') return 0;
  return 1;
}

static int parse_count(const char *s, int *out) {
  char *end;
  long v = strtol(s, &end, 10);
  if (!isdigit((unsigned char)*s) || *end || v > 10000000) return -1;
  *out = (int)v;
  return 0;
}

/* START: FRAME | SIGNAL[#K][+N] */
static int parse_start(char *tok, scripted *it) {
  if (isdigit((unsigned char)*tok)) return parse_count(tok, &it->frame);
  char *plus = strchr(tok, '+'), *hash = strchr(tok, '#');
  it->nth = 1;
  if (plus) {
    *plus = 0;
    if (parse_count(plus + 1, &it->offset) != 0) return -1;
  }
  if (hash) {
    *hash = 0;
    if (parse_count(hash + 1, &it->nth) != 0 || it->nth < 1) return -1;
  }
  if (!valid_name(tok)) return -1;
  snprintf(it->signal, sizeof it->signal, "%s", tok);
  return 0;
}

/* WHEN: START [every N] [until SIGNAL] [times K] */
static int parse_when(char *when, scripted *it) {
  char *save = NULL, *tok = strtok_r(when, " \t", &save);
  if (!tok || parse_start(tok, it) != 0) return -1;
  while ((tok = strtok_r(NULL, " \t", &save))) {
    char *val = strtok_r(NULL, " \t", &save);
    if (!val) return -1;
    if (!strcmp(tok, "every")) {
      /* N frames, or Nms of wall clock. */
      size_t len = strlen(val);
      int ms = len > 2 && !strcmp(val + len - 2, "ms");
      if (ms) val[len - 2] = 0;
      if (parse_count(val, ms ? &it->every_ms : &it->every) != 0 ||
          (ms ? it->every_ms : it->every) < 1)
        return -1;
    } else if (!strcmp(tok, "times")) {
      if (parse_count(val, &it->times) != 0 || it->times < 1) return -1;
    } else if (!strcmp(tok, "until")) {
      if (!valid_name(val)) return -1;
      snprintf(it->until, sizeof it->until, "%s", val);
    } else {
      return -1;
    }
  }
  if ((it->until[0] || it->times) && !it->every && !it->every_ms) return -1;
  if (it->every && it->every_ms) return -1;
  if (it->tap && it->every && it->every < 3) return -1; /* the taps would overlap */
  return 0;
}

/* WHEN:ACTION:X,Y */
static int parse_item(char *item, scripted *it) {
  char *c2 = strrchr(item, ':');
  if (!c2) return -1;
  *c2 = 0;
  char *c1 = strrchr(item, ':');
  if (!c1) return -1;
  *c1 = 0;
  char action[8], tail;
  if (sscanf(c2 + 1, " %f,%f %c", &it->x, &it->y, &tail) != 2) return -1;
  if (sscanf(c1 + 1, " %7[a-z] %c", action, &tail) != 1) return -1;
  if (!strcmp(action, "down")) it->kind = TOUCH_DOWN;
  else if (!strcmp(action, "move")) it->kind = TOUCH_MOVE;
  else if (!strcmp(action, "up")) it->kind = TOUCH_UP;
  else if (!strcmp(action, "tap")) it->kind = TOUCH_DOWN, it->tap = 1;
  else return -1;
  return parse_when(item, it);
}

int input_script_add(const char *spec) {
  char *copy = strdup(spec), *save = NULL;
  int rc = 0;
  for (char *item = strtok_r(copy, ";", &save); item && rc == 0;
       item = strtok_r(NULL, ";", &save)) {
    char shown[256];
    snprintf(shown, sizeof shown, "%s", item);
    scripted it = {.up_at = -1, .next = -1};
    if (n_script == SCRIPT_MAX || parse_item(item, &it) != 0) {
      fprintf(stderr, "[input] bad --touch item \"%s\"\n", shown);
      rc = -1;
    } else {
      if (!it.signal[0]) it.next = it.frame;
      script[n_script++] = it;
    }
  }
  free(copy);
  return rc;
}

static double mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* The wall clock for `every Nms` (the self-test fakes it). */
static double (*script_clock)(void) = mono_ms;

int input_script_plan(int frame) {
  double now = script_clock();
  pthread_mutex_lock(&signal_lock);
  /* Signals from now on count for this frame. */
  signal_frame = planned_frame = frame;
  int due = 0;
  for (int i = 0; i < n_script; i++) {
    scripted *it = &script[i];
    it->fire = 0;
    if (it->up_at == frame) due++;
    if (it->done) continue;
    if (it->next < 0) {
      /* Waiting for the start signal. */
      int s = signal_find(it->signal);
      if (s < 0 || signals[s].count < it->nth) continue;
      int at = it->nth <= FRAMES_KEPT ? signals[s].frames[it->nth - 1] : frame - 1;
      it->next = at + (it->offset > 1 ? it->offset : 1);
      if (it->next < frame) it->next = frame;
      printf("[input] item %d: %s#%d came in frame %d, starts at frame %d\n", i + 1, it->signal,
             it->nth, at, it->next);
    }
    /* A wall-clock repeat is due from its earliest frame on, once its time
     * has come; everything else in exactly its frame. */
    if (it->every_ms && it->count > 0 ? frame < it->next || now < it->next_ms : it->next != frame)
      continue;
    if (it->count == 0) it->until_base = signal_count(it->until);
    else if (it->until[0] && signal_count(it->until) > it->until_base) {
      printf("[input] item %d: %s came, stopped after %d touches\n", i + 1, it->until, it->count);
      it->done = 1;
      continue;
    }
    it->fire = 1;
    due++;
    it->count++;
    if (it->tap) it->up_at = frame + 2;
    if (it->times && it->count >= it->times) it->done = 1;
    else if (it->every) it->next = frame + it->every;
    else if (it->every_ms) {
      /* From this touch on, without catching up after a slow frame; a tap
       * is released before the next one starts. */
      it->next = frame + (it->tap ? 3 : 1);
      it->next_ms = now + it->every_ms;
    } else {
      it->done = 1;
    }
  }
  pthread_mutex_unlock(&signal_lock);
  if (due) last_touch_frame = frame;
  return due;
}

void input_script_frame(int frame, void (*emit)(touch_kind kind, float x, float y)) {
  if (frame != planned_frame) input_script_plan(frame);
  for (int i = 0; i < n_script; i++)
    if (script[i].up_at == frame) {
      script[i].up_at = -1;
      emit(TOUCH_UP, script[i].x, script[i].y);
    }
  for (int i = 0; i < n_script; i++)
    if (script[i].fire) {
      script[i].fire = 0;
      emit(script[i].kind, script[i].x, script[i].y);
    }
}

int input_script_done(void) {
  for (int i = 0; i < n_script; i++)
    if (!script[i].done || script[i].up_at >= 0) return 0;
  return 1;
}

int input_script_last_touch(void) { return last_touch_frame; }

int input_script_report_pending(void) {
  int pending = 0;
  pthread_mutex_lock(&signal_lock);
  for (int i = 0; i < n_script; i++) {
    const scripted *it = &script[i];
    if (it->done) continue;
    pending++;
    if (it->next < 0)
      fprintf(stderr, "[input] item %d never ran: waiting for %s#%d (it came %d times)\n", i + 1,
              it->signal, it->nth, signal_count(it->signal));
    else if (it->count == 0)
      fprintf(stderr, "[input] item %d never ran: due at frame %d\n", i + 1, it->next);
    else if (it->until[0])
      fprintf(stderr, "[input] item %d still repeating after %d touches: %s never came\n", i + 1,
              it->count, it->until);
  }
  pthread_mutex_unlock(&signal_lock);
  return pending;
}

int input_delivered(void) { return delivered; }

/* --- --script-test: the script against made-up signals, no game --------- */

static void script_reset(void) {
  n_script = n_signals = 0;
  signal_frame = planned_frame = last_touch_frame = -1;
}

#define REC_MAX 64
static struct {
  int frame;
  touch_kind kind;
  float x;
} rec[REC_MAX];
static int n_rec, rec_frame;
/* A fake wall clock for `every Nms`: ms_per_frame per frame. */
static double ms_per_frame;
static double fake_clock(void) { return rec_frame * ms_per_frame; }

static void record(touch_kind kind, float x, float y) {
  (void)y;
  if (n_rec < REC_MAX) rec[n_rec].frame = rec_frame, rec[n_rec].kind = kind, rec[n_rec].x = x;
  n_rec++;
}

/* Runs spec for frames 0..frames-1; at frame signal_at[i] (before that
 * frame's plan) the game "sends" signal_name[i]. want: "FRAME KIND X" per
 * touch (kind d/m/u), space separated, in order. */
static int script_case(const char *spec, int frames, const char *const *signal_name,
                       const int *signal_at, int n_sig, const char *want) {
  script_reset();
  n_rec = 0;
  if (input_script_add(spec) != 0) {
    fprintf(stderr, "script-test: \"%s\" didn't parse\n", spec);
    return -1;
  }
  for (int f = 0; f < frames; f++) {
    for (int i = 0; i < n_sig; i++)
      if (signal_at[i] == f) input_signal(signal_name[i]);
    rec_frame = f;
    input_script_plan(f);
    input_script_frame(f, record);
  }
  char got[1024] = "", *p = got;
  for (int i = 0; i < n_rec && i < REC_MAX; i++)
    p += snprintf(p, sizeof got - (p - got), "%s%d %c %g", i ? " " : "", rec[i].frame,
                  "dmuc"[rec[i].kind], rec[i].x);
  if (strcmp(got, want)) {
    fprintf(stderr, "script-test: \"%s\"\n  got  \"%s\"\n  want \"%s\"\n", spec, got, want);
    return -1;
  }
  return 0;
}

int input_script_selftest(void) {
  int bad = 0;
  /* The frame-number syntax (tickets 07-09) is unchanged. */
  bad |= script_case("300:down:6,1;302:move:7,1;306:up:8,1;320:tap:9,1", 400, NULL, NULL, 0,
                     "300 d 6 302 m 7 306 u 8 320 d 9 322 u 9");
  bad |= script_case(" 5:tap:1,2 ; 5:tap:2,2", 10, NULL, NULL, 0, "5 d 1 5 d 2 7 u 1 7 u 2");
  /* A signal sent before plan(50) came in frame 49: +10 is frame 59; +0 and
   * +1 are the next frame; a wait that never comes never touches. */
  const char *a[] = {"sig_a", "sig_b", "sig_a", "sig_b"};
  int at[] = {50, 80, 120, 200};
  bad |= script_case("sig_a+10:tap:1,0", 300, a, at, 1, "59 d 1 61 u 1");
  bad |= script_case("sig_a:down:1,0;sig_a+1:up:2,0;never:tap:3,0", 300, a, at, 1,
                     "50 d 1 50 u 2");
  /* The K-th time. */
  bad |= script_case("sig_a#2+3:tap:4,0", 300, a, at, 3, "122 d 4 124 u 4");
  /* Repeats: times, until (and until only counts from the item's start:
   * sig_b at 80 is before it, the one at 200 stops it). */
  bad |= script_case("100 every 40 times 3:tap:5,0", 400, NULL, NULL, 0,
                     "100 d 5 102 u 5 140 d 5 142 u 5 180 d 5 182 u 5");
  bad |= script_case("sig_a#2+1 every 30 until sig_b:tap:6,0", 400, a, at, 4,
                     "120 d 6 122 u 6 150 d 6 152 u 6 180 d 6 182 u 6");
  bad |= script_case("10 every 5 until sig_b times 2:move:7,0", 100, a, at, 2, "10 m 7 15 m 7");
  /* Wall-clock repeats: every 100 ms is every 10 frames at 10 ms a frame;
   * at 50 ms a frame each tap is still released before the next. */
  script_clock = fake_clock;
  ms_per_frame = 10;
  bad |= script_case("10 every 100ms times 3:tap:8,0", 100, NULL, NULL, 0,
                     "10 d 8 12 u 8 20 d 8 22 u 8 30 d 8 32 u 8");
  ms_per_frame = 50;
  bad |= script_case("10 every 100ms times 3:tap:8,0", 100, NULL, NULL, 0,
                     "10 d 8 12 u 8 13 d 8 15 u 8 16 d 8 18 u 8");
  script_clock = mono_ms;
  /* Malformed items are refused. */
  static const char *const refused[] = {
      "abc",          "5:jump:1,1",        "x+:tap:1,1",        "5:tap:1",
      "5 every 2:tap:1,1", "5 until a:tap:1,1", "5 times 2:tap:1,1", "5 every 10 often 2:tap:1,1",
      "-5:tap:1,1",   "9x:tap:1,1",        "a#0:tap:1,1",       "5:tap:1,1,1",
      "5 every 0ms:tap:1,1", "5 every ms:tap:1,1", "5 every 10mss:tap:1,1",
  };
  for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
    script_reset();
    if (input_script_add(refused[i]) == 0) {
      fprintf(stderr, "script-test: \"%s\" was accepted\n", refused[i]);
      bad = 1;
    }
  }
  /* A pending wait is reported, and the script isn't done. */
  script_reset();
  input_script_add("never+5:tap:1,1");
  for (int f = 0; f < 10; f++) input_script_plan(f);
  if (input_script_done() || input_script_report_pending() != 1) bad = 1;
  script_reset();
  return bad ? -1 : 0;
}
