/*
 * File descriptors, file-system calls and process identity.
 *
 * Guest fds are host fds (as stdio and AAsset_openFileDescriptor already
 * hand out), except that fds 0-2 belong to the host: closing or reading them
 * fails with EBADF, and writes to 1 and 2 go to the guest's stdout and stderr
 * streams. Fake sockets (net.c) live in their own fd range.
 *
 * Paths follow stdio's policy: only the data directory is reachable, plus
 * /dev/urandom and /dev/random for reading (OpenSSL seeds from them), and
 * the virtual files below. Everything else fails with ENOENT, like system
 * files on an unrooted phone.
 *
 * Guest layouts (bionic, 32-bit ARM, Linux numbers): open flags and fcntl
 * commands are Linux's; struct stat is the kernel's 104-byte stat64; struct
 * dirent is 280 bytes (u64 ino, s64 off, u16 reclen, u8 type, name[256]).
 * Mode bits and access() bits are the same on both sides.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "../cpu.h"
#include "../guest_mem.h"
#include "reimpl.h"

/* Linux open flags on ARM. */
#define L_O_ACCMODE 03
#define L_O_CREAT 0100
#define L_O_EXCL 0200
#define L_O_NOCTTY 0400
#define L_O_TRUNC 01000
#define L_O_APPEND 02000
#define L_O_NONBLOCK 04000
#define L_O_DIRECTORY 040000
#define L_O_NOFOLLOW 0100000
#define L_O_CLOEXEC 02000000

#define L_EBADF 9
#define L_EINVAL 22
#define L_ENOSYS 38

#define G_STAT_SIZE 104
#define G_DIRENT_SIZE 280
#define MAX_DIRS 16

/* Fixed identity of the app process: an ordinary app uid, never root. */
#define G_PID 4242
#define G_PPID 1
#define G_UID 10123

static uint32_t guest_umask = 022;

static struct {
  uint32_t g; /* guest DIR handle: a guest block holding the dirent */
  DIR *h;
} dirs[MAX_DIRS];

static void fail(cpu *c, int linux_errno) {
  gm_w32(guest_errno_addr(), (uint32_t)linux_errno);
  cpu_ret(c, (uint32_t)-1);
}

/* Returns r (0 or a count) to the guest, or -1 with errno from the host. */
static void ret_host(cpu *c, long r) {
  if (r < 0) {
    guest_set_errno(errno);
    cpu_ret(c, (uint32_t)-1);
    return;
  }
  cpu_ret(c, (uint32_t)r);
}

static int path_ok(const char *call, const char *path) {
  if (stdio_path_allowed(path)) return 1;
  fprintf(stderr, "[file] %s(\"%s\"): outside the data directory, ENOENT\n", call, path);
  return 0;
}

/* /proc/cpuinfo as a 2-core ARMv7 Cortex-A15 kernel shows it, with the same
 * features getauxval reports (libc.c). FMOD's CPU detection parses the
 * Processor, Features and CPU architecture lines, and EventSystem::init fails
 * when the file is missing. */
static const char cpuinfo[] =
    "Processor\t: ARMv7 Processor rev 0 (v7l)\n"
    "processor\t: 0\n"
    "BogoMIPS\t: 38.40\n"
    "\n"
    "processor\t: 1\n"
    "BogoMIPS\t: 38.40\n"
    "\n"
    "Features\t: swp half thumb fastmult vfp edsp neon vfpv3 tls vfpd32\n"
    "CPU implementer\t: 0x41\n"
    "CPU architecture: 7\n"
    "CPU variant\t: 0x0\n"
    "CPU part\t: 0xc0f\n"
    "CPU revision\t: 0\n"
    "\n"
    "Hardware\t: droidlift iOS\n"
    "Revision\t: 0000\n"
    "Serial\t\t: 0000000000000000\n";

