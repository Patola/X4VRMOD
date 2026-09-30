#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Copy X4VRMOD's own extensions (extension/<name>/ in this repo) into X4's
# extensions folder, when -- and only when -- the installed copy differs.
#
#   launch/deploy-extensions.sh <X4 install dir>
#
# Called by x4vr-launch.sh under X4VR_EXTENSIONS=1. Only directories this repo
# owns are touched; every other extension in X4's folder is left alone. It
# says what it did, including "already current", so a run's transcript shows
# which version X4 loaded.
set -uo pipefail
CDPATH=
ROOT="$(cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DIR="${1:?usage: deploy-extensions.sh <X4 install dir>}"
DEST="$GAME_DIR/extensions"
[ -d "$DEST" ] || { echo "deploy-extensions: no extensions/ in '$GAME_DIR'" >&2; exit 1; }

rc=0
for src in "$ROOT"/extension/*/; do
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
exit "$rc"
