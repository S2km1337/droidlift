#include "storage.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>

const char *dl_storage_dir(const char *name) {
  static char dir[PATH_MAX];
  if (dir[0]) return dir;
  /* No organisation folder: the app's own folder is enough, and on iOS the
   * whole Application Support directory is the app's anyway. */
  char *p = SDL_GetPrefPath("", name && *name ? name : "droidlift");
  if (!p) {
    fprintf(stderr, "[storage] no app storage: %s\n", SDL_GetError());
    return NULL;
  }
  snprintf(dir, sizeof dir, "%s", p);
  SDL_free(p);
  return dir;
}

/* mkdir -p */
static int make_dir(const char *path) {
  char p[PATH_MAX];
  snprintf(p, sizeof p, "%s", path);
  for (char *s = p + 1; *s; s++)
    if (*s == '/') {
      *s = 0;
      mkdir(p, 0755);
      *s = '/';
    }
  if (mkdir(p, 0755) == 0 || errno == EEXIST) return 0;
  perror(path);
  return -1;
}

int dl_storage_prepare(const char *data_dir) {
  char path[PATH_MAX];
  if (make_dir(data_dir) != 0) return -1;
  snprintf(path, sizeof path, "%s/files", data_dir);
  if (make_dir(path) != 0) return -1;
  snprintf(path, sizeof path, "%s/cache", data_dir);
  return make_dir(path);
}
