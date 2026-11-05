/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NK_SWITCH_GE_DK_H
#define NK_SWITCH_GE_DK_H

#include <stdint.h>

/* Why a state took the full fragment shader. */
enum {
    DK_FULL_FORMAT, DK_FULL_BLEND, DK_FULL_DEPTH_RANGE, DK_FULL_TEXFUNC, DK_FULL_LOD, DK_FULL_ALPHA_TEST,
    DK_FULL_REASONS
};

/* What the renderer did since the last ge_dk_stats_take. */
typedef struct {
    uint32_t draws, presents, target_switches, targets_created;
    uint32_t target_uploads, target_readbacks, snapshots;
    uint32_t tex_uploads, tex_new, tex_revalidated, tex_no_hash, tex_evictions;
    uint32_t unsupported_fb, present_outside_vram;
    uint32_t approx_write_mask, approx_color_test, approx_logic_op, approx_mip;
    uint32_t tnl_prims, tnl_tris, tex_chains;
    uint32_t frag_draws[3];                /* draws by shader: full, short, short with alpha test */
    uint32_t frag_full_why[DK_FULL_REASONS];
    uint64_t state_ns, tex_ns;    /* time in state rebuilds, and in texture lookups inside them */
    uint64_t fence_ns, acquire_ns; /* time waiting for an earlier frame, and for a screen image */
    uint64_t gpu_ns;              /* GPU time of the commands that finished in this period */
} DkGeStats;

/* Start deko3d on the window and register the GE hooks. Returns 0 on failure. */
int ge_dk_init(void *native_window);
void ge_dk_shutdown(void);
/* Show the framebuffer at fbaddr. Returns 0 when it is not in VRAM, for a CPU path to show it. */
int ge_dk_present(uint32_t fbaddr, int fmt, uint32_t stride);
void ge_dk_stats_take(DkGeStats *out);

#endif
