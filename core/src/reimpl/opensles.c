/*
 * libdl and a minimal OpenSL ES, enough for FMOD's Android output.
 *
 * FMOD Ex loads libOpenSLES.so with dlopen and looks up slCreateEngine and
 * five interface IDs with dlsym; it has no other output on Android, and
 * EventSystem::init fails without it. So dlopen knows exactly one library,
 * whose symbols are host thunks (slCreateEngine) and guest data (the IIDs).
 * Any other library or symbol fails, with the reason in dlerror.
 *
 * OpenSL objects live in guest memory as the ABI wants: an interface is the
 * address of a word holding the address of its vtable, whose slots are host
 * thunks. Supported: the engine (Engine interface), the output mix, and an
 * audio player fed by an Android simple buffer queue (Play, buffer queue and
 * Android configuration interfaces). Anything else answers
 * SL_RESULT_FEATURE_UNSUPPORTED, which is a valid OpenSL answer.
 *
 * Playback: the first time a player is set to PLAYING it gets a feeder
 * thread with its own guest cpu. The feeder hands the head buffer to the host
 * output (audio.h) whenever less than two buffers' worth is still queued
 * there, then calls the guest's buffer-queue callback, as Android's audio
 * thread does when a buffer completes. FMOD mixes the next block in that
 * callback and enqueues it again.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../audio.h"
#include "../cpu.h"
#include "../dynlib.h"
#include "../guest_mem.h"
#include "reimpl.h"

#define SL_RESULT_SUCCESS 0
#define SL_RESULT_PARAMETER_INVALID 2
#define SL_RESULT_MEMORY_FAILURE 3
#define SL_RESULT_PRECONDITIONS_VIOLATED 1
#define SL_RESULT_BUFFER_INSUFFICIENT 7
#define SL_RESULT_FEATURE_UNSUPPORTED 12

#define SL_OBJECT_STATE_UNREALIZED 1
#define SL_OBJECT_STATE_REALIZED 2
#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PLAYING 3

#define SL_DATALOCATOR_BUFFERQUEUE 6
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE 0x800007BDu
#define SL_DATAFORMAT_PCM 2

/* bionic's RTLD_DEFAULT on 32-bit */
#define G_RTLD_DEFAULT 0xFFFFFFFFu

enum { IID_ENGINE, IID_BUFFERQUEUE, IID_CONFIGURATION, IID_PLAY, IID_RECORD, N_IIDS };
static const char *const iid_names[N_IIDS] = {"SL_IID_ENGINE", "SL_IID_ANDROIDSIMPLEBUFFERQUEUE",
                                              "SL_IID_ANDROIDCONFIGURATION", "SL_IID_PLAY",
                                              "SL_IID_RECORD"};

/* Interface words in an object's guest block. */
enum { ITF_OBJECT, ITF_ENGINE, ITF_PLAY, ITF_BUFFERQUEUE, ITF_CONFIGURATION, N_ITFS };

enum { KIND_ENGINE, KIND_OUTPUT_MIX, KIND_PLAYER };

#define MAX_OBJECTS 16
#define MAX_QUEUE 16
#define FEEDER_STACK 0x40000u

typedef struct sl_object {
  uint32_t g; /* guest block of N_ITFS words; 0 = free slot */
  uint32_t serial; /* tells a reused slot (and guest block) from its predecessor */
  int kind, state;
  /* player */
  uint32_t play_state, play_cb, play_ctx, play_mask;
  uint32_t bq_cb, bq_ctx, bq_cap, bq_count, bq_head, bq_played;
  uint32_t bq_buf[MAX_QUEUE], bq_size[MAX_QUEUE];
  uint64_t played_bytes;
  uint32_t channels, rate_mhz, bits;
  int feeding; /* a feeder thread serves this player */
} sl_object;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static sl_object objects[MAX_OBJECTS];
static uint32_t vtables[N_ITFS];
static uint32_t next_serial;
static uint32_t iid_vars;       /* N_IIDS pointer words, then the 16-byte IDs */
static uint32_t lib_handle;     /* dlopen("libOpenSLES.so") */
static uint32_t create_engine;  /* thunk */
static uint32_t dl_error;       /* guest buffer for dlerror */
static int dl_error_set;

