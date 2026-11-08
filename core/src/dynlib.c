#include "dynlib.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "guest_mem.h"
#include "reimpl/reimpl.h"

#define STUB(lib, name) {lib, name, DL_STUB, 0, 0, 0, NULL}
#define DATA(lib, name, size) {lib, name, DL_DATA, size, 0, 0, NULL}
#define WEAK_NULL(lib, name) {lib, name, DL_WEAK_NULL, 0, 0, 0, NULL}

/* The system imports droidlift implements or stubs: grown from the games
 * ported so far (--check lists any a new game needs that is missing here).
 * Keep sorted within each library. */
static dynlib_entry default_dynlib[] = {
    /* libc.so */
    STUB("libc.so", "__aeabi_atexit"),
    STUB("libc.so", "__assert2"),
    STUB("libc.so", "__errno"),
    STUB("libc.so", "__gnu_Unwind_Find_exidx"),
    DATA("libc.so", "__sF", 3 * 84 /* bionic FILE is 84 bytes */),
    STUB("libc.so", "__srget"),
    STUB("libc.so", "__stack_chk_fail"),
    DATA("libc.so", "__stack_chk_guard", 4),
    DATA("libc.so", "_ctype_", 4 /* const char * */),
    DATA("libc.so", "_tolower_tab_", 4 /* const short * */),
    DATA("libc.so", "_toupper_tab_", 4 /* const short * */),
    STUB("libc.so", "abort"),
    STUB("libc.so", "accept"),
    STUB("libc.so", "access"),
    STUB("libc.so", "alarm"),
    STUB("libc.so", "atoi"),
    STUB("libc.so", "atol"),
    STUB("libc.so", "atoll"),
    STUB("libc.so", "basename"),
    STUB("libc.so", "bind"),
    STUB("libc.so", "bsd_signal"),
    STUB("libc.so", "btowc"),
    STUB("libc.so", "calloc"),
    STUB("libc.so", "chmod"),
    STUB("libc.so", "chown"),
    STUB("libc.so", "clock_gettime"),
    STUB("libc.so", "close"),
    STUB("libc.so", "closedir"),
    STUB("libc.so", "connect"),
    STUB("libc.so", "dup2"),
    STUB("libc.so", "execl"),
    STUB("libc.so", "exit"),
    STUB("libc.so", "fclose"),
    STUB("libc.so", "fcntl"),
    STUB("libc.so", "fdopen"),
    STUB("libc.so", "fflush"),
    STUB("libc.so", "fgets"),
    STUB("libc.so", "fopen"),
    STUB("libc.so", "fork"),
    STUB("libc.so", "fprintf"),
    STUB("libc.so", "fputc"),
    STUB("libc.so", "fputs"),
    STUB("libc.so", "fread"),
    STUB("libc.so", "free"),
    STUB("libc.so", "freeaddrinfo"),
    STUB("libc.so", "freopen"),
    STUB("libc.so", "fscanf"),
    STUB("libc.so", "fseek"),
    STUB("libc.so", "fseeko"),
    STUB("libc.so", "fstat"),
    STUB("libc.so", "ftell"),
    STUB("libc.so", "ftello"),
    STUB("libc.so", "ftruncate"),
    STUB("libc.so", "fwrite"),
    STUB("libc.so", "getaddrinfo"),
    STUB("libc.so", "getauxval"),
    STUB("libc.so", "getc"),
    STUB("libc.so", "getcwd"),
    STUB("libc.so", "getegid"),
    STUB("libc.so", "getenv"),
    STUB("libc.so", "geteuid"),
    STUB("libc.so", "getgid"),
    STUB("libc.so", "gethostbyname"),
    STUB("libc.so", "gethostname"),
    STUB("libc.so", "getpeername"),
    STUB("libc.so", "getpid"),
    STUB("libc.so", "getppid"),
    STUB("libc.so", "getpwuid"),
    STUB("libc.so", "getsockname"),
    STUB("libc.so", "getsockopt"),
    STUB("libc.so", "gettimeofday"),
    STUB("libc.so", "getuid"),
    STUB("libc.so", "getwc"),
    STUB("libc.so", "gmtime_r"),
    STUB("libc.so", "if_nametoindex"),
    STUB("libc.so", "inet_addr"),
    STUB("libc.so", "inet_ntoa"),
    STUB("libc.so", "inet_ntop"),
    STUB("libc.so", "inet_pton"),
    STUB("libc.so", "ioctl"),
    STUB("libc.so", "isalnum"),
    STUB("libc.so", "isgraph"),
    STUB("libc.so", "islower"),
    STUB("libc.so", "isprint"),
    STUB("libc.so", "isupper"),
    STUB("libc.so", "iswctype"),
    STUB("libc.so", "jrand48"),
    STUB("libc.so", "kill"),
    STUB("libc.so", "listen"),
    STUB("libc.so", "localtime"),
    STUB("libc.so", "longjmp"),
    STUB("libc.so", "lrand48"),
    STUB("libc.so", "lseek"),
    STUB("libc.so", "malloc"),
    STUB("libc.so", "mbrtowc"),
    STUB("libc.so", "mbsrtowcs"),
    STUB("libc.so", "memalign"),
    STUB("libc.so", "memchr"),
    STUB("libc.so", "memcmp"),
    STUB("libc.so", "memcpy"),
    STUB("libc.so", "memmem"),
    STUB("libc.so", "memmove"),
    STUB("libc.so", "memrchr"),
    STUB("libc.so", "memset"),
    STUB("libc.so", "mkdir"),
    STUB("libc.so", "open"),
    STUB("libc.so", "opendir"),
    STUB("libc.so", "poll"),
    STUB("libc.so", "printf"),
    STUB("libc.so", "pthread_attr_destroy"),
    STUB("libc.so", "pthread_attr_init"),
    STUB("libc.so", "pthread_attr_setdetachstate"),
    STUB("libc.so", "pthread_attr_setstacksize"),
    STUB("libc.so", "pthread_cond_broadcast"),
    STUB("libc.so", "pthread_cond_destroy"),
    STUB("libc.so", "pthread_cond_init"),
    STUB("libc.so", "pthread_cond_signal"),
    STUB("libc.so", "pthread_cond_wait"),
    STUB("libc.so", "pthread_create"),
    STUB("libc.so", "pthread_getspecific"),
    STUB("libc.so", "pthread_join"),
    STUB("libc.so", "pthread_key_create"),
    STUB("libc.so", "pthread_key_delete"),
    STUB("libc.so", "pthread_mutex_destroy"),
    STUB("libc.so", "pthread_mutex_init"),
    STUB("libc.so", "pthread_mutex_lock"),
    STUB("libc.so", "pthread_mutex_unlock"),
    STUB("libc.so", "pthread_mutexattr_init"),
    STUB("libc.so", "pthread_mutexattr_settype"),
    STUB("libc.so", "pthread_once"),
    STUB("libc.so", "pthread_self"),
    STUB("libc.so", "pthread_setspecific"),
    STUB("libc.so", "putc"),
    STUB("libc.so", "puts"),
    STUB("libc.so", "putwc"),
    STUB("libc.so", "qsort"),
    STUB("libc.so", "raise"),
    STUB("libc.so", "read"),
    STUB("libc.so", "readdir"),
    STUB("libc.so", "realloc"),
    STUB("libc.so", "recv"),
    STUB("libc.so", "recvfrom"),
    STUB("libc.so", "rename"),
    STUB("libc.so", "rewind"),
    STUB("libc.so", "rmdir"),
    STUB("libc.so", "select"),
    STUB("libc.so", "sem_destroy"),
    STUB("libc.so", "sem_init"),
    STUB("libc.so", "sem_post"),
    STUB("libc.so", "sem_trywait"),
    STUB("libc.so", "sem_wait"),
    STUB("libc.so", "send"),
    STUB("libc.so", "sendto"),
    STUB("libc.so", "setjmp"),
    STUB("libc.so", "setlocale"),
    STUB("libc.so", "setpriority"),
    STUB("libc.so", "setsockopt"),
    STUB("libc.so", "setvbuf"),
    STUB("libc.so", "shutdown"),
    STUB("libc.so", "sigaction"),
    STUB("libc.so", "siglongjmp"),
    STUB("libc.so", "sigprocmask"),
    STUB("libc.so", "sigsetjmp"),
    STUB("libc.so", "snprintf"),
    STUB("libc.so", "socket"),
    STUB("libc.so", "socketpair"),
    STUB("libc.so", "sprintf"),
    STUB("libc.so", "srand48"),
    STUB("libc.so", "sscanf"),
    STUB("libc.so", "stat"),
    STUB("libc.so", "strcasecmp"),
    STUB("libc.so", "strcat"),
    STUB("libc.so", "strchr"),
    STUB("libc.so", "strcmp"),
    STUB("libc.so", "strcoll"),
    STUB("libc.so", "strcpy"),
    STUB("libc.so", "strcspn"),
    STUB("libc.so", "strdup"),
    STUB("libc.so", "strerror"),
    STUB("libc.so", "strerror_r"),
    STUB("libc.so", "strftime"),
    STUB("libc.so", "strlen"),
    STUB("libc.so", "strncasecmp"),
    STUB("libc.so", "strncat"),
    STUB("libc.so", "strncmp"),
    STUB("libc.so", "strncpy"),
    STUB("libc.so", "strpbrk"),
    STUB("libc.so", "strrchr"),
    STUB("libc.so", "strstr"),
    STUB("libc.so", "strtod"),
    STUB("libc.so", "strtok"),
    STUB("libc.so", "strtok_r"),
    STUB("libc.so", "strtol"),
    STUB("libc.so", "strtoll"),
    STUB("libc.so", "strtoul"),
    STUB("libc.so", "strxfrm"),
    STUB("libc.so", "syscall"),
    STUB("libc.so", "time"),
    STUB("libc.so", "tolower"),
    STUB("libc.so", "toupper"),
    STUB("libc.so", "towlower"),
    STUB("libc.so", "towupper"),
    STUB("libc.so", "umask"),
    STUB("libc.so", "ungetc"),
    STUB("libc.so", "ungetwc"),
    STUB("libc.so", "unlink"),
    STUB("libc.so", "usleep"),
    STUB("libc.so", "utime"),
    STUB("libc.so", "vfprintf"),
    STUB("libc.so", "vsnprintf"),
    STUB("libc.so", "vsprintf"),
    STUB("libc.so", "waitpid"),
    STUB("libc.so", "wcrtomb"),
    STUB("libc.so", "wcscoll"),
    STUB("libc.so", "wcsftime"),
    STUB("libc.so", "wcslen"),
    STUB("libc.so", "wcsrtombs"),
    STUB("libc.so", "wcsxfrm"),
    STUB("libc.so", "wctob"),
    STUB("libc.so", "wctype"),
    STUB("libc.so", "wmemchr"),
    STUB("libc.so", "wmemcmp"),
    STUB("libc.so", "wmemcpy"),
    STUB("libc.so", "wmemmove"),
    STUB("libc.so", "wmemset"),
    STUB("libc.so", "write"),
    /* libm.so */
    STUB("libm.so", "__isfinitef"),
    STUB("libm.so", "acos"),
    STUB("libm.so", "asin"),
    STUB("libm.so", "atan"),
    STUB("libm.so", "atan2"),
    STUB("libm.so", "ceil"),
    STUB("libm.so", "cos"),
    STUB("libm.so", "cosh"),
    STUB("libm.so", "exp"),
    STUB("libm.so", "floor"),
    STUB("libm.so", "fmod"),
    STUB("libm.so", "frexp"),
    STUB("libm.so", "ldexp"),
    STUB("libm.so", "log"),
    STUB("libm.so", "log10"),
    STUB("libm.so", "log10f"),
    STUB("libm.so", "lrintf"),
    STUB("libm.so", "modf"),
    STUB("libm.so", "pow"),
    STUB("libm.so", "rint"),
    STUB("libm.so", "sin"),
    STUB("libm.so", "sinh"),
    STUB("libm.so", "sqrt"),
    STUB("libm.so", "tan"),
    STUB("libm.so", "tanh"),
    /* libdl.so */
    STUB("libdl.so", "dlclose"),
    STUB("libdl.so", "dlerror"),
    STUB("libdl.so", "dlopen"),
    STUB("libdl.so", "dlsym"),
    /* liblog.so */
    STUB("liblog.so", "__android_log_print"),
    STUB("liblog.so", "__android_log_write"),
    /* libstdc++.so */
    STUB("libstdc++.so", "_ZdlPv"),
    STUB("libstdc++.so", "__cxa_atexit"),
    WEAK_NULL("libstdc++.so", "__cxa_begin_cleanup"),
    WEAK_NULL("libstdc++.so", "__cxa_call_unexpected"),
    STUB("libstdc++.so", "__cxa_finalize"),
    STUB("libstdc++.so", "__cxa_pure_virtual"),
    WEAK_NULL("libstdc++.so", "__cxa_type_match"),
    /* libandroid.so */
    STUB("libandroid.so", "AAssetDir_close"),
    STUB("libandroid.so", "AAssetDir_getNextFileName"),
    STUB("libandroid.so", "AAssetManager_fromJava"),
    STUB("libandroid.so", "AAssetManager_open"),
    STUB("libandroid.so", "AAssetManager_openDir"),
    STUB("libandroid.so", "AAsset_close"),
    STUB("libandroid.so", "AAsset_getLength"),
    STUB("libandroid.so", "AAsset_getRemainingLength"),
    STUB("libandroid.so", "AAsset_openFileDescriptor"),
    STUB("libandroid.so", "AAsset_read"),
    STUB("libandroid.so", "AAsset_seek"),
    /* libEGL.so */
    STUB("libEGL.so", "eglGetProcAddress"),
    /* libGLESv2.so */
    STUB("libGLESv2.so", "glActiveTexture"),
    STUB("libGLESv2.so", "glAttachShader"),
    STUB("libGLESv2.so", "glBindAttribLocation"),
    STUB("libGLESv2.so", "glBindBuffer"),
    STUB("libGLESv2.so", "glBindTexture"),
    STUB("libGLESv2.so", "glBlendEquationSeparate"),
    STUB("libGLESv2.so", "glBlendFuncSeparate"),
    STUB("libGLESv2.so", "glBufferData"),
    STUB("libGLESv2.so", "glBufferSubData"),
    STUB("libGLESv2.so", "glClear"),
    STUB("libGLESv2.so", "glClearColor"),
    STUB("libGLESv2.so", "glClearDepthf"),
    STUB("libGLESv2.so", "glColorMask"),
    STUB("libGLESv2.so", "glCompileShader"),
    STUB("libGLESv2.so", "glCompressedTexImage2D"),
    STUB("libGLESv2.so", "glCreateProgram"),
    STUB("libGLESv2.so", "glCreateShader"),
    STUB("libGLESv2.so", "glCullFace"),
    STUB("libGLESv2.so", "glDeleteBuffers"),
    STUB("libGLESv2.so", "glDeleteProgram"),
    STUB("libGLESv2.so", "glDeleteShader"),
    STUB("libGLESv2.so", "glDeleteTextures"),
    STUB("libGLESv2.so", "glDepthFunc"),
    STUB("libGLESv2.so", "glDepthMask"),
    STUB("libGLESv2.so", "glDisable"),
    STUB("libGLESv2.so", "glDisableVertexAttribArray"),
    STUB("libGLESv2.so", "glDrawArrays"),
    STUB("libGLESv2.so", "glDrawElements"),
    STUB("libGLESv2.so", "glEnable"),
    STUB("libGLESv2.so", "glEnableVertexAttribArray"),
    STUB("libGLESv2.so", "glFinish"),
    STUB("libGLESv2.so", "glGenBuffers"),
    STUB("libGLESv2.so", "glGenTextures"),
    STUB("libGLESv2.so", "glGetActiveAttrib"),
    STUB("libGLESv2.so", "glGetActiveUniform"),
    STUB("libGLESv2.so", "glGetError"),
    STUB("libGLESv2.so", "glGetIntegerv"),
    STUB("libGLESv2.so", "glGetProgramInfoLog"),
    STUB("libGLESv2.so", "glGetProgramiv"),
    STUB("libGLESv2.so", "glGetShaderInfoLog"),
    STUB("libGLESv2.so", "glGetShaderiv"),
    STUB("libGLESv2.so", "glGetString"),
    STUB("libGLESv2.so", "glGetUniformLocation"),
    STUB("libGLESv2.so", "glLinkProgram"),
    STUB("libGLESv2.so", "glReadPixels"),
    STUB("libGLESv2.so", "glShaderSource"),
    STUB("libGLESv2.so", "glStencilFuncSeparate"),
    STUB("libGLESv2.so", "glStencilMask"),
    STUB("libGLESv2.so", "glStencilOpSeparate"),
    STUB("libGLESv2.so", "glTexImage2D"),
    STUB("libGLESv2.so", "glTexParameterf"),
    STUB("libGLESv2.so", "glTexParameteri"),
    STUB("libGLESv2.so", "glUniform1fv"),
    STUB("libGLESv2.so", "glUniform1i"),
    STUB("libGLESv2.so", "glUniform2fv"),
    STUB("libGLESv2.so", "glUniform3fv"),
    STUB("libGLESv2.so", "glUniform4fv"),
    STUB("libGLESv2.so", "glUniformMatrix3fv"),
    STUB("libGLESv2.so", "glUniformMatrix4fv"),
    STUB("libGLESv2.so", "glUseProgram"),
    STUB("libGLESv2.so", "glVertexAttribPointer"),
    STUB("libGLESv2.so", "glViewport"),
};

