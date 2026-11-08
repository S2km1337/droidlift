#!/bin/zsh
# Copies a built <app>.app to a staging folder outside iCloud and
# prints the copy's path. The repository is in iCloud's Documents, and the
# file provider keeps putting com.apple.FinderInfo and
# com.apple.fileprovider.fpfs#P on bundle folders there, even under *.nosync
# and right after `xattr -cr`. codesign then refuses the bundle ("resource
# fork, Finder information, or similar detritus not allowed"), and
# `codesign --verify --strict` fails on a bundle signed elsewhere and copied
# back. So the install scripts sign and install this copy, then delete it:
#   STAGED=$(ios/stage-app.sh "$APP" ios-device)
#   trap 'rm -rf "${STAGED:h}"' EXIT
# The copy has no extended attributes, resource forks or quarantine.
# usage: ios/stage-app.sh APP NAME
#   NAME  the staging folder's name, so the device and Simulator copies
#         don't meet: <root>/NAME/<app>.app (replaced if it exists)
#   DL_STAGING_DIR  the root (default $DL_STAGING_DIR; must not
#                     be in iCloud's Documents or Desktop)
set -euo pipefail
APP=${1:A} NAME=$2
ROOT=${DL_STAGING_DIR:-$HOME/Library/Caches/droidlift}
ROOT=${ROOT:A}
case $ROOT/ in
  "$HOME"/Documents/*|"$HOME"/Desktop/*|"$HOME"/Library/Mobile\ Documents/*)
    echo "stage-app: $ROOT is synced by iCloud; set DL_STAGING_DIR to a folder outside it" >&2
    exit 1 ;;
esac
[[ -d $APP ]] || { echo "stage-app: no $APP" >&2; exit 1; }
D=$ROOT/$NAME
rm -rf "$D"
mkdir -p "$D"
ditto --norsrc --noextattr --noqtn "$APP" "$D/${APP:t}"
xattr -cr "$D/${APP:t}"
print -r -- "$D/${APP:t}"
