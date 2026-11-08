#!/bin/zsh
# Gets the free team's provisioning profile for the iPhone app. Apple issues
# free-team profiles only through Xcode, and they last 7 days. This writes the stub project DroidliftSigning.xcodeproj
# (ios/signing-project.sh: same bundle id and team) and builds it with
# `xcodebuild -allowProvisioningUpdates`, which signs in with the Apple ID in
# Xcode's accounts, registers the iPhone with the team and makes or renews
# the profile. ios/sign-device.sh then uses it.
# The stub lives outside iCloud ($DL_STAGING_DIR/signing, or
# $DL_STAGING_DIR/signing): in the repository's Documents folder the
# stub's own CodeSign fails on iCloud's file attributes.
# Success means a valid profile for TEAM.BUNDLE_ID exists afterwards, even
# if xcodebuild's build of the stub failed after making it. It says whether
# the profile is new or the one that was there.
# If there is none, it says how to do the same once in Xcode itself.
# usage: scripts/ios-profile.sh [--device UDID] [--open]
#   --device  the iPhone to register (default: the one connected; see
#             ios/device-config.sh)
#   --open    just open the stub project in Xcode
set -euo pipefail
DL_ROOT=${0:A:h:h}
source "$DL_ROOT/scripts/lib/project.sh"

UDID= OPEN=0
while (( $# )); do
  case $1 in
    --device) UDID=$2; shift 2 ;;
    --open) OPEN=1; shift ;;
    *) echo "usage: $0 [--device UDID] [--open]" >&2; exit 2 ;;
  esac
done
source "$DL_ROOT/ios/device-config.sh"
dl_device_config

S=${DL_STAGING_DIR:-$HOME/Library/Caches/droidlift}/signing
case ${S:A}/ in
  "$HOME"/Documents/*|"$HOME"/Desktop/*|"$HOME"/Library/Mobile\ Documents/*)
    echo "ios-profile: $S is synced by iCloud; set DL_STAGING_DIR to a folder outside it" >&2
    exit 1 ;;
esac
# Its build products go stale with every new profile: start clean.
rm -rf "$S"
PROJ=$("$DL_ROOT"/ios/signing-project.sh "$S" "$DL_IOS_BUNDLE_ID" "$DL_TEAM_ID" 18.0)

gui_steps() {
  cat >&2 <<EOF

Xcode couldn't make the profile from the command line. Do it once in Xcode:
  1. The iPhone is connected by cable, unlocked, and trusts this Mac.
  2. open "$PROJ"
  3. Click DroidliftSigning (the blue project icon) > target DroidliftSigning >
     Signing & Capabilities. Leave "Automatically manage signing" on; Team
     should already be your "(Personal Team)"; pick it if not.
  4. In the toolbar, choose your iPhone as the run destination.
  5. Product > Build (Cmd-B). Don't press Run: it would install this empty
     stub over the game (harmless, but the game is gone until the next
     install).
  6. Once "Signing & Capabilities" shows no errors, run your command again.
If the error says the bundle id isn't available, set another one:
  DL_IOS_BUNDLE_ID=$DL_BUNDLE_PREFIX.<something> in device.local.env
  (only before the first install; it names the saves' container).
EOF
}

if (( OPEN )); then
  open "$PROJ"
  gui_steps 2>&1 | sed -n '/^  3\./,$p'
  exit 0
fi

[[ -n $UDID ]] || UDID=$(dl_find_device)
# The profile ios/sign-device.sh would use, or nothing.
profile() { "$DL_ROOT"/ios/sign-device.sh --check "$DL_IOS_BUNDLE_ID" "$DL_TEAM_ID" "$UDID" 2>/dev/null || true; }
BEFORE=$(profile)
mkdir -p build/ios-device/logs
LOG=$PWD/build/ios-device/logs/profile-$(date +%Y%m%d-%H%M%S).log
echo "asking Xcode for the profile of $DL_TEAM_ID.$DL_IOS_BUNDLE_ID (device $UDID); log $LOG"
built=1
xcodebuild -project "$PROJ" -scheme DroidliftSigning -configuration Debug \
  -destination "id=$UDID" \
  -allowProvisioningUpdates -allowProvisioningDeviceRegistration \
  SYMROOT="$S/out" OBJROOT="$S/obj" build > "$LOG" 2>&1 || built=0
AFTER=$(profile)

if [[ -z $AFTER ]]; then
  grep -E "error:|warning: .*(profile|team|certificate)" "$LOG" | head -8 >&2 || true
  gui_steps
  exit 1
fi
if (( ! built )); then
  echo "xcodebuild's build of the stub failed (see the log), but the profile is there:"
  grep -E "error:" "$LOG" | head -3 | sed 's/^/  /' || true
fi
if [[ $AFTER == "$BEFORE" ]]; then
  echo "profile ok, unchanged (Xcode kept the one still valid): ${AFTER#profile }"
else
  echo "profile ok, new: ${AFTER#profile }"
fi
