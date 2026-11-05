#version 460
// SPDX-License-Identifier: GPL-3.0-or-later

/* The short fragment shader, for the states most pixels are drawn with: an 8888 target, modulate
 * or replace texturing, fog, and a simple alpha test. ge_dk.c sends every other state to ge_dk.frag,
 * which follows the PSP's integer arithmetic step by step. Here the same formulas run in floats,
 * and the differences stay below one 8-bit step or one sixteenth of a texel: the GPU runs at its
 * lowest clock in the console's performance mode, and each instruction costs every pixel.
 *
 * Without the alpha test there is no discard, so the depth test can reject a pixel before it is
 * shaded. Makefile.switch builds the second form from this file by flipping ALPHA_TEST. */
#define ALPHA_TEST 0

layout(location = 0) noperspective in vec2  v_uv;
layout(location = 1) noperspective in float v_rw;
layout(location = 2) noperspective in float v_fog;
layout(location = 3) noperspective in vec4  v_color;

layout(location = 0) out vec4 o_color;

layout(binding = 0) uniform sampler2D u_tex;

/* Filled by ge_dk.c frag_choose. Fields this shader does not read keep their ge_dk.frag meaning. */
layout(std140, binding = 0) uniform PC {
    ivec4 cfg;      /* x >> 8: flags (1 textured, 8 fog) */
    vec4  texenv;   /* a = highest mip level */
    vec4  fogcol;   /* rgb = fog colour */
    vec4  texsize;  /* zw = texel offset inside a render target */
    ivec4 dith;
    ivec4 bl;
    vec4  fixa;     /* xy = 1 / texture size, z = lowest alpha that passes, w = 1 modulate, 0 replace */
    vec4  fixb;     /* x = texture alpha used, y = colour doubling, z = constant level of detail,
                       w = 1 when the level of detail follows the depth (slope mode) */
} pc;

void main() {
    int flags = pc.cfg.x >> 8;
    vec4 col = v_color;
    if ((flags & 1) != 0) {
        float rw = abs(v_rw);
        vec2 uv = (v_uv / rw + pc.texsize.zw) * pc.fixa.xy;
        vec4 t = textureLod(u_tex, uv, clamp(pc.fixb.z - pc.fixb.w * log2(rw), 0.0, pc.texenv.a));
        col.a = mix(col.a, mix(1.0, col.a, pc.fixa.w) * t.a, pc.fixb.x);
        col.rgb = t.rgb * mix(vec3(1.0), col.rgb, pc.fixa.w) * pc.fixb.y;
    }
#if ALPHA_TEST
    if (col.a < pc.fixa.z) discard;
#endif
    if ((flags & 8) != 0) col.rgb = mix(pc.fogcol.rgb, col.rgb, clamp(v_fog, 0.0, 1.0));
    o_color = col;
}
