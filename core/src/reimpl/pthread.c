/*
 * pthread and semaphore shims on bionic's 32-bit ABI.
 *
 * bionic's pthread_mutex_t, pthread_cond_t, sem_t, pthread_once_t and
 * pthread_mutexattr_t are each one 32-bit word, and a zeroed word is a valid
 * static initializer. Each guest sync object is backed by a host object found
 * by its guest address and created on first use; for a mutex, the type comes
 * from the word's bits 14-15 as bionic encodes it (so
 * PTHREAD_RECURSIVE_MUTEX_INITIALIZER, 0x4000, works).
 *
 * Thread-specific data lives in host thread-locals; guest threads run on host
 * threads (thread.c), so it is per guest thread.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

#include "../cpu.h"
#include "../guest_mem.h"
#include "reimpl.h"

/* bionic's errno values for the codes returned here (Linux numbering). */
#define G_EBUSY 16
#define G_EINVAL 22
#define G_EAGAIN 11
#define G_EDEADLK 35

#define MUTEX_TYPE_SHIFT 14
#define MUTEX_TYPE_MASK (3u << MUTEX_TYPE_SHIFT)

enum { OBJ_MUTEX = 1, OBJ_COND, OBJ_SEM };

typedef struct sync_obj {
  int kind;
  pthread_mutex_t mutex; /* the mutex itself, or the lock of a cond/sem */
  pthread_cond_t cond;
  uint32_t count; /* semaphores */
} sync_obj;

/* Open-addressing table: guest address -> host object. */
#define TABLE_SIZE 8192
static struct {
  uint32_t addr;
  sync_obj *obj;
} table[TABLE_SIZE];
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;

static sync_obj *new_obj(int kind, uint32_t addr) {
  sync_obj *o = calloc(1, sizeof *o);
  o->kind = kind;
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  if (kind == OBJ_MUTEX) {
    uint32_t type = (gm_r32(addr) & MUTEX_TYPE_MASK) >> MUTEX_TYPE_SHIFT;
    pthread_mutexattr_settype(&a, type == 1   ? PTHREAD_MUTEX_RECURSIVE
                                  : type == 2 ? PTHREAD_MUTEX_ERRORCHECK
                                              : PTHREAD_MUTEX_NORMAL);
  }
  pthread_mutex_init(&o->mutex, &a);
  pthread_mutexattr_destroy(&a);
  pthread_cond_init(&o->cond, NULL);
  return o;
}

/* The host object behind a guest sync word, created on first use. Returns
 * NULL only if the table is full. */
static sync_obj *lookup(uint32_t addr, int kind) {
  pthread_mutex_lock(&table_lock);
  uint32_t h = (addr >> 2) * 2654435761u % TABLE_SIZE;
  sync_obj *o = NULL;
  for (int probe = 0; probe < TABLE_SIZE; probe++, h = (h + 1) % TABLE_SIZE) {
    if (table[h].addr == addr) {
      if (!table[h].obj) table[h].obj = new_obj(kind, addr);
      o = table[h].obj;
      break;
    }
    if (table[h].addr == 0) {
      table[h].addr = addr;
      o = table[h].obj = new_obj(kind, addr);
      break;
    }
  }
  pthread_mutex_unlock(&table_lock);
  if (!o) fprintf(stderr, "[pthread] sync object table full\n");
  return o;
}

/* Forgets the host object so the guest word can be re-initialised. */
static void forget(uint32_t addr) {
  pthread_mutex_lock(&table_lock);
  uint32_t h = (addr >> 2) * 2654435761u % TABLE_SIZE;
  for (int probe = 0; probe < TABLE_SIZE && table[h].addr; probe++, h = (h + 1) % TABLE_SIZE) {
    if (table[h].addr == addr) {
      sync_obj *o = table[h].obj;
      table[h].obj = NULL; /* keep the key so probe chains stay intact */
      if (o) {
        pthread_mutex_destroy(&o->mutex);
        pthread_cond_destroy(&o->cond);
        free(o);
      }
      break;
    }
  }
  pthread_mutex_unlock(&table_lock);
}

static uint32_t host_err(int e) {
  switch (e) {
  case 0: return 0;
  case EBUSY: return G_EBUSY;
  case EDEADLK: return G_EDEADLK;
  case EAGAIN: return G_EAGAIN;
  default: return G_EINVAL;
  }
}

/* --- mutexes --- */

