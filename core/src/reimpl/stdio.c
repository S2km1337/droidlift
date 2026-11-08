/*
 * stdio: guest FILE streams over host FILEs, and the scanf family.
 *
 * A guest FILE is an 84-byte bionic struct __sFILE in guest memory; a table
 * maps its address to the host FILE. Only the fields the stdio.h macros read
 * are kept up to date: _r stays 0, so getc's inline fast path always falls
 * through to __srget; _flags carries the open mode and the EOF and error bits
 * (feof, ferror and clearerr are macros on bionic); _file is the host fd
 * (guest fd = host fd).
 *
 * Paths: the game may only reach its data directory (the getFilePath and
 * getCachePath roots). Anything else, such as root checks on /system, fails
 * with ENOENT as it would on an unrooted phone.
 *
 * Safe saves: a file the game opens with "w" or "wb" is written
 * to "<path>.dl-new" next to it, and only when the game closes it (or the
 * app quits, stdio_commit_all) is it synced to disk and renamed over the
 * real file in one step. A crash or kill mid-save leaves the last complete
 * save in place, never a truncated one. While such a file is open, opening
 * the same path for reading still sees the old contents.
 *
 * __sF[0..2] are stdin (always at EOF), stdout and stderr; output to the last
 * two is collected per line and logged on the host's stderr as "[guest] ...".
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../cpu.h"
#include "../guest_mem.h"
#include "gargs.h"
#include "reimpl.h"

#define G_FILE_SIZE 84
#define G_FILE_FLAGS 12 /* short _flags */
#define G_FILE_FD 14    /* short _file */
#define G_SRD 0x0004
#define G_SWR 0x0008
#define G_SRW 0x0010
#define G_SEOF 0x0020
#define G_SERR 0x0040
#define G_EOF ((uint32_t)-1)

#define MAX_FILES 64

static struct {
  uint32_t g;
  FILE *h;
  char *target; /* a safe save in progress: h writes target's SAVE_SUFFIX file */
} files[MAX_FILES];

#define SAVE_SUFFIX ".dl-new"

static int trace_files(void) {
  static int on = -1;
  if (on < 0) on = getenv("DL_TRACE_FILES") != NULL;
  return on;
}

static char data_root[PATH_MAX];

/* Line buffers for the guest's stdout and stderr. */
static struct {
  char line[1024];
  size_t len;
} std_out[2];

void stdio_set_data_root(const char *dir) {
  if (!realpath(dir, data_root)) snprintf(data_root, sizeof data_root, "%s", dir);
}

const char *stdio_data_root(void) { return data_root; }

int stdio_path_allowed(const char *path) {
  size_t n = strlen(data_root);
  if (!n || strncmp(path, data_root, n) != 0 || (path[n] != '/' && path[n] != 0)) return 0;
  return strstr(path, "/../") == NULL;
}

uint32_t stdio_stream(int i) { return dynlib_find("__sF")->addr + (uint32_t)i * G_FILE_SIZE; }

/* -1 for a guest FILE that isn't one of ours, else 0-2 for __sF, 3 for a file. */
static int lookup(uint32_t g, FILE **h) {
  for (int i = 0; i < 3; i++)
    if (g == stdio_stream(i)) {
      *h = NULL;
      return i;
    }
  for (int i = 0; i < MAX_FILES; i++)
    if (files[i].g && files[i].g == g) {
      *h = files[i].h;
      return 3;
    }
  fprintf(stderr, "[stdio] unknown FILE 0x%08x\n", g);
  return -1;
}

static uint16_t open_flags(const char *mode) {
  if (strchr(mode, '+')) return G_SRW;
  return mode[0] == 'r' ? G_SRD : G_SWR;
}

static void set_flags(uint32_t g, uint16_t flags) { memcpy(g2h(g + G_FILE_FLAGS), &flags, 2); }

static uint16_t get_flags(uint32_t g) {
  uint16_t f;
  memcpy(&f, g2h(g + G_FILE_FLAGS), 2);
  return f;
}

/* Mirrors the host's EOF and error state into the guest _flags. A clearerr
 * macro in the guest only clears the guest bits, so a set host bit that the
 * guest has cleared is cleared on the host first. */
