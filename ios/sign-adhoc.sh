#!/bin/zsh
# Signs <app>.app ad hoc ("-"): its ANGLE frameworks, then the app. That is
# all the Simulator needs (no Apple ID). Used by ios/bundle-app.sh after each
# link and by scripts/run-ios-sim.sh on the copy it installs.
# codesign refuses Finder and file-provider attributes anywhere in the
# bundle, and in iCloud's Documents folder the file provider tags new
# .framework folders a moment after they appear, so this clears and retries;
# outside iCloud (ios/stage-app.sh) the first try works.
# usage: ios/sign-adhoc.sh APP
set -euo pipefail
APP=$1
sign() {
  local i
  for i in 1 2 3 4 5; do
    xattr -cr "$APP"
    codesign --force --sign - --timestamp=none "$@" 2>/dev/null && return 0
    sleep 1
  done
  codesign --force --sign - --timestamp=none "$@"
}
for fw in "$APP"/Frameworks/*.framework(N); do sign "$fw"; done
sign "$APP"