static uint32_t iid(int i) { return gm_r32(iid_vars + 4 * (uint32_t)i); }

static int iid_index(uint32_t id) {
  for (int i = 0; i < N_IIDS; i++)
    if (id && id == iid(i)) return i;
  return -1;
}

/* The object whose interface self is, and which interface; NULL if none. */
static sl_object *object_of(uint32_t self, int *itf) {
  for (int i = 0; i < MAX_OBJECTS; i++) {
    uint32_t g = objects[i].g;
    if (g && self >= g && self < g + 4 * N_ITFS && !((self - g) & 3)) {
      if (itf) *itf = (int)((self - g) / 4);
      return &objects[i];
    }
  }
  return NULL;
}

static sl_object *new_object(int kind) {
  for (int i = 0; i < MAX_OBJECTS; i++) {
    sl_object *o = &objects[i];
    if (o->g) continue;
    memset(o, 0, sizeof *o);
    o->g = guest_malloc(4 * N_ITFS);
    if (!o->g) return NULL;
    for (int k = 0; k < N_ITFS; k++) gm_w32(o->g + 4 * (uint32_t)k, vtables[k]);
    o->kind = kind;
    o->serial = ++next_serial;
    o->state = SL_OBJECT_STATE_UNREALIZED;
    o->play_state = SL_PLAYSTATE_STOPPED;
    return o;
  }
  return NULL;
}

/* Which interfaces an object kind has, by IID index. */
static int has_iid(int kind, int id) {
  switch (kind) {
  case KIND_ENGINE: return id == IID_ENGINE;
  case KIND_PLAYER: return id == IID_PLAY || id == IID_BUFFERQUEUE || id == IID_CONFIGURATION;
  default: return 0;
  }
}

static int itf_of_iid(int id) {
  switch (id) {
  case IID_ENGINE: return ITF_ENGINE;
  case IID_PLAY: return ITF_PLAY;
  case IID_BUFFERQUEUE: return ITF_BUFFERQUEUE;
  case IID_CONFIGURATION: return ITF_CONFIGURATION;
  default: return -1;
  }
}

/* Checks a creation call's interface list: a required interface the object
 * kind lacks makes creation fail, as on Android. */
static int check_iids(int kind, uint32_t n, uint32_t ids, uint32_t required) {
  for (uint32_t i = 0; i < n; i++) {
    int id = iid_index(gm_r32(ids + 4 * i));
    if (!has_iid(kind, id) && required && gm_r32(required + 4 * i)) {
      fprintf(stderr, "[opensl] required interface %s unsupported\n",
              id >= 0 ? iid_names[id] : "(unknown)");
      return 0;
    }
  }
  return 1;
}

static void ret(cpu *c, uint32_t result) {
  pthread_mutex_unlock(&lock);
  cpu_ret(c, result);
}

#define RET(c, r)                                                                                  \
  do {                                                                                             \
    ret(c, r);                                                                                     \
    return;                                                                                        \
  } while (0)

static sl_object *enter(cpu *c, const char *what) {
  pthread_mutex_lock(&lock);
  sl_object *o = object_of(cpu_arg(c, 0), NULL);
  if (!o) fprintf(stderr, "[opensl] %s on unknown interface 0x%08x\n", what, cpu_arg(c, 0));
  return o;
}

/* ---- SLObjectItf ---- */

static void obj_realize(cpu *c) {
  sl_object *o = enter(c, "Realize");
  if (!o) RET(c, SL_RESULT_PARAMETER_INVALID);
  uint32_t r = o->state == SL_OBJECT_STATE_UNREALIZED ? SL_RESULT_SUCCESS
                                                      : SL_RESULT_PRECONDITIONS_VIOLATED;
  o->state = SL_OBJECT_STATE_REALIZED;
  ret(c, r);
}

