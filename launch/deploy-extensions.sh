#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Copy X4VRMOD's own extension into X4's extensions folder, when -- and only when
# -- the installed copy differs.
#
#   launch/deploy-extensions.sh <X4 install dir>
#
# Also removes the directories of our ids until v0.9.8: x4vrmod_lookaround and
# x4vrmod_nosway, merged into x4vrmod, and x4vrmod_headlook, dropped. Left in
# X4's folder, a stale copy could load beside it.
#
# Called by x4vr-launch.sh under X4VR_EXTENSIONS=1. Only directories this repo
# owns are touched; every other extension in X4's folder is left alone. It
# says what it did, including "already current", so a run's transcript shows
# which version X4 loaded.
set -uo pipefail
CDPATH=
ROOT="$(cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DIR="${1:?usage: deploy-extensions.sh <X4 install dir>}"
[ $# -eq 1 ] || { echo "deploy-extensions: unknown argument '$2'" >&2; exit 2; }
DEST="$GAME_DIR/extensions"
[ -d "$GAME_DIR" ] || { echo "deploy-extensions: no folder '$GAME_DIR'" >&2; exit 1; }
# A copy with no DLC may have no extensions/ yet.
mkdir -p "$DEST" || { echo "deploy-extensions: can't create '$DEST'" >&2; exit 1; }
RETIRED=(x4vrmod_lookaround x4vrmod_nosway x4vrmod_headlook)

rc=0
# What ships: a build's copy holds the loading screen. A zip or a package has only this one.
SHIPPED="$ROOT/extension"
[ -d "${X4VR_BUILD:-$ROOT/build}/extension" ] && SHIPPED="${X4VR_BUILD:-$ROOT/build}/extension"
for src in "$SHIPPED"/*/; do
    [ -f "$src/content.xml" ] || continue
    name=$(basename "$src")
    dst="$DEST/$name"
    if [ -d "$dst" ] && diff -rq "$src" "$dst" >/dev/null 2>&1; then
        echo "deploy-extensions: $name already current" >&2
        continue
    fi
    mkdir -p "$dst" && cp -r "$src"/. "$dst"/ || { rc=1; continue; }
    # Anything the repo no longer ships is removed from OUR directory only.
    (cd "$dst" && find . -type f | while read -r f; do
        [ -e "$src/$f" ] || rm -f -- "$f"
    done)
    if diff -rq "$src" "$dst" >/dev/null 2>&1; then
        echo "deploy-extensions: $name DEPLOYED to $dst" >&2
    else
        echo "deploy-extensions: $name copy does not match after deploying" >&2
        rc=1
    fi
done
for name in "${RETIRED[@]}"; do
    # Only these names, by exact path -- never anything else.
    dst="$DEST/$name"
    [ -d "$dst" ] || continue
    rm -rf -- "$dst" && echo "deploy-extensions: $name REMOVED (retired after v0.9.8)" >&2 \
        || { echo "deploy-extensions: $name (retired) could not be removed" >&2; rc=1; }
done
exit "$rc"
