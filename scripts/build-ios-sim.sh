#!/bin/zsh
# Builds the iOS Simulator app: build/ios-sim/<app>.app, for Apple
# Silicon (arm64 Simulator), running the game on recomp by default (dynarmic
# and interp stay selectable with --cpu). One command:
# fetches and builds the iOS dependencies on first use (fetch-ios-deps.sh),
# then configures CMake for iOS (Ninja) and builds, with the recompiled
# backend (--cpu recomp; its C comes from the Mac build in build/); the app is filled from
# the APK and signed ad hoc after the link (ios/bundle-app.sh). No Apple ID.
# See docs/ios.md.
# usage: scripts/build-ios-sim.sh [--apk PATH]   (default: droidlift.env's DL_APK, or $DL_APK)
#   then scripts/run-ios-sim.sh to boot a Simulator, install and launch it.
set -euo pipefail
DL_ROOT=${0:A:h:h}
source "$DL_ROOT/scripts/lib/project.sh"

APK=$DL_APK
if [[ ${1:-} == --apk && -n ${2:-} ]]; then APK=${2:A}; fi
IOS_MIN=18.0
B=$PWD/build/ios-sim

[[ -f third_party/dynarmic/CMakeLists.txt ]] || "$DL_ROOT"/scripts/fetch-deps.sh
"$DL_ROOT"/scripts/fetch-ios-deps.sh

# The recompiled backend: the recompiler runs on the Mac, so a Mac
# build (build/) generates the C that this build compiles for iOS; it's
# configured here on first use. DL_RECOMP=OFF leaves the backend out.
RECOMP=${DL_RECOMP:-ON}
# Default backend: recomp, like the iPhone (no JIT; 3x faster start-up than
# dynarmic in the Simulator). DL_SIM_CPU=dynarmic|interp overrides.
if [[ $RECOMP == ON ]]; then CPU=${DL_SIM_CPU:-recomp}; else CPU=${DL_SIM_CPU:-dynarmic}; fi
if [[ $RECOMP == ON && ! -f build/CMakeCache.txt ]]; then
  [[ -f third_party/dynarmic/CMakeLists.txt ]] || "$DL_ROOT"/scripts/fetch-deps.sh
  cmake -S . -B build -G Ninja -DDL_APK="$APK" >/dev/null
fi

# dynarmic's CMake writes its generated Mach exception code (mig) into its
# source tree, which is shared with the Mac builds: build from a copy.
mkdir -p "$B"
rsync -a --delete --exclude .git --exclude 'src/dynarmic/backend/*/mig/' \
  third_party/dynarmic/ "$B/dynarmic-src/"

# The iOS SDK lacks mach_exc.defs (dynarmic's fastmem fault handler needs it,
# else every guest memory access takes the slow path); the macOS SDK's is the
# same interface.
MACH_DEFS=$(xcrun --sdk macosx --show-sdk-path)/usr/include

cmake -S . -B "$B" -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$IOS_MIN \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/third_party/sdl3-ios-sim-arm64" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
  -DDL_DYNARMIC_DIR="$B/dynarmic-src" \
  -DMACH_EXC_DEFS_DIR="$MACH_DEFS" \
  -DDL_RECOMP="$RECOMP" \
  -DDL_DEFAULT_CPU="$CPU" \
  -DDL_APK="$APK"
cmake --build "$B"
echo "built $B/$DL_APP_NAME.app"