static void obj_resume(cpu *c) {
  sl_object *o = enter(c, "Resume");
  ret(c, o ? SL_RESULT_SUCCESS : SL_RESULT_PARAMETER_INVALID);
}

static void obj_get_state(cpu *c) {
  sl_object *o = enter(c, "GetState");
  if (!o || !cpu_arg(c, 1)) RET(c, SL_RESULT_PARAMETER_INVALID);
  gm_w32(cpu_arg(c, 1), (uint32_t)o->state);
  ret(c, SL_RESULT_SUCCESS);
}

/* GetInterface(self, iid, SLInterfaceID *out) */
static void obj_get_interface(cpu *c) {
  sl_object *o = enter(c, "GetInterface");
  uint32_t out = cpu_arg(c, 2);
  if (!o || !out) RET(c, SL_RESULT_PARAMETER_INVALID);
  int id = iid_index(cpu_arg(c, 1));
  if (!has_iid(o->kind, id)) {
    fprintf(stderr, "[opensl] GetInterface(%s): unsupported\n",
            id >= 0 ? iid_names[id] : "(unknown)");
    RET(c, SL_RESULT_FEATURE_UNSUPPORTED);
  }
  /* Android hands out the configuration interface before Realize, so the
   * stream type can be set first (FMOD does). */
  if (o->state != SL_OBJECT_STATE_REALIZED && id != IID_CONFIGURATION)
    RET(c, SL_RESULT_PRECONDITIONS_VIOLATED);
  gm_w32(out, o->g + 4 * (uint32_t)itf_of_iid(id));
  ret(c, SL_RESULT_SUCCESS);
}

static void obj_register_callback(cpu *c) {
  sl_object *o = enter(c, "Object::RegisterCallback");
  ret(c, o ? SL_RESULT_SUCCESS : SL_RESULT_PARAMETER_INVALID);
}

static void obj_abort(cpu *c) {
  pthread_mutex_lock(&lock);
  ret(c, SL_RESULT_SUCCESS);
}

static void obj_destroy(cpu *c) {
  sl_object *o = enter(c, "Destroy");
  if (o) {
    guest_free(o->g);
    o->g = 0;
  }
  pthread_mutex_unlock(&lock);
}

static void unsupported(cpu *c) {
  fprintf(stderr, "[opensl] unsupported call on 0x%08x, lr %08x\n", cpu_arg(c, 0),
          c->ops->get_reg(c, CPU_REG_LR));
  cpu_ret(c, SL_RESULT_FEATURE_UNSUPPORTED);
}

/* ---- SLEngineItf ---- */

/* CreateOutputMix(self, SLObjectItf *mix, numInterfaces, ids, required) */
static void engine_create_output_mix(cpu *c) {
  pthread_mutex_lock(&lock);
  uint32_t out = cpu_arg(c, 1);
  if (!out) RET(c, SL_RESULT_PARAMETER_INVALID);
  if (!check_iids(KIND_OUTPUT_MIX, cpu_arg(c, 2), cpu_arg(c, 3), cpu_arg(c, 4)))
    RET(c, SL_RESULT_FEATURE_UNSUPPORTED);
  sl_object *o = new_object(KIND_OUTPUT_MIX);
  if (!o) RET(c, SL_RESULT_MEMORY_FAILURE);
  gm_w32(out, o->g + 4 * ITF_OBJECT);
  ret(c, SL_RESULT_SUCCESS);
}

/* CreateAudioPlayer(self, SLObjectItf *player, SLDataSource *src, SLDataSink *snk,
 *                   numInterfaces, ids, required) */
