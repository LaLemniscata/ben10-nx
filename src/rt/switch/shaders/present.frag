#version 460
// SPDX-License-Identifier: GPL-3.0-or-later
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(binding = 0) uniform sampler2D u_tex;

void main() {
    o_color = vec4(texture(u_tex, v_uv).rgb, 1.0);
}
