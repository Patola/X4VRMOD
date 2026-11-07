#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# A .deb or .rpm for the distribution this runs on, named after it.
#     packaging/ci/package-cpack.sh DEB|RPM <label, like ubuntu:24.04> <out dir>
set -euo pipefail
gen=$1 label=${2//:/-} out=$3
git config --global --add safe.directory "$PWD" 2>/dev/null || true
cmake -S . -B build-pkg -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DX4VR_BUILD_TESTS=OFF
cmake --build build-pkg -j"$(nproc)"
(cd build-pkg && cpack -G "$gen")
mkdir -p "$out"
for f in build-pkg/*.deb build-pkg/*.rpm; do
    [ -f "$f" ] || continue
    base=${f##*/}
    # x4vrmod_0.9.10_amd64.deb -> x4vrmod_0.9.10_ubuntu-24.04_amd64.deb, and the same for rpm
    case "$base" in
        *.deb) cp "$f" "$out/${base%_*}_${label}_${base##*_}" ;;
        *.rpm) cp "$f" "$out/${base%.*.rpm}.${label}.${base#"${base%.*.rpm}."}" ;;
    esac
done
ls -l "$out"
