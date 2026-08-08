#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Embed SPIR-V modules into a C++ header.

The layer draws a textured quad (task #17) and therefore needs shaders of its
own. It cannot read them from disk at run time: a Vulkan layer is loaded into
someone else's process from a path the loader chose, and "where are my shaders"
has no good answer there -- so they are compiled ahead of time and linked in.

Committing the generated header, like `tests/` commits its `.spv`, means the
build needs no glslc. The GLSL source sits next to it so the bytes are never
the only record of what they are.

    tools/spv2hpp.py layer/cursor_shaders.hpp \\
        kCursorQuadVert=layer/cursor_quad.vert.spv \\
        kCursorQuadFrag=layer/cursor_quad.frag.spv
"""
import struct
import sys

SPIRV_MAGIC = 0x07230203


def load(path):
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) % 4:
        raise SystemExit(f"{path}: {len(blob)} bytes is not a whole number of words")
    words = list(struct.unpack(f"<{len(blob) // 4}I", blob))
    # Checked rather than assumed: a big-endian module, or a file that is not
    # SPIR-V at all, would otherwise be embedded as plausible-looking garbage
    # and only fail inside vkCreateShaderModule at run time.
    if not words or words[0] != SPIRV_MAGIC:
        got = f"0x{words[0]:08x}" if words else "empty"
        raise SystemExit(f"{path}: magic {got}, expected 0x{SPIRV_MAGIC:08x}")
    return words


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    out, specs = argv[1], argv[2:]
    parts = [
        "// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception",
        "//",
        "// GENERATED -- do not edit. Regenerate with:",
        "//",
        "//     tools/spv2hpp.py " + " ".join(sys.argv[1:]),
        "//",
        "// The GLSL sources sit beside the .spv files they came from.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace x4vr {",
        "",
    ]
    for spec in specs:
        name, _, path = spec.partition("=")
        words = load(path)
        parts.append(f"// from {path} ({len(words)} words)")
        parts.append(f"inline const uint32_t {name}[] = {{")
        for i in range(0, len(words), 6):
            row = ", ".join(f"0x{w:08x}" for w in words[i : i + 6])
            parts.append(f"    {row},")
        parts.append("};")
        parts.append("")
    parts.append("} // namespace x4vr")
    parts.append("")
    with open(out, "w") as f:
        f.write("\n".join(parts))
    print(f"wrote {out}")


if __name__ == "__main__":
    main(sys.argv)
