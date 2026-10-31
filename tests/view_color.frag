#version 450
#extension GL_EXT_multiview : require
// A different colour per view, so a test can tell which layer a later pass
// read: red 0.25 in view 0 (the left eye), 0.75 in view 1.
layout(location = 0) out vec4 o_colour;
void main() {
    o_colour = gl_ViewIndex == 0 ? vec4(0.25, 0.5, 0.5, 1.0)
                                 : vec4(0.75, 0.5, 0.5, 1.0);
}
