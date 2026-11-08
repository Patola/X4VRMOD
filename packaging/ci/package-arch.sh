#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# The real PKGBUILD, building this checkout rather than a published tag. makepkg won't run as
# root, so in a container it gets a user of its own.
#     packaging/ci/package-arch.sh <out dir>
set -euo pipefail
out=$(realpath -m "$1")
src=$PWD
git config --global --add safe.directory "$src" 2>/dev/null || true
# pkgver takes no "-": v1.0.0-rc1 is 1.0.0rc1, which pacman sorts before 1.0.0.
ver=$(git describe --tags --match 'v[0-9]*' | sed -E 's/^v//; s/-([0-9]+)-g/.r\1.g/; s/-(rc[0-9]+)/\1/')
work=$(mktemp -d)
sed -E "s|^pkgver=.*|pkgver=$ver|;
        s|^source=.*|source=(\"x4vrmod::git+file://$src#commit=$(git rev-parse HEAD)\")|" \
    packaging/arch/PKGBUILD > "$work/PKGBUILD"
mkdir -p "$out"
if [ "$(id -u)" = 0 ]; then
    useradd -m builder 2>/dev/null || true
    chown -R builder "$work"
    git config --system --add safe.directory "$src"
    git config --system --add safe.directory "$src/.git"
    (cd "$work" && su builder -c 'makepkg --noconfirm --nodeps')
else
    (cd "$work" && makepkg --noconfirm --nodeps)
fi
cp "$work"/*.pkg.tar.zst "$out/"
ls -l "$out"