#define N_DYNLIB ((int)(sizeof(default_dynlib) / sizeof(default_dynlib[0])))

static uint32_t unimplemented_calls;

static const dynlib_impl *const impl_tables[] = {reimpl_mem, reimpl_runtime, reimpl_pthread,
                                                   reimpl_libc, reimpl_signal,
                                                   reimpl_android_asset, reimpl_string,
                                                   reimpl_printf, reimpl_time, reimpl_stdio,
                                                   reimpl_thread, reimpl_fileio, reimpl_net,
                                                   reimpl_gles, reimpl_libm, reimpl_opensles};

static int bind_handlers(void) {
  for (size_t t = 0; t < sizeof(impl_tables) / sizeof(impl_tables[0]); t++) {
    for (const dynlib_impl *im = impl_tables[t]; im->name; im++) {
      int found = 0;
      for (int i = 0; i < N_DYNLIB; i++) {
        if (default_dynlib[i].kind == DL_STUB && strcmp(default_dynlib[i].name, im->name) == 0) {
          default_dynlib[i].handler = im->handler;
          found = 1;
        }
      }
      if (!found) {
        fprintf(stderr, "[dynlib] host implementation of %s matches no import\n", im->name);
        return -1;
      }
    }
  }
  return 0;
}

int dynlib_init(void) {
  if (bind_handlers() != 0) return -1;
  int n_thunks = 0;
  uint32_t data_size = 0;
  for (int i = 0; i < N_DYNLIB; i++) {
    if (default_dynlib[i].kind == DL_STUB) n_thunks++;
    if (default_dynlib[i].kind == DL_DATA) data_size += (default_dynlib[i].size + 15) & ~15u;
  }

  uint32_t thunks = gm_alloc((uint32_t)n_thunks * 8, 0, "dynlib thunks");
  uint32_t data = gm_alloc(data_size, 0, "dynlib data");
  if (!thunks || !data) return -1;

  for (int i = 0; i < N_DYNLIB; i++) {
    dynlib_entry *e = &default_dynlib[i];
    switch (e->kind) {
    case DL_STUB:
      e->addr = thunks;
      gm_w32(thunks, 0xEF000000u | DYNLIB_SVC_BASE | (uint32_t)i); /* svc #imm */
      gm_w32(thunks + 4, 0xE12FFF1Eu);                              /* bx lr */
      thunks += 8;
      break;
    case DL_DATA:
      e->addr = data;
      data += (e->size + 15) & ~15u;
      break;
    case DL_WEAK_NULL:
      e->addr = 0;
      break;
    }
  }
  return libc_init_data();
}

