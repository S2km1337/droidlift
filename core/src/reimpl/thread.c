/*
 * Guest threads on host threads: pthread_create, join, self and the
 * pthread_attr calls, on bionic's 32-bit ABI.
 *
 * Each guest thread gets its own host thread and its own cpu of the creating
 * cpu's backend (so its own registers, JIT state and exclusive-monitor slot),
 * a guest stack, and through host thread-locals its own errno and
 * thread-specific data. The pthread_t handed to the guest is the guest
 * address of a small block, as bionic's is a pointer to its thread struct.
 *
 * Nothing in the libraries ported so far reads TPIDRURO directly (no
 * mrc p15,0,rX,c13,c0,3), so no guest TLS block is set up.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cpu.h"
#include "../guest_mem.h"
#include "reimpl.h"

#define G_EINVAL 22
#define G_EAGAIN 11
#define G_ESRCH 3

/* bionic's pthread_attr_t: flags, stack_base, stack_size, guard_size,
 * sched_policy, sched_priority. */
#define ATTR_FLAGS 0
#define ATTR_STACK_SIZE 8
#define ATTR_GUARD_SIZE 12
#define ATTR_FLAG_DETACHED 1u
#define DEFAULT_STACK (1u << 20) /* bionic's default */
#define MIN_STACK 0x2000u        /* PTHREAD_STACK_MIN on 32-bit bionic: 2 pages */

typedef struct gthread {
  uint32_t handle; /* the guest pthread_t */
  pthread_t host;
  /* From the creating cpu, which may be gone by the time this thread runs. */
  const cpu_ops *ops;
  uint32_t ret_sentinel, cpsr, fpscr;
  uint32_t start, arg, stack, stack_size;
  uint32_t retval;
  int detached, joined, finished;
  struct gthread *next;
} gthread;

static gthread *threads;
static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread uint32_t self_handle;
/* pthread_exit: the value it passed, and that it was called (the cpu halts). */
static __thread uint32_t exit_value;
static __thread int exit_called;
static int faults;
static int live; /* guest threads whose start routine hasn't returned */

/* Stacks of finished threads, reused by later ones: guest regions are never
 * unmapped, and a game may start many short-lived threads. */
#define MAX_FREE_STACKS 32
static struct {
  uint32_t addr, size;
} free_stacks[MAX_FREE_STACKS];
static int n_free_stacks;

static uint32_t stack_get(uint32_t size) {
  pthread_mutex_lock(&threads_lock);
  for (int i = 0; i < n_free_stacks; i++) {
    if (free_stacks[i].size == size) {
      uint32_t a = free_stacks[i].addr;
      free_stacks[i] = free_stacks[--n_free_stacks];
      pthread_mutex_unlock(&threads_lock);
      return a;
    }
  }
  pthread_mutex_unlock(&threads_lock);
  return gm_alloc(size, 0, "guest thread stack");
}

static void stack_put(uint32_t addr, uint32_t size) {
  pthread_mutex_lock(&threads_lock);
  if (n_free_stacks < MAX_FREE_STACKS) {
    free_stacks[n_free_stacks].addr = addr;
    free_stacks[n_free_stacks].size = size;
    n_free_stacks++;
  }
  pthread_mutex_unlock(&threads_lock);
}

static gthread *find(uint32_t handle) {
  gthread *t = threads;
  while (t && t->handle != handle) t = t->next;
  return t;
}

/* Unlinks and frees a thread record; threads_lock held. */
static void reap(gthread *t) {
  for (gthread **p = &threads; *p; p = &(*p)->next) {
    if (*p == t) {
      *p = t->next;
      break;
    }
  }
  guest_free(t->handle);
  free(t);
}

