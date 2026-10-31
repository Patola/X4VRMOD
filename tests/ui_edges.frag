#version 450
// Edges at fixed fractions of the target, from fullscreen_uv.vert's
// coordinate: dark left half, bright right half, and a white stripe 2% wide at
// 30% (2.56 px of a 128-wide target). Drawn at 2x and filtered down
// (X4VR_UI_SS) the halves must come out identical and the edges and the stripe
// in place, anti-aliased.
layout(location = 0) in vec2 i_uv;
layout(location = 0) out vec4 o_colour;
void main() {
    float v = i_uv.x < 0.5 ? 0.2 : 0.8;
    if (i_uv.x > 0.30 && i_uv.x < 0.32)
        v = 1.0;
    o_colour = vec4(v, v, v, 1.0);
}