static void pull_state(uint32_t g, FILE *h) {
  if (!(get_flags(g) & (G_SEOF | G_SERR))) clearerr(h);
}

static void push_state(uint32_t g, FILE *h) {
  uint16_t f = get_flags(g) & ~(G_SEOF | G_SERR);
  if (feof(h)) f |= G_SEOF;
  if (ferror(h)) f |= G_SERR;
  set_flags(g, f);
}

/* Ends files[i]'s safe save, if one is in progress: the new contents are
 * synced to disk and renamed over the target. If anything failed, the old file
 * stays and the new one is dropped. With close_it, the host FILE is closed
 * too. Returns 0, or -1 if the close or the save failed. */
static int finish_save(int i, int close_it) {
  FILE *h = files[i].h;
  char *target = files[i].target;
  int ok = fflush(h) == 0 && !ferror(h);
  if (target && ok && fsync(fileno(h)) != 0) ok = 0;
  int r = close_it ? fclose(h) : 0;
  if (!target) return r ? -1 : 0;
  files[i].target = NULL;
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof tmp, "%s" SAVE_SUFFIX, target);
  if (ok && r == 0 && rename(tmp, target) == 0) {
    /* The rename itself reaches the disk with the directory. */
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", target);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    int d = slash ? open(dir, O_RDONLY) : -1;
    if (d >= 0) {
      fsync(d);
      close(d);
    }
    if (trace_files()) fprintf(stderr, "[file] saved %s\n", target);
    free(target);
    return 0;
  }
  fprintf(stderr, "[stdio] saving %s failed (%s); the previous file is kept\n", target,
          strerror(errno));
  unlink(tmp);
  free(target);
  return -1;
}

void stdio_commit_all(void) {
  for (int i = 0; i < MAX_FILES; i++)
    if (files[i].g) finish_save(i, 0);
}

/* Wraps a host FILE in a new guest FILE; 0 if the table is full. target: a
 * safe save's real path (open_path), owned by the table from here on. */
static uint32_t wrap(FILE *h, const char *mode, char *target) {
  for (int i = 0; i < MAX_FILES; i++) {
    if (files[i].g) continue;
    uint32_t g = guest_malloc(G_FILE_SIZE);
    memset(g2h(g), 0, G_FILE_SIZE);
    set_flags(g, open_flags(mode));
    int16_t fd = (int16_t)fileno(h);
    memcpy(g2h(g + G_FILE_FD), &fd, 2);
    files[i].g = g;
    files[i].h = h;
    files[i].target = target;
    return g;
  }
  fclose(h);
  if (target) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s" SAVE_SUFFIX, target);
    unlink(tmp);
    free(target);
  }
  gm_w32(guest_errno_addr(), 24 /* EMFILE */);
  return 0;
}

/* Opens a guest path. "w" and "wb" start a safe save: the stream writes
 * path SAVE_SUFFIX, and *target is set to a copy of path (else NULL). */
static FILE *open_path(const char *path, const char *mode, char **target) {
  *target = NULL;
  int vfd = mode[0] == 'r' && !strchr(mode, '+') ? fileio_open_virtual(path) : -1;
  if (vfd >= 0) return fdopen(vfd, mode);
  if (!stdio_path_allowed(path)) {
    fprintf(stderr, "[stdio] fopen(\"%s\", \"%s\"): outside the data directory, ENOENT\n", path,
            mode);
    errno = ENOENT;
    return NULL;
  }
  if (trace_files()) fprintf(stderr, "[file] fopen(\"%s\", \"%s\")\n", path, mode);
  if (mode[0] != 'w' || strchr(mode, '+')) return fopen(path, mode);
  char tmp[PATH_MAX];
  if (snprintf(tmp, sizeof tmp, "%s" SAVE_SUFFIX, path) >= (int)sizeof tmp) {
    errno = ENAMETOOLONG;
    return NULL;
  }
  FILE *h = fopen(tmp, mode);
  if (h && !(*target = strdup(path))) {
    fclose(h);
    unlink(tmp);
    errno = ENOMEM;
    return NULL;
  }
  return h;
}

static void h_fopen(cpu *c) {
  const char *mode = g2h(cpu_arg(c, 1));
  char *target;
  FILE *h = open_path(g2h(cpu_arg(c, 0)), mode, &target);
  if (!h) {
    guest_set_errno(errno);
    cpu_ret(c, 0);
    return;
  }
  cpu_ret(c, wrap(h, mode, target));
}

