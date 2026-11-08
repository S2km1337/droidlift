#include "apk.h"

#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>

extern char **environ;

static int run_unzip(char *const argv[], const char *apk_path) {
  pid_t pid;
  int status;
  if (posix_spawnp(&pid, "unzip", NULL, NULL, argv, environ) != 0) {
    perror("apk_unpack: spawn unzip");
    return -1;
  }
  if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "apk_unpack: unzip of %s failed\n", apk_path);
    return -1;
  }
  return 0;
}

int apk_unpack_libs(const char *apk_path, const char *out_dir) {
  char *argv[] = {"unzip", "-o", "-q", (char *)apk_path, APK_LIB_DIR "/*.so",
                  "-d", (char *)out_dir, NULL};
  return run_unzip(argv, apk_path);
}

/* -n: the assets are 71 MB and never change, so existing files are kept. */
int apk_unpack_assets(const char *apk_path, const char *out_dir) {
  char *argv[] = {"unzip", "-n", "-q", (char *)apk_path, APK_ASSET_DIR "/*",
                  "-d", (char *)out_dir, NULL};
  return run_unzip(argv, apk_path);
}