static void engine_create_audio_player(cpu *c) {
  pthread_mutex_lock(&lock);
  uint32_t out = cpu_arg(c, 1), src = cpu_arg(c, 2);
  if (!out || !src) RET(c, SL_RESULT_PARAMETER_INVALID);
  uint32_t loc = gm_r32(src), fmt = gm_r32(src + 4);
  uint32_t loc_type = loc ? gm_r32(loc) : 0;
  if ((loc_type != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE &&
       loc_type != SL_DATALOCATOR_BUFFERQUEUE) ||
      !fmt || gm_r32(fmt) != SL_DATAFORMAT_PCM) {
    fprintf(stderr, "[opensl] CreateAudioPlayer: only PCM buffer queues (locator 0x%x)\n",
            loc_type);
    RET(c, SL_RESULT_FEATURE_UNSUPPORTED);
  }
  if (gm_r32(fmt + 12) != 16) {
    fprintf(stderr, "[opensl] CreateAudioPlayer: only 16-bit PCM (got %u)\n", gm_r32(fmt + 12));
    RET(c, SL_RESULT_FEATURE_UNSUPPORTED);
  }
  if (!check_iids(KIND_PLAYER, cpu_arg(c, 4), cpu_arg(c, 5), cpu_arg(c, 6)))
    RET(c, SL_RESULT_FEATURE_UNSUPPORTED);
  sl_object *o = new_object(KIND_PLAYER);
  if (!o) RET(c, SL_RESULT_MEMORY_FAILURE);
  uint32_t n = gm_r32(loc + 4);
  o->bq_cap = n < 1 ? 1 : n > MAX_QUEUE ? MAX_QUEUE : n;
  o->channels = gm_r32(fmt + 4);
  o->rate_mhz = gm_r32(fmt + 8);
  o->bits = gm_r32(fmt + 12);
  fprintf(stderr, "[opensl] audio player: %u buffers, %u ch, %u Hz, %u bit\n", o->bq_cap,
          o->channels, o->rate_mhz / 1000, o->bits);
  gm_w32(out, o->g + 4 * ITF_OBJECT);
  ret(c, SL_RESULT_SUCCESS);
}

/* QueryNumSupportedInterfaces etc. are never needed for an output. */

/* ---- playback ---- */

typedef struct feeder {
  const cpu_ops *ops;
  uint32_t ret_sentinel, cpsr, fpscr;
  uint32_t g, serial;
} feeder;

static void *feeder_main(void *p) {
  feeder f = *(feeder *)p;
  free(p);
  cpu *c = f.ops->create();
  uint32_t stack = gm_alloc(FEEDER_STACK, 0, "opensl feeder stack");
  c->ret_sentinel = f.ret_sentinel;
  c->ops->set_reg(c, CPU_REG_SP, stack + FEEDER_STACK);
  c->ops->set_cpsr(c, f.cpsr);
  c->ops->set_fpscr(c, f.fpscr);
  int opened = 0;
  for (;;) {
    pthread_mutex_lock(&lock);
    sl_object *o = object_of(f.g, NULL);
    if (!o || o->serial != f.serial) {
      pthread_mutex_unlock(&lock);
      break; /* destroyed */
    }
    uint32_t cb = 0, ctx = 0, size = 0;
    if (o->play_state == SL_PLAYSTATE_PLAYING && o->bq_count) {
      if (!opened) {
        audio_open((int)o->channels, (int)(o->rate_mhz / 1000));
        opened = 1;
      }
      uint32_t head = o->bq_head;
      if (audio_queued() < 2 * o->bq_size[head]) {
        size = o->bq_size[head];
        audio_put(g2h(o->bq_buf[head]), size);
        o->bq_head = (head + 1) % o->bq_cap;
        o->bq_count--;
        o->bq_played++;
        o->played_bytes += size;
        cb = o->bq_cb;
        ctx = o->bq_ctx;
      }
    }
    pthread_mutex_unlock(&lock);
    if (!size) {
      usleep(1000);
    } else if (cb) {
      uint32_t args[] = {f.g + 4 * ITF_BUFFERQUEUE, ctx};
      cpu_call(c, cb, 2, args);
      if (c->faulted) {
        fprintf(stderr, "[opensl] buffer-queue callback 0x%08x faulted\n", cb);
        break;
      }
    }
  }
  c->ops->destroy(c);
  return NULL;
}