static void h_fdopen(cpu *c) {
  const char *mode = g2h(cpu_arg(c, 1));
  FILE *h = fdopen((int)cpu_arg(c, 0), mode);
  if (!h) {
    guest_set_errno(errno);
    cpu_ret(c, 0);
    return;
  }
  cpu_ret(c, wrap(h, mode, NULL));
}

/* freopen(path, mode, f): only reopening a file of ours by path. */
static void h_freopen(cpu *c) {
  uint32_t g = cpu_arg(c, 2);
  const char *mode = g2h(cpu_arg(c, 1));
  FILE *h;
  if (!cpu_arg(c, 0) || lookup(g, &h) != 3) {
    fprintf(stderr, "[stdio] freopen on a standard stream or without a path is unsupported\n");
    gm_w32(guest_errno_addr(), 22 /* EINVAL */);
    cpu_ret(c, 0);
    return;
  }
  int i = 0;
  while (files[i].g != g) i++;
  finish_save(i, 1);
  char *target;
  FILE *n = open_path(g2h(cpu_arg(c, 0)), mode, &target);
  if (!n) {
    guest_set_errno(errno);
    files[i].g = 0;
    guest_free(g);
    cpu_ret(c, 0);
    return;
  }
  files[i].h = n;
  files[i].target = target;
  set_flags(g, open_flags(mode));
  int16_t fd = (int16_t)fileno(n);
  memcpy(g2h(g + G_FILE_FD), &fd, 2);
  cpu_ret(c, g);
}

static void h_fclose(cpu *c) {
  uint32_t g = cpu_arg(c, 0);
  FILE *h;
  int k = lookup(g, &h);
  if (k != 3) {
    cpu_ret(c, k < 0 ? G_EOF : 0);
    return;
  }
  int i = 0;
  while (files[i].g != g) i++;
  int r = finish_save(i, 1);
  files[i].g = 0;
  memset(g2h(g), 0, G_FILE_SIZE);
  guest_free(g);
  cpu_ret(c, r ? G_EOF : 0);
}

static void flush_std(int i) {
  if (!std_out[i].len) return;
  fprintf(stderr, "[guest] %.*s\n", (int)std_out[i].len, std_out[i].line);
  std_out[i].len = 0;
}

int stdio_write(uint32_t g, const char *s, size_t len) {
  FILE *h;
  int k = lookup(g, &h);
  if (k < 0 || k == 0) return -1;
  if (k == 3) {
    pull_state(g, h);
    size_t n = fwrite(s, 1, len, h);
    push_state(g, h);
    return n == len ? (int)len : -1;
  }
  int i = k - 1;
  for (size_t j = 0; j < len; j++) {
    if (s[j] == '\n' || std_out[i].len == sizeof std_out[i].line) flush_std(i);
    if (s[j] != '\n') std_out[i].line[std_out[i].len++] = s[j];
  }
  return (int)len;
}

static void h_fflush(cpu *c) {
  uint32_t g = cpu_arg(c, 0);
  FILE *h;
  if (!g) {
    for (int i = 0; i < MAX_FILES; i++)
      if (files[i].g) fflush(files[i].h);
    cpu_ret(c, 0);
    return;
  }
  int k = lookup(g, &h);
  cpu_ret(c, k < 0 ? G_EOF : k == 3 ? (fflush(h) ? G_EOF : 0) : 0);
}

static void h_fwrite(cpu *c) {
  uint32_t size = cpu_arg(c, 1), n = cpu_arg(c, 2);
  if (!size || !n) {
    cpu_ret(c, 0);
    return;
  }
  int r = stdio_write(cpu_arg(c, 3), g2h(cpu_arg(c, 0)), (size_t)size * n);
  cpu_ret(c, r < 0 ? 0 : n);
}

static void h_fputs(cpu *c) {
  const char *s = g2h(cpu_arg(c, 0));
  cpu_ret(c, stdio_write(cpu_arg(c, 1), s, strlen(s)) < 0 ? G_EOF : 0);
}

