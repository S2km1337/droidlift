#!/bin/zsh
# Launches the installed iPhone app with its output on this
# terminal (devicectl --console) until it exits or Ctrl-C, saved to
# build/ios-device/logs/run-<time>.log as well. Muted unless DL_AUDIO=on.
# The app also logs to its own container whether launched this way or from
# the home screen: scripts/fetch-ios-log.sh copies those logs.
# usage: scripts/run-ios-device.sh [-- APP ARGS...]
#   APP ARGS go to the loader after the app's own (ios/ios_shell.h), e.g.
#   -- --cpu interp --frames 1800
set -euo pipefail
DL_ROOT=${0:A:h:h}
source "$DL_ROOT/scripts/lib/project.sh"
[[ ${1:-} == -- ]] && shift
source "$DL_ROOT/ios/device-config.sh"
dl_device_config
UDID=$(dl_find_device)
mkdir -p build/ios-device/logs
LOG=build/ios-device/logs/run-$(date +%Y%m%d-%H%M%S).log
echo "launching $DL_IOS_BUNDLE_ID on $UDID; log $LOG"
# Environment variables reach the app with the DEVICECTL_CHILD_ prefix.
DEVICECTL_CHILD_DL_AUDIO=${DL_AUDIO:-off} \
  xcrun devicectl device process launch --device "$UDID" --console --terminate-existing \
  "$DL_IOS_BUNDLE_ID" -- "$@" 2>&1 | tee "$LOG"
