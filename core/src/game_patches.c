/*
 * Load-time patches to a game's own code and data: the engine (see
 * game_patches.h). The table, dl_game_patches, comes from the game
 * (droidlift_add_game PATCHES), or from patches_none.c.
 */
#include "game_patches.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>



const game_patch *game_patch_at(int i) {
  if (i < 0) return NULL;
  for (int k = 0; k <= i; k++)
    if (!dl_game_patches[k].soname) return NULL;
  return &dl_game_patches[i];
}

game_patch_result game_patch_apply(const game_patch *p, uint8_t *image, uint32_t size) {
  if (p->vaddr > size || p->len > size - p->vaddr || p->guard_vaddr > size ||
      p->guard_len > size - p->guard_vaddr)
    return GP_OUT_OF_RANGE;
  if (memcmp(image + p->guard_vaddr, p->guard, p->guard_len) != 0) return GP_MISMATCH;
  if (memcmp(image + p->vaddr, p->after, p->len) == 0) return GP_ALREADY;
  if (memcmp(image + p->vaddr, p->before, p->len) != 0) return GP_MISMATCH;
  memcpy(image + p->vaddr, p->after, p->len);
  return GP_APPLIED;
}

int game_patches_apply(const char *soname, uint8_t *image, uint32_t size) {
  int n = 0;
  for (const game_patch *p = dl_game_patches; p->soname; p++) {
    if (strcmp(p->soname, soname) != 0) continue;
    game_patch_result r = game_patch_apply(p, image, size);
    static const char *const what[] = {
        [GP_APPLIED] = "applied",
        [GP_ALREADY] = "already applied",
        [GP_MISMATCH] = "SKIPPED: the bytes differ (another build of the game?)",
        [GP_OUT_OF_RANGE] = "SKIPPED: the library is too small (another build of the game?)",
    };
    printf("[patch] %s: %s at 0x%x %s (%s)\n", soname, p->name, p->vaddr, what[r], p->why);
    if (r == GP_APPLIED || r == GP_ALREADY) n++;
  }
  return n;
}

int game_patches_enabled(void) {
  const char *e = getenv("DL_GAME_PATCHES");
  return !(e && (!strcmp(e, "off") || !strcmp(e, "0")));
}