const dynlib_entry *dynlib_find(const char *name) {
  for (int i = 0; i < N_DYNLIB; i++)
    if (strcmp(default_dynlib[i].name, name) == 0) return &default_dynlib[i];
  return NULL;
}

int dynlib_count(void) { return N_DYNLIB; }

const dynlib_entry *dynlib_at(int index) {
  return index >= 0 && index < N_DYNLIB ? &default_dynlib[index] : NULL;
}

/* DL_TRACE_IMPORTS=1 logs every implemented import call with r0-r3, lr and
 * the result (debug aid). */
static int trace_imports(void) {
  static int on = -1;
  if (on < 0) on = getenv("DL_TRACE_IMPORTS") != NULL;
  return on;
}

void dynlib_dispatch(struct cpu *c, uint32_t index) {
  if (index >= (uint32_t)N_DYNLIB || default_dynlib[index].kind != DL_STUB) {
    fprintf(stderr, "[dynlib] bad thunk index %u\n", index);
    __atomic_fetch_add(&unimplemented_calls, 1, __ATOMIC_RELAXED);
    return;
  }
  dynlib_entry *e = &default_dynlib[index];
  __atomic_fetch_add(&e->calls, 1, __ATOMIC_RELAXED);
  if (e->handler) {
    if (trace_imports()) {
      uint32_t lr = c->ops->get_reg(c, CPU_REG_LR);
      fprintf(stderr, "[import] %s(%08x, %08x, %08x, %08x) lr %08x", e->name,
              c->ops->get_reg(c, 0), c->ops->get_reg(c, 1), c->ops->get_reg(c, 2),
              c->ops->get_reg(c, 3), lr);
      e->handler(c);
      fprintf(stderr, " -> %08x\n", c->ops->get_reg(c, 0));
      return;
    }
    e->handler(c);
    return;
  }
  fprintf(stderr, "[stub] %s!%s called (unimplemented): r0-r3 %08x %08x %08x %08x, lr %08x\n",
          e->lib, e->name, c->ops->get_reg(c, 0), c->ops->get_reg(c, 1), c->ops->get_reg(c, 2),
          c->ops->get_reg(c, 3), c->ops->get_reg(c, CPU_REG_LR));
  __atomic_fetch_add(&unimplemented_calls, 1, __ATOMIC_RELAXED);
}