static void *thread_main(void *p) {
  gthread *t = p;
  self_handle = t->handle;
  cpu *c = t->ops->create();
  c->ret_sentinel = t->ret_sentinel;
  c->ops->set_reg(c, CPU_REG_SP, t->stack + t->stack_size);
  c->ops->set_cpsr(c, t->cpsr);
  c->ops->set_fpscr(c, t->fpscr);

  uint32_t r = cpu_call(c, t->start, 1, &t->arg);
  if (exit_called) r = exit_value;
  if (c->faulted) {
    fprintf(stderr, "[thread] guest thread 0x%08x (start 0x%08x) faulted\n", t->handle, t->start);
    __atomic_fetch_add(&faults, 1, __ATOMIC_RELAXED);
  } else {
    pthread_run_key_destructors(c);
  }
  c->ops->destroy(c);
  stack_put(t->stack, t->stack_size);
  __atomic_fetch_sub(&live, 1, __ATOMIC_RELEASE);

  pthread_mutex_lock(&threads_lock);
  t->retval = r;
  t->finished = 1;
  if (t->detached) reap(t);
  pthread_mutex_unlock(&threads_lock);
  return NULL;
}

/* --- attributes --- */

static void h_attr_init(cpu *c) {
  uint32_t a = cpu_arg(c, 0);
  memset(g2h(a), 0, 24);
  gm_w32(a + ATTR_STACK_SIZE, DEFAULT_STACK);
  gm_w32(a + ATTR_GUARD_SIZE, GUEST_PAGE);
  cpu_ret(c, 0);
}

static void h_attr_destroy(cpu *c) { cpu_ret(c, 0); }

static void h_attr_setstacksize(cpu *c) {
  uint32_t size = cpu_arg(c, 1);
  if (size < MIN_STACK) {
    cpu_ret(c, G_EINVAL);
    return;
  }
  gm_w32(cpu_arg(c, 0) + ATTR_STACK_SIZE, size);
  cpu_ret(c, 0);
}

/* PTHREAD_CREATE_JOINABLE 0, PTHREAD_CREATE_DETACHED 1. */
static void h_attr_setdetachstate(cpu *c) {
  uint32_t a = cpu_arg(c, 0), state = cpu_arg(c, 1);
  if (state > 1) {
    cpu_ret(c, G_EINVAL);
    return;
  }
  uint32_t flags = gm_r32(a + ATTR_FLAGS) & ~ATTR_FLAG_DETACHED;
  gm_w32(a + ATTR_FLAGS, flags | (state ? ATTR_FLAG_DETACHED : 0));
  cpu_ret(c, 0);
}

/* --- threads --- */

/* pthread_create(pthread_t *thread, const pthread_attr_t *attr,
 *                void *(*start)(void *), void *arg) */
static void h_create(cpu *c) {
  uint32_t out = cpu_arg(c, 0), attr = cpu_arg(c, 1);
  uint32_t size = attr ? gm_r32(attr + ATTR_STACK_SIZE) : DEFAULT_STACK;
  size = (size + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);

  gthread *t = calloc(1, sizeof *t);
  t->ops = c->ops;
  t->ret_sentinel = c->ret_sentinel;
  t->cpsr = c->ops->get_cpsr(c) & ~CPU_CPSR_T;
  t->fpscr = c->ops->get_fpscr(c);
  t->start = cpu_arg(c, 2);
  t->arg = cpu_arg(c, 3);
  t->detached = attr && (gm_r32(attr + ATTR_FLAGS) & ATTR_FLAG_DETACHED);
  t->stack_size = size;
  t->stack = stack_get(size);
  t->handle = guest_malloc(16);
  if (!t->stack || !t->handle) {
    if (t->handle) guest_free(t->handle);
    free(t);
    cpu_ret(c, G_EAGAIN);
    return;
  }

  /* Publish the handle before the thread can run, so pthread_self and a
   * racing pthread_join both see it. */
  pthread_mutex_lock(&threads_lock);
  t->next = threads;
  threads = t;
  gm_w32(out, t->handle);
  pthread_attr_t ha;
  pthread_attr_init(&ha);
  pthread_attr_setstacksize(&ha, cpu_host_stack_size()); /* host frames: the backend and the shims */
  __atomic_fetch_add(&live, 1, __ATOMIC_RELAXED);
  int err = pthread_create(&t->host, &ha, thread_main, t);
  if (err != 0) __atomic_fetch_sub(&live, 1, __ATOMIC_RELAXED);
  pthread_attr_destroy(&ha);
  if (err == 0 && t->detached) pthread_detach(t->host);
  if (err != 0) reap(t);
  pthread_mutex_unlock(&threads_lock);
  if (err != 0) {
    fprintf(stderr, "[thread] host pthread_create failed (%d)\n", err);
    cpu_ret(c, G_EAGAIN);
    return;
  }
  cpu_ret(c, 0);
}

