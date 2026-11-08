/*
 * libandroid's AAssetManager API over the APK's assets/ directory, unpacked
 * into the work directory (apk_unpack_assets). Assets are plain host files, so
 * an AAsset is a host FILE* behind a small guest handle.
 *
 * There is one asset manager: AAssetManager_fromJava returns the same handle
 * for every Java AssetManager.
 */
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../cpu.h"
#include "../guest_mem.h"
#include "reimpl.h"

#define MAX_ASSETS 256
#define G_SEEK_SET 0
#define G_SEEK_CUR 1
#define G_SEEK_END 2

static char asset_root[PATH_MAX];
static uint32_t manager; /* guest handle of the one AAssetManager */

/* Guest handles are 4-byte guest blocks holding a 1-based slot number, so a
 * stale or foreign pointer is caught instead of dereferenced on the host. */
typedef struct host_asset {
  uint32_t handle;
  FILE *f;
  char path[PATH_MAX];
  long length;
} host_asset;
static host_asset assets[MAX_ASSETS];

typedef struct host_dir {
  uint32_t handle;
  DIR *d;
  char path[PATH_MAX];
  uint32_t name_buf; /* guest copy of the last name returned */
} host_dir;
static host_dir dirs[16];

void android_asset_set_root(const char *dir) {
  snprintf(asset_root, sizeof(asset_root), "%s", dir);
}

static int asset_verbose = -1;

void android_asset_set_verbose(int on) { asset_verbose = on; }

static int verbose(void) {
  if (asset_verbose < 0) {
    const char *e = getenv("DL_VERBOSE");
    asset_verbose = e && *e && *e != '0';
  }
  return asset_verbose;
}

static void asset_path(char *out, size_t n, uint32_t gname) {
  const char *name = gname ? (const char *)g2h(gname) : "";
  while (*name == '/') name++;
  snprintf(out, n, "%s/%s", asset_root, name);
}

static host_asset *asset_get(uint32_t handle) {
  if (!handle) return NULL;
  uint32_t slot = gm_r32(handle);
  if (slot == 0 || slot > MAX_ASSETS || assets[slot - 1].handle != handle) {
    fprintf(stderr, "[asset] bad AAsset* 0x%08x\n", handle);
    return NULL;
  }
  return &assets[slot - 1];
}

static void h_AAssetManager_fromJava(cpu *c) {
  if (!manager) manager = guest_malloc(4);
  cpu_ret(c, manager);
}

/* AAssetManager_open(mgr, filename, mode): the mode (streaming, buffer, ...)
 * is only a hint on Android and changes nothing here. */
static void h_AAssetManager_open(cpu *c) {
  uint32_t gname = cpu_arg(c, 1);
  int slot = -1;
  for (int i = 0; i < MAX_ASSETS; i++)
    if (!assets[i].handle) {
      slot = i;
      break;
    }
  host_asset *a = &assets[slot < 0 ? 0 : slot];
  char path[PATH_MAX];
  asset_path(path, sizeof(path), gname);
  FILE *f = slot < 0 ? NULL : fopen(path, "rb");
  struct stat st;
  if (!f || fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
    if (f) fclose(f);
    /* A miss is usually the engine's format probing (.pvr, .ktx, then .png);
     * running out of slots is a real problem and always logged. */
    if (slot < 0)
      fprintf(stderr, "[asset] open(\"%s\"): all %d asset slots in use -> NULL\n",
              gname ? (const char *)g2h(gname) : "", MAX_ASSETS);
    else if (verbose())
      fprintf(stderr, "[asset] open(\"%s\") -> NULL\n", gname ? (const char *)g2h(gname) : "");
    cpu_ret(c, 0);
    return;
  }
  a->handle = guest_malloc(4);
  gm_w32(a->handle, (uint32_t)slot + 1);
  a->f = f;
  a->length = (long)st.st_size;
  snprintf(a->path, sizeof(a->path), "%s", path);
  if (verbose())
    fprintf(stderr, "[asset] open(\"%s\") -> %ld bytes\n", (const char *)g2h(gname), a->length);
  cpu_ret(c, a->handle);
}

static void h_AAsset_close(cpu *c) {
  host_asset *a = asset_get(cpu_arg(c, 0));
  if (!a) return;
  fclose(a->f);
  gm_w32(a->handle, 0);
  guest_free(a->handle);
  memset(a, 0, sizeof(*a));
}

static void h_AAsset_read(cpu *c) {
  host_asset *a = asset_get(cpu_arg(c, 0));
  uint32_t buf = cpu_arg(c, 1), count = cpu_arg(c, 2);
  if (!a) {
    cpu_ret(c, (uint32_t)-1);
    return;
  }
  cpu_ret(c, (uint32_t)fread(g2h(buf), 1, count, a->f));
}

