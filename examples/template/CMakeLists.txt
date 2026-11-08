cmake_minimum_required(VERSION 3.24)
project(my_port C CXX)

# The game's APK: your own copy, never committed. droidlift's scripts pass
# DL_APK too.
set(DL_APK "${CMAKE_SOURCE_DIR}/original/game.apk" CACHE FILEPATH "The game's APK")

add_subdirectory(droidlift)

droidlift_add_game(my-game
  APK "${DL_APK}"
  SOURCES game/profile.c
  # Once the game runs on dynarmic: the libraries to recompile ahead of time,
  # library:C prefix:shards (shards build in parallel). Needed for the
  # iPhone; configure with -DDL_RECOMP=OFF until then.
  RECOMP "libgame:game:16"
  # PATCHES game/patches.c          # load-time fixes to the game (game_patches.h)
  APP_NAME MyGame                   # the iOS app; keep in step with droidlift.env
  ICON res/drawable-xxhdpi/icon.png # the launcher icon inside the APK
  ORIENTATION landscape)