/* Starts o's feeder on the first PLAYING; lock held. */
static void start_feeder(cpu *c, sl_object *o) {
  if (o->kind != KIND_PLAYER || o->feeding) return;
  feeder *f = malloc(sizeof *f);
  f->ops = c->ops;
  f->ret_sentinel = c->ret_sentinel;
  f->cpsr = c->ops->get_cpsr(c) & ~CPU_CPSR_T;
  f->fpscr = c->ops->get_fpscr(c);
  f->g = o->g;
  f->serial = o->serial;
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, cpu_host_stack_size()); /* host frames: the backend and the shims */
  if (pthread_create(&t, &a, feeder_main, f) == 0) {
    pthread_detach(t);
    o->feeding = 1;
  } else {
    fprintf(stderr, "[opensl] can't start the feeder thread\n");
    free(f);
  }
  pthread_attr_destroy(&a);
}

/* ---- SLPlayItf ---- */

static void play_set_state(cpu *c) {
  sl_object *o = enter(c, "SetPlayState");
  if (!o || cpu_arg(c, 1) < 1 || cpu_arg(c, 1) > 3) RET(c, SL_RESULT_PARAMETER_INVALID);
  o->play_state = cpu_arg(c, 1);
  if (o->play_state == SL_PLAYSTATE_PLAYING) start_feeder(c, o);
  ret(c, SL_RESULT_SUCCESS);
}

static void play_get_state(cpu *c) {
  sl_object *o = enter(c, "GetPlayState");
  if (!o || !cpu_arg(c, 1)) RET(c, SL_RESULT_PARAMETER_INVALID);
  gm_w32(cpu_arg(c, 1), o->play_state);
  ret(c, SL_RESULT_SUCCESS);
}

static void play_register_callback(cpu *c) {
  sl_object *o = enter(c, "Play::RegisterCallback");
  if (!o) RET(c, SL_RESULT_PARAMETER_INVALID);
  o->play_cb = cpu_arg(c, 1);
  o->play_ctx = cpu_arg(c, 2);
  ret(c, SL_RESULT_SUCCESS);
}

static void play_set_events_mask(cpu *c) {
  sl_object *o = enter(c, "SetCallbackEventsMask");
  if (!o) RET(c, SL_RESULT_PARAMETER_INVALID);
  o->play_mask = cpu_arg(c, 1);
  ret(c, SL_RESULT_SUCCESS);
}

/* GetPosition(self, SLmillisecond *pos): the time of the buffers handed to the output. */
static void play_get_position(cpu *c) {
  sl_object *o = enter(c, "GetPosition");
  if (!o || !cpu_arg(c, 1)) RET(c, SL_RESULT_PARAMETER_INVALID);
  uint32_t frame = o->channels * 2, rate = o->rate_mhz / 1000;
  gm_w32(cpu_arg(c, 1), frame && rate ? (uint32_t)(o->played_bytes / frame * 1000 / rate) : 0);
  ret(c, SL_RESULT_SUCCESS);
}

/* ---- SLAndroidSimpleBufferQueueItf ---- */

/* Enqueue(self, const void *buffer, SLuint32 size) */
static void bq_enqueue(cpu *c) {
  sl_object *o = enter(c, "Enqueue");
  if (!o || !cpu_arg(c, 1) || !cpu_arg(c, 2)) RET(c, SL_RESULT_PARAMETER_INVALID);
  if (o->bq_count >= o->bq_cap) RET(c, SL_RESULT_BUFFER_INSUFFICIENT);
  uint32_t slot = (o->bq_head + o->bq_count) % o->bq_cap;
  o->bq_buf[slot] = cpu_arg(c, 1);
  o->bq_size[slot] = cpu_arg(c, 2);
  o->bq_count++;
  ret(c, SL_RESULT_SUCCESS);
}