/* pthread_join(pthread_t thread, void **retval) */
static void h_join(cpu *c) {
  uint32_t handle = cpu_arg(c, 0), out = cpu_arg(c, 1);
  pthread_mutex_lock(&threads_lock);
  gthread *t = find(handle);
  int ok = t && !t->detached && !t->joined;
  if (ok) t->joined = 1;
  pthread_mutex_unlock(&threads_lock);
  if (!ok) {
    cpu_ret(c, t ? G_EINVAL : G_ESRCH);
    return;
  }
  pthread_join(t->host, NULL);
  if (out) gm_w32(out, t->retval);
  pthread_mutex_lock(&threads_lock);
  reap(t);
  pthread_mutex_unlock(&threads_lock);
  cpu_ret(c, 0);
}

/* pthread_detach(pthread_t thread) */
static void h_detach(cpu *c) {
  pthread_mutex_lock(&threads_lock);
  gthread *t = find(cpu_arg(c, 0));
  uint32_t err = !t ? G_ESRCH : t->detached || t->joined ? G_EINVAL : 0;
  if (!err) {
    t->detached = 1;
    pthread_detach(t->host);
    if (t->finished) reap(t); /* nobody will join it now */
  }
  pthread_mutex_unlock(&threads_lock);
  cpu_ret(c, err);
}

/* pthread_exit(void *value): ends the calling guest thread. Its cpu halts,
 * so the cpu_call in thread_main returns and the thread finishes as if its
 * start routine had returned value. */
static void h_exit(cpu *c) {
  exit_value = cpu_arg(c, 0);
  exit_called = 1;
  if (!self_handle) fprintf(stderr, "[thread] pthread_exit on a thread droidlift didn't start\n");
  c->ops->halt(c);
}

/* pthread_kill(thread, sig): no signals are delivered; signal 0 (the
 * existence check) answers truthfully. */
static void h_kill(cpu *c) {
  uint32_t handle = cpu_arg(c, 0), sig = cpu_arg(c, 1);
  pthread_mutex_lock(&threads_lock);
  int found = find(handle) != NULL || handle == self_handle;
  pthread_mutex_unlock(&threads_lock);
  if (found && sig != 0) fprintf(stderr, "[thread] pthread_kill(signal %u) ignored\n", sig);
  cpu_ret(c, found ? 0 : G_ESRCH);
}

static void h_self(cpu *c) {
  /* The main thread and other host-made threads get a handle on first use. */
  if (!self_handle) self_handle = guest_malloc(16);
  cpu_ret(c, self_handle);
}

int thread_fault_count(void) { return __atomic_load_n(&faults, __ATOMIC_RELAXED); }

int thread_live_count(void) { return __atomic_load_n(&live, __ATOMIC_ACQUIRE); }

const dynlib_impl reimpl_thread[] = {
    {"pthread_attr_destroy", h_attr_destroy},
    {"pthread_attr_init", h_attr_init},
    {"pthread_attr_setdetachstate", h_attr_setdetachstate},
    {"pthread_attr_setstacksize", h_attr_setstacksize},
    {"pthread_create", h_create},
    {"pthread_join", h_join},
    {"pthread_self", h_self},
    {"pthread_detach", h_detach},
    {"pthread_exit", h_exit},
    {"pthread_kill", h_kill},
    {NULL, NULL},
};
