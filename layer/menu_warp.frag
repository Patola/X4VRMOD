// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4's frozen menu frame, redrawn for where the head points now
// (x4vr_menu_warp.hpp says why).
//
// For each pixel of the target: its direction in the view the target declares,
// turned by M into the view X4 rendered, projected into X4's image. That is a
// pure rotation about the eye, so it is exact at every depth.
//
// Filtering is in LINEAR light and covers the pixel's footprint: a box of up to
// 4x4 bilinear taps, as many as the footprint is texels long on each axis. One
// tap where the footprint is a texel or less, at exactly the mapped point, so
// an unturned frame comes out as the frame and not a blur of it. The footprint
// comes from the mapping itself at the neighbouring pixels, not from dFdx,
// whose 2x2-quad granularity is the implementation's choice.
//
// Bytes in, bytes out: X4's eye image holds sRGB-encoded bytes, and the copy
// this replaces moves them unchanged. So texels are decoded to linear unless
// the view already did it, and the result is encoded again unless the target's
// view will.
//
// Regenerate with:  glslangValidator -V menu_warp.frag -o menu_warp.frag.spv
#version 450

layout(set = 0, binding = 0) uniform sampler2DArray u_src;

layout(push_constant) uniform Push {
    vec4 m0;      // M's rows; m0.w = the layer
    vec4 m1;      // m1.w: bit 0 decode the source, bit 1 encode the result
    vec4 m2;
    vec4 tan_dst; // the target's field: tan left, right, up, down
    vec4 tan_src; // the field X4 rendered
    vec4 ext;     // target w, h; source w, h
} pc;

layout(location = 0) out vec4 o_color;

vec3 decode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)),
               greaterThan(c, vec3(0.04045)));
}

vec3 encode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
               greaterThan(c, vec3(0.0031308)));
}

// Where a point of the target (in pixels) lands in the source (in pixels).
// `ok` is false behind the rendered eye.
vec2 source_px(vec2 p, out bool ok) {
    vec2 uv = p / pc.ext.xy;
    vec3 d = vec3(mix(pc.tan_dst.x, pc.tan_dst.y, uv.x),
                  mix(pc.tan_dst.z, pc.tan_dst.w, uv.y), -1.0);
    vec3 r = vec3(dot(pc.m0.xyz, d), dot(pc.m1.xyz, d), dot(pc.m2.xyz, d));
    ok = r.z < -1e-4;
    vec2 t = r.xy / -r.z;
    vec2 s = vec2((t.x - pc.tan_src.x) / (pc.tan_src.y - pc.tan_src.x),
                  (pc.tan_src.z - t.y) / (pc.tan_src.z - pc.tan_src.w));
    return s * pc.ext.zw;
}

vec4 texel(ivec2 q, int layer, bool dec) {
    if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, ivec2(pc.ext.zw))))
        return vec4(0.0); // past X4's picture: black, as the runtime shows it
    vec4 c = texelFetch(u_src, ivec3(q, layer), 0);
    if (dec)
        c.rgb = decode(c.rgb);
    return c;
}

vec4 bilinear(vec2 p, int layer, bool dec) {
    vec2 b = p - 0.5;
    ivec2 i = ivec2(floor(b));
    vec2 f = b - floor(b);
    // Float error in the round trip through tangents leaves an unturned pixel
    // a few ten-thousandths off its texel. Snapped, so it is that texel.
    f = mix(f, vec2(0.0), lessThan(f, vec2(1e-3)));
    bvec2 up = greaterThan(f, vec2(1.0 - 1e-3));
    i += ivec2(up);
    f = mix(f, vec2(0.0), up);
    if (f == vec2(0.0))
        return texel(i, layer, dec);
    vec4 a = mix(texel(i, layer, dec), texel(i + ivec2(1, 0), layer, dec), f.x);
    vec4 c = mix(texel(i + ivec2(0, 1), layer, dec),
                 texel(i + ivec2(1, 1), layer, dec), f.x);
    return mix(a, c, f.y);
}

void main() {
    const int layer = int(pc.m0.w);
    const int flags = int(pc.m1.w);
    const bool dec = (flags & 1) != 0;
    const bool enc = (flags & 2) != 0;

    bool ok, okx, oky;
    vec2 p = source_px(gl_FragCoord.xy, ok);
    if (!ok) {
        o_color = vec4(0.0);
        return;
    }
    vec2 ax = source_px(gl_FragCoord.xy + vec2(1.0, 0.0), okx) - p;
    vec2 ay = source_px(gl_FragCoord.xy + vec2(0.0, 1.0), oky) - p;
    // Over a texel by a hundredth before a second tap: at 1:1 the footprint
    // measures 1.0000x, and two taps there would be a blur.
    int nx = okx ? int(clamp(ceil(length(ax) - 0.01), 1.0, 4.0)) : 1;
    int ny = oky ? int(clamp(ceil(length(ay) - 0.01), 1.0, 4.0)) : 1;

    vec4 acc = vec4(0.0);
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            vec2 off = ((float(i) + 0.5) / float(nx) - 0.5) * ax +
                       ((float(j) + 0.5) / float(ny) - 0.5) * ay;
            acc += bilinear(p + off, layer, dec);
        }
    acc /= float(nx * ny);
    if (enc)
        acc.rgb = encode(acc.rgb);
    o_color = acc;
}
