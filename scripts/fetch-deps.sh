#!/bin/zsh
# Fetch third-party code into the git-ignored third_party/ directory, pinned to
# exact commits (see THIRD_PARTY.md). Idempotent.
# usage: scripts/fetch-deps.sh
set -euo pipefail
DL_ROOT=${0:A:h:h}
source "$DL_ROOT/scripts/lib/project.sh"

# dynarmic: the Azahar fork, the maintained successor of merryhime/dynarmic
# (the original repository no longer exists).
DYNARMIC_URL=https://github.com/azahar-emu/dynarmic
DYNARMIC_REV=a46601580d5512d324104f985b5f0209dc980ddc
# Only the externals the arm64 macOS build needs.
DYNARMIC_EXTERNALS=(externals/fmt externals/mcl externals/oaknut externals/robin-map)

mkdir -p third_party
if [[ ! -d third_party/dynarmic/.git ]]; then
  git clone -q "$DYNARMIC_URL" third_party/dynarmic
fi
if [[ "$(git -C third_party/dynarmic rev-parse HEAD)" != "$DYNARMIC_REV" ]]; then
  git -C third_party/dynarmic fetch -q origin
  git -C third_party/dynarmic checkout -q "$DYNARMIC_REV"
fi
git -C third_party/dynarmic submodule update --init -q "${DYNARMIC_EXTERNALS[@]}"

# Boost headers (dynarmic needs them), from Homebrew.
if ! brew list boost >/dev/null 2>&1; then
  brew install boost
fi
