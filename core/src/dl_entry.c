/*
 * The process entry point: dl_main with the port's profile (DL_MAIN). On iOS
 * SDL owns the real main (UIApplicationMain), which calls this one as
 * SDL_main; SDL_main.h does the renaming and must be included in exactly
 * one file, this one.
 */
#include "droidlift.h"

#ifdef DL_IOS
#include <SDL3/SDL_main.h>
#endif

int main(int argc, char **argv) { return dl_main(dl_the_game, argc, argv); }
