#!/bin/zsh
# Copies the iPhone app's logs (ios/ios_log.c keeps the last 5
# runs in <container>/Documents/logs) to build/ios-device/device-logs/ and
# prints the newest one's path. They are also in Finder (the iPhone >
# Files > <app>) and the Files app (On My iPhone > <app> > logs).
# usage: scripts/fetch-ios-log.sh
set -euo pipefail
DL_ROOT=${0:A:h:h}
source "$DL_ROOT/scripts/lib/project.sh"
source "$DL_ROOT/ios/device-config.sh"
dl_device_config
UDID=$(dl_find_device)
DEST=$PWD/build/ios-device/device-logs
mkdir -p "$DEST"
xcrun devicectl device copy from --device "$UDID" --domain-type appDataContainer \
  --domain-identifier "$DL_IOS_BUNDLE_ID" --source Documents/logs --destination "$DEST" >/dev/null
NEWEST=("$DEST"/**/run-*.log(N.om[1]))
if (( ${#NEWEST} )); then echo "${NEWEST[1]}"; else echo "no logs copied into $DEST" >&2; exit 1; fi