static void h_AAsset_seek(cpu *c) {
  host_asset *a = asset_get(cpu_arg(c, 0));
  int32_t off = (int32_t)cpu_arg(c, 1);
  uint32_t whence = cpu_arg(c, 2);
  int w = whence == G_SEEK_SET ? SEEK_SET : whence == G_SEEK_CUR ? SEEK_CUR : SEEK_END;
  if (!a || whence > G_SEEK_END || fseek(a->f, off, w) != 0) {
    cpu_ret(c, (uint32_t)-1);
    return;
  }
  cpu_ret(c, (uint32_t)ftell(a->f));
}

static void h_AAsset_getLength(cpu *c) {
  host_asset *a = asset_get(cpu_arg(c, 0));
  cpu_ret(c, a ? (uint32_t)a->length : 0);
}

static void h_AAsset_getRemainingLength(cpu *c) {
  host_asset *a = asset_get(cpu_arg(c, 0));
  cpu_ret(c, a ? (uint32_t)(a->length - ftell(a->f)) : 0);
}

/* AAsset_openFileDescriptor(asset, off_t *start, off_t *length): Android hands
 * out the APK's fd with the stored asset's offset. Here the asset is its own
 * file, so a fresh host fd with start 0. Guest fds are host fds. */
static void h_AAsset_openFileDescriptor(cpu *c) {
  host_asset *a = asset_get(cpu_arg(c, 0));
  uint32_t start = cpu_arg(c, 1), length = cpu_arg(c, 2);
  FILE *f = a ? fopen(a->path, "rb") : NULL;
  if (!f) {
    cpu_ret(c, (uint32_t)-1);
    return;
  }
  int fd = dup(fileno(f));
  fclose(f);
  if (start) gm_w32(start, 0);
  if (length) gm_w32(length, (uint32_t)a->length);
  cpu_ret(c, (uint32_t)fd);
}

static void h_AAssetManager_openDir(cpu *c) {
  uint32_t gname = cpu_arg(c, 1);
  host_dir *hd = NULL;
  for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
    if (!dirs[i].handle) {
      hd = &dirs[i];
      break;
    }
  char path[PATH_MAX];
  asset_path(path, sizeof(path), gname);
  /* Android returns a (possibly empty) directory even for missing paths. */
  if (!hd) {
    cpu_ret(c, 0);
    return;
  }
  hd->d = opendir(path);
  snprintf(hd->path, sizeof(hd->path), "%s", path);
  hd->handle = guest_malloc(4);
  gm_w32(hd->handle, (uint32_t)(hd - dirs) + 1);
  hd->name_buf = guest_malloc(NAME_MAX + 1);
  cpu_ret(c, hd->handle);
}

static host_dir *dir_get(uint32_t handle) {
  if (!handle) return NULL;
  uint32_t slot = gm_r32(handle);
  size_t n = sizeof(dirs) / sizeof(dirs[0]);
  if (slot == 0 || slot > n || dirs[slot - 1].handle != handle) return NULL;
  return &dirs[slot - 1];
}

/* Only regular files are listed, like Android's AAssetDir. */
static void h_AAssetDir_getNextFileName(cpu *c) {
  host_dir *hd = dir_get(cpu_arg(c, 0));
  struct dirent *e;
  while (hd && hd->d && (e = readdir(hd->d))) {
    char p[PATH_MAX];
    struct stat st;
    snprintf(p, sizeof(p), "%s/%s", hd->path, e->d_name);
    if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
      snprintf(g2h(hd->name_buf), NAME_MAX + 1, "%s", e->d_name);
      cpu_ret(c, hd->name_buf);
      return;
    }
  }
  cpu_ret(c, 0);
}

static void h_AAssetDir_close(cpu *c) {
  host_dir *hd = dir_get(cpu_arg(c, 0));
  if (!hd) return;
  if (hd->d) closedir(hd->d);
  guest_free(hd->name_buf);
  gm_w32(hd->handle, 0);
  guest_free(hd->handle);
  memset(hd, 0, sizeof(*hd));
}

const dynlib_impl reimpl_android_asset[] = {
    {"AAssetDir_close", h_AAssetDir_close},
    {"AAssetDir_getNextFileName", h_AAssetDir_getNextFileName},
    {"AAssetManager_fromJava", h_AAssetManager_fromJava},
    {"AAssetManager_open", h_AAssetManager_open},
    {"AAssetManager_openDir", h_AAssetManager_openDir},
    {"AAsset_close", h_AAsset_close},
    {"AAsset_getLength", h_AAsset_getLength},
    {"AAsset_getRemainingLength", h_AAsset_getRemainingLength},
    {"AAsset_openFileDescriptor", h_AAsset_openFileDescriptor},
    {"AAsset_read", h_AAsset_read},
    {"AAsset_seek", h_AAsset_seek},
    {NULL, NULL},
};
