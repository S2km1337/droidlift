#!/bin/zsh
# Fetch and build ANGLE (GLES 2.0 on Metal) into the git-ignored
# third_party/ directory, pinned to exact commits (see THIRD_PARTY.md).
# Slow the first time (gclient pulls ANGLE's own dependencies). Idempotent.
# usage: scripts/fetch-angle.sh
set -euo pipefail
DL_ROOT=${0:A:h:h}
source "$DL_ROOT/scripts/lib/project.sh"

DEPOT_TOOLS_URL=https://chromium.googlesource.com/chromium/tools/depot_tools.git
DEPOT_TOOLS_REV=8defb316a8fccb3645e5bcf3ed078b757106c857
ANGLE_URL=https://chromium.googlesource.com/angle/angle.git
ANGLE_REV=09dcbb2998e5459118a1bc8f6ea1c386e3664f68   # branch chromium/8139

mkdir -p third_party
if [[ ! -d third_party/depot_tools/.git ]]; then
  git clone -q "$DEPOT_TOOLS_URL" third_party/depot_tools
fi
git -C third_party/depot_tools checkout -q "$DEPOT_TOOLS_REV"
export DEPOT_TOOLS_UPDATE=0
export PATH="$PWD/third_party/depot_tools:$PATH"
# With updates off, depot_tools still needs its bundled Python and CIPD tools.
[[ -f third_party/depot_tools/python3_bin_reldir.txt ]] || (cd third_party/depot_tools && ./ensure_bootstrap)

# Already built at the pinned commit: nothing to do.
if [[ -f third_party/angle/out/mac-metal/libGLESv2.dylib ]] &&
   [[ "$(git -C third_party/angle rev-parse HEAD)" == "$ANGLE_REV" ]]; then
  exit 0
fi

if [[ ! -d third_party/angle/.git ]]; then
  git clone -q "$ANGLE_URL" third_party/angle
fi
cd third_party/angle
if [[ "$(git rev-parse HEAD)" != "$ANGLE_REV" ]]; then
  git fetch -q origin "$ANGLE_REV"
  git checkout -q "$ANGLE_REV"
fi
if [[ ! -f ../.gclient ]] && [[ ! -f .gclient ]]; then
  python3 scripts/bootstrap.py
fi
# ANGLE checks out its own depot_tools, which its hooks use; bootstrap that
# copy between the sync and the hooks.
gclient sync --no-history --nohooks -j8
[[ -f third_party/depot_tools/python3_bin_reldir.txt ]] || (cd third_party/depot_tools && ./ensure_bootstrap)
gclient runhooks

# Only the Metal backend; release dylibs libEGL and libGLESv2.
gn gen out/mac-metal --args='is_debug=false angle_enable_metal=true
  angle_enable_vulkan=false angle_enable_gl=false angle_enable_null=false
  angle_enable_swiftshader=false angle_build_tests=false
  angle_enable_wgpu=false target_cpu="arm64"'
autoninja -C out/mac-metal libEGL libGLESv2