int fileio_open_virtual(const char *path) {
  if (strcmp(path, "/proc/cpuinfo") != 0) return -1;
  /* An unlinked file in the data directory, so read, lseek and fstat all
   * behave like a regular file. */
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof tmp, "%s/.vfile-XXXXXX", stdio_data_root());
  int fd = mkstemp(tmp);
  if (fd < 0) return -1;
  unlink(tmp);
  if (write(fd, cpuinfo, sizeof cpuinfo - 1) != (ssize_t)(sizeof cpuinfo - 1) ||
      lseek(fd, 0, SEEK_SET) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int host_fd_ok(int fd) { return fd > 2 && !net_is_socket(fd); }

static int open_flags_to_host(uint32_t f) {
  int h = (f & L_O_ACCMODE) == 1 ? O_WRONLY : (f & L_O_ACCMODE) == 2 ? O_RDWR : O_RDONLY;
  if (f & L_O_CREAT) h |= O_CREAT;
  if (f & L_O_EXCL) h |= O_EXCL;
  if (f & L_O_NOCTTY) h |= O_NOCTTY;
  if (f & L_O_TRUNC) h |= O_TRUNC;
  if (f & L_O_APPEND) h |= O_APPEND;
  if (f & L_O_NONBLOCK) h |= O_NONBLOCK;
  if (f & L_O_DIRECTORY) h |= O_DIRECTORY;
  if (f & L_O_NOFOLLOW) h |= O_NOFOLLOW;
  if (f & L_O_CLOEXEC) h |= O_CLOEXEC;
  return h;
}

static uint32_t status_flags_to_guest(int h) {
  uint32_t f = (h & O_ACCMODE) == O_WRONLY ? 1 : (h & O_ACCMODE) == O_RDWR ? 2 : 0;
  if (h & O_APPEND) f |= L_O_APPEND;
  if (h & O_NONBLOCK) f |= L_O_NONBLOCK;
  return f;
}

/* open(path, flags, mode) */
static void h_open(cpu *c) {
  const char *path = g2h(cpu_arg(c, 0));
  uint32_t flags = cpu_arg(c, 1);
  int is_random = !strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random");
  if ((flags & L_O_ACCMODE) == 0) {
    int fd = fileio_open_virtual(path);
    if (fd >= 0) {
      cpu_ret(c, (uint32_t)fd);
      return;
    }
  }
  if (is_random ? (flags & L_O_ACCMODE) != 0 : !path_ok("open", path)) {
    fail(c, 2 /* ENOENT */);
    return;
  }
  if (!is_random && getenv("DL_TRACE_FILES"))
    fprintf(stderr, "[file] open(\"%s\", 0x%x)\n", path, flags);
  /* Both random devices read from the host's urandom, which never blocks. */
  ret_host(c, open(is_random ? "/dev/urandom" : path, open_flags_to_host(flags),
                   (mode_t)(cpu_arg(c, 2) & 07777)));
}

static void h_close(cpu *c) {
  int fd = (int)cpu_arg(c, 0);
  if (net_is_socket(fd)) {
    cpu_ret(c, (uint32_t)net_close(fd));
    return;
  }
  if (!host_fd_ok(fd)) {
    fail(c, L_EBADF);
    return;
  }
  ret_host(c, close(fd));
}

static void h_read(cpu *c) {
  int fd = (int)cpu_arg(c, 0);
  if (!host_fd_ok(fd)) {
    fail(c, L_EBADF);
    return;
  }
  ret_host(c, read(fd, g2h(cpu_arg(c, 1)), cpu_arg(c, 2)));
}

static void h_write(cpu *c) {
  int fd = (int)cpu_arg(c, 0);
  uint32_t n = cpu_arg(c, 2);
  if (fd == 1 || fd == 2) {
    cpu_ret(c, (uint32_t)stdio_write(stdio_stream(fd), g2h(cpu_arg(c, 1)), n));
    return;
  }
  if (!host_fd_ok(fd)) {
    fail(c, L_EBADF);
    return;
  }
  ret_host(c, write(fd, g2h(cpu_arg(c, 1)), n));
}

/* lseek(fd, off_t offset, whence): off_t is 32-bit; SEEK_* match. */
static void h_lseek(cpu *c) {
  int fd = (int)cpu_arg(c, 0);
  if (!host_fd_ok(fd)) {
    fail(c, L_EBADF);
    return;
  }
  off_t r = lseek(fd, (int32_t)cpu_arg(c, 1), (int)cpu_arg(c, 2));
  if (r > INT32_MAX) {
    fail(c, 75 /* EOVERFLOW */);
    return;
  }
  ret_host(c, (long)r);
}

static void h_ftruncate(cpu *c) {
  int fd = (int)cpu_arg(c, 0);
  if (!host_fd_ok(fd)) {
    fail(c, L_EBADF);
    return;
  }
  ret_host(c, ftruncate(fd, (int32_t)cpu_arg(c, 1)));
}

/* poll(struct pollfd *fds, nfds, timeout): 8-byte entries {int fd; short
 * events, revents}; the event bits match. Host fds are polled for real; fake
 * sockets report a hang-up, anything else POLLNVAL. */
static void h_poll(cpu *c) {
  uint32_t g = cpu_arg(c, 0), n = cpu_arg(c, 1);
  struct pollfd p[64];
  if (n > 64) {
    fail(c, L_EINVAL);
    return;
  }
  for (uint32_t i = 0; i < n; i++) {
    int fd = (int32_t)gm_r32(g + 8 * i);
    p[i].fd = host_fd_ok(fd) ? fd : -1;
    memcpy(&p[i].events, g2h(g + 8 * i + 4), 2);
    p[i].revents = 0;
  }
  int r = poll(p, n, (int32_t)cpu_arg(c, 2));
  if (r < 0) {
    ret_host(c, r);
    return;
  }
  for (uint32_t i = 0; i < n; i++) {
    int fd = (int32_t)gm_r32(g + 8 * i);
    short rev = p[i].revents;
    struct stat st;
    /* macOS poll can't wait on character devices (POLLNVAL); the only ones
     * the guest can open are the random devices, which are always ready. */
    if ((rev & POLLNVAL) && p[i].fd >= 0 && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode)) {
      rev = p[i].events & (POLLIN | POLLRDNORM);
      if (!rev) r--; /* host poll counted the POLLNVAL */
    }
    if (net_is_socket(fd)) rev = POLLHUP;
    else if (fd >= 0 && !host_fd_ok(fd)) rev = POLLNVAL;
    if (p[i].fd < 0 && rev) r++;
    memcpy(g2h(g + 8 * i + 6), &rev, 2);
  }
  cpu_ret(c, (uint32_t)r);
}

