# droidlift: the toolkit. A port adds it with add_subdirectory() and builds
# its game with droidlift_add_game() (cmake/droidlift.cmake, docs/porting.md).
#
#   add_subdirectory(droidlift)
#   droidlift_add_game(my-game
#     APK     "${MY_APK}"
#     LIBS    libengine.so libgame.so
#     RECOMP  "libengine:engine:8" "libgame:game:32"
#     SOURCES game/profile.c ...)
cmake_minimum_required(VERSION 3.24)
project(droidlift C CXX)

if (NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 11)
  set(CMAKE_C_STANDARD_REQUIRED ON)
endif()
if (NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

set(DL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "droidlift's source directory")
# Third-party sources (scripts/fetch-deps.sh, scripts/fetch-angle.sh,
# scripts/fetch-ios-deps.sh) go here; by default the port's third_party/.
set(DL_THIRD_PARTY "${CMAKE_SOURCE_DIR}/third_party" CACHE PATH "Where the fetch scripts put dynarmic, ANGLE and SDL3")
# The APK's native library directory. Older games ship lib/armeabi (ARMv5TE/v6
# code, which the ARMv7 backends also run) instead of lib/armeabi-v7a.
set(DL_APK_LIB_DIR "lib/armeabi-v7a" CACHE STRING "The APK's native library directory")

# ---- CPU backends -------------------------------------------------------------
# dynarmic is a JIT, which a stock iPhone does not allow, so the device build
# leaves it out and runs the recompiled libraries, with our interpreter as the
# fallback and as a backend of its own.
if (IOS AND NOT CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
  set(_dl_jit OFF)
else()
  set(_dl_jit ON)
endif()
option(DL_WITH_DYNARMIC "Build the dynarmic JIT backend" ${_dl_jit})
# The recompiled backend (--cpu recomp): each game's libraries as C. OFF
# builds games without it (faster to configure; no recomp on the iPhone).
option(DL_RECOMP "Link the statically recompiled libraries (--cpu recomp)" ON)
if (DL_WITH_DYNARMIC)
  set(_dl_cpu dynarmic)
elseif (DL_RECOMP)
  set(_dl_cpu recomp)
else()
  set(_dl_cpu interp)
endif()
set(DL_DEFAULT_CPU "${_dl_cpu}" CACHE STRING "Default CPU backend (--cpu overrides it at launch)")
if (DL_DEFAULT_CPU STREQUAL "dynarmic" AND NOT DL_WITH_DYNARMIC)
  message(FATAL_ERROR "DL_DEFAULT_CPU=dynarmic needs DL_WITH_DYNARMIC=ON")
endif()
if (DL_DEFAULT_CPU STREQUAL "recomp" AND NOT DL_RECOMP)
  message(FATAL_ERROR "DL_DEFAULT_CPU=recomp needs DL_RECOMP=ON")
endif()

if (DL_WITH_DYNARMIC)
  execute_process(COMMAND brew list --versions boost OUTPUT_VARIABLE _boost_ver OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" DL_BOOST_VERSION "${_boost_ver}")
  # The iOS build points this at its own copy (build-ios-sim.sh): dynarmic's
  # build writes generated files into its source tree.
  set(DL_DYNARMIC_DIR "${DL_THIRD_PARTY}/dynarmic" CACHE PATH "dynarmic's source")
  if (NOT EXISTS "${DL_DYNARMIC_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "dynarmic is missing: run droidlift/scripts/fetch-deps.sh")
  endif()
  # Boost from its Homebrew keg, so /opt/homebrew/include (which also holds
  # Homebrew's newer fmt) does not shadow dynarmic's bundled fmt headers.
  set(Boost_DIR "/opt/homebrew/opt/boost/lib/cmake/Boost-${DL_BOOST_VERSION}" CACHE PATH "")
  set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
  set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
  set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
  set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
  add_subdirectory("${DL_DYNARMIC_DIR}" third_party/dynarmic EXCLUDE_FROM_ALL SYSTEM)
endif()

# Graphics: ANGLE's Metal backend, built by scripts/fetch-angle.sh and loaded
# at run time.
set(DL_ANGLE_DIR "${DL_THIRD_PARTY}/angle/out/mac-metal" CACHE PATH
    "Directory with ANGLE's libEGL.dylib and libGLESv2.dylib")

# ---- the recompiler's runtime, the interpreter and the core --------------------
add_library(rc_runtime STATIC recomp/runtime/rc_runtime.c)
target_include_directories(rc_runtime PUBLIC recomp/runtime)
target_compile_options(rc_runtime PRIVATE -Wall -Wextra -Werror)

add_subdirectory(core)

# The recompiler is a Mac tool: iOS builds compile the C a Mac build made.
if (NOT IOS)
  add_subdirectory(recomp)
endif()

include(cmake/droidlift.cmake)
