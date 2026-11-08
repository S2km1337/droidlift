#!/bin/zsh
# Runs the iOS Simulator app built by scripts/build-ios-sim.sh:
# boots a Simulator ("<app> <type>": an iPhone 14 if a runtime runs one, made
# on first use; see --type), installs build/ios-sim/<app>.app over any
# earlier copy (its saves stay; from a copy signed outside iCloud,
# ios/stage-app.sh), launches it, and after --wait seconds saves
# a screenshot with `xcrun simctl io` to build/ios-sim/shots/. The app's log
# goes to build/ios-sim/logs/. Muted unless --sound (or DL_AUDIO=on).
# usage: scripts/run-ios-sim.sh [--wait SECONDS] [--quit] [--show] [--type TYPE]
#          [--cpu NAME] [--sound] [--fresh] [-- APP ARGS...]
#   --wait  seconds from launch to the screenshot (default 25)
#   --quit  terminate the app after the screenshot (default: leave it running)
#   --show  also open the Simulator window
#   --type  Simulator device type, by identifier or name: iPhone-14, "iPhone 14"
#           (default: see above; `xcrun simctl list devicetypes`)
#   --cpu   CPU backend for this launch: dynarmic (the Simulator build's
#           default), interp or recomp; passed to the app as --cpu NAME
#   --sound launch with sound on (DL_AUDIO=on)
#   --fresh uninstall first, so the saves go and the Terms show again
#   APP ARGS go to the loader after the app's own (ios/ios_shell.h), e.g.
#   -- --touch '700:tap:1266,1000' to script a tap in surface pixels.
set -euo pipefail
DL_ROOT=${0:A:h:h}
source "$DL_ROOT/scripts/lib/project.sh"

WAIT=25 QUIT=0 SHOW=0 TYPE= CPU= FRESH=0 AUDIO=${DL_AUDIO:-off}
while (( $# )); do
  case $1 in
    --wait) WAIT=$2; shift 2 ;;
    --quit) QUIT=1; shift ;;
    --show) SHOW=1; shift ;;
    --type) TYPE=$2; shift 2 ;;
    --cpu) CPU=$2; shift 2 ;;
    --sound) AUDIO=on; shift ;;
    --fresh) FRESH=1; shift ;;
    --) shift; break ;;
    *) echo "usage: $0 [--wait SECONDS] [--quit] [--show] [--type TYPE] [--cpu NAME] [--sound] [--fresh] [-- APP ARGS...]" >&2; exit 2 ;;
  esac
done
[[ -n $CPU ]] && set -- --cpu "$CPU" "$@"

