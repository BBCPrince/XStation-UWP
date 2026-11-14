// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
void main() {
    vec2 position = vec2(float((gl_VertexIndex & 1) << 2), float((gl_VertexIndex & 2) << 1));
    gl_Position = vec4(position - 1.0, 0.5, 1.0);
}