static void bq_clear(cpu *c) {
  sl_object *o = enter(c, "Clear");
  if (!o) RET(c, SL_RESULT_PARAMETER_INVALID);
  o->bq_count = 0;
  o->bq_head = 0;
  ret(c, SL_RESULT_SUCCESS);
}

/* GetState(self, SLAndroidSimpleBufferQueueState *state): {count, index} */
static void bq_get_state(cpu *c) {
  sl_object *o = enter(c, "BufferQueue::GetState");
  uint32_t st = cpu_arg(c, 1);
  if (!o || !st) RET(c, SL_RESULT_PARAMETER_INVALID);
  gm_w32(st, o->bq_count);
  gm_w32(st + 4, o->bq_played);
  ret(c, SL_RESULT_SUCCESS);
}

/* RegisterCallback(self, slAndroidSimpleBufferQueueCallback cb, void *ctx) */
static void bq_register_callback(cpu *c) {
  sl_object *o = enter(c, "BufferQueue::RegisterCallback");
  if (!o) RET(c, SL_RESULT_PARAMETER_INVALID);
  if (o->play_state != SL_PLAYSTATE_STOPPED) RET(c, SL_RESULT_PRECONDITIONS_VIOLATED);
  o->bq_cb = cpu_arg(c, 1);
  o->bq_ctx = cpu_arg(c, 2);
  ret(c, SL_RESULT_SUCCESS);
}

/* ---- SLAndroidConfigurationItf ---- */

/* SetConfiguration(self, const SLchar *key, const void *value, SLuint32 size):
 * accepted and ignored (FMOD sets the stream type). */
static void config_set(cpu *c) {
  sl_object *o = enter(c, "SetConfiguration");
  if (o && cpu_arg(c, 1))
    fprintf(stderr, "[opensl] SetConfiguration(\"%s\") ignored\n", (char *)g2h(cpu_arg(c, 1)));
  ret(c, o ? SL_RESULT_SUCCESS : SL_RESULT_PARAMETER_INVALID);
}

/* ---- slCreateEngine and the vtables ---- */

/* slCreateEngine(SLObjectItf *engine, numOptions, options, numInterfaces, ids, required) */
static void sl_create_engine(cpu *c) {
  pthread_mutex_lock(&lock);
  uint32_t out = cpu_arg(c, 0);
  if (!out) RET(c, SL_RESULT_PARAMETER_INVALID);
  if (!check_iids(KIND_ENGINE, cpu_arg(c, 3), cpu_arg(c, 4), cpu_arg(c, 5)))
    RET(c, SL_RESULT_FEATURE_UNSUPPORTED);
  sl_object *o = new_object(KIND_ENGINE);
  if (!o) RET(c, SL_RESULT_MEMORY_FAILURE);
  gm_w32(out, o->g + 4 * ITF_OBJECT);
  ret(c, SL_RESULT_SUCCESS);
}

static uint32_t make_vtable(const char *name, const dl_handler *slots, int n) {
  uint32_t t = gm_alloc((uint32_t)n * 4, 0, name);
  for (int i = 0; i < n; i++) gm_w32(t + 4 * (uint32_t)i, dynlib_host_thunk(name, slots[i]));
  return t;
}

