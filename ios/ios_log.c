/*
 * The iOS app's log. On a device stdout and stderr go to
 * /dev/null, so everything the loader prints is also copied, line by line,
 * to a file in the app's container and to os_log:
 *   <container>/Documents/logs/run-YYYYMMDD-HHMMSS.log  (the last few runs)
 *   os_log, subsystem = the bundle id, category "loader"   (Console.app)
 * Documents is shared (UIFileSharingEnabled), so the logs show in Finder's
 * iPhone pane and in the Files app; scripts/fetch-ios-log.sh copies them.
 * Where stdout already reaches something (the Simulator's
 * `simctl launch --console-pty`, `devicectl device process launch
 * --console`), the lines go there as well.
 */
#include "ios_log.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <os/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KEEP_LOGS 5

static int log_fd = -1, console_fd = -1;
static os_log_t oslog;
static char log_path[PATH_MAX];
static int have_tee;
/* ios_log_flush's NUL markers: written, and seen by the tee thread. */
static unsigned flush_sent, flush_seen;

static int is_dev_null(int fd) {
  struct stat a, b;
  return fstat(fd, &a) == 0 && stat("/dev/null", &b) == 0 && S_ISCHR(a.st_mode) && a.st_rdev == b.st_rdev;
}

static void write_all(int fd, const char *p, size_t n) {
  while (n) {
    ssize_t w = write(fd, p, n);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return;
    p += w;
    n -= (size_t)w;
  }
}

/* Copies the pipe that fds 1 and 2 now point at. */
static void *tee_thread(void *arg) {
  int in = (int)(intptr_t)arg;
  char buf[4096], line[4096];
  size_t len = 0;
  for (;;) {
    ssize_t n = read(in, buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    /* Drop ios_log_flush's NUL markers, counting them. */
    unsigned markers = 0;
    ssize_t m = 0;
    for (ssize_t i = 0; i < n; i++) {
      if (buf[i]) buf[m++] = buf[i];
      else markers++;
    }
    n = m;
    if (log_fd >= 0) write_all(log_fd, buf, (size_t)n);
    if (console_fd >= 0) write_all(console_fd, buf, (size_t)n);
    /* os_log takes whole lines. */
    for (ssize_t i = 0; i < n; i++) {
      if (buf[i] == '\n' || len == sizeof line - 1) {
        line[len] = 0;
        os_log(oslog, "%{public}s", line);
        len = 0;
        if (buf[i] == '\n') continue;
      }
      line[len++] = buf[i];
    }
    if (markers) __atomic_fetch_add(&flush_seen, markers, __ATOMIC_RELEASE);
  }
  return NULL;
}

static int by_name(const struct dirent **a, const struct dirent **b) {
  return strcmp((*a)->d_name, (*b)->d_name);
}
static int is_log(const struct dirent *d) {
  return !strncmp(d->d_name, "run-", 4) && strstr(d->d_name, ".log") != NULL;
}

/* Keeps the newest KEEP_LOGS - 1, making room for this run's. */
static void prune(const char *dir) {
  struct dirent **names;
  int n = scandir(dir, &names, is_log, by_name);
  if (n < 0) return;
  for (int i = 0; i < n; i++) {
    if (i < n - (KEEP_LOGS - 1)) {
      char p[PATH_MAX];
      snprintf(p, sizeof p, "%s/%s", dir, names[i]->d_name);
      unlink(p);
    }
    free(names[i]);
  }
  free(names);
}

const char *ios_log_path(void) { return log_path[0] ? log_path : NULL; }

void ios_log_flush(void) {
  if (!have_tee) return;
  fflush(stdout);
  unsigned want = __atomic_add_fetch(&flush_sent, 1, __ATOMIC_ACQ_REL);
  write_all(STDERR_FILENO, "", 1);
  /* Until the tee thread has copied everything before the marker; at most
   * a second. */
  for (int i = 0; i < 200 && (int)(__atomic_load_n(&flush_seen, __ATOMIC_ACQUIRE) - want) < 0; i++)
    usleep(5000);
}

void ios_log_start(void) {
  if (oslog) return;
  char id[256] = "droidlift";
  CFStringRef bundle_id = CFBundleGetIdentifier(CFBundleGetMainBundle());
  if (bundle_id) CFStringGetCString(bundle_id, id, sizeof id, kCFStringEncodingUTF8);
  oslog = os_log_create(id, "loader");

  /* HOME is the app's data container on iOS. */
  const char *home = getenv("HOME");
  if (home && *home) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/Documents", home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/Documents/logs", home);
    mkdir(dir, 0755);
    prune(dir);
    char stamp[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    snprintf(log_path, sizeof log_path, "%s/run-%s.log", dir, stamp);
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (log_fd < 0) log_path[0] = 0;
  }

  /* Keep the original stdout too, unless it goes nowhere. */
  if (!is_dev_null(STDOUT_FILENO)) console_fd = dup(STDOUT_FILENO);

  int p[2];
  if (pipe(p) != 0) return;
  fflush(stdout);
  fflush(stderr);
  dup2(p[1], STDOUT_FILENO);
  dup2(p[1], STDERR_FILENO);
  close(p[1]);
  /* Line by line, so a crash loses at most the line being written. */
  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  pthread_t tee;
  if (pthread_create(&tee, NULL, tee_thread, (void *)(intptr_t)p[0]) == 0) {
    pthread_detach(tee);
    have_tee = 1;
    /* For exit() (UIKit ending the app); the loader's own _exit path calls
     * ios_log_flush first (ios_shell_exit). */
    atexit(ios_log_flush);
  }
}
