/*
 * setjmp/longjmp and the signal calls.
 *
 * setjmp saves the callee-saved state the AAPCS requires (r4-r11, sp, the
 * return address, d8-d15, fpscr) in the guest's jmp_buf (bionic: 64 words, so
 * our layout fits); longjmp restores it and returns through the import thunk's
 * `bx lr` to the setjmp call site. Lua's error handling relies on this.
 * A jump must not cross a cpu_callback boundary; nothing does that so far.
 *
 * The guest gets no signals: sigaction and sigprocmask record nothing and
 * succeed. Code that probes CPU features with SIGILL (OpenSSL) then just runs
 * the probe, which our backends execute.
 */
#include <stdio.h>
#include <string.h>

#include "../cpu.h"
#include "../guest_mem.h"
#include "reimpl.h"

#define JB_MAGIC 0x524A4D50u /* "PMJR" */
enum { JB_TAG, JB_R4, JB_SP = JB_R4 + 8, JB_LR, JB_D8, JB_FPSCR = JB_D8 + 16, JB_WORDS };

static void save(cpu *c, uint32_t buf) {
  gm_w32(buf + JB_TAG * 4, JB_MAGIC);
  for (int i = 0; i < 8; i++) gm_w32(buf + (JB_R4 + i) * 4, c->ops->get_reg(c, 4 + i));
  gm_w32(buf + JB_SP * 4, c->ops->get_reg(c, CPU_REG_SP));
  gm_w32(buf + JB_LR * 4, c->ops->get_reg(c, CPU_REG_LR));
  for (int i = 0; i < 16; i++) gm_w32(buf + (JB_D8 + i) * 4, c->ops->get_ext(c, 16 + i));
  gm_w32(buf + JB_FPSCR * 4, c->ops->get_fpscr(c));
  cpu_ret(c, 0);
}

static void restore(cpu *c, uint32_t buf, uint32_t val) {
  if (gm_r32(buf + JB_TAG * 4) != JB_MAGIC) {
    fprintf(stderr, "[signal] longjmp to an unset jmp_buf 0x%08x\n", buf);
    c->faulted = 1;
    c->ops->halt(c);
    return;
  }
  for (int i = 0; i < 8; i++) c->ops->set_reg(c, 4 + i, gm_r32(buf + (JB_R4 + i) * 4));
  c->ops->set_reg(c, CPU_REG_SP, gm_r32(buf + JB_SP * 4));
  c->ops->set_reg(c, CPU_REG_LR, gm_r32(buf + JB_LR * 4));
  for (int i = 0; i < 16; i++) c->ops->set_ext(c, 16 + i, gm_r32(buf + (JB_D8 + i) * 4));
  c->ops->set_fpscr(c, gm_r32(buf + JB_FPSCR * 4));
  cpu_ret(c, val ? val : 1);
}

static void h_setjmp(cpu *c) { save(c, cpu_arg(c, 0)); }

/* sigsetjmp(buf, savemask): no signal mask to save. */
static void h_sigsetjmp(cpu *c) { save(c, cpu_arg(c, 0)); }

static void h_longjmp(cpu *c) { restore(c, cpu_arg(c, 0), cpu_arg(c, 1)); }

/* sigaction(sig, act, oldact): oldact reads back as SIG_DFL. bionic's struct
 * sigaction is 16 bytes on ARM. */
static void h_sigaction(cpu *c) {
  if (cpu_arg(c, 2)) memset(g2h(cpu_arg(c, 2)), 0, 16);
  cpu_ret(c, 0);
}

/* sigprocmask(how, set, oldset): oldset reads back empty (bionic sigset_t is 4 bytes). */
static void h_sigprocmask(cpu *c) {
  if (cpu_arg(c, 2)) gm_w32(cpu_arg(c, 2), 0);
  cpu_ret(c, 0);
}

const dynlib_impl reimpl_signal[] = {
    {"longjmp", h_longjmp},       {"setjmp", h_setjmp},           {"sigaction", h_sigaction},
    {"siglongjmp", h_longjmp},    {"sigprocmask", h_sigprocmask}, {"sigsetjmp", h_sigsetjmp},
    {NULL, NULL},
};
