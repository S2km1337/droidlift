#!/bin/zsh
# Fetch and build the iOS (arm64) dependencies into the git-ignored
# third_party/ directory, pinned (see THIRD_PARTY.md). Tickets 18 (Simulator)
# and 15 (device). Idempotent; each platform has its own outputs, and the Mac
# outputs (Homebrew SDL3, third_party/angle/out/mac-metal) are never touched.
#   - SDL3: the official release tarball, sha256-checked, built static, into
#     third_party/sdl3-ios-sim-arm64 or third_party/sdl3-ios-arm64.
#   - ANGLE: the checkout made by fetch-angle.sh, built again as iOS
#     frameworks (libEGL.framework, libGLESv2.framework) in
#     out/ios-sim-arm64 or out/ios-arm64.
# usage: scripts/fetch-ios-deps.sh [sim|device]   (default: sim)
set -euo pipefail
DL_ROOT=${0:A:h:h}
source "$DL_ROOT/scripts/lib/project.sh"

PLATFORM=${1:-sim}
case $PLATFORM in
  sim) SYSROOT=iphonesimulator ANGLE_ENV=simulator SUFFIX=ios-sim-arm64 SDL_BUILD=build-ios-sim ;;
  device) SYSROOT=iphoneos ANGLE_ENV=device SUFFIX=ios-arm64 SDL_BUILD=build-ios-device ;;
  *) echo "usage: $0 [sim|device]" >&2; exit 2 ;;
esac

# Every iOS piece targets the same minimum: ANGLE's own default (18.0).
IOS_MIN=18.0
TP=$PWD/third_party

SDL_VER=3.4.18
SDL_SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
SDL_URL=https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL3-$SDL_VER.tar.gz
SDL_SRC=$TP/SDL3-$SDL_VER
SDL_PREFIX=$TP/sdl3-$SUFFIX

if [[ ! -f $SDL_PREFIX/lib/libSDL3.a ]]; then
  if [[ ! -f $SDL_SRC/CMakeLists.txt ]]; then
    tmp=$(mktemp -d)
    curl -sSLf -o "$tmp/sdl.tar.gz" "$SDL_URL"
    if [[ "$(shasum -a 256 "$tmp/sdl.tar.gz" | cut -d' ' -f1)" != "$SDL_SHA256" ]]; then
      echo "fetch-ios-deps: SDL3 tarball checksum mismatch" >&2
      exit 1
    fi
    tar xzf "$tmp/sdl.tar.gz" -C "$TP"
    rm -rf "$tmp"
  fi
  cmake -S "$SDL_SRC" -B "$SDL_SRC/$SDL_BUILD" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$SYSROOT \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$IOS_MIN \
    -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF \
    -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX"
  cmake --build "$SDL_SRC/$SDL_BUILD"
  cmake --install "$SDL_SRC/$SDL_BUILD"
fi

ANGLE=$TP/angle
ANGLE_OUT=$ANGLE/out/$SUFFIX
if [[ ! -f $ANGLE_OUT/libGLESv2.framework/libGLESv2 || ! -f $ANGLE_OUT/libEGL.framework/libEGL ]]; then
  [[ -f $ANGLE/out/mac-metal/libGLESv2.dylib ]] || "$DL_ROOT"/scripts/fetch-angle.sh
  # The host python3 runs ANGLE's build scripts, not its bundled cpython3:
  # parts of that copy's standard library were evicted by iCloud before
  # third_party became a .nosync folder, and reading them hangs.
  # ANGLE doesn't sign: bundle-app.sh signs the frameworks with the app.
  "$ANGLE/buildtools/mac/gn" gen --root="$ANGLE" --script-executable="$(command -v python3)" \
    "$ANGLE_OUT" --args="target_os=\"ios\" target_environment=\"$ANGLE_ENV\"
    target_cpu=\"arm64\" is_debug=false ios_enable_code_signing=false
    angle_enable_metal=true angle_enable_vulkan=false angle_enable_gl=false
    angle_enable_null=false angle_enable_swiftshader=false angle_build_tests=false
    angle_enable_wgpu=false"
  ninja -C "$ANGLE_OUT" libEGL_framework libGLESv2_framework
fi
