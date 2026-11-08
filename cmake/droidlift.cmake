# droidlift_add_game(<target>
#   APK <path>                 the game's APK (read-only input, never copied
#                              into the source tree)
#   SOURCES <file>...          the game profile and its glue (droidlift.h)
#   [RECOMP <lib:prefix:shards>...]
#                              the libraries to recompile ahead of time,
#                              e.g. "libgame:game:32": lib/armeabi-v7a/libgame.so,
#                              C symbols game_*, split into 32 files that build
#                              in parallel. Required when DL_RECOMP is ON.
#   [PATCHES <file>]           the game's load-time patch table
#                              (game_patches.h: dl_game_patches)
#   [APP_NAME <name>]          iOS: the app's executable and bundle name
#                              (default: <target>)
#   [DISPLAY_NAME <name>]      iOS: the name under the icon (default: APP_NAME)
#   [BUNDLE_ID <id>]           iOS: the default bundle identifier (the device
#                              scripts replace it with one for the free team)
#   [VERSION <x.y.z>]          iOS: CFBundleShortVersionString (default 1.0)
#   [ICON <path in APK>]       iOS: the launcher icon inside the APK, e.g.
#                              res/drawable-xxhdpi-v4/icon.png
#   [ORIENTATION landscape|portrait|all])
#
# Builds the loader for the game: on the Mac an executable, on iOS the app
# bundle (with the APK's libraries and assets, ANGLE and the icon inside).
# With DL_RECOMP it also adds the targets <target>-recomp-gen (only the C)
# and <target>-recomp-census (coverage on stdout), and on the Mac
# DL_RECOMP_OUT_<target> (a cache variable) says where the C goes.

# _dl_recomp_files(<dir> <prefix> <shards> <out-var>): the files
# `droidlift-recomp --all --shards <shards> --prefix <prefix>` writes.
function(_dl_recomp_files dir pfx shards out)
  set(_files "${dir}/${pfx}_table.c")
  math(EXPR _last "${shards} - 1")
  foreach(i RANGE ${_last})
    string(LENGTH "${i}" _len)
    set(_num "${i}")
    if (_len EQUAL 1)
      set(_num "00${i}")
    elseif (_len EQUAL 2)
      set(_num "0${i}")
    endif()
    list(APPEND _files "${dir}/${pfx}_funcs_${_num}.c")
  endforeach()
  set(${out} "${_files}" PARENT_SCOPE)
endfunction()

# Generated C needs two roundings for VFP multiply-accumulate and sqrt as one
# instruction; it has unused locals, labels and redundant parentheses by
# design (-w).
set(DL_RECOMP_CFLAGS "-ffp-contract=off;-fno-math-errno;-w" CACHE INTERNAL "")

function(droidlift_add_game target)
  cmake_parse_arguments(G "" "APK;PATCHES;APP_NAME;DISPLAY_NAME;BUNDLE_ID;VERSION;ICON;ORIENTATION"
                        "SOURCES;RECOMP" ${ARGN})
  if (NOT G_APK OR NOT G_SOURCES)
    message(FATAL_ERROR "droidlift_add_game(${target}): APK and SOURCES are required")
  endif()
  if (DL_RECOMP AND NOT G_RECOMP)
    message(FATAL_ERROR "droidlift_add_game(${target}): DL_RECOMP is ON, so RECOMP must list the libraries")
  endif()

  add_executable(${target} ${G_SOURCES})
  set_property(TARGET ${target} PROPERTY DL_RECOMP_LIBS "${G_RECOMP}")
  target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(${target} PRIVATE droidlift_core)
  if (G_PATCHES)
    target_sources(${target} PRIVATE "${G_PATCHES}")
  else()
    target_sources(${target} PRIVATE "${DL_ROOT}/core/src/patches_none.c")
  endif()

  if (DL_RECOMP)
    _dl_add_recomp(${target})
  endif()
  if (IOS)
    _dl_add_ios_app(${target})
  endif()
endfunction()

