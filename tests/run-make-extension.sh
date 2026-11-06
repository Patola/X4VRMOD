#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# tools/make_extension.py: X4's catalog format, a picture for each of X4's screen shapes at
# X4's own sizes, the version in the picture, and nothing else of the extension changed.
set -uo pipefail
CDPATH=
LC_ALL=C
export LC_ALL
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }
make() { python3 tools/make_extension.py --src extension/x4vrmod --art launcher/art/loading-screen.png "$@"; }

make --version v1.2.3-4-gabcdef0 --date 2026-11-06 --out "$T/a/x4vrmod" || bad "the tool failed"
diff -q extension/x4vrmod/content.xml "$T/a/x4vrmod/content.xml" >/dev/null && ok "content.xml is copied" || bad "content.xml differs"
diff -rq extension/x4vrmod/libraries "$T/a/x4vrmod/libraries" >/dev/null && ok "libraries are copied" || bad "libraries differ"
[ ! -e extension/x4vrmod/ext_01.cat ] && ok "the source tree stays clean" || bad "the source tree got a catalog"
python3 - "$T/a/x4vrmod" <<'PY' && ok "the catalog: X4's format, offsets, hashes, and X4's own names and sizes" || bad "the catalog"
import hashlib,io,sys
from PIL import Image
d=sys.argv[1]
want={"x4_1_1":(1920,1920),"x4_5_4":(1920,1536),"x4_4_3":(1920,1440),"x4_3_2":(1920,1280),"x4_16_10":(1920,1200),"x4_16_9":(1920,1080),"x4_17_9":(2040,1080),"x4_21_9":(2560,1080),"x4_32_9":(3840,1080)}
dat=open(d+"/ext_01.dat","rb").read(); off=0; seen=set()
for l in open(d+"/ext_01.cat"):
    n,s,t,m=l.rstrip("\n").rsplit(" ",3); s=int(s); b=dat[off:off+s]; off+=s
    assert hashlib.md5(b).hexdigest()==m,n
    assert n.startswith("assets/textures/ui/loadingscreens/") and n.endswith(".jpg"),n
    k=n.split("/")[-1][:-4]; im=Image.open(io.BytesIO(b)); assert im.format=="JPEG" and im.size==want[k],(n,im.size)
    seen.add(k)
assert seen==set(want),seen; assert off==len(dat)
PY
make --version v1.2.3-5-gfffffff --date 2026-11-07 --out "$T/b/x4vrmod"
python3 - "$T/a/x4vrmod" "$T/b/x4vrmod" <<'PY' && ok "the version shows, below the art and nowhere else" || bad "the version text"
import io,sys
from PIL import Image,ImageChops
def pic(d):
    dat=open(d+"/ext_01.dat","rb").read(); off=0
    for l in open(d+"/ext_01.cat"):
        n,s,t,m=l.rstrip("\n").rsplit(" ",3); s=int(s)
        if "x4_1_1" in n: return Image.open(io.BytesIO(dat[off:off+s])).convert("RGB")
        off+=s
a,b=pic(sys.argv[1]),pic(sys.argv[2])
box=ImageChops.difference(a,b).point(lambda v:255 if v>24 else 0).getbbox()
# On the square one, used in VR: under the picture, clear of the corners a lens hides.
assert box and box[1]>a.height*0.7 and box[3]<a.height*0.9 and box[0]>a.width*0.1 and box[2]<a.width*0.9,box
PY
A=$(stat -c %Y "$T/a/x4vrmod/ext_01.dat")
touch -d '2001-01-01' "$T/a/x4vrmod/ext_01.dat"
make --version v1.2.3-4-gabcdef0 --date 2026-11-06 --out "$T/a/x4vrmod"
[ "$(stat -c %Y "$T/a/x4vrmod/ext_01.dat")" != "$A" ] && ok "nothing changed: it's left alone" || bad "it rebuilt with nothing changed"
make --version v1.2.3-6-g0000000 --date 2026-11-06 --out "$T/a/x4vrmod"
[ "$(date -r "$T/a/x4vrmod/ext_01.dat" +%Y)" != 2001 ] && ok "a new version: it's rebuilt" || bad "it didn't rebuild for a new version"
X4VR_NO_PILLOW=1 make --version v1 --date 2026-11-06 --out "$T/c/x4vrmod" 2>/dev/null
[ -f "$T/c/x4vrmod/content.xml" ] && [ ! -e "$T/c/x4vrmod/ext_01.cat" ] && ok "no Pillow: the rest still ships, without pictures" || bad "no Pillow"
grep -q 'id="ego_dlc_timelines" optional="true"' extension/x4vrmod/content.xml && ok "it loads after Timelines, when that's there" || bad "no dependency on Timelines"
echo
if [ "$fail" = 0 ]; then echo "all cases passed"; else echo "SOME CASES FAILED"; fi
exit "$fail"