APP=$PWD/build/ios-sim/$DL_APP_NAME.app
[[ -d $APP ]] || { echo "no $APP: run scripts/build-ios-sim.sh" >&2; exit 1; }
BUNDLE_ID=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP/Info.plist")
# No --type: an iPhone 14 if an installed runtime runs one (iOS 26.5, 23F73,
# ), else the first iPhone the newest runtime runs (some
# runtimes run one device only: iOS 27.1, 24A94425, ran only "iPhone Duo").
# Never downloads a runtime.
if [[ -n $TYPE ]]; then
  # A name ("iPhone 14", any case) or an identifier's last part ("iPhone-14").
  TYPE=$(xcrun simctl list devicetypes -j | python3 -c '
import json, sys
want = sys.argv[1].strip().lower()
for d in json.load(sys.stdin)["devicetypes"]:
    short = d["identifier"].rsplit(".", 1)[1]
    if want in (d["name"].lower(), short.lower(), short.lower().replace("-", " ")):
        print(short); break' "$TYPE")
  [[ -n $TYPE ]] || { echo "unknown device type; see xcrun simctl list devicetypes" >&2; exit 1; }
else
  TYPE=$(xcrun simctl list runtimes -j | python3 -c '
import json, sys
rts = sorted((r for r in json.load(sys.stdin)["runtimes"] if r["platform"] == "iOS" and r["isAvailable"]),
             key=lambda r: [int(x) for x in r["version"].split(".")], reverse=True)
types = [d["identifier"].rsplit(".", 1)[1] for r in rts for d in r["supportedDeviceTypes"]
         if d["productFamily"] == "iPhone"]
print("iPhone-14" if "iPhone-14" in types else (types[0] if types else ""))')
  [[ -n $TYPE ]] || { echo "no iPhone Simulator runtime installed" >&2; exit 1; }
fi
NAME="$DL_APP_NAME ${TYPE//-/ }"

UDID=$(xcrun simctl list devices -j | python3 -c '
import json, sys
for rt, devs in json.load(sys.stdin)["devices"].items():
    for d in devs:
        if d["name"] == sys.argv[1] and d["isAvailable"]:
            print(d["udid"]); sys.exit()' "$NAME")
if [[ -z $UDID ]]; then
  # The newest iOS runtime that runs this device type (some runtimes are
  # made for one new device only).
  RUNTIME=$(xcrun simctl list runtimes -j | python3 -c '
import json, sys
want = "com.apple.CoreSimulator.SimDeviceType." + sys.argv[1]
rts = [r for r in json.load(sys.stdin)["runtimes"] if r["platform"] == "iOS" and r["isAvailable"]
       and any(d["identifier"] == want for d in r["supportedDeviceTypes"])]
print(max(rts, key=lambda r: [int(x) for x in r["version"].split(".")])["identifier"] if rts else "")' "$TYPE")
  [[ -n $RUNTIME ]] || { echo "no iOS Simulator runtime for $TYPE" >&2; exit 1; }
  UDID=$(xcrun simctl create "$NAME" com.apple.CoreSimulator.SimDeviceType.$TYPE "$RUNTIME")
  echo "created $NAME ($RUNTIME): $UDID"
fi
xcrun simctl boot "$UDID" 2>/dev/null || true
xcrun simctl bootstatus "$UDID" -b >/dev/null
# The window: Simulator.app up to Xcode 26; Xcode 27 has none and shows
# Simulators in DeviceHub (Xcode.app/Contents/Applications) instead.
if (( SHOW )); then
  DEV=$(xcode-select -p)
  if [[ -d $DEV/Applications/Simulator.app ]]; then
    open "$DEV/Applications/Simulator.app" --args -CurrentDeviceUDID "$UDID"
  else
    open "${DEV:h}/Applications/DeviceHub.app"
  fi
fi

xcrun simctl terminate "$UDID" "$BUNDLE_ID" 2>/dev/null || true
(( FRESH )) && { xcrun simctl uninstall "$UDID" "$BUNDLE_ID" 2>/dev/null || true; }
# Install a copy outside iCloud, signed there: in the repository's Documents
# folder codesign may refuse the bundle (ios/stage-app.sh).
STAGED=$("$DL_ROOT"/ios/stage-app.sh "$APP" ios-sim)
"$DL_ROOT"/ios/sign-adhoc.sh "$STAGED"
xcrun simctl install "$UDID" "$STAGED"
rm -rf "${STAGED:h}"
mkdir -p build/ios-sim/logs build/ios-sim/shots
STAMP=$(date +%Y%m%d-%H%M%S)
LOG=build/ios-sim/logs/run-$STAMP.log
SHOT=build/ios-sim/shots/shot-$STAMP.png
# Environment variables reach the app with the SIMCTL_CHILD_ prefix.
SIMCTL_CHILD_DL_AUDIO=$AUDIO \
  xcrun simctl launch --console-pty --terminate-running-process "$UDID" "$BUNDLE_ID" "$@" \
  > "$LOG" 2>&1 &
LAUNCH_PID=$!
echo "launched $BUNDLE_ID on $NAME ($UDID), audio $AUDIO${CPU:+, cpu $CPU}; log $LOG"
sleep "$WAIT"
xcrun simctl io "$UDID" screenshot "$SHOT" >/dev/null 2>&1
echo "screenshot $SHOT"
if (( QUIT )); then
  xcrun simctl terminate "$UDID" "$BUNDLE_ID" 2>/dev/null || true
  wait $LAUNCH_PID 2>/dev/null || true
fi
