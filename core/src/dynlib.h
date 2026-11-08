/*
 * Host-provided imports: every symbol the three libraries import from Android
 * system libraries (libc, libm, GLES, ...) must be named here, modelled on
 * leos-fortune-vita's default_dynlib[] table.
 *
 * Each function import gets a guest thunk:
 *     svc #(DYNLIB_SVC_BASE | index)
 *     bx  lr
 * The CPU backend traps the SVC and calls dynlib_dispatch, which runs the host
 * implementation from reimpl/ or, until one exists, logs the call (a logging
 * stub).
 */
#ifndef DYNLIB_H
#define DYNLIB_H

#include <stdint.h>

struct cpu;

/* Host implementation of an import: reads its arguments with cpu_arg and
 * returns through r0 (and r1). */
typedef void (*dl_handler)(struct cpu *c);

#define DYNLIB_SVC_BASE 0x800000u

typedef enum {
  DL_STUB,     /* function: guest thunk -> host handler or logging stub */
  DL_DATA,     /* data object: zeroed guest block, filled in by the shims */
  DL_WEAK_NULL /* weak import Android leaves unresolved: binds to 0 */
} dl_kind;

typedef struct dynlib_entry {
  const char *lib;
  const char *name;
  dl_kind kind;
  uint32_t size;  /* DL_DATA only */
  uint32_t addr;  /* guest address after dynlib_init() */
  uint32_t calls; /* times dispatched */
  dl_handler handler; /* NULL: logging stub */
} dynlib_entry;

int dynlib_init(void);
const dynlib_entry *dynlib_find(const char *name);
int dynlib_count(void);
const dynlib_entry *dynlib_at(int index);

/* Host implementations, bound by name in dynlib_init. */
typedef struct dynlib_impl {
  const char *name;
  dl_handler handler;
} dynlib_impl;

/* Called through cpu_svc for svc #(DYNLIB_SVC_BASE | index). */
void dynlib_dispatch(struct cpu *c, uint32_t index);

/* Host functions that aren't imports but that guest code reaches through a
 * pointer: dlsym and eglGetProcAddress results, and the vtables of OpenSL ES
 * objects. Each gets a guest thunk
 *     svc #(DYNLIB_HOST_SVC | index)
 *     bx  lr
 * Returns the thunk's guest address (ARM state), or 0 if the table is full.
 * The name is for logs only. */
#define DYNLIB_HOST_SVC 0x420000u
#define DYNLIB_HOST_MASK 0xFF0000u
uint32_t dynlib_host_thunk(const char *name, dl_handler fn);
void dynlib_host_dispatch(struct cpu *c, uint32_t index);
uint32_t dynlib_unimplemented_calls(void);

#endif
