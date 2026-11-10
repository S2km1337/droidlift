/*
 * Time and the rand48 generators.
 *
 * Guest layouts (bionic, 32-bit ARM): time_t and long are 32-bit, so
 * struct timeval and struct timespec are two 32-bit words. struct tm is nine
 * ints, then long tm_gmtoff and const char *tm_zone (44 bytes). Clock ids are
 * Linux's.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#include "../cpu.h"
#include "../guest_mem.h"
#include "reimpl.h"

#define G_TM_SIZE 44
#define G_EINVAL 22

static void h_time(cpu *c) {
  uint32_t t = (uint32_t)time(NULL);
  if (cpu_arg(c, 0)) gm_w32(cpu_arg(c, 0), t);
  cpu_ret(c, t);
}

static void h_gettimeofday(cpu *c) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  uint32_t p = cpu_arg(c, 0);
  if (p) {
    gm_w32(p, (uint32_t)tv.tv_sec);
    gm_w32(p + 4, (uint32_t)tv.tv_usec);
  }
  cpu_ret(c, 0);
}

static void h_clock_gettime(cpu *c) {
  clockid_t id;
  switch (cpu_arg(c, 0)) {
  case 0: id = CLOCK_REALTIME; break;
  case 1: /* CLOCK_MONOTONIC */
  case 4: /* CLOCK_MONOTONIC_RAW */
  case 7: /* CLOCK_BOOTTIME */ id = CLOCK_MONOTONIC; break;
  case 2: id = CLOCK_PROCESS_CPUTIME_ID; break;
  case 3: id = CLOCK_THREAD_CPUTIME_ID; break;
  default:
    gm_w32(guest_errno_addr(), G_EINVAL);
    cpu_ret(c, (uint32_t)-1);
    return;
  }
  struct timespec ts;
  clock_gettime(id, &ts);
  gm_w32(cpu_arg(c, 1), (uint32_t)ts.tv_sec);
  gm_w32(cpu_arg(c, 1) + 4, (uint32_t)ts.tv_nsec);
  cpu_ret(c, 0);
}

static void h_usleep(cpu *c) {
  usleep(cpu_arg(c, 0));
  cpu_ret(c, 0);
}

/* --- struct tm --- */

static void tm_to_guest(uint32_t g, const struct tm *t) {
  const int f[9] = {t->tm_sec, t->tm_min, t->tm_hour, t->tm_mday, t->tm_mon,
                    t->tm_year, t->tm_wday, t->tm_yday, t->tm_isdst};
  for (int i = 0; i < 9; i++) gm_w32(g + 4 * (uint32_t)i, (uint32_t)f[i]);
  gm_w32(g + 36, (uint32_t)t->tm_gmtoff);
  gm_w32(g + 40, 0); /* tm_zone: no guest copy of the zone name */
}

static void tm_from_guest(struct tm *t, uint32_t g) {
  memset(t, 0, sizeof *t);
  int *f[9] = {&t->tm_sec, &t->tm_min, &t->tm_hour, &t->tm_mday, &t->tm_mon,
               &t->tm_year, &t->tm_wday, &t->tm_yday, &t->tm_isdst};
  for (int i = 0; i < 9; i++) *f[i] = (int32_t)gm_r32(g + 4 * (uint32_t)i);
  t->tm_gmtoff = (int32_t)gm_r32(g + 36);
}

static void h_localtime(cpu *c) {
  static uint32_t tm_buf;
  if (!tm_buf) tm_buf = guest_malloc(G_TM_SIZE);
  time_t t = (int32_t)gm_r32(cpu_arg(c, 0));
  struct tm h;
  localtime_r(&t, &h);
  tm_to_guest(tm_buf, &h);
  cpu_ret(c, tm_buf);
}

