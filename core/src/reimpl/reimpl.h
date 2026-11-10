/*
 * Host implementations of the system imports, grouped like leos-fortune-vita's
 * reimpl/. Each group is a NULL-terminated table that dynlib_init binds by
 * name; every name must match an entry in dynlib.c's import table.
 */
#ifndef REIMPL_H
#define REIMPL_H

#include <stddef.h>

#include "../dynlib.h"

/* mem.c: the guest heap (malloc family) and the mem and str primitives. */
extern const dynlib_impl reimpl_mem[];

/* runtime.c: atexit, errno, abort and the other process-ending calls. */
extern const dynlib_impl reimpl_runtime[];
int runtime_atexit_count(void);
uint32_t guest_errno_addr(void);
/* Sets the guest errno from a host errno value, translated to Linux numbers. */
void guest_set_errno(int host_errno);
/* A host errno value as Linux numbers it (EIO for one with no equivalent). */
int linux_errno(int host_errno);

/* pthread.c: mutexes, condition variables, semaphores, once, keys. */
extern const dynlib_impl reimpl_pthread[];
/* Runs the guest key destructors for the calling thread's values (cpu c is
 * that thread's top-level cpu). */
void pthread_run_key_destructors(struct cpu *c);

/* thread.c: pthread_create/join/self and pthread_attr_*: guest threads on host
 * threads, each with its own cpu, guest stack, errno and key values. */
extern const dynlib_impl reimpl_thread[];
/* Guest threads that ended in a fault. */
int thread_fault_count(void);
/* Guest threads still running (FMOD's mixer and the game's workers never end). */
int thread_live_count(void);

/* gles.c: the GLES 2.0 imports, forwarded to the host context (gfx.h).
 * Resolves the host functions; call after gfx_init_*. Returns 0 or -1. */
int gles_bind(void);
/* Texture uploads (glTexImage2D, glCompressedTexImage2D) the host GL rejected. */
int gles_upload_failures(void);
/* Keeps the game's level-0 PVRTC uploads (a few, one per size) from now on. */
void gles_pvrtc_keep(void);
/* Pillarbox: the game draws only into the w x h rectangle at (x, y) of the
 * surface (GL's bottom-left origin), as if it were the whole surface. */
void gles_set_content(int x, int y, int w, int h);
/* 1 if the host's current viewport lies inside that rectangle. */
int gles_viewport_in_content(void);
/* Before each nativeRender: paints the surface outside that rectangle black. */
void gles_begin_frame(void);
/* Decodes the kept textures on the GPU and with pvrtc.c and compares them.
 * Returns 0 with the largest and mean channel difference, or -1 if nothing
 * was kept, the host lacks PVRTC, or GL failed. Clobbers the GL state. */
int gles_pvrtc_check(int *max_diff, double *mean_diff);
/* Uploads a small texture in each uncompressed format the game uses (RGB,
 * RGBA, RGBA 4444, LUMINANCE, LUMINANCE_ALPHA), draws it and requires the
 * texels GLES2 defines. Returns 0 or -1. Clobbers the GL state. */
int gles_format_check(void);

/* libc.c: getenv, number parsing, the C-locale wide-character class calls. */
extern const dynlib_impl reimpl_libc[];
/* Fills the _ctype_, _tolower_tab_ and _toupper_tab_ data imports. */
int libc_init_data(void);

/* signal.c: setjmp/longjmp, and signal calls that succeed without signals. */
extern const dynlib_impl reimpl_signal[];

/* string.c: the rest of the str* calls, strtok, strerror, atoi, strtod. */
extern const dynlib_impl reimpl_string[];

/* printf.c: the printf family over guest varargs and va_lists. */
extern const dynlib_impl reimpl_printf[];
/* 1 if DL_TRACE_FMT is set: the printf and scanf families log each call. */
int printf_trace_on(void);

/* stdio.c: FILE streams over host FILEs, and the scanf family. Guest paths
 * outside the data directory fail with ENOENT, like system files on an
 * unrooted phone. stdout and stderr go to the host's stderr. */
extern const dynlib_impl reimpl_stdio[];
void stdio_set_data_root(const char *dir);
const char *stdio_data_root(void);
int stdio_path_allowed(const char *path);
/* A guest path as an absolute host one: relative paths are taken from the
 * working directory, "/" as on Android, and repeated slashes and "./" are
 * dropped. Returns a thread-local buffer (four in rotation). */
const char *stdio_guest_path(const char *path);
/* Guest address of __sF[i] (0 stdin, 1 stdout, 2 stderr). */
uint32_t stdio_stream(int i);
/* Writes to a guest FILE; returns len, or -1 on error. */
int stdio_write(uint32_t file, const char *s, size_t len);
/* Flushes every open guest FILE and completes the safe saves in progress
 * ("w" files are written beside their target and renamed over it when
 * closed), for when the app quits with files still open. */
void stdio_commit_all(void);

/* time.c: clocks, struct tm, strftime and the rand48 generators. */
extern const dynlib_impl reimpl_time[];

/* android_asset.c: AAssetManager over the unpacked APK assets/ directory. */
extern const dynlib_impl reimpl_android_asset[];
void android_asset_set_root(const char *dir);
/* The unpacked assets/ directory (the APK's assets, on the host). */
const char *android_asset_root(void);
/* Log every AAssetManager_open (off by default: the engine probes each
 * texture as .pvr, .ktx, .png and most probes miss). DL_VERBOSE=1 also
 * turns it on. */
void android_asset_set_verbose(int on);

/* fileio.c: fds, file-system calls under the data directory, process ids,
 * syscall(gettid). Guest fds are host fds; 0-2 stay the host's. */
extern const dynlib_impl reimpl_fileio[];
/* Opens a read-only virtual system file (/proc/cpuinfo) as a host fd; -1 if
 * path isn't one. */
int fileio_open_virtual(const char *path);

/* net.c: real host sockets (guest socket fds are host fds), name lookup,
 * select, and epoll emulated over poll. DL_NET=off runs offline instead:
 * connects fail and lookups find nothing. */
extern const dynlib_impl reimpl_net[];
/* 1 for an fd that net.c emulates rather than the host providing it (an
 * epoll instance): read, write and fstat don't apply to it. */
int net_is_socket(int fd);
/* Closes such an fd; returns 0, or -1 with the guest errno set. */
int net_close(int fd);

/* opensles.c: dlopen/dlsym for libOpenSLES.so only, and a minimal OpenSL ES
 * (engine, output mix, buffer-queue player) for FMOD's output. */
extern const dynlib_impl reimpl_opensles[];

/* libm.c: the math library, softfp. */
extern const dynlib_impl reimpl_libm[];

/* gles.c: GLES 2.0 forwarded to ANGLE, with GL strings, limits and errors
 * answered on the host. */
extern const dynlib_impl reimpl_gles[];

/* Guest heap, also used by host code that hands memory to the guest. */
uint32_t guest_malloc(uint32_t size);
void guest_free(uint32_t ptr);
/* The heap allocator's host-side state, so differential tests can rewind
 * the heap along with guest memory. [base, next) is the part of
 * the heap region handed out so far. */
typedef struct guest_heap_state {
  uint32_t base, next, free_list[31];
} guest_heap_state;
void guest_heap_save(guest_heap_state *st);
void guest_heap_restore(const guest_heap_state *st);
uint32_t guest_memalign(uint32_t align, uint32_t size);
/* "[mem] guest heap: N MiB of M MiB used" on stderr, once the heap exists
 * (at exit: always on iOS, with DL_MEM_STATS=1 on the Mac). */
void guest_heap_stats(void);

#endif