uint32_t dynlib_unimplemented_calls(void) { return unimplemented_calls; }

#define MAX_HOST_THUNKS 256

static struct {
  const char *name;
  dl_handler fn;
} host_fns[MAX_HOST_THUNKS];
static uint32_t host_thunks, n_host_fns;
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t dynlib_host_thunk(const char *name, dl_handler fn) {
  pthread_mutex_lock(&host_lock);
  uint32_t addr = 0;
  if (!host_thunks) host_thunks = gm_alloc(MAX_HOST_THUNKS * 8, 0, "host thunks");
  if (host_thunks && n_host_fns < MAX_HOST_THUNKS) {
    uint32_t i = n_host_fns;
    addr = host_thunks + i * 8;
    host_fns[i].name = name;
    host_fns[i].fn = fn;
    gm_w32(addr, 0xEF000000u | DYNLIB_HOST_SVC | i); /* svc #imm */
    gm_w32(addr + 4, 0xE12FFF1Eu);                   /* bx lr */
    __atomic_store_n(&n_host_fns, i + 1, __ATOMIC_RELEASE);
  } else {
    fprintf(stderr, "[dynlib] no room for host thunk %s\n", name);
  }
  pthread_mutex_unlock(&host_lock);
  return addr;
}

void dynlib_host_dispatch(struct cpu *c, uint32_t index) {
  if (index >= __atomic_load_n(&n_host_fns, __ATOMIC_ACQUIRE)) {
    fprintf(stderr, "[dynlib] bad host thunk index %u\n", index);
    __atomic_fetch_add(&unimplemented_calls, 1, __ATOMIC_RELAXED);
    return;
  }
  if (trace_imports()) {
    fprintf(stderr, "[host] %s(%08x, %08x, %08x, %08x) lr %08x", host_fns[index].name,
            c->ops->get_reg(c, 0), c->ops->get_reg(c, 1), c->ops->get_reg(c, 2),
            c->ops->get_reg(c, 3), c->ops->get_reg(c, CPU_REG_LR));
    host_fns[index].fn(c);
    fprintf(stderr, " -> %08x\n", c->ops->get_reg(c, 0));
    return;
  }
  host_fns[index].fn(c);
}