static void h_puts(cpu *c) {
  const char *s = g2h(cpu_arg(c, 0));
  int r = stdio_write(stdio_stream(1), s, strlen(s));
  if (r >= 0) r = stdio_write(stdio_stream(1), "\n", 1);
  cpu_ret(c, r < 0 ? G_EOF : 0);
}

/* fputc(int c, FILE *f) and putc */
static void h_fputc(cpu *c) {
  char ch = (char)cpu_arg(c, 0);
  cpu_ret(c, stdio_write(cpu_arg(c, 1), &ch, 1) < 0 ? G_EOF : (uint8_t)ch);
}

/* Reading calls: the host FILE of a guest file, or NULL for stdin (always at
 * EOF) and anything that can't be read. */
static FILE *readable(uint32_t g) {
  FILE *h;
  int k = lookup(g, &h);
  if (k == 0) set_flags(g, get_flags(g) | G_SEOF);
  if (k != 3) return NULL;
  pull_state(g, h);
  return h;
}

static void h_fread(cpu *c) {
  uint32_t g = cpu_arg(c, 3), size = cpu_arg(c, 1), n = cpu_arg(c, 2);
  FILE *h = readable(g);
  if (!h || !size || !n) {
    cpu_ret(c, 0);
    return;
  }
  cpu_ret(c, (uint32_t)fread(g2h(cpu_arg(c, 0)), size, n, h));
  push_state(g, h);
}

static void h_fgets(cpu *c) {
  uint32_t buf = cpu_arg(c, 0), g = cpu_arg(c, 2);
  int n = (int)cpu_arg(c, 1);
  FILE *h = readable(g);
  if (!h || n <= 0) {
    cpu_ret(c, 0);
    return;
  }
  cpu_ret(c, fgets(g2h(buf), n, h) ? buf : 0);
  push_state(g, h);
}

/* getc(f) and __srget(f), which getc's inline fast path calls once _r runs
 * out. That path has decremented _r, so it goes back to 0. */
static void h_getc(cpu *c) {
  uint32_t g = cpu_arg(c, 0);
  gm_w32(g + 4, 0);
  FILE *h = readable(g);
  if (!h) {
    cpu_ret(c, G_EOF);
    return;
  }
  int ch = getc(h);
  push_state(g, h);
  cpu_ret(c, (uint32_t)ch);
}

static void h_ungetc(cpu *c) {
  uint32_t g = cpu_arg(c, 1);
  FILE *h = readable(g);
  if (!h) {
    cpu_ret(c, G_EOF);
    return;
  }
  int r = ungetc((int)cpu_arg(c, 0), h);
  push_state(g, h);
  cpu_ret(c, (uint32_t)r);
}

/* fseek and fseeko: off_t and long are both 32-bit on the guest; the whence
 * values match the host's. */
static void h_fseek(cpu *c) {
  uint32_t g = cpu_arg(c, 0);
  FILE *h;
  if (lookup(g, &h) != 3) {
    gm_w32(guest_errno_addr(), 29 /* ESPIPE */);
    cpu_ret(c, (uint32_t)-1);
    return;
  }
  int r = fseeko(h, (off_t)(int32_t)cpu_arg(c, 1), (int)cpu_arg(c, 2));
  if (r) guest_set_errno(errno);
  push_state(g, h);
  cpu_ret(c, (uint32_t)r);
}

static void h_ftell(cpu *c) {
  FILE *h;
  if (lookup(cpu_arg(c, 0), &h) != 3) {
    gm_w32(guest_errno_addr(), 29 /* ESPIPE */);
    cpu_ret(c, (uint32_t)-1);
    return;
  }
  off_t pos = ftello(h);
  if (pos > INT32_MAX) {
    gm_w32(guest_errno_addr(), 75 /* EOVERFLOW */);
    pos = -1;
  }
  cpu_ret(c, (uint32_t)pos);
}

/* setvbuf(f, buf, mode, size): the host keeps its own buffer; the mode values
 * match. */
static void h_setvbuf(cpu *c) {
  FILE *h;
  if (lookup(cpu_arg(c, 0), &h) == 3) setvbuf(h, NULL, (int)cpu_arg(c, 2), 0);
  cpu_ret(c, 0);
}

/* --- scanf --- */

/* Input of one scanf call: a host string or a host FILE. */
typedef struct scan_src {
  const char *s;
  FILE *f;
  size_t pos; /* characters consumed so far */
} scan_src;

