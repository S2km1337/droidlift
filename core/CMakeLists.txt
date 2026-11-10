# The loader core: everything a port links (droidlift.h). Game code comes in
# through the profile; droidlift_add_game() adds cpu_recomp.c and the game's
# recompiled libraries to the game's own target.

# Our ARMv7 interpreter: a CPU backend of its own, and the recompiler's
# fallback (dl_interp_rc, with rc_runtime).
add_library(dl_interp STATIC
  src/interp/interp.c
  src/interp/interp_arm.c
  src/interp/interp_thumb.c
  src/interp/interp_media.c
  src/interp/interp_vfp.c
  src/interp/interp_neon.c
)
target_compile_options(dl_interp PRIVATE -Wall -Wextra -Werror)
target_include_directories(dl_interp PUBLIC src)
add_library(dl_interp_rc STATIC src/interp/interp_rc.c)
target_compile_options(dl_interp_rc PRIVATE -Wall -Wextra -Werror)
target_link_libraries(dl_interp_rc PUBLIC dl_interp rc_runtime)

add_library(droidlift_core STATIC
  src/dl_main.c
  src/dl_entry.c
  src/storage.c
  src/startup.c
  src/lifecycle.c
  src/apk.c
  src/dynlib.c
  src/guest_mem.c
  src/kuser.c
  src/so_util.c
  src/cpu.c
  src/cpu_selftest.c
  src/interp_selftest.c
  src/fake_jni.c
  src/jni_selftest.c
  src/gfx.c
  src/fmt_selftest.c
  src/game_patches.c
  src/reimpl/android_asset.c
  src/reimpl/mem.c
  src/reimpl/runtime.c
  src/reimpl/pthread.c
  src/reimpl/libc.c
  src/reimpl/signal.c
  src/reimpl/string.c
  src/reimpl/printf.c
  src/reimpl/time.c
  src/reimpl/stdio.c
  src/reimpl/thread.c
  src/reimpl/fileio.c
  src/reimpl/net.c
  src/reimpl/gles.c
  src/pvrtc.c
  src/reimpl/libm.c
  src/reimpl/opensles.c
  src/audio.c
  src/input.c
)
target_compile_options(droidlift_core PRIVATE -Wall -Wextra -Werror)
target_include_directories(droidlift_core PUBLIC src)
# Window, input and audio output on the host: SDL3 from Homebrew on the Mac;
# for iOS, the static build of scripts/fetch-ios-deps.sh.
find_package(SDL3 REQUIRED CONFIG)
target_link_libraries(droidlift_core PUBLIC z SDL3::SDL3 dl_interp_rc)
target_compile_definitions(droidlift_core PUBLIC DL_DEFAULT_CPU="${DL_DEFAULT_CPU}" APK_LIB_DIR="${DL_APK_LIB_DIR}")
if (DL_WITH_DYNARMIC)
  target_sources(droidlift_core PRIVATE src/cpu_dynarmic.cpp)
  target_link_libraries(droidlift_core PUBLIC dynarmic)
else()
  target_sources(droidlift_core PRIVATE src/cpu_dynarmic_none.c)
  target_compile_definitions(droidlift_core PUBLIC DL_NO_DYNARMIC=1)
endif()
if (DL_RECOMP)
  # cpu_recomp_create comes from the game's target (droidlift_add_game).
  target_compile_definitions(droidlift_core PUBLIC DL_HAVE_RECOMP)
endif()
if (IOS)
  target_sources(droidlift_core PRIVATE "${DL_ROOT}/ios/ios_shell.c" "${DL_ROOT}/ios/ios_log.c")
  target_include_directories(droidlift_core PUBLIC "${DL_ROOT}/ios")
  target_compile_definitions(droidlift_core PUBLIC DL_IOS=1)
endif()