/* --- stat --- */

static void w64(uint32_t g, uint64_t v) {
  gm_w32(g, (uint32_t)v);
  gm_w32(g + 4, (uint32_t)(v >> 32));
}

static void stat_to_guest(uint32_t g, const struct stat *st) {
  memset(g2h(g), 0, G_STAT_SIZE);
  w64(g + 0, (uint64_t)st->st_dev);
  gm_w32(g + 12, (uint32_t)st->st_ino);
  gm_w32(g + 16, st->st_mode);
  gm_w32(g + 20, st->st_nlink);
  gm_w32(g + 24, G_UID);
  gm_w32(g + 28, G_UID);
  w64(g + 32, (uint64_t)st->st_rdev);
  w64(g + 48, (uint64_t)st->st_size);
  gm_w32(g + 56, (uint32_t)st->st_blksize);
  w64(g + 64, (uint64_t)st->st_blocks);
  gm_w32(g + 72, (uint32_t)st->st_atimespec.tv_sec);
  gm_w32(g + 76, (uint32_t)st->st_atimespec.tv_nsec);
  gm_w32(g + 80, (uint32_t)st->st_mtimespec.tv_sec);
  gm_w32(g + 84, (uint32_t)st->st_mtimespec.tv_nsec);
  gm_w32(g + 88, (uint32_t)st->st_ctimespec.tv_sec);
  gm_w32(g + 92, (uint32_t)st->st_ctimespec.tv_nsec);
  w64(g + 96, (uint64_t)st->st_ino);
}

static void h_stat(cpu *c) {
  const char *path = g2h(cpu_arg(c, 0));
  struct stat st;
  if (!path_ok("stat", path)) {
    fail(c, 2);
    return;
  }
  if (stat(path, &st) != 0) {
    ret_host(c, -1);
    return;
  }
  stat_to_guest(cpu_arg(c, 1), &st);
  cpu_ret(c, 0);
}

static void h_fstat(cpu *c) {
  int fd = (int)cpu_arg(c, 0);
  struct stat st;
  if (!host_fd_ok(fd)) {
    fail(c, L_EBADF);
    return;
  }
  if (fstat(fd, &st) != 0) {
    ret_host(c, -1);
    return;
  }
  stat_to_guest(cpu_arg(c, 1), &st);
  cpu_ret(c, 0);
}

/* fcntl(fd, cmd, arg): descriptor and status flags; locks always succeed
 * (only this process touches the data directory). */