/* Runs one host scanf on spec, which ends in "%n". dst is NULL for a
 * suppressed or literal-only directive. Returns the host scanf result and
 * advances the source by the characters consumed, or returns -2 if the
 * directive didn't match up to its end. */
static int scan_step(scan_src *src, const char *spec, void *dst) {
  int n = -1, r;
  if (src->f) r = dst ? fscanf(src->f, spec, dst, &n) : fscanf(src->f, spec, &n);
  else r = dst ? sscanf(src->s + src->pos, spec, dst, &n) : sscanf(src->s + src->pos, spec, &n);
  if (n < 0) return r == EOF ? EOF : -2;
  src->pos += (size_t)n;
  return r;
}

/* Scans src by the guest format fmt, storing through guest pointers from a.
 * Each conversion, with the literal text before it, becomes one host scanf
 * call. Integers are read as 64-bit and stored at the guest's width (L, like
 * l, z and t, means a 32-bit int on bionic; ll, q and j are 64-bit). %f and
 * friends are read straight into a float (strtof, as bionic does; going
 * through a double would round twice), %lf and %Lf into a double. Returns
 * the number of assignments, or EOF on an input failure before the first
 * one (as on bionic, a suppressed conversion before it doesn't count). */
static int guest_scan(scan_src *src, const char *fmt, gargs *a) {
  char spec[512];
  size_t sl = 0;
  int assigned = 0;
  for (const char *p = fmt;;) {
    /* Literal text, kept for the next host call. */
    while (*p && !(p[0] == '%' && p[1] != '%') && sl < sizeof spec - 64) {
      if (*p == '%') spec[sl++] = *p++;
      spec[sl++] = *p++;
    }
    if (sl >= sizeof spec - 64) {
      fprintf(stderr, "[scanf] format too long: \"%s\"\n", fmt);
      return assigned;
    }
    if (!*p) {
      if (sl) {
        memcpy(spec + sl, "%n", 3);
        scan_step(src, spec, NULL);
      }
      return assigned;
    }

    p++; /* '%' */
    int suppress = *p == '*';
    if (suppress) p++;
    char width[12];
    int wl = 0;
    while (*p >= '0' && *p <= '9' && wl < 10) width[wl++] = *p++;
    width[wl] = 0;
    char len = 0;
    if (p[0] == 'h' && p[1] == 'h') len = 'H', p += 2;
    else if (*p == 'h') len = 'h', p++;
    else if (p[0] == 'l' && p[1] == 'l') len = 'q', p += 2;
    else if (*p == 'q' || *p == 'j') len = 'q', p++;
    else if (*p == 'L') len = 'L', p++;
    else if (*p == 'l') len = 'l', p++;
    else if (*p == 'z' || *p == 't') len = '-', p++;
    char conv = *p ? *p++ : 0;

    char tail[300];
    uint64_t ival = 0;
    double dval = 0;
    float fval = 0;
    void *dst = NULL;
    int store = 0; /* 1 int, 2 double, 3 direct, 4 float */
    uint32_t ptr = 0;
    switch (conv) {
    case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p':
      snprintf(tail, sizeof tail, "ll%c", conv == 'p' ? 'x' : conv);
      dst = &ival, store = 1;
      break;
    case 'e': case 'f': case 'g': case 'a': case 'E': case 'F': case 'G': case 'A':
      if (len == 'l' || len == 'L' || len == 'q') {
        snprintf(tail, sizeof tail, "l%c", conv);
        dst = &dval, store = 2;
      } else {
        tail[0] = conv, tail[1] = 0;
        dst = &fval, store = 4;
      }
      break;
    case 's': case 'c':
      tail[0] = conv, tail[1] = 0;
      store = 3;
      break;
    case '[': {
      const char *q = p;
      if (*q == '^') q++;
      if (*q == ']') q++;
      while (*q && *q != ']') q++;
      if (!*q || (size_t)(q - p) + 3 > sizeof tail) {
        fprintf(stderr, "[scanf] bad scanset in \"%s\"\n", fmt);
        return assigned;
      }
      snprintf(tail, sizeof tail, "[%.*s]", (int)(q - p), p);
      p = q + 1;
      store = 3;
      break;
    }
    case 'n':
      /* Match the pending literal text, then report the count. */
      if (sl) {
        memcpy(spec + sl, "%n", 3);
        if (scan_step(src, spec, NULL) < 0) return assigned ? assigned : EOF;
        sl = 0;
      }
      if (!suppress) {
        ptr = next32(a);
        if (len == 'H') *(uint8_t *)g2h(ptr) = (uint8_t)src->pos;
        else if (len == 'h') *(uint16_t *)g2h(ptr) = (uint16_t)src->pos;
        else if (len == 'q') gm_w32(ptr, (uint32_t)src->pos), gm_w32(ptr + 4, 0);
        else gm_w32(ptr, (uint32_t)src->pos);
      }
      continue;
    default:
      fprintf(stderr, "[scanf] unsupported conversion '%c' in \"%s\"\n", conv, fmt);
      return assigned;
    }
    if (len == 'l' && store == 3) {
      fprintf(stderr, "[scanf] wide conversions unsupported in \"%s\"\n", fmt);
      return assigned;
    }
    if (!suppress) {
      ptr = next32(a);
      if (store == 3) dst = g2h(ptr);
    } else {
      dst = NULL;
    }
    snprintf(spec + sl, sizeof spec - sl, "%%%s%s%s%%n", suppress ? "*" : "", width, tail);
    sl = 0;
    int r = scan_step(src, spec, dst);
    if (r == EOF) return assigned ? assigned : EOF;
    if (r == -2 || (!suppress && r != 1)) return assigned;
    if (suppress) continue;
    assigned++;
    if (store == 1) {
      if (len == 'H') *(uint8_t *)g2h(ptr) = (uint8_t)ival;
      else if (len == 'h') *(uint16_t *)g2h(ptr) = (uint16_t)ival;
      else if (len == 'q') gm_w32(ptr, (uint32_t)ival), gm_w32(ptr + 4, (uint32_t)(ival >> 32));
      else gm_w32(ptr, (uint32_t)ival);
    } else if (store == 2) {
      memcpy(g2h(ptr), &dval, 8);
    } else if (store == 4) {
      memcpy(g2h(ptr), &fval, 4);
    }
  }
}

