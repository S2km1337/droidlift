#!/bin/zsh
# Fills a freshly linked <app>.app, run by CMake after each link:
#   apk/lib/armeabi-v7a/*.so and apk/assets/  from the user's APK (unpacked
#                                             into STAGING first; game
#                                             data only in ignored folders)
#   Frameworks/libEGL.framework, libGLESv2.framework  ANGLE for iOS
#   AppIcon60x60@2x.png, @3x.png              the APK's launcher icon (ICON, a
#                                             path inside the APK; none if empty)
# then tries to sign it ad hoc in place (ios/sign-adhoc.sh; see the end).
# usage: ios/bundle-app.sh APP APK ANGLE_OUT STAGING [ICON]
set -euo pipefail
APP=$1 APK=$2 ANGLE_OUT=$3 STAGING=$4 ICON=${5:-}

if [[ ! -f $APK ]]; then
  echo "bundle-app: no APK at $APK (droidlift_add_game APK)" >&2
  exit 1
fi
# Unpack again only when the APK changed.
if [[ ! -f $STAGING/.stamp || $APK -nt $STAGING/.stamp ]]; then
  rm -rf "$STAGING"
  mkdir -p "$STAGING"
  unzip -q "$APK" 'lib/armeabi-v7a/*.so' 'assets/*' ${ICON:+"$ICON"} -d "$STAGING"
  touch "$STAGING/.stamp"
fi
mkdir -p "$APP/apk" "$APP/Frameworks"
rsync -a --delete "$STAGING/lib" "$STAGING/assets" "$APP/apk/"
rsync -a --delete "$ANGLE_OUT/libEGL.framework" "$ANGLE_OUT/libGLESv2.framework" "$APP/Frameworks/"
if [[ -n $ICON ]]; then
  sips -z 120 120 "$STAGING/$ICON" --out "$APP/AppIcon60x60@2x.png" >/dev/null
  sips -z 180 180 "$STAGING/$ICON" --out "$APP/AppIcon60x60@3x.png" >/dev/null
fi

# Signed ad hoc in place, as far as iCloud lets it. In the repository's
# Documents folder codesign may refuse the bundle for good (iCloud's file
# attributes); that doesn't fail the build, because the install
# scripts sign a clean copy outside iCloud anyway: scripts/run-ios-sim.sh ad
# hoc, scripts/build-ios-device.sh with the free team's identity.
if ! "${0:A:h}/sign-adhoc.sh" "$APP" 2>/dev/null; then
  echo "bundle-app: warning: couldn't sign $APP in place (iCloud's file attributes);" >&2
  echo "  scripts/run-ios-sim.sh and "$DL_ROOT"/scripts/build-ios-device.sh sign a copy outside iCloud" >&2
fi