static void h_gmtime_r(cpu *c) {
  time_t t = (int32_t)gm_r32(cpu_arg(c, 0));
  struct tm h;
  gmtime_r(&t, &h);
  tm_to_guest(cpu_arg(c, 1), &h);
  cpu_ret(c, cpu_arg(c, 1));
}

static void h_localtime_r(cpu *c) {
  time_t t = (int32_t)gm_r32(cpu_arg(c, 0));
  struct tm h;
  localtime_r(&t, &h);
  tm_to_guest(cpu_arg(c, 1), &h);
  cpu_ret(c, cpu_arg(c, 1));
}

static void h_gmtime(cpu *c) {
  static uint32_t tm_buf;
  if (!tm_buf) tm_buf = guest_malloc(G_TM_SIZE);
  time_t t = (int32_t)gm_r32(cpu_arg(c, 0));
  struct tm h;
  gmtime_r(&t, &h);
  tm_to_guest(tm_buf, &h);
  cpu_ret(c, tm_buf);
}

/* mktime(tm): local time; normalises the guest's struct in place. */
static void h_mktime(cpu *c) {
  struct tm t;
  tm_from_guest(&t, cpu_arg(c, 0));
  time_t r = mktime(&t);
  if (r != (time_t)-1) tm_to_guest(cpu_arg(c, 0), &t);
  cpu_ret(c, (uint32_t)(int32_t)r);
}

/* difftime(time_t t1, time_t t0): 32-bit arguments, a double result (softfp). */
static void h_difftime(cpu *c) {
  double d = (double)(int32_t)cpu_arg(c, 0) - (double)(int32_t)cpu_arg(c, 1);
  uint64_t b;
  memcpy(&b, &d, sizeof b);
  cpu_ret64(c, b);
}

/* clock(): CPU time; CLOCKS_PER_SEC is 1000000 on both bionic and the host. */
static void h_clock(cpu *c) { cpu_ret(c, (uint32_t)clock()); }

static void h_sleep(cpu *c) { cpu_ret(c, sleep(cpu_arg(c, 0))); }

/* strftime(buf, max, fmt, tm) in the C locale. */
static void h_strftime(cpu *c) {
  struct tm t;
  tm_from_guest(&t, cpu_arg(c, 3));
  cpu_ret(c, (uint32_t)strftime(g2h(cpu_arg(c, 0)), cpu_arg(c, 1), g2h(cpu_arg(c, 2)), &t));
}

/* --- rand48: one 48-bit state, as in bionic --- */

static unsigned short rand48_state[3] = {0x330e, 0, 0};

static void h_srand48(cpu *c) {
  uint32_t seed = cpu_arg(c, 0);
  rand48_state[0] = 0x330e;
  rand48_state[1] = (unsigned short)seed;
  rand48_state[2] = (unsigned short)(seed >> 16);
}

static void h_lrand48(cpu *c) { cpu_ret(c, (uint32_t)nrand48(rand48_state)); }

/* jrand48(unsigned short xsubi[3]): the state is in guest memory. */
static void h_jrand48(cpu *c) {
  unsigned short x[3];
  memcpy(x, g2h(cpu_arg(c, 0)), sizeof x);
  long r = jrand48(x);
  memcpy(g2h(cpu_arg(c, 0)), x, sizeof x);
  cpu_ret(c, (uint32_t)(int32_t)r);
}

const dynlib_impl reimpl_time[] = {
    {"clock_gettime", h_clock_gettime}, {"gettimeofday", h_gettimeofday}, {"gmtime_r", h_gmtime_r},
    {"jrand48", h_jrand48},             {"localtime", h_localtime},       {"lrand48", h_lrand48},
    {"srand48", h_srand48},             {"strftime", h_strftime},         {"time", h_time},
    {"usleep", h_usleep},               {"localtime_r", h_localtime_r},   {"gmtime", h_gmtime},
    {"mktime", h_mktime},               {"difftime", h_difftime},         {"clock", h_clock},
    {"sleep", h_sleep},                 {NULL, NULL},
};
