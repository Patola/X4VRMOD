#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Pull X4's shipped GLSL out of its .cat/.dat archives, into /tmp.

    tools/extract_glsl.py                      # all of shadergl/, to /tmp/x4-shadersrc
    tools/extract_glsl.py --grep ui.vert       # just what matches

**The extracted files are EGOSOFT'S COPYRIGHTED CONTENT.** They go to /tmp and
are never committed, never quoted at length, and never checked into this repo.
This script is ours; its output is not. The hard-coded destination under /tmp
is part of that rule, not a convenience.

Why this exists: `/tmp/x4-shadersrc` has been the source of two fixes -- #70's
`B_csmdebugcolor` and #76's `U_fsr3g_useuitexture` -- both found by READING what
X4 already ships rather than by instrumenting a frame or patching a module. But
/tmp is cleared on reboot and the recipe was never written down, so the archive
had to be reverse-engineered again. Now it is a script.

The format: each .cat line is `path size mtime md5`, and the matching .dat is
every file concatenated in that order, so a file's offset is the sum of the
sizes before it. Paths contain spaces, sizes and hashes do not, so the line is
split from the RIGHT.
"""
import argparse
import os
import sys

GAME = '/nvme/SteamLibrary/steamapps/common/X4 Foundations'
OUT = '/tmp/x4-shadersrc'


def entries(cat):
    """(path, offset, size) for every file in one catalogue."""
    off = 0
    with open(cat, encoding='utf-8', errors='replace') as f:
        for line in f:
            parts = line.rstrip('\n').rsplit(' ', 3)
            if len(parts) != 4:
                continue
            path, size = parts[0], parts[1]
            try:
                n = int(size)
            except ValueError:
                continue
            yield path, off, n
            off += n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=GAME)
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--prefix', default='shadergl/',
                    help='archive path prefix to extract (default shadergl/)')
    ap.add_argument('--grep', default=None,
                    help='only paths containing this substring')
    a = ap.parse_args()

    if not os.path.isdir(a.game):
        sys.exit(f'no X4 install at {a.game!r}')
    if not a.out.startswith('/tmp/'):
        sys.exit(f'refusing to write outside /tmp: {a.out!r}. X4\'s GLSL is '
                 f'Egosoft\'s copyrighted content and must not land in the '
                 f'repo or the home directory.')
    os.makedirs(a.out, exist_ok=True)

    cats = sorted(f for f in os.listdir(a.game)
                  if f.endswith('.cat') and '_sig' not in f)
    if not cats:
        sys.exit(f'no .cat archives in {a.game!r}')

    n_written, n_seen = 0, 0
    # Later catalogues override earlier ones, which is how X4 patches itself;
    # extracting in order and letting the last write win reproduces that.
    for cat in cats:
        dat = os.path.join(a.game, cat[:-4] + '.dat')
        if not os.path.exists(dat):
            print(f'  {cat}: no matching .dat, skipped', file=sys.stderr)
            continue
        want = []
        for path, off, size in entries(os.path.join(a.game, cat)):
            if not path.startswith(a.prefix) or path.endswith('.sig'):
                continue
            if a.grep and a.grep not in path:
                continue
            want.append((path, off, size))
        if not want:
            continue
        n_seen += len(want)
        with open(dat, 'rb') as d:
            for path, off, size in want:
                d.seek(off)
                blob = d.read(size)
                if len(blob) != size:
                    print(f'  SHORT READ {path}: {len(blob)} of {size} — the '
                          f'offset arithmetic is wrong, not the file',
                          file=sys.stderr)
                    continue
                dst = os.path.join(a.out, os.path.basename(path))
                with open(dst, 'wb') as o:
                    o.write(blob)
                n_written += 1
        print(f'  {cat}: {len(want)} matching entr(ies)')

    print(f'{n_written} file(s) written to {a.out} (of {n_seen} matched)')
    if n_written == 0:
        sys.exit('nothing extracted — check --prefix/--grep against a .cat')


if __name__ == '__main__':
    main()
