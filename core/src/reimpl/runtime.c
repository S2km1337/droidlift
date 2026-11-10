/*
 * C/C++ runtime glue: atexit registration, errno, and the calls that end the
 * process (abort, failed asserts, stack-protector and pure-virtual traps).
 *
 * The guest never exits normally (Android kills the process), so atexit
 * handlers are recorded but never run. Fatal calls fault the guest call that
 * made them, so tests fail loudly instead of the host exiting.
 */
#include <errno.h>
#include <stdio.h>

#include "../cpu.h"
#include "../guest_mem.h"
#include "../so_util.h"
#include "reimpl.h"

#define MAX_ATEXIT 1024

static struct {
  uint32_t fn, arg, dso;
} atexit_list[MAX_ATEXIT];
static int n_atexit;

static __thread uint32_t errno_addr; /* per guest thread: they run on host threads */

static void guest_fatal(cpu *c, const char *what) {
  fprintf(stderr, "[runtime] guest %s (lr 0x%08x)\n", what, c->ops->get_reg(c, CPU_REG_LR));
  c->faulted = 1;
  c->ops->halt(c);
}

static void register_atexit(cpu *c, uint32_t fn, uint32_t arg, uint32_t dso) {
  if (n_atexit == MAX_ATEXIT) {
    cpu_ret(c, (uint32_t)-1);
    return;
  }
  atexit_list[n_atexit].fn = fn;
  atexit_list[n_atexit].arg = arg;
  atexit_list[n_atexit].dso = dso;
  n_atexit++;
  cpu_ret(c, 0);
}

/* __cxa_atexit(void (*fn)(void *), void *arg, void *dso) */
static void h_cxa_atexit(cpu *c) { register_atexit(c, cpu_arg(c, 0), cpu_arg(c, 1), cpu_arg(c, 2)); }

/* __aeabi_atexit(void *arg, void (*fn)(void *), void *dso): arguments swapped. */
static void h_aeabi_atexit(cpu *c) {
  register_atexit(c, cpu_arg(c, 1), cpu_arg(c, 0), cpu_arg(c, 2));
}

static void h_cxa_finalize(cpu *c) { (void)c; }

uint32_t guest_errno_addr(void) {
  if (!errno_addr) errno_addr = guest_malloc(4);
  return errno_addr;
}

int linux_errno(int host_errno) {
  /* Linux and Darwin share errno numbers 1-34; map the common ones above,
   * including the socket errors a game's network code tests for. */
  switch (host_errno) {
  case EAGAIN: return 11;
  case ENAMETOOLONG: return 36;
  case ENOSYS: return 38;
  case ENOTEMPTY: return 39;
  case ELOOP: return 40;
  case EDESTADDRREQ: return 89;
  case ENOTSOCK: return 88;
  case EMSGSIZE: return 90;
  case EPROTOTYPE: return 91;
  case ENOPROTOOPT: return 92;
  case EPROTONOSUPPORT: return 93;
  case EOPNOTSUPP: return 95;
  case EAFNOSUPPORT: return 97;
  case EADDRINUSE: return 98;
  case EADDRNOTAVAIL: return 99;
  case ENETDOWN: return 100;
  case ENETUNREACH: return 101;
  case ENETRESET: return 102;
  case ECONNABORTED: return 103;
  case ECONNRESET: return 104;
  case ENOBUFS: return 105;
  case EISCONN: return 106;
  case ENOTCONN: return 107;
  case ETIMEDOUT: return 110;
  case ECONNREFUSED: return 111;
  case EHOSTDOWN: return 112;
  case EHOSTUNREACH: return 113;
  case EALREADY: return 114;
  case EINPROGRESS: return 115;
  default: return host_errno > 0 && host_errno <= 34 ? host_errno : 5 /* EIO */;
  }
}

void guest_set_errno(int host_errno) { gm_w32(guest_errno_addr(), (uint32_t)linux_errno(host_errno)); }

static void h_errno(cpu *c) { cpu_ret(c, guest_errno_addr()); }

static void h_abort(cpu *c) { guest_fatal(c, "abort()"); }

/* exit / _exit: the game gave up (LuaJIT's panic handler calls exit(1)).
 * Stop it like abort rather than returning into code that can't go on. */
static void h_exit(cpu *c) {
  char why[32];
  snprintf(why, sizeof why, "exit(%d)", (int32_t)cpu_arg(c, 0));
  guest_fatal(c, why);
}

static void h_stack_chk_fail(cpu *c) { guest_fatal(c, "stack smashing detected"); }

static void h_pure_virtual(cpu *c) { guest_fatal(c, "pure virtual call"); }

/* __assert2(const char *file, int line, const char *func, const char *expr) */
static void h_assert2(cpu *c) {
  fprintf(stderr, "[runtime] assertion \"%s\" failed: %s:%u (%s)\n",
          (const char *)g2h(cpu_arg(c, 3)), (const char *)g2h(cpu_arg(c, 0)), cpu_arg(c, 1),
          (const char *)g2h(cpu_arg(c, 2)));
  guest_fatal(c, "assert");
}

/* __gnu_Unwind_Find_exidx(pc, int *count): the .ARM.exidx table of the module
 * containing pc, for the unwinder that C++ exceptions use (dl_unwind_find_exidx
 * in bionic's linker). */
static void h_unwind_find_exidx(cpu *c) {
  const so_module *m = so_module_containing(cpu_arg(c, 0));
  uint32_t count = cpu_arg(c, 1);
  if (!m || !m->exidx) {
    if (count) gm_w32(count, 0);
    cpu_ret(c, 0);
    return;
  }
  if (count) gm_w32(count, m->exidx_count);
  cpu_ret(c, m->exidx);
}

int runtime_atexit_count(void) { return n_atexit; }

const dynlib_impl reimpl_runtime[] = {
    {"__aeabi_atexit", h_aeabi_atexit},
    {"__assert2", h_assert2},
    {"__cxa_atexit", h_cxa_atexit},
    {"__cxa_finalize", h_cxa_finalize},
    {"__cxa_pure_virtual", h_pure_virtual},
    {"__errno", h_errno},
    {"__gnu_Unwind_Find_exidx", h_unwind_find_exidx},
    {"__stack_chk_fail", h_stack_chk_fail},
    {"_exit", h_exit},
    {"abort", h_abort},
    {"exit", h_exit},
    {NULL, NULL},
};