static void h_fcntl(cpu *c) {
  int fd = (int)cpu_arg(c, 0);
  uint32_t cmd = cpu_arg(c, 1), arg = cpu_arg(c, 2);
  if (net_is_socket(fd)) {
    cpu_ret(c, cmd == 3 /* F_GETFL */ ? 2 /* O_RDWR */ : 0);
    return;
  }
  if (!host_fd_ok(fd)) {
    fail(c, L_EBADF);
    return;
  }
  switch (cmd) {
  case 1: /* F_GETFD */ ret_host(c, fcntl(fd, F_GETFD)); return;
  case 2: /* F_SETFD */ ret_host(c, fcntl(fd, F_SETFD, (arg & 1) ? FD_CLOEXEC : 0)); return;
  case 3: { /* F_GETFL */
    int r = fcntl(fd, F_GETFL);
    if (r < 0) ret_host(c, r);
    else cpu_ret(c, status_flags_to_guest(r));
    return;
  }
  case 4: /* F_SETFL: only O_APPEND and O_NONBLOCK can change */
    ret_host(c, fcntl(fd, F_SETFL, open_flags_to_host(arg & (L_O_APPEND | L_O_NONBLOCK))));
    return;
  case 5: case 6: case 7: case 12: case 13: case 14: /* F_GETLK/SETLK/SETLKW(64) */
    if (cmd == 5 || cmd == 12) gm_w32(arg, (gm_r32(arg) & 0xffff0000u) | 2 /* F_UNLCK */);
    cpu_ret(c, 0);
    return;
  default:
    fprintf(stderr, "[file] fcntl(%d, %u) unsupported\n", fd, cmd);
    fail(c, L_EINVAL);
  }
}

/* --- paths --- */

static void h_access(cpu *c) {
  const char *path = g2h(cpu_arg(c, 0));
  if (!path_ok("access", path)) {
    fail(c, 2);
    return;
  }
  ret_host(c, access(path, (int)(cpu_arg(c, 1) & 7)));
}

static void h_mkdir(cpu *c) {
  const char *path = g2h(cpu_arg(c, 0));
  if (!path_ok("mkdir", path)) {
    fail(c, 13 /* EACCES */);
    return;
  }
  ret_host(c, mkdir(path, (mode_t)(cpu_arg(c, 1) & ~guest_umask & 07777)));
}

static void h_rmdir(cpu *c) {
  const char *path = g2h(cpu_arg(c, 0));
  if (!path_ok("rmdir", path)) {
    fail(c, 2);
    return;
  }
  ret_host(c, rmdir(path));
}

static void h_unlink(cpu *c) {
  const char *path = g2h(cpu_arg(c, 0));
  if (!path_ok("unlink", path)) {
    fail(c, 2);
    return;
  }
  ret_host(c, unlink(path));
}

static void h_rename(cpu *c) {
  const char *from = g2h(cpu_arg(c, 0)), *to = g2h(cpu_arg(c, 1));
  if (!path_ok("rename", from) || !path_ok("rename", to)) {
    fail(c, 2);
    return;
  }
  if (getenv("DL_TRACE_FILES")) fprintf(stderr, "[file] rename(\"%s\", \"%s\")\n", from, to);
  ret_host(c, rename(from, to));
}

static void h_chmod(cpu *c) {
  const char *path = g2h(cpu_arg(c, 0));
  if (!path_ok("chmod", path)) {
    fail(c, 2);
    return;
  }
  ret_host(c, chmod(path, (mode_t)(cpu_arg(c, 1) & 07777)));
}

/* utime(path, const struct utimbuf *times): two 32-bit time_t, or NULL. */
static void h_utime(cpu *c) {
  const char *path = g2h(cpu_arg(c, 0));
  if (!path_ok("utime", path)) {
    fail(c, 2);
    return;
  }
  uint32_t t = cpu_arg(c, 1);
  struct utimbuf ub;
  if (t) {
    ub.actime = (int32_t)gm_r32(t);
    ub.modtime = (int32_t)gm_r32(t + 4);
  }
  ret_host(c, utime(path, t ? &ub : NULL));
}

/* umask only shapes the modes mkdir passes on; the host umask is left alone. */
static void h_umask(cpu *c) {
  uint32_t old = guest_umask;
  guest_umask = cpu_arg(c, 0) & 0777;
  cpu_ret(c, old);
}

/* --- directories --- */

