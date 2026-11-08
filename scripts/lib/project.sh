# Sourced by droidlift's scripts: finds the port (the directory holding
# droidlift.env: $DL_PROJECT, or the current directory or one above it),
# changes into it and loads its settings. The caller sets DL_ROOT (droidlift's
# own directory) first.
#
# droidlift.env is zsh, sourced as is. It sets (with `: ${NAME:=value}`, so
# the environment can override each one):
#   DL_APP_NAME      the iOS app's name, as APP_NAME in droidlift_add_game
#                    (required)
#   DL_APK           the game's APK (default original/game.apk)
#   DL_BUNDLE_PREFIX the free team's bundle ids are <prefix>.<team id>
#                    (default local.droidlift.<app name>)
#   DL_STAGING_DIR   signed copies and the signing stub, outside iCloud
#                    (default ~/Library/Caches/droidlift-<app name>)
_dl_find_project() {
  local d=${DL_PROJECT:-$PWD}
  d=${d:A}
  while true; do
    [[ -f $d/droidlift.env ]] && { print -r -- "$d"; return 0; }
    [[ $d == / ]] && return 1
    d=${d:h}
  done
}
if ! DL_PROJECT=$(_dl_find_project); then
  echo "no droidlift.env in ${DL_PROJECT:-$PWD} or above: run this from the port's directory (or set DL_PROJECT)" >&2
  exit 1
fi
export DL_PROJECT DL_ROOT
cd "$DL_PROJECT"
source ./droidlift.env
if [[ -z ${DL_APP_NAME:-} ]]; then
  echo "$DL_PROJECT/droidlift.env must set DL_APP_NAME" >&2
  exit 1
fi
: ${DL_APK:=original/game.apk}
DL_APK=${DL_APK:A}
: ${DL_BUNDLE_PREFIX:=local.droidlift.${DL_APP_NAME:l}}
: ${DL_STAGING_DIR:=$HOME/Library/Caches/droidlift-${DL_APP_NAME:l}}
export DL_APP_NAME DL_APK DL_BUNDLE_PREFIX DL_STAGING_DIR
