#!/bin/zsh
# The weekly refresh: a free team's app stops opening 7
# days after its profile was made. This asks Xcode for a fresh profile,
# rebuilds if anything changed, signs, and installs over the app on the
# iPhone, which keeps its saves: it never uninstalls.
# Connect the iPhone (cable, unlocked) and run:
#   scripts/resign-ios.sh
# Arguments go to scripts/build-ios-device.sh (e.g. --apk PATH).
# It prints the profile's expiry date and whether the profile is new. Xcode
# keeps a profile that is still valid (seen 6 days before expiry, ticket
# 16), so run it once the app stops opening or the expiry is near.
# The iPhone is the one physical device connected, or DL_IOS_DEVICE.
# Signing and installing use a copy outside iCloud (ios/stage-app.sh).
set -euo pipefail
DL_ROOT=${0:A:h:h}
source "$DL_ROOT/scripts/lib/project.sh"
exec "$DL_ROOT"/scripts/build-ios-device.sh --renew --install "$@"
