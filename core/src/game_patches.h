/*
 * Load-time patches to a game's own code and data.
 *
 * The APK is never modified: each patch is applied to a library's image in
 * guest memory after so_relocate and before any guest code runs. A patch
 * names the exact bytes it expects (its target and a guard on the code that
 * uses it); if any of them differ, as in another build of the game, it is
 * skipped and the image is left untouched.
 *
 * The table, dl_game_patches, is the game's (a source file passed to
 * droidlift_add_game as PATCHES; patches_none.c otherwise): plain C with no
 * loader dependencies, so the static recompiler (recomp/) links it too and
 * applies the same patches to its input with game_patches_apply on the file
 * image. It ends with an entry whose soname is NULL.
 */
#ifndef GAME_PATCHES_H
#define GAME_PATCHES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct game_patch {
  const char *soname; /* the library it applies to, e.g. "libgame.so" */
  const char *name;   /* short id for logs */
  const char *why;    /* one line for logs */
  /* The patched bytes: `before` must be at vaddr, `after` replaces it. */
  uint32_t vaddr, len;
  const char *before, *after;
  /* Guard: these bytes at guard_vaddr must match too (the code that uses
   * the patched bytes, so the patch is tied to this build). */
  uint32_t guard_vaddr, guard_len;
  const char *guard;
} game_patch;

typedef enum {
  GP_APPLIED,         /* before matched; after written */
  GP_ALREADY,         /* after already there (e.g. applied twice) */
  GP_MISMATCH,        /* target or guard differ: left untouched */
  GP_OUT_OF_RANGE,    /* the image is too small: left untouched */
} game_patch_result;

/* The patch table, terminated by an entry with soname NULL. */
/* The game's table, ending with {0}. */
extern const game_patch dl_game_patches[];

const game_patch *game_patch_at(int i);

/* Applies one patch to an image: the library's bytes from vaddr 0 (in guest
 * memory, base + vaddr). The file works too while every patched and guarded
 * address lies in the first PT_LOAD, which these libraries map from file
 * offset 0, as GNU ld lays out armeabi-v7a libraries. */
game_patch_result game_patch_apply(const game_patch *p, uint8_t *image, uint32_t size);

/* Applies every patch for soname, logging one line per patch to stdout.
 * Returns the number applied or already present. */
int game_patches_apply(const char *soname, uint8_t *image, uint32_t size);

/* 0 if DL_GAME_PATCHES=off (the game exactly as on Android). */
int game_patches_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
