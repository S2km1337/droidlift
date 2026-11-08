#!/bin/zsh
# Checks dl_find_device (ios/device-config.sh) on saved `devicectl list
# devices` JSON: Simulators never count, one physical device is
# picked on its own, several need exactly one connected, DL_IOS_DEVICE
# overrides. The JSON is made up, shaped like Xcode 27.1's (2026-11-03).
# usage: ios/tests/find-device-test.sh   (prints "FIND DEVICE OK")
set -euo pipefail
cd "${0:A:h}/../.."
source ios/device-config.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# dev UDID NAME REALITY STATE: one device entry, with both the 'properties'
# dictionary and the deprecated fields devicectl still writes.
dev() {
  local provider=com.apple.dt.CoreDeviceCore visibility=default
  [[ $3 == simulated ]] && provider=com.apple.CoreSimulator.SimulatorCoreDevicePlugin visibility=simulators
  print -r -- "{\"identifier\": \"core-$1\", \"visibilityClass\": \"$visibility\",
   \"hardwareProperties\": {\"cpuType\": {\"name\": \"arm64\"}, \"platform\": \"iOS\", \"reality\": \"$3\", \"udid\": \"$1\"},
   \"deviceProperties\": {\"name\": \"$2\", \"provider\": \"$provider\"},
   \"connectionProperties\": {\"tunnelState\": \"$4\"},
   \"properties\": {\"connection\": {\"state\": \"$4\"},
     \"hardware\": {\"platform\": \"iOS\", \"reality\": \"$3\", \"udid\": \"$1\"},
     \"state\": {\"name\": \"$2\", \"visibilityClass\": \"$visibility\"}}}"
}
devices() { print -r -- "{\"result\": {\"devices\": [${(j:,:)@}]}}" > "$T/devs.json"; }

fails=0
expect() {  # expect WANT: dl_find_device prints WANT ("" = must fail)
  local got rc=0
  got=$(DL_DEVICECTL_JSON=$T/devs.json dl_find_device 2>"$T/err") || rc=$?
  if [[ -z $1 ]]; then
    if (( rc == 0 )); then echo "FAIL ($2): picked $got, wanted an error"; fails=$((fails + 1)); fi
  elif [[ $got != "$1" || $rc != 0 ]]; then
    echo "FAIL ($2): got '$got' (rc $rc), wanted $1"; sed 's/^/  /' "$T/err"; fails=$((fails + 1))
  fi
}

SIM1=$(dev AAAA-SIM "Game iPhone 14" simulated disconnected)
SIM2=$(dev BBBB-SIM "iPad Pro" simulated disconnected)
PH1=$(dev 00008110-0000000000000001 "Phone One" physical connected)
PH2=$(dev 00008110-0000000000000002 "Phone Two" physical disconnected)
PH2C=$(dev 00008110-0000000000000002 "Phone Two" physical connected)

devices "$SIM1" "$SIM2" "$PH1";  expect 00008110-0000000000000001 "Simulators and one iPhone"
devices "$SIM1" "$PH2";          expect 00008110-0000000000000002 "one iPhone, not connected yet"
devices "$SIM1" "$SIM2";         expect "" "Simulators only"
devices;                         expect "" "nothing"
devices "$PH1" "$PH2" "$SIM1";   expect 00008110-0000000000000001 "two iPhones, one connected"
devices "$PH1" "$PH2C";          expect "" "two iPhones, both connected"
# Without 'properties': the deprecated fields alone.
devices "$(dev AAAA-SIM S simulated disconnected | python3 -I -c 'import json,sys; d=json.load(sys.stdin); d.pop("properties"); print(json.dumps(d))')" \
        "$(dev 00008110-0000000000000003 P physical connected | python3 -I -c 'import json,sys; d=json.load(sys.stdin); d.pop("properties"); print(json.dumps(d))')"
expect 00008110-0000000000000003 "deprecated fields only"
devices "$SIM1" "$SIM2"
got=$(DL_IOS_DEVICE="Phone Two" DL_DEVICECTL_JSON=$T/devs.json dl_find_device)
[[ $got == "Phone Two" ]] || { echo "FAIL (DL_IOS_DEVICE): got '$got'"; fails=$((fails + 1)); }

(( fails == 0 )) && echo "FIND DEVICE OK"
exit $(( fails > 0 ))
