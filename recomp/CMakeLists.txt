# Static ARMv7 -> C recompiler.
#
# droidlift-recomp turns a library (an armeabi-v7a ELF .so) into C that
# operates on a guest-state struct and guest memory (src/); rc_runtime
# dispatches between the recompiled functions (runtime/, built by the
# top-level CMakeLists.txt). droidlift_add_game() runs it on a game's
# libraries; a game with load-time patches gets its own copy of the tool
# (<game>-recomp) with the patch table linked in, so the recompiled code
# matches the patched image the loader runs.

add_library(dl_recomp_objs OBJECT src/main.cpp src/elf.cpp src/decode.cpp src/emit.cpp src/program.cpp)
target_compile_options(dl_recomp_objs PRIVATE -Wall -Wextra -Werror)
target_include_directories(dl_recomp_objs PRIVATE "${DL_ROOT}/core/src")

# The tool without game patches.
add_executable(droidlift-recomp $<TARGET_OBJECTS:dl_recomp_objs>
  "${DL_ROOT}/core/src/game_patches.c" "${DL_ROOT}/core/src/patches_none.c")
target_include_directories(droidlift-recomp PRIVATE "${DL_ROOT}/core/src")