# The recompiled libraries, linked into the game: rc_<prefix> per library,
# a registry the backend walks (dl_recomp_modules), and cpu_recomp.c.
macro(_dl_add_recomp target)
  set(_libs)
  set(_c_all)
  set(_externs "")
  set(_entries "")
  if (NOT IOS)
    # The game's libraries straight from the APK, unpacked into the build tree.
    set(_apk_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}-apk")
    set(_lib_dir "${_apk_dir}/lib/armeabi-v7a")
    set(_lib_files)
    set(_members)
    foreach(l IN LISTS G_RECOMP)
      string(REPLACE ":" ";" _parts "${l}")
      list(GET _parts 0 _lib)
      list(APPEND _lib_files "${_lib_dir}/${_lib}.so")
      list(APPEND _members "lib/armeabi-v7a/${_lib}.so")
    endforeach()
    add_custom_command(OUTPUT ${_lib_files}
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_apk_dir}"
      COMMAND "${CMAKE_COMMAND}" -E chdir "${_apk_dir}" "${CMAKE_COMMAND}" -E tar xf "${G_APK}" -- ${_members}
      DEPENDS "${G_APK}"
      COMMENT "Unpacking ${target}'s libraries for the recompiler")
    set(DL_RECOMP_LIB_DIR_${target} "${_lib_dir}" CACHE INTERNAL "")
    set(DL_RECOMP_LIB_FILES_${target} "${_lib_files}" CACHE INTERNAL "")
    # Recompiled C is derived from the game binary: it goes to an ignored
    # directory, never into git.
    set(DL_RECOMP_OUT_${target} "${CMAKE_SOURCE_DIR}/generated.nosync/recomp" CACHE PATH
        "Where ${target}'s recompiled C goes")
    set(_out_dir "${DL_RECOMP_OUT_${target}}")
    # A game with patches recompiles the patched image, with its own tool.
    if (G_PATCHES)
      set(_tool ${target}-recomp)
      add_executable(${_tool} $<TARGET_OBJECTS:dl_recomp_objs> "${DL_ROOT}/core/src/game_patches.c" "${G_PATCHES}")
      target_include_directories(${_tool} PRIVATE "${DL_ROOT}/core/src")
    else()
      set(_tool droidlift-recomp)
    endif()
    set(DL_RECOMP_TOOL_${target} ${_tool} CACHE INTERNAL "")
  else()
    # iOS: a Mac build of the same project generates the C (the recompiler is
    # a Mac tool); this build compiles it.
    set(DL_RECOMP_HOST_BUILD "${CMAKE_SOURCE_DIR}/build" CACHE PATH
        "A Mac build of this project, which generates the recompiled C")
    if (NOT EXISTS "${DL_RECOMP_HOST_BUILD}/CMakeCache.txt")
      message(FATAL_ERROR "DL_RECOMP needs a Mac build to generate the C: "
              "cmake -S . -B build -G Ninja (or DL_RECOMP=OFF)")
    endif()
    load_cache("${DL_RECOMP_HOST_BUILD}" READ_WITH_PREFIX _host_ DL_RECOMP_OUT_${target})
    set(_out_dir "${_host_DL_RECOMP_OUT_${target}}")
  endif()

  set(_census)
  foreach(l IN LISTS G_RECOMP)
    string(REPLACE ":" ";" _parts "${l}")
    list(GET _parts 0 _lib)
    list(GET _parts 1 _pfx)
    list(GET _parts 2 _n)
    set(_dir "${_out_dir}/all")
    _dl_recomp_files("${_dir}" ${_pfx} ${_n} _c)
    if (NOT IOS)
      add_custom_command(OUTPUT ${_c}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}"
        COMMAND ${_tool} --elf "${_lib_dir}/${_lib}.so" --out "${_dir}" --prefix ${_pfx}
                --soname ${_lib}.so --all --shards ${_n}
        DEPENDS ${_tool} "${_lib_dir}/${_lib}.so"
        COMMENT "Recompiling all of ${_lib}.so to C")
      list(APPEND _census COMMAND ${_tool} --elf "${_lib_dir}/${_lib}.so" --census)
    else()
      set_source_files_properties(${_c} PROPERTIES GENERATED TRUE)
    endif()
    set_source_files_properties(${_c} PROPERTIES COMPILE_OPTIONS "${DL_RECOMP_CFLAGS}")
    add_library(rc_${_pfx} STATIC ${_c})
    target_link_libraries(rc_${_pfx} PRIVATE rc_runtime)
    list(APPEND _libs rc_${_pfx})
    list(APPEND _c_all ${_c})
    string(APPEND _externs "extern const rc_module ${_pfx}_module;\n")
    string(APPEND _entries "    &${_pfx}_module,\n")
  endforeach()

  set(_registry "${CMAKE_CURRENT_BINARY_DIR}/${target}_recomp_modules.c")
  file(CONFIGURE OUTPUT "${_registry}" CONTENT
"/* Generated by droidlift_add_game(${target}): its recompiled libraries. */
#include \"rc_runtime.h\"

${_externs}
const rc_module *const dl_recomp_modules[] = {
${_entries}    0,
};
" @ONLY)
  target_sources(${target} PRIVATE "${DL_ROOT}/core/src/cpu_recomp.c" "${_registry}")
  target_link_libraries(${target} PRIVATE ${_libs})

  if (NOT IOS)
    # Only the C, not compiled: what the iOS builds ask this build for.
    add_custom_target(${target}-recomp-gen DEPENDS ${_c_all})
    add_custom_target(${target}-recomp-census ${_census} DEPENDS ${_tool} ${_lib_files})
  else()
    # Always runs; the Mac build regenerates only when the tool, the patch
    # table or the APK changed, and ninja recompiles only the C that changed.
    add_custom_target(${target}-recomp-gen-host
      COMMAND "${CMAKE_COMMAND}" --build "${DL_RECOMP_HOST_BUILD}" --target ${target}-recomp-gen
      BYPRODUCTS ${_c_all}
      COMMENT "Generating the recompiled C in the Mac build (${DL_RECOMP_HOST_BUILD})"
      USES_TERMINAL)
    foreach(t IN LISTS _libs)
      add_dependencies(${t} ${target}-recomp-gen-host)
    endforeach()
  endif()
endmacro()

# The iOS app: the game's target itself, built as <APP_NAME>.app.
macro(_dl_add_ios_app target)
  if (G_APP_NAME)
    set(DL_APP_NAME "${G_APP_NAME}")
  else()
    set(DL_APP_NAME "${target}")
  endif()
  if (G_DISPLAY_NAME)
    set(DL_APP_DISPLAY_NAME "${G_DISPLAY_NAME}")
  else()
    set(DL_APP_DISPLAY_NAME "${DL_APP_NAME}")
  endif()
  if (G_VERSION)
    set(DL_APP_VERSION "${G_VERSION}")
  else()
    set(DL_APP_VERSION "1.0")
  endif()
  if (NOT G_BUNDLE_ID)
    string(TOLOWER "local.droidlift.${DL_APP_NAME}" G_BUNDLE_ID)
  endif()
  # The device scripts pass the free team's bundle id here.
  set(DL_IOS_BUNDLE_ID "${G_BUNDLE_ID}" CACHE STRING "The iOS app's bundle identifier")
  set(DL_APP_BUNDLE_ID "${DL_IOS_BUNDLE_ID}")
  set(_land "\t\t<string>UIInterfaceOrientationLandscapeLeft</string>\n\t\t<string>UIInterfaceOrientationLandscapeRight</string>")
  set(_port "\t\t<string>UIInterfaceOrientationPortrait</string>")
  if (G_ORIENTATION STREQUAL "portrait")
    set(DL_APP_ORIENTATIONS "${_port}")
  elseif (G_ORIENTATION STREQUAL "all")
    set(DL_APP_ORIENTATIONS "${_port}\n${_land}")
  else()
    set(DL_APP_ORIENTATIONS "${_land}")
  endif()
  if (CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(DL_IOS_PLATFORM "iPhoneSimulator")
    set(_angle_out ios-sim-arm64)
  else()
    # The device: signed afterwards by ios/sign-device.sh with the free
    # team's profile; bundle-app.sh's ad hoc signature is a placeholder.
    set(DL_IOS_PLATFORM "iPhoneOS")
    set(_angle_out ios-arm64)
  endif()
  set(DL_ANGLE_IOS_DIR "${DL_THIRD_PARTY}/angle/out/${_angle_out}" CACHE PATH
      "ANGLE's iOS build (libEGL.framework, libGLESv2.framework), made by scripts/fetch-ios-deps.sh")
  set(_plist "${CMAKE_CURRENT_BINARY_DIR}/${target}-Info.plist")
  configure_file("${DL_ROOT}/ios/Info.plist.in" "${_plist}" @ONLY)
  set_target_properties(${target} PROPERTIES
    MACOSX_BUNDLE TRUE
    OUTPUT_NAME "${DL_APP_NAME}"
    MACOSX_BUNDLE_INFO_PLIST "${_plist}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  # ANGLE's frameworks are opened with dlopen from the bundle (gfx.c); libEGL
  # finds libGLESv2 through this rpath.
  target_link_options(${target} PRIVATE "LINKER:-rpath,@executable_path/Frameworks")
  # After each link: the game's files from the APK, ANGLE, the icon, and an
  # ad hoc signature.
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${DL_ROOT}/ios/bundle-app.sh" "$<TARGET_BUNDLE_DIR:${target}>"
            "${G_APK}" "${DL_ANGLE_IOS_DIR}" "${CMAKE_BINARY_DIR}/apk-staging" "${G_ICON}"
    VERBATIM)
endmacro()

# droidlift_add_recomp_sampletest(<game> <test-name> [SIZE n] [SEED s] [CLOSURE k]
#                                 [ALSO <lib> <hex addr>...]...)
# A differential test of the recompiler on a game: a random sample of
# functions (with their callees) from each RECOMP library, recompiled under
# their own prefixes, run as C and under dynarmic from the same random
# states, compared register by register and in memory. ALSO adds regression
# addresses to one library's sample. Needs DL_WITH_DYNARMIC (Mac only).
function(droidlift_add_recomp_sampletest game name)
  cmake_parse_arguments(S "" "SIZE;SEED;CLOSURE;ITERS" "ALSO" ${ARGN})
  if (NOT S_SIZE)
    set(S_SIZE 60)
  endif()
  if (NOT S_SEED)
    set(S_SEED 11)
  endif()
  if (NOT S_CLOSURE)
    set(S_CLOSURE 24)
  endif()
  if (NOT S_ITERS)
    set(S_ITERS 30)
  endif()
  get_property(_recomp TARGET ${game} PROPERTY DL_RECOMP_LIBS)
  set(_tool ${DL_RECOMP_TOOL_${game}})
  set(_lib_dir ${DL_RECOMP_LIB_DIR_${game}})
  set(_out_dir "${DL_RECOMP_OUT_${game}}/sample")
  set(_srcs)
  set(_list "")
  foreach(l IN LISTS _recomp)
    string(REPLACE ":" ";" _parts "${l}")
    list(GET _parts 0 _lib)
    list(GET _parts 1 _pfx)
    set(_spfx "${_pfx}s")
    # ALSO entries for this library: pairs of <lib> <address>.
    set(_also)
    set(_i 0)
    list(LENGTH S_ALSO _na)
    while (_i LESS _na)
      list(GET S_ALSO ${_i} _al)
      math(EXPR _j "${_i} + 1")
      list(GET S_ALSO ${_j} _aa)
      if (_al STREQUAL _lib)
        list(APPEND _also --also ${_aa})
      endif()
      math(EXPR _i "${_i} + 2")
    endwhile()
    set(_out "${_out_dir}/${_spfx}_funcs.c" "${_out_dir}/${_spfx}_table.c" "${_out_dir}/${_spfx}_sample.c")
    add_custom_command(OUTPUT ${_out}
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_out_dir}"
      COMMAND ${_tool} --elf "${_lib_dir}/${_lib}.so" --out "${_out_dir}" --prefix ${_spfx}
              --soname ${_lib}.so --sample ${S_SIZE} --seed ${S_SEED} --closure ${S_CLOSURE} ${_also}
      DEPENDS ${_tool} "${_lib_dir}/${_lib}.so"
      COMMENT "Recompiling a random sample of ${_lib}.so to C")
    list(APPEND _srcs ${_out})
    string(APPEND _list "  X(${_spfx}, \"${_lib}.so\") \\\n")
  endforeach()
  set(_hdr "${CMAKE_CURRENT_BINARY_DIR}/${name}_libs.h")
  file(CONFIGURE OUTPUT "${_hdr}" CONTENT
"/* Generated by droidlift_add_recomp_sampletest(${game}): X(prefix, soname). */
#define DL_SAMPLE_LIBS(X) \\
${_list}
" @ONLY)
  set_source_files_properties(${_srcs} PROPERTIES COMPILE_OPTIONS "${DL_RECOMP_CFLAGS};-DRC_TEST_BUDGET")
  add_executable(${name} "${DL_ROOT}/recomp/tests/sampletest.c" ${_srcs}
    "${DL_ROOT}/core/src/cpu.c" "${DL_ROOT}/core/src/cpu_dynarmic.cpp" "${DL_ROOT}/core/src/guest_mem.c")
  target_include_directories(${name} PRIVATE "${DL_ROOT}/core/src" "${CMAKE_CURRENT_BINARY_DIR}")
  target_compile_definitions(${name} PRIVATE DL_SAMPLE_HEADER="${name}_libs.h" DL_DEFAULT_CPU="dynarmic")
  target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(${name} PRIVATE rc_runtime dynarmic dl_interp)
  add_test(NAME ${name} COMMAND ${name} --dir "${_lib_dir}" --iters ${S_ITERS})
  set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "SAMPLETEST OK"
    ENVIRONMENT "DL_AUDIO=off" TIMEOUT 600)
endfunction()

# droidlift_add_recomp_difftest(<game> <test-name> LIB <lib> PREFIX <p> FUNCS <name>...
#                               [ITERS n])
# A differential test of hand-picked functions (by symbol) from one RECOMP
# library: each recompiled function against dynarmic on random inputs.
function(droidlift_add_recomp_difftest game name)
  cmake_parse_arguments(D "" "LIB;PREFIX;ITERS" "FUNCS" ${ARGN})
  if (NOT D_ITERS)
    set(D_ITERS 500)
  endif()
  set(_tool ${DL_RECOMP_TOOL_${game}})
  set(_lib_dir ${DL_RECOMP_LIB_DIR_${game}})
  set(_out_dir "${DL_RECOMP_OUT_${game}}")
  set(_args)
  foreach(f IN LISTS D_FUNCS)
    list(APPEND _args --func ${f})
  endforeach()
  set(_gen "${_out_dir}/${D_PREFIX}_funcs.c" "${_out_dir}/${D_PREFIX}_table.c")
  add_custom_command(OUTPUT ${_gen}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_out_dir}"
    COMMAND ${_tool} --elf "${_lib_dir}/${D_LIB}.so" --out "${_out_dir}" --prefix ${D_PREFIX}
            --soname ${D_LIB}.so ${_args}
    DEPENDS ${_tool} "${_lib_dir}/${D_LIB}.so"
    COMMENT "Recompiling the test functions from ${D_LIB}.so to C")
  set_source_files_properties(${_gen} PROPERTIES COMPILE_OPTIONS "${DL_RECOMP_CFLAGS}")
  add_executable(${name} "${DL_ROOT}/recomp/tests/difftest.c" ${_gen}
    "${DL_ROOT}/core/src/cpu.c" "${DL_ROOT}/core/src/cpu_dynarmic.cpp" "${DL_ROOT}/core/src/guest_mem.c")
  target_include_directories(${name} PRIVATE "${DL_ROOT}/core/src")
  target_compile_definitions(${name} PRIVATE DL_DIFFTEST_PREFIX=${D_PREFIX} DL_DEFAULT_CPU="dynarmic")
  target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(${name} PRIVATE rc_runtime dynarmic dl_interp)
  add_test(NAME ${name} COMMAND ${name} --elf "${_lib_dir}/${D_LIB}.so" --iters ${D_ITERS})
  set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "DIFFTEST OK"
    ENVIRONMENT "DL_AUDIO=off" TIMEOUT 300)
endfunction()