static int opensl_init(void) {
  if (lib_handle) return 0;
  static const dl_handler object[] = {
      obj_realize,  obj_resume,  obj_get_state, obj_get_interface, obj_register_callback,
      obj_abort,    obj_destroy, unsupported,   unsupported,       unsupported,
  };
  static const dl_handler engine[] = {
      unsupported, unsupported, engine_create_audio_player, unsupported,
      unsupported, unsupported, unsupported, engine_create_output_mix,
      unsupported, unsupported, unsupported, unsupported,
      unsupported, unsupported, unsupported,
  };
  static const dl_handler play[] = {
      play_set_state,       play_get_state, unsupported, play_get_position,
      play_register_callback, play_set_events_mask, unsupported, unsupported,
      unsupported,          unsupported,    unsupported, unsupported,
  };
  static const dl_handler bq[] = {bq_enqueue, bq_clear, bq_get_state, bq_register_callback};
  static const dl_handler config[] = {config_set, unsupported};
  vtables[ITF_OBJECT] = make_vtable("SLObjectItf", object, 10);
  vtables[ITF_ENGINE] = make_vtable("SLEngineItf", engine, 15);
  vtables[ITF_PLAY] = make_vtable("SLPlayItf", play, 12);
  vtables[ITF_BUFFERQUEUE] = make_vtable("SLAndroidSimpleBufferQueueItf", bq, 4);
  vtables[ITF_CONFIGURATION] = make_vtable("SLAndroidConfigurationItf", config, 2);

  /* Each SL_IID_* is a pointer variable to a 16-byte ID; ours only need to be
   * distinct, since the game can only get them through dlsym. */
  iid_vars = gm_alloc(N_IIDS * 4 + N_IIDS * 16, 0, "opensl iids");
  for (uint32_t i = 0; i < N_IIDS; i++) {
    uint32_t id = iid_vars + N_IIDS * 4 + 16 * i;
    memset(g2h(id), 0, 16);
    gm_w32(id, 0x52554132u + i);
    gm_w32(iid_vars + 4 * i, id);
  }
  create_engine = dynlib_host_thunk("slCreateEngine", sl_create_engine);
  dl_error = gm_alloc(256, 0, "dlerror");
  lib_handle = gm_alloc(16, 0, "libOpenSLES handle");
  return lib_handle && create_engine && iid_vars && dl_error ? 0 : -1;
}

/* ---- libdl ---- */

static void set_dl_error(const char *fmt, const char *arg) {
  snprintf(g2h(dl_error), 256, fmt, arg);
  dl_error_set = 1;
}

static int is_opensl(const char *path) {
  const char *base = strrchr(path, '/');
  return !strcmp(base ? base + 1 : path, "libOpenSLES.so");
}

/* dlopen(path, flags) */
static void h_dlopen(cpu *c) {
  pthread_mutex_lock(&lock);
  int ok = opensl_init() == 0;
  pthread_mutex_unlock(&lock);
  uint32_t path = cpu_arg(c, 0);
  if (ok && path && is_opensl(g2h(path))) {
    cpu_ret(c, lib_handle);
    return;
  }
  const char *p = path ? (const char *)g2h(path) : "(main program)";
  fprintf(stderr, "[dl] dlopen(\"%s\"): not available\n", p);
  if (dl_error) set_dl_error("dlopen failed: library \"%s\" not found", p);
  cpu_ret(c, 0);
}

/* dlsym(handle, name) */
static void h_dlsym(cpu *c) {
  uint32_t handle = cpu_arg(c, 0);
  const char *name = g2h(cpu_arg(c, 1));
  if (lib_handle && (handle == lib_handle || handle == G_RTLD_DEFAULT)) {
    if (!strcmp(name, "slCreateEngine")) {
      cpu_ret(c, create_engine);
      return;
    }
    for (int i = 0; i < N_IIDS; i++)
      if (!strcmp(name, iid_names[i])) {
        cpu_ret(c, iid_vars + 4 * (uint32_t)i);
        return;
      }
  }
  fprintf(stderr, "[dl] dlsym(0x%08x, \"%s\"): not found\n", handle, name);
  if (dl_error) set_dl_error("undefined symbol: %s", name);
  cpu_ret(c, 0);
}

static void h_dlclose(cpu *c) { cpu_ret(c, 0); }

static void h_dlerror(cpu *c) {
  cpu_ret(c, dl_error_set ? dl_error : 0);
  dl_error_set = 0;
}

const dynlib_impl reimpl_opensles[] = {
    {"dlopen", h_dlopen},
    {"dlsym", h_dlsym},
    {"dlclose", h_dlclose},
    {"dlerror", h_dlerror},
    {NULL, NULL},
};
