#!/usr/bin/env bash
# Rebuild tests/data/census-base.log, the run the census plants its lines on.
#
# The census used /tmp/x4vr-take306.log until /tmp was cleaned by age (take
# 329) and it began SKIPping with exit 0. The base is now a committed, small
# log: a real run's first 200 lines (a render-pass inventory, so the scorer
# reads it at all), with the run line of take 303's era -- none of the
# features later blocks are gated on asked for -- and those features' own
# start-up lines removed, so every "not asked -> silent" case still means it.
#
#   tests/make-census-base.sh /tmp/x4vr-take329.log
set -euo pipefail
CDPATH=
LC_ALL=C
export LC_ALL
src=${1:?usage: $0 <a real take log>}
cd "$(dirname "$0")/.."
run='X4VR_TAKE=census-base X4VR_CANVAS_M=1000 X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_H=2816 X4VR_DECLARE_RENDERED_POS=1 X4VR_W=5632 X4VR_ROLL_STEREO=1 X4VR_HEADLOOK_LEAD=1.0 X4VR_RES=2816x2816 X4VR_VR=1 X4VR_OFFAXIS=off X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_HEADLOOK=1 X4VR_SBS_LAYERS=2 X4VR_UI_WGATE=0.02 X4VR_POSE=0 X4VR_HEADLOOK_RAW=191 X4VR_HEADLOOK_KP=0.25 X4VR_POSELOG=1 X4VR_UI_WGATE_W1=0 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_SBS=1 X4VR_PROJ_INVPROJ=1 X4VR_CAMDIFF=1 X4VR_LOG=/tmp/x4vr-census-base.log X4VR_CAMSTAMP=1 X4VR_DECLARE_RENDERED_ROT=2 X4VR_MASK_PRESENT=1 X4VR_UI_FOV=1.0 X4VR_IPD=0.064 X4VR_FOV=1.4917 X4VR_BINDLESS_MIRROR=1 X4VR_CAMREAD=1 X4VR_CAMLOOP=1 X4VR_HEADLOOK_GAIN=0.115 ./launch/x4vr-launch.sh'
# ...plus the run's first camera reads: the clamp block judges only runs that
# read X4's camera, and 306 had them. The head's roll is a gameplay sample.
# Live reads only: a raw 0,0 is a dead read-back and judges nothing.
# ...and the run's periodic `vr copy`/`vr summary` series: the pace block's
# per-window rate is built from consecutive copy counts (take 303's base had
# the whole series).
{ head -n 200 "$src"
  awk '/inject  camread: X4 / && !/raw 0\.0000,0\.0000/ && n < 20 {print; n++}' "$src"
  awk 'NR > 200 && /layer   vr (copy|summary) \(periodic\)/' "$src"; } |
    grep -v -E 'inject  (content:|csmfit:|headlook write|sdl: X4 has set no cursor|lua\[)|fopen\(.*(content|config)\.xml|inject  config: serving|layer   roll: |layer   vr pace ' |
    sed -E "s#(inject  env: run = ).*#\\1${run}#" |
    sed -E 's#/EgoSoft/X4/[0-9]+/#/EgoSoft/X4/00000000/#g' > tests/data/census-base.log
echo "wrote tests/data/census-base.log ($(wc -l < tests/data/census-base.log) lines)"
