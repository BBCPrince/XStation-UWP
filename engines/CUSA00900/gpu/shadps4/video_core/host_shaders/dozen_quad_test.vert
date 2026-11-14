// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(location = 0) out vec4 attributes;
void main() {
    const vec2 corners[4] = vec2[4](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 1));
    vec2 uv = corners[gl_VertexIndex];
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
    attributes = vec4(uv, 0.25, 1.0);
}