static void scan_string(cpu *c, uint32_t str, uint32_t fmt, gargs a) {
  scan_src src = {g2h(str), NULL, 0};
  int r = guest_scan(&src, g2h(fmt), &a);
  if (printf_trace_on())
    fprintf(stderr, "[fmt] sscanf \"%s\" on \"%s\" -> %d\n", (const char *)g2h(fmt),
            (const char *)g2h(str), r);
  cpu_ret(c, (uint32_t)r);
}

static void h_sscanf(cpu *c) { scan_string(c, cpu_arg(c, 0), cpu_arg(c, 1), variadic_args(c, 2)); }

static void h_fscanf(cpu *c) {
  uint32_t g = cpu_arg(c, 0);
  FILE *h = readable(g);
  if (!h) {
    cpu_ret(c, G_EOF);
    return;
  }
  gargs a = variadic_args(c, 2);
  scan_src src = {NULL, h, 0};
  int r = guest_scan(&src, g2h(cpu_arg(c, 1)), &a);
  if (printf_trace_on())
    fprintf(stderr, "[fmt] fscanf \"%s\" (%zu chars) -> %d\n", (const char *)g2h(cpu_arg(c, 1)),
            src.pos, r);
  cpu_ret(c, (uint32_t)r);
  push_state(g, h);
}

const dynlib_impl reimpl_stdio[] = {
    {"__srget", h_getc}, {"fclose", h_fclose}, {"fdopen", h_fdopen}, {"fflush", h_fflush},
    {"fgets", h_fgets},   {"fopen", h_fopen},   {"fputc", h_fputc},   {"fputs", h_fputs},
    {"fread", h_fread},   {"freopen", h_freopen}, {"fscanf", h_fscanf}, {"fseek", h_fseek},
    {"fseeko", h_fseek},  {"ftell", h_ftell},   {"ftello", h_ftell},  {"fwrite", h_fwrite},
    {"getc", h_getc},     {"putc", h_fputc},    {"puts", h_puts},     {"setvbuf", h_setvbuf},
    {"sscanf", h_sscanf}, {"ungetc", h_ungetc}, {NULL, NULL},
};