static void h_mutexattr_init(cpu *c) {
  gm_w32(cpu_arg(c, 0), 0);
  cpu_ret(c, 0);
}

static void h_mutexattr_settype(cpu *c) {
  uint32_t type = cpu_arg(c, 1);
  if (type > 2) {
    cpu_ret(c, G_EINVAL);
    return;
  }
  gm_w32(cpu_arg(c, 0), type);
  cpu_ret(c, 0);
}

static void h_mutex_init(cpu *c) {
  uint32_t m = cpu_arg(c, 0), attr = cpu_arg(c, 1);
  forget(m);
  uint32_t type = attr ? gm_r32(attr) & 3 : 0;
  gm_w32(m, type << MUTEX_TYPE_SHIFT);
  cpu_ret(c, 0);
}

static void h_mutex_destroy(cpu *c) {
  forget(cpu_arg(c, 0));
  cpu_ret(c, 0);
}

static void h_mutex_lock(cpu *c) {
  sync_obj *o = lookup(cpu_arg(c, 0), OBJ_MUTEX);
  cpu_ret(c, o ? host_err(pthread_mutex_lock(&o->mutex)) : G_EINVAL);
}

static void h_mutex_unlock(cpu *c) {
  sync_obj *o = lookup(cpu_arg(c, 0), OBJ_MUTEX);
  cpu_ret(c, o ? host_err(pthread_mutex_unlock(&o->mutex)) : G_EINVAL);
}

/* --- condition variables --- */

static void h_cond_init(cpu *c) {
  forget(cpu_arg(c, 0));
  gm_w32(cpu_arg(c, 0), 0);
  cpu_ret(c, 0);
}

static void h_cond_destroy(cpu *c) {
  forget(cpu_arg(c, 0));
  cpu_ret(c, 0);
}

static void h_cond_wait(cpu *c) {
  sync_obj *cv = lookup(cpu_arg(c, 0), OBJ_COND);
  sync_obj *m = lookup(cpu_arg(c, 1), OBJ_MUTEX);
  cpu_ret(c, cv && m ? host_err(pthread_cond_wait(&cv->cond, &m->mutex)) : G_EINVAL);
}

static void h_cond_signal(cpu *c) {
  sync_obj *cv = lookup(cpu_arg(c, 0), OBJ_COND);
  cpu_ret(c, cv ? host_err(pthread_cond_signal(&cv->cond)) : G_EINVAL);
}

static void h_cond_broadcast(cpu *c) {
  sync_obj *cv = lookup(cpu_arg(c, 0), OBJ_COND);
  cpu_ret(c, cv ? host_err(pthread_cond_broadcast(&cv->cond)) : G_EINVAL);
}

/* --- semaphores (bionic returns -1 and sets errno) --- */

static void sem_fail(cpu *c, uint32_t err) {
  gm_w32(guest_errno_addr(), err);
  cpu_ret(c, (uint32_t)-1);
}

/* sem_init(sem_t *sem, int pshared, unsigned value) */
static void h_sem_init(cpu *c) {
  uint32_t s = cpu_arg(c, 0);
  forget(s);
  sync_obj *o = lookup(s, OBJ_SEM);
  if (!o) {
    sem_fail(c, G_EINVAL);
    return;
  }
  o->count = cpu_arg(c, 2);
  cpu_ret(c, 0);
}

static void h_sem_destroy(cpu *c) {
  forget(cpu_arg(c, 0));
  cpu_ret(c, 0);
}

static void h_sem_post(cpu *c) {
  sync_obj *o = lookup(cpu_arg(c, 0), OBJ_SEM);
  if (!o) {
    sem_fail(c, G_EINVAL);
    return;
  }
  pthread_mutex_lock(&o->mutex);
  o->count++;
  pthread_cond_signal(&o->cond);
  pthread_mutex_unlock(&o->mutex);
  cpu_ret(c, 0);
}

static void h_sem_wait(cpu *c) {
  sync_obj *o = lookup(cpu_arg(c, 0), OBJ_SEM);
  if (!o) {
    sem_fail(c, G_EINVAL);
    return;
  }
  pthread_mutex_lock(&o->mutex);
  while (o->count == 0) pthread_cond_wait(&o->cond, &o->mutex);
  o->count--;
  pthread_mutex_unlock(&o->mutex);
  cpu_ret(c, 0);
}

