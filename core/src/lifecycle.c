#include "lifecycle.h"

#include <SDL3/SDL.h>
#include <stdio.h>

#include "droidlift.h"

static cpu *life_cpu;

static bool SDLCALL on_event(void *userdata, SDL_Event *e) {
  (void)userdata;
  /* UIKit's notifications and SDL's app delegate both report each change,
   * so each event comes twice; the steps run once anyway (startup.c). */
  static Uint32 last;
  if (e->type == last) return true;
  if (e->type >= SDL_EVENT_TERMINATING && e->type <= SDL_EVENT_DID_ENTER_FOREGROUND) last = e->type;
  int r = 0;
  switch (e->type) {
  case SDL_EVENT_WILL_ENTER_BACKGROUND: /* applicationWillResignActive */
    printf("[life] will resign active: pause\n");
    r = dl_app_pause(life_cpu);
    break;
  case SDL_EVENT_DID_ENTER_BACKGROUND:
    printf("[life] background: stop and save\n");
    r = dl_app_stop(life_cpu);
    break;
  case SDL_EVENT_TERMINATING:
    printf("[life] terminating: stop and save\n");
    r = dl_app_stop(life_cpu);
    break;
  case SDL_EVENT_DID_ENTER_FOREGROUND: /* applicationDidBecomeActive, also at launch */
    if (!dl_app_is_paused()) return true;
    printf("[life] active again: resume\n");
    r = dl_app_resume(life_cpu);
    break;
  default:
    return true;
  }
  if (r != 0) fprintf(stderr, "[life] the game faulted in a lifecycle call\n");
  fflush(stdout);
  return true;
}

void lifecycle_attach(cpu *c) {
  if (life_cpu) return;
  life_cpu = c;
  SDL_AddEventWatch(on_event, NULL);
}
