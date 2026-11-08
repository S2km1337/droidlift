# Sourced by the iPhone scripts: settles the free team and the
# bundle id, and keeps them in device.local.env (git-ignored).
#   DL_TEAM_ID        the Personal Team's id (10 characters). Default: the
#                       one free team in Xcode's accounts (Xcode > Settings >
#                       Accounts), read from Xcode's preferences.
#   DL_IOS_BUNDLE_ID  default <DL_BUNDLE_PREFIX>.<team id, lower case>: an App
#                       ID must be unique across all of Apple's teams, and
#                       the team id is.
# The bundle id names the app's data container, so once used it must never
# change (a new one is a new, empty app). That's why it is written down on
# first use; the environment overrides the file.
# Needs: zsh, the repository root as the working directory.

DL_DEVICE_ENV=${DL_PROJECT:-$PWD}/device.local.env
typeset _env_team=${DL_TEAM_ID:-} _env_bundle=${DL_IOS_BUNDLE_ID:-}
[[ -f $DL_DEVICE_ENV ]] && source "$DL_DEVICE_ENV"
[[ -n $_env_team ]] && DL_TEAM_ID=$_env_team
[[ -n $_env_bundle ]] && DL_IOS_BUNDLE_ID=$_env_bundle

# The free ("Personal Team") teams Xcode knows, one id per line.
dl_xcode_free_teams() {
  defaults export com.apple.dt.Xcode - 2>/dev/null | python3 -I -c '
import plistlib, sys
try:
    d = plistlib.loads(sys.stdin.buffer.read()).get("IDEProvisioningTeamByIdentifier", {})
except Exception:
    d = {}
ids = {t.get("teamID") for teams in d.values() for t in teams if t.get("isFreeProvisioningTeam") and t.get("teamID")}
print("\n".join(sorted(ids)))'
}

# Sets DL_TEAM_ID and DL_IOS_BUNDLE_ID, or explains and returns 1.
dl_device_config() {
  if [[ -z ${DL_TEAM_ID:-} ]]; then
    local teams=("${(@f)$(dl_xcode_free_teams)}")
    teams=(${teams:#})
    if (( ${#teams} == 1 )); then
      DL_TEAM_ID=${teams[1]}
    elif (( ${#teams} == 0 )); then
      echo "No free team in Xcode. Open Xcode > Settings > Accounts, add your Apple ID, then run this again." >&2
      return 1
    else
      echo "Xcode knows several free teams (${teams[*]}): run again with DL_TEAM_ID=<one of them>." >&2
      return 1
    fi
  fi
  [[ -n ${DL_IOS_BUNDLE_ID:-} ]] || DL_IOS_BUNDLE_ID=$DL_BUNDLE_PREFIX.${DL_TEAM_ID:l}
  if [[ ! -f $DL_DEVICE_ENV ]]; then
    print -r -- "# The iPhone build's team and bundle id, written on first use.
# Keep the bundle id: it names the app's data container, so changing it
# starts a new, empty app.
DL_TEAM_ID=$DL_TEAM_ID
DL_IOS_BUNDLE_ID=$DL_IOS_BUNDLE_ID" > "$DL_DEVICE_ENV"
    echo "wrote $DL_DEVICE_ENV (team $DL_TEAM_ID, bundle id $DL_IOS_BUNDLE_ID)"
  fi
  export DL_TEAM_ID DL_IOS_BUNDLE_ID
}

# The iPhone for devicectl and xcodebuild: DL_IOS_DEVICE (a name, UDID or
# CoreDevice id), else the one physical iOS device devicectl knows.
# Simulators don't count (devicectl lists them too, "Reality: simulated").
# With several physical devices, the one connected right now. Prints its UDID
# or explains and returns 1.
dl_find_device() {
  if [[ -n ${DL_IOS_DEVICE:-} ]]; then print -r -- "$DL_IOS_DEVICE"; return 0; fi
  local json; json=$(mktemp)
  if [[ -n ${DL_DEVICECTL_JSON:-} ]]; then
    cp "$DL_DEVICECTL_JSON" "$json"  # tests: a saved `devicectl list devices` JSON
  elif ! xcrun devicectl list devices --json-output "$json" >/dev/null 2>&1; then
    rm -f "$json"
    echo "devicectl couldn't list devices (is Xcode 27 installed and selected?)" >&2
    return 1
  fi
  local found rc=0
  found=$(python3 -I - "$json" <<'EOF'
import json, sys

def get(d, *paths):
    """The first plain value at any of the dotted paths. Xcode 27's devicectl
    has the 'properties' dictionary and, deprecated, hardwareProperties,
    deviceProperties and connectionProperties (seen 2026-11-03)."""
    for path in paths:
        v = d
        for k in path.split("."):
            v = v.get(k) if isinstance(v, dict) else None
        if v is not None and not isinstance(v, (dict, list)):
            return v
    return None

devs = json.load(open(sys.argv[1])).get("result", {}).get("devices", [])
phys = []
for d in devs:
    if str(get(d, "properties.hardware.platform", "hardwareProperties.platform") or "").lower() != "ios":
        continue
    reality = get(d, "properties.hardware.reality", "hardwareProperties.reality")
    provider = str(get(d, "deviceProperties.provider") or "")
    visibility = get(d, "properties.state.visibilityClass", "visibilityClass")
    if reality != "physical" or "Simulator" in provider or visibility == "simulators":
        continue
    udid = get(d, "properties.hardware.udid", "hardwareProperties.udid") or d.get("identifier")
    name = get(d, "properties.state.name", "deviceProperties.name") or "?"
    state = get(d, "properties.connection.state", "connectionProperties.tunnelState")
    phys.append((udid, name, state == "connected"))
connected = [p for p in phys if p[2]]
pick = phys if len(phys) == 1 else connected
if len(pick) == 1:
    print(pick[0][0])
    sys.exit(0)
for udid, name, conn in phys:
    print(f"  {udid}  {name}  ({'connected' if conn else 'not connected'})", file=sys.stderr)
sys.exit(2 if phys else 3)
EOF
) || rc=$?
  rm -f "$json"
  if (( rc == 3 )); then
    echo "No iPhone found (Simulators don't count). Connect it with a cable, unlock it, tap Trust, and run this again." >&2
    return 1
  elif (( rc == 2 )); then
    echo "Several iPhones are known (above) and not exactly one is connected; pick one with DL_IOS_DEVICE=<UDID or name>." >&2
    return 1
  elif (( rc != 0 )); then
    return 1
  fi
  print -r -- "$found"
}
