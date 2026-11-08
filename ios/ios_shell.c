#include "ios_shell.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "ios_log.h"
#include "droidlift.h"
#include "storage.h"

#define MAX_ARGS 128

void ios_shell_exit(void) { ios_log_flush(); }

void ios_shell_args(int *argc, char ***argv) {
  /* The log: a file in the container and os_log (a device's stdout goes
   * nowhere), and the console where there is one (ios_log.h). */
  ios_log_start();

  /* Landscape only, either way round (Info.plist says the same). */
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
  /* The home indicator dims, and a swipe from the bottom edge needs a second
   * swipe to leave, so a swipe in the game doesn't quit it. */
  SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
  /* Fingers are the game's touches (gfx.c); no made-up mouse as well. */
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
  /* The audio session of a game: SoloAmbient follows the silent switch and
   * stops other apps' audio while the game plays (SDL's default, Playback,
   * would ignore the silent switch). DL_AUDIO_CATEGORY=ambient instead
   * lets other apps' music play on. */
  const char *category = getenv("DL_AUDIO_CATEGORY");
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY,
              category && *category ? category : "AVAudioSessionCategorySoloAmbient");

  /* The app bundle: read-only, holds apk/ and Frameworks/ (build-ios-sim.sh). */
  static char work[PATH_MAX], angle[PATH_MAX];
  const char *base = SDL_GetBasePath();
  if (!base) base = "./";
  snprintf(work, sizeof work, "%sapk", base);
  snprintf(angle, sizeof angle, "%sFrameworks", base);
  const char *data = dl_storage_dir(dl_game_active()->storage);
  printf("[ios] bundle %s\n[ios] data %s\n[ios] log %s\n[ios] default cpu %s\n", base, data ? data : "(none)",
         ios_log_path() ? ios_log_path() : "(none)", cpu_default_backend());

  static char *args[MAX_ARGS + 1];
  int n = 0;
  args[n++] = (*argv)[0];
  static char *const defaults[] = {"--unpacked", "--work", work, "--angle", angle,
                                   "--init-test", "--window"};
  for (size_t i = 0; i < sizeof defaults / sizeof *defaults; i++) args[n++] = defaults[i];
  for (int i = 1; i < *argc && n < MAX_ARGS; i++) {
    const char *a = (*argv)[i];
    /* Xcode and launchd pass NSUserDefaults pairs such as
     * "-NSDocumentRevisionsDebugMode YES": not ours. */
    if (a[0] == '-' && a[1] != '-') {
      if (i + 1 < *argc && (*argv)[i + 1][0] != '-') i++;
      continue;
    }
    args[n++] = (*argv)[i];
  }
  args[n] = NULL;
  *argc = n;
  *argv = args;
}
