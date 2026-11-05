#version 460
// SPDX-License-Identifier: GPL-3.0-or-later
/* The screen quad: the shown part of a 512x272 target, stretched over the output. */
layout(std140, binding = 0) uniform Show { vec4 uv_scale; } show;
layout(location = 0) out vec2 v_uv;

void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    gl_Position = vec4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
    v_uv = corner * show.uv_scale.xy;
}