static void h_opendir(cpu *c) {
  const char *path = g2h(cpu_arg(c, 0));
  if (!path_ok("opendir", path)) {
    gm_w32(guest_errno_addr(), 2);
    cpu_ret(c, 0);
    return;
  }
  for (int i = 0; i < MAX_DIRS; i++) {
    if (dirs[i].g) continue;
    DIR *h = opendir(path);
    if (!h) {
      guest_set_errno(errno);
      cpu_ret(c, 0);
      return;
    }
    dirs[i].h = h;
    dirs[i].g = guest_malloc(G_DIRENT_SIZE);
    cpu_ret(c, dirs[i].g);
    return;
  }
  gm_w32(guest_errno_addr(), 24 /* EMFILE */);
  cpu_ret(c, 0);
}

static int dir_slot(uint32_t g) {
  for (int i = 0; i < MAX_DIRS; i++)
    if (g && dirs[i].g == g) return i;
  fprintf(stderr, "[file] unknown DIR 0x%08x\n", g);
  return -1;
}

/* readdir(dir): the entry lives in the DIR's own guest block, as in bionic
 * (valid until the next readdir). d_type values match. */
static void h_readdir(cpu *c) {
  int i = dir_slot(cpu_arg(c, 0));
  struct dirent *e = i < 0 ? NULL : readdir(dirs[i].h);
  if (!e) {
    cpu_ret(c, 0);
    return;
  }
  uint32_t g = dirs[i].g;
  memset(g2h(g), 0, G_DIRENT_SIZE);
  w64(g, e->d_ino);
  uint16_t reclen = G_DIRENT_SIZE;
  memcpy(g2h(g + 16), &reclen, 2);
  *(uint8_t *)g2h(g + 18) = e->d_type;
  snprintf(g2h(g + 19), 256, "%s", e->d_name);
  cpu_ret(c, g);
}

static void h_closedir(cpu *c) {
  int i = dir_slot(cpu_arg(c, 0));
  if (i < 0) {
    fail(c, L_EBADF);
    return;
  }
  closedir(dirs[i].h);
  guest_free(dirs[i].g);
  dirs[i].g = 0;
  dirs[i].h = NULL;
  cpu_ret(c, 0);
}

/* --- process identity --- */

static void h_getpid(cpu *c) { cpu_ret(c, G_PID); }
static void h_getppid(cpu *c) { cpu_ret(c, G_PPID); }
static void h_getuid(cpu *c) { cpu_ret(c, G_UID); }

/* syscall(number, ...): only gettid. Each host thread gets a small distinct id;
 * the main thread's is the pid, as on Linux. */
static void h_syscall(cpu *c) {
  static uint32_t next_tid = G_PID + 1;
  static __thread uint32_t tid;
  static __thread int is_main_set;
  uint32_t nr = cpu_arg(c, 0);
  if (nr == 224 /* __NR_gettid */) {
    if (!is_main_set) {
      is_main_set = 1;
      tid = pthread_main_np() ? G_PID : __atomic_fetch_add(&next_tid, 1, __ATOMIC_RELAXED);
    }
    cpu_ret(c, tid);
    return;
  }
  fprintf(stderr, "[file] syscall(%u) unsupported, ENOSYS\n", nr);
  fail(c, L_ENOSYS);
}

/* setpriority(which, who, prio): thread priorities are the host's business. */
static void h_setpriority(cpu *c) { cpu_ret(c, 0); }

const dynlib_impl reimpl_fileio[] = {
    {"access", h_access},   {"chmod", h_chmod},       {"close", h_close},
    {"closedir", h_closedir}, {"fcntl", h_fcntl},     {"fstat", h_fstat},
    {"ftruncate", h_ftruncate}, {"getegid", h_getuid}, {"geteuid", h_getuid},
    {"getgid", h_getuid},   {"getpid", h_getpid},     {"getppid", h_getppid},
    {"getuid", h_getuid},   {"lseek", h_lseek},       {"mkdir", h_mkdir},
    {"open", h_open},       {"opendir", h_opendir},   {"poll", h_poll},   {"read", h_read},
    {"readdir", h_readdir}, {"rename", h_rename},     {"rmdir", h_rmdir},
    {"setpriority", h_setpriority}, {"stat", h_stat}, {"syscall", h_syscall},
    {"umask", h_umask},     {"unlink", h_unlink},     {"utime", h_utime},
    {"write", h_write},     {NULL, NULL},
};