static void h_sem_trywait(cpu *c) {
  sync_obj *o = lookup(cpu_arg(c, 0), OBJ_SEM);
  if (!o) {
    sem_fail(c, G_EINVAL);
    return;
  }
  pthread_mutex_lock(&o->mutex);
  int ok = o->count > 0;
  if (ok) o->count--;
  pthread_mutex_unlock(&o->mutex);
  if (ok) cpu_ret(c, 0);
  else sem_fail(c, G_EAGAIN);
}

/* --- pthread_once --- */

/* pthread_once(pthread_once_t *once, void (*init)(void)): 0 = not run,
 * 1 = running, 2 = done. Other threads wait while it runs. */
static void h_once(cpu *c) {
  uint32_t *word = g2h(cpu_arg(c, 0));
  uint32_t expected = 0;
  if (__atomic_compare_exchange_n(word, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    cpu_callback(c, cpu_arg(c, 1), 0, NULL);
    __atomic_store_n(word, 2, __ATOMIC_RELEASE);
  } else {
    while (__atomic_load_n(word, __ATOMIC_ACQUIRE) != 2) sched_yield();
  }
  cpu_ret(c, 0);
}

/* --- thread-specific data --- */

#define MAX_KEYS 128 /* bionic's PTHREAD_KEYS_MAX */

static uint32_t key_used[MAX_KEYS], key_dtor[MAX_KEYS];
static __thread uint32_t key_value[MAX_KEYS];

static void h_key_create(cpu *c) {
  pthread_mutex_lock(&table_lock);
  int k = 0;
  while (k < MAX_KEYS && key_used[k]) k++;
  if (k < MAX_KEYS) {
    key_used[k] = 1;
    key_dtor[k] = cpu_arg(c, 1); /* run at guest thread exit */
  }
  pthread_mutex_unlock(&table_lock);
  if (k == MAX_KEYS) {
    cpu_ret(c, G_EAGAIN);
    return;
  }
  gm_w32(cpu_arg(c, 0), (uint32_t)k);
  cpu_ret(c, 0);
}

static void h_key_delete(cpu *c) {
  uint32_t k = cpu_arg(c, 0);
  if (k < MAX_KEYS) key_used[k] = 0;
  cpu_ret(c, k < MAX_KEYS ? 0 : G_EINVAL);
}

static void h_getspecific(cpu *c) {
  uint32_t k = cpu_arg(c, 0);
  cpu_ret(c, k < MAX_KEYS ? key_value[k] : 0);
}

static void h_setspecific(cpu *c) {
  uint32_t k = cpu_arg(c, 0);
  if (k >= MAX_KEYS || !key_used[k]) {
    cpu_ret(c, G_EINVAL);
    return;
  }
  key_value[k] = cpu_arg(c, 1);
  cpu_ret(c, 0);
}

/* At guest thread exit, as bionic does: up to 4 passes calling each key's
 * destructor on its non-NULL value, clearing the value first. */
void pthread_run_key_destructors(cpu *c) {
  for (int pass = 0; pass < 4; pass++) {
    int called = 0;
    for (int k = 0; k < MAX_KEYS; k++) {
      uint32_t v = key_value[k], dtor = key_dtor[k];
      if (!v || !dtor || !key_used[k]) continue;
      key_value[k] = 0;
      cpu_call(c, dtor, 1, &v);
      called = 1;
      if (c->faulted) return;
    }
    if (!called) return;
  }
}

const dynlib_impl reimpl_pthread[] = {
    {"pthread_cond_broadcast", h_cond_broadcast},
    {"pthread_cond_destroy", h_cond_destroy},
    {"pthread_cond_init", h_cond_init},
    {"pthread_cond_signal", h_cond_signal},
    {"pthread_cond_wait", h_cond_wait},
    {"pthread_getspecific", h_getspecific},
    {"pthread_key_create", h_key_create},
    {"pthread_key_delete", h_key_delete},
    {"pthread_mutex_destroy", h_mutex_destroy},
    {"pthread_mutex_init", h_mutex_init},
    {"pthread_mutex_lock", h_mutex_lock},
    {"pthread_mutex_unlock", h_mutex_unlock},
    {"pthread_mutexattr_init", h_mutexattr_init},
    {"pthread_mutexattr_settype", h_mutexattr_settype},
    {"pthread_once", h_once},
    {"pthread_setspecific", h_setspecific},
    {"sem_destroy", h_sem_destroy},
    {"sem_init", h_sem_init},
    {"sem_post", h_sem_post},
    {"sem_trywait", h_sem_trywait},
    {"sem_wait", h_sem_wait},
    {NULL, NULL},
};
