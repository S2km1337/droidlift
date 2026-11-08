#!/bin/zsh
# Signs <app>.app for the iPhone with the free team's
# provisioning profile, which Xcode made for the stub project
# (ios/signing-project.sh), and the matching "Apple Development" identity
# from the login keychain. Both are found automatically:
#   profile:  in Xcode's profile folders, for exactly TEAM.BUNDLE_ID, not
#             expired; the one that lasts longest.
#   identity: a valid code-signing identity whose certificate is in that
#             profile.
# The profile is embedded as embedded.mobileprovision; the entitlements are
# the ones Xcode would sign with (application id, team id, get-task-allow,
# keychain group, plus Increased Memory Limit if the profile has it).
# Exit status 3: no usable profile yet (scripts/ios-profile.sh makes one).
# Sign a copy outside iCloud (ios/stage-app.sh): in the repository's
# Documents folder codesign refuses the bundle; scripts/build-ios-device.sh
# does that.
# usage: ios/sign-device.sh APP BUNDLE_ID TEAM_ID [DEVICE_UDID]
#        ios/sign-device.sh --check BUNDLE_ID TEAM_ID [DEVICE_UDID]
#   DEVICE_UDID: warn if the profile doesn't list that iPhone.
#   --check: sign nothing; print the profile it would use
#            ("profile NAME valid until DATE: FILE") or exit 3.
#   DL_PROFILE_DIR: look only there (for tests).
set -euo pipefail
CHECK=0
if [[ ${1:-} == --check ]]; then CHECK=1; shift; set -- "" "$@"; fi
APP=$1 BUNDLE_ID=$2 TEAM_ID=$3 UDID=${4:-}

# Xcode 16 and later keep them in UserData; older Xcodes in MobileDevice.
PROFILE_DIRS=("$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles"
              "$HOME/Library/MobileDevice/Provisioning Profiles")
[[ -n ${DL_PROFILE_DIR:-} ]] && PROFILE_DIRS=("$DL_PROFILE_DIR")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Decode every profile (CMS-signed plists) into WORK/N.plist.
i=0
for d in $PROFILE_DIRS; do
  [[ -d $d ]] || continue
  for f in "$d"/*.mobileprovision(N) "$d"/*.provisionprofile(N); do
    i=$((i + 1))
    if security cms -D -i "$f" > "$WORK/$i.plist" 2>/dev/null; then
      print -r -- "$f" > "$WORK/$i.path"
    else
      rm -f "$WORK/$i.plist"
    fi
  done
done

# The SHA-1s of the valid code-signing identities.
security find-identity -v -p codesigning | awk '/^ *[0-9]+\)/ {print $2}' > "$WORK/identities"

rc=0
python3 -I - "$WORK" "$BUNDLE_ID" "$TEAM_ID" "$UDID" <<'EOF' || rc=$?
import datetime, glob, hashlib, os, plistlib, sys
work, bundle, team, udid = sys.argv[1:5]
want = f"{team}.{bundle}"
ids = set(open(os.path.join(work, "identities")).read().split())
now = datetime.datetime.now(datetime.timezone.utc)
best = None
for p in glob.glob(os.path.join(work, "*.plist")):
    try:
        pl = plistlib.load(open(p, "rb"))
    except Exception:
        continue
    ent = pl.get("Entitlements", {})
    if ent.get("application-identifier") != want:
        continue
    exp = pl.get("ExpirationDate")
    if exp is None:
        continue
    if exp.tzinfo is None:
        exp = exp.replace(tzinfo=datetime.timezone.utc)
    if exp <= now + datetime.timedelta(minutes=10):
        continue
    certs = [hashlib.sha1(c).hexdigest().upper() for c in pl.get("DeveloperCertificates", [])]
    usable = [c for c in certs if c in ids]
    if not usable:
        continue
    if best is None or exp > best[0]:
        best = (exp, p, pl, usable[0])
if best is None:
    sys.exit(3)
exp, p, pl, sha1 = best
ent = pl["Entitlements"]
out = {
    "application-identifier": want,
    "com.apple.developer.team-identifier": team,
    "get-task-allow": bool(ent.get("get-task-allow", True)),
}
if "keychain-access-groups" in ent:
    out["keychain-access-groups"] = [want]
for k in ("com.apple.developer.kernel.increased-memory-limit",):
    if ent.get(k) is True:
        out[k] = True
plistlib.dump(out, open(os.path.join(work, "entitlements.plist"), "wb"))
open(os.path.join(work, "chosen"), "w").write(
    "\n".join([open(p[:-len(".plist")] + ".path").read().strip(), sha1,
               exp.astimezone().strftime("%Y-%m-%d %H:%M"), pl.get("Name", "?")]) + "\n")
devices = pl.get("ProvisionedDevices", [])
if udid and udid not in devices and not pl.get("ProvisionsAllDevices"):
    print(f"sign-device: warning: the profile doesn't list device {udid} "
          f"(it lists {len(devices)}); install may fail", file=sys.stderr)
EOF
if (( rc == 3 )); then
  echo "sign-device: no valid provisioning profile for $TEAM_ID.$BUNDLE_ID with a signing identity in the keychain" >&2
  exit 3
fi
(( rc == 0 )) || exit $rc

PROFILE=$(sed -n 1p "$WORK/chosen") IDENTITY=$(sed -n 2p "$WORK/chosen")
EXPIRES=$(sed -n 3p "$WORK/chosen") NAME=$(sed -n 4p "$WORK/chosen")
if (( CHECK )); then
  echo "profile \"$NAME\" valid until $EXPIRES: $PROFILE"
  exit 0
fi

cp "$PROFILE" "$APP/embedded.mobileprovision"
# codesign refuses Finder and file-provider attributes (iCloud's Documents
# folder tags .framework folders a moment after they appear): clear, retry.
# In a staged copy outside iCloud the first try works.
sign() {
  local i
  for i in 1 2 3 4 5; do
    xattr -cr "$APP"
    codesign --force --sign "$IDENTITY" --timestamp=none "$@" 2>/dev/null && return 0
    sleep 1
  done
  codesign --force --sign "$IDENTITY" --timestamp=none "$@"
}
for fw in "$APP"/Frameworks/*.framework(N); do sign "$fw"; done
sign --entitlements "$WORK/entitlements.plist" "$APP"
codesign --verify --deep --strict "$APP"
echo "signed $APP"
echo "  profile \"$NAME\" (valid until $EXPIRES)"
echo "  identity $IDENTITY"
