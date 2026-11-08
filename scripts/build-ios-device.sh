#!/bin/zsh
# Builds the iPhone app: build/ios-device/<app>.app for iphoneos
# arm64, running the game on its statically recompiled code (the iPhone
# allows no JIT), with our interpreter as the fallback
# and still selectable with --cpu interp. The recompiled C comes from the
# Mac build in build/ (configured on first use). Then signs a copy outside
# iCloud ($DL_STAGING_DIR/ios-device; ios/stage-app.sh) with the
# free team's profile and, with --install, installs it over the app on the
# iPhone (its saves stay) and deletes the copy. The iPhone is the one
# physical iOS device connected (Simulators don't count), or
# DL_IOS_DEVICE. See docs/ios.md.
#
# usage: scripts/build-ios-device.sh [--apk PATH] [--cpu NAME] [--no-sign] [--renew] [--install]
#   --apk      the game's APK (default: droidlift.env's DL_APK, or $DL_APK)
#   --cpu      the default CPU backend built in: recomp (default) or interp;
#              --cpu at launch still overrides it (scripts/run-ios-device.sh
#              -- --cpu interp). DL_RECOMP=OFF leaves recomp out (then
#              --cpu interp is needed)
#   --no-sign  stop after the build (ad hoc signed: won't install)
#   --renew    ask Xcode for a fresh profile first (scripts/ios-profile.sh);
#              done anyway when there's no valid one
#   --install  install on the connected iPhone (xcrun devicectl; never
#              uninstalls, so the game's data is kept); without it the
#              signed copy stays in $DL_STAGING_DIR/ios-device
#   DL_STAGING_DIR  where the signed copy goes instead of
#              $DL_STAGING_DIR (not in iCloud)
# Team and bundle id: ios/device-config.sh (found automatically, kept in
# device.local.env). The weekly refresh is scripts/resign-ios.sh.
set -euo pipefail
DL_ROOT=${0:A:h:h}
source "$DL_ROOT/scripts/lib/project.sh"

APK=$DL_APK
CPU=recomp SIGN=1 RENEW=0 INSTALL=0
while (( $# )); do
  case $1 in
    --apk) APK=${2:A}; shift 2 ;;
    --cpu) CPU=$2; shift 2 ;;
    --no-sign) SIGN=0; shift ;;
    --renew) RENEW=1; shift ;;
    --install) INSTALL=1; shift ;;
    *) sed -n '/^# usage:/,/^# Team/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2 ;;
  esac
done
IOS_MIN=18.0
B=$PWD/build/ios-device

source "$DL_ROOT/ios/device-config.sh"
if (( SIGN )); then
  dl_device_config
else
  # Unsigned builds need no team; the bundle id from the environment or
  # device.local.env, else a placeholder. Nothing is written down.
  DL_IOS_BUNDLE_ID=${DL_IOS_BUNDLE_ID:-$DL_BUNDLE_PREFIX.unsigned}
fi

"$DL_ROOT"/scripts/fetch-ios-deps.sh device

# The recompiled backend: the recompiler runs on the Mac, so a Mac
# build (build/) generates the C that this build compiles for iOS; it's
# configured here on first use. DL_RECOMP=OFF leaves the backend out.
RECOMP=${DL_RECOMP:-ON}
if [[ $RECOMP == ON && ! -f build/CMakeCache.txt ]]; then
  [[ -f third_party/dynarmic/CMakeLists.txt ]] || "$DL_ROOT"/scripts/fetch-deps.sh
  cmake -S . -B build -G Ninja -DDL_APK="$APK" >/dev/null
fi

# No dynarmic (DL_WITH_DYNARMIC=OFF by default for iphoneos): JIT can't run
# on the iPhone.
cmake -S . -B "$B" -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$IOS_MIN \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/third_party/sdl3-ios-arm64" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
  -DDL_DEFAULT_CPU="$CPU" \
  -DDL_RECOMP="$RECOMP" \
  -DDL_IOS_BUNDLE_ID="$DL_IOS_BUNDLE_ID" \
  -DDL_APK="$APK"
cmake --build "$B"
APP=$B/$DL_APP_NAME.app
echo "built $APP (bundle id $DL_IOS_BUNDLE_ID, default cpu $CPU)"
(( SIGN )) || exit 0

UDID=
if (( INSTALL || RENEW )); then UDID=$(dl_find_device); fi
if (( RENEW )); then "$DL_ROOT"/scripts/ios-profile.sh --device "$UDID"; fi

# Sign (and install) a copy outside iCloud: in the repository's Documents
# folder codesign refuses the bundle (ios/stage-app.sh). With --install the
# copy is deleted afterwards; without, it stays as the signed app.
SIGNED=$("$DL_ROOT"/ios/stage-app.sh "$APP" ios-device)
if (( INSTALL )); then trap 'rm -rf "${SIGNED:h}"' EXIT; fi
rc=0
"$DL_ROOT"/ios/sign-device.sh "$SIGNED" "$DL_IOS_BUNDLE_ID" "$DL_TEAM_ID" "$UDID" || rc=$?
if (( rc == 3 && ! RENEW )); then
  # No profile yet, or it expired: let Xcode make one, then sign again.
  [[ -n $UDID ]] || UDID=$(dl_find_device)
  "$DL_ROOT"/scripts/ios-profile.sh --device "$UDID"
  "$DL_ROOT"/ios/sign-device.sh "$SIGNED" "$DL_IOS_BUNDLE_ID" "$DL_TEAM_ID" "$UDID"
elif (( rc != 0 )); then
  exit $rc
fi

if (( INSTALL )); then
  # Over the installed app: iOS keeps its data container. Never uninstall.
  xcrun devicectl device install app --device "$UDID" "$SIGNED"
  echo "installed $DL_IOS_BUNDLE_ID on $UDID"
  echo "first time only: on the iPhone, Settings > General > VPN & Device Management >"
  echo "  (your Apple ID) > Trust, then open $DL_APP_NAME from the home screen."
else
  echo "the signed app is $SIGNED (outside iCloud; --install installs it)"
fi
