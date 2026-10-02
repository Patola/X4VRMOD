#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Copy X4VRMOD's own extensions (extension/<name>/ in this repo) into X4's
# extensions folder, when -- and only when -- the installed copy differs.
#
#   launch/deploy-extensions.sh <X4 install dir> [--without NAME]...
#
# --without NAME leaves extension NAME out of this session AND removes our copy
# from X4's folder if one is there: an opt-in extension (x4vrmod_headlook,
# write mode only) should not sit in the folder vanilla X4 also reads, relying
# only on its manifest's enabled="0" default.
#
# Called by x4vr-launch.sh under X4VR_EXTENSIONS=1. Only directories this repo
# owns are touched; every other extension in X4's folder is left alone. It
# says what it did, including "already current", so a run's transcript shows
# which version X4 loaded.
set -uo pipefail
CDPATH=
ROOT="$(cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DIR="${1:?usage: deploy-extensions.sh <X4 install dir> [--without NAME]...}"
shift
WITHOUT=()
while [ $# -gt 0 ]; do
    case "$1" in
        --without) WITHOUT+=("${2:?--without needs a name}"); shift 2 ;;
        *) echo "deploy-extensions: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
DEST="$GAME_DIR/extensions"
[ -d "$DEST" ] || { echo "deploy-extensions: no extensions/ in '$GAME_DIR'" >&2; exit 1; }

rc=0
for src in "$ROOT"/extension/*/; do
    [ -f "$src/content.xml" ] || continue
    name=$(basename "$src")
    dst="$DEST/$name"
    skip=0
    for w in "${WITHOUT[@]}"; do [ "$w" = "$name" ] && skip=1; done
    if [ "$skip" = 1 ]; then
        # Only a directory this repo ships, by its own name -- never anything else.
        if [ -d "$dst" ]; then
            rm -rf -- "$dst" && echo "deploy-extensions: $name REMOVED (not this session)" >&2 \
                || { echo "deploy-extensions: $name could not be removed" >&2; rc=1; }
        else
            echo "deploy-extensions: $name left out (not this session)" >&2
        fi
        continue
    fi
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
