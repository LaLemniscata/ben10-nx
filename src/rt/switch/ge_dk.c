/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The GE rasterizer hooks on deko3d, for the Nintendo Switch. ge.c stays the arbiter of GE
 * semantics and hands over screen-space primitives; this file turns them into draws. The state
 * rules follow gpu_sdl3vk/ge_gpu.c (build_state), with one image per framebuffer in VRAM.
 *
 * Targets are drawn at a multiple of the PSP's resolution, and guest memory keeps the native size:
 * an upload stretches up and a readback shrinks down through a transfer image.
 *
 * What it does not model yet, each drawn as if the state were off: color test, logic ops, partial
 * write masks, dither under blending, and depth readback to guest memory. A count of every such
 * draw goes to the log.
 */
#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <deko3d.h>
#include <switch.h>

#include <SDL3/nk_switch_sdl.h>

#include "ge_dk.h"
#include "ge_shared.h"
#include "recomp.h"

#define FB_W 512
#define FB_H 272
#define SHOW_W 480
#define SHOW_H 272
/* The screen images hold the largest output, and a crop selects the current one. */
#define SWAP_W 1920
#define SWAP_H 1080
#define HANDHELD_W 1280
#define HANDHELD_H 720
#define SWAP_IMAGES 3
#define DEFAULT_RENDER_SCALE 2u
/* A 30 fps game has 33 ms for a frame. This much GPU time per frame, averaged over this many frames
 * this many times in a row, means the current scale costs frames: one step lower it goes. */
#define GPU_OVERLOAD_NS 31000000ull
#define GPU_WINDOW_FRAMES 60u
#define GPU_OVERLOAD_WINDOWS 2u

#define SLOT_COUNT 3
#define CMD_BYTES (8u * 1024u * 1024u)
#define VERT_CAPACITY (384u * 1024u)
#define TNL_VERT_CAPACITY (192u * 1024u)
#define TNL_IDX_CAPACITY (768u * 1024u)
#define TNL_PARAM_CAPACITY 2048u
#define STAGING_BYTES (24u * 1024u * 1024u)
#define READBACK_BYTES (FB_W * FB_H * 4u)
#define UBO_BYTES 0x100u
#define STAMP_BYTES 32u
#define CODE_BYTES (256u * 1024u)
#define MAX_IMAGE_IDS 1024
#define MAX_TEXTURES 768
#define TEX_MAP_SLOTS 2048u    /* a power of two, well above MAX_TEXTURES */
#define TEX_MAP_SHIFT 53       /* keeps the top 11 bits of a 64-bit hash */
#define TEX_EVICT_BATCH 64
#define MAX_TARGETS 16
#define MAX_DEPTHS 8
#define TARGET_FBA_MAIN_RAM 0x40000000u
#define TEX_SCRATCH_TEXELS (512u * 512u)
#define MAX_MIP_LEVELS 8u

/* Samplers: mip mode (none, nearest, linear), filter variant (min linear << 1 | mag linear),
 * clamp u, clamp v. */
#define SAMPLER_VARIANTS 16u
#define SAMPLER_COUNT (3u * SAMPLER_VARIANTS)
#define SAMPLER_PRESENT SAMPLER_COUNT
#define IMAGE_WHITE 0
#define IMAGE_SNAPSHOT 1
#define IMAGE_FIRST_FREE 2

/* Fragment flags: the same bits as shaders/ge_dk.frag. */
#define F_TEX (1 << 0)
#define F_RGBA (1 << 1)
#define F_DBL (1 << 2)
#define F_FOG (1 << 3)
#define F_PERSP (1 << 4)
#define F_CLEAR (1 << 5)
#define F_NEAREST (1 << 6)
#define F_SA2X (1 << 7)
#define F_SA2XI (1 << 8)
#define F_DITHER (1 << 9)
#define F_FMT_SHIFT 10
#define F_SHBLEND (1 << 12)
#define F_STENCIL (1 << 13)
#define F_LODFRAG (1 << 14)
#define F_SALPHA (1 << 15)

/* Level-of-detail settings in the top byte of cfg[1]: the same bits as shaders/ge_dk.frag. */
#define LOD_SLOPE 1u
#define LOD_LINEAR 2u
#define LOD_MAX_SHIFT 2
#define LOD_CONSTANT 32u
#define LOD_CFG_SHIFT 24

/* The fragment shader a draw runs: the full one, or the short one with or without its alpha test. */
enum { FRAG_FULL, FRAG_FAST, FRAG_FAST_ALPHA_TEST };

#define SFIX_NONE (-1)
#define SFIX_KEEP (-2)

extern const uint8_t ge_dk_vert_dksh[];
extern const uint32_t ge_dk_vert_dksh_size;
extern const uint8_t ge_dk_tnl_vert_dksh[];
extern const uint32_t ge_dk_tnl_vert_dksh_size;
extern const uint8_t ge_dk_frag_dksh[];
extern const uint32_t ge_dk_frag_dksh_size;
extern const uint8_t ge_dk_fast_frag_dksh[];
extern const uint32_t ge_dk_fast_frag_dksh_size;
extern const uint8_t ge_dk_fast_at_frag_dksh[];
extern const uint32_t ge_dk_fast_at_frag_dksh_size;
extern const uint8_t present_vert_dksh[];
extern const uint32_t present_vert_dksh_size;
extern const uint8_t present_frag_dksh[];
extern const uint32_t present_frag_dksh_size;

void ge_decode_tex_rgba(uint32_t *out);

typedef struct {
    float x, y, z, rw, u, v, fog;
    uint32_t rgba;
} GpuVert;

_Static_assert(sizeof(GeTnlParams) == 247u * 4u, "ge_dk_tnl.vert P_WORDS");
_Static_assert(sizeof(GeTnlVtx) == 72u, "ge_dk_tnl.vert vertex layout");

typedef struct {
    int32_t cfg[4];
    float texenv[4];
    float fogcol[4];
    float texsize[4];
    int32_t dith[4];
    int32_t bl[4];
    float fixa[4];
    float fixb[4];
} PushConstants;

/* Everything that decides one draw's commands. Compared whole, so unused bytes stay zero. */
typedef struct {
    PushConstants pc;
    float bconst[4];
    uint32_t sx, sy, sw, sh;
    uint32_t cmask;
    uint32_t image_id, sampler_id;
    uint8_t blend_on, srcf, dstf, eq, azero;
    uint8_t ztest, zwrite, zfunc;
    uint8_t tnl, cull;
    uint8_t reads_dst, frag;
    uint8_t pad[4];
} DrawState;

typedef struct {
    DkMemBlock mem;
    DkImage image;
    DkImageView view;
} GpuImage;

typedef struct {
    int used;
    uint32_t fba, base, stride, fmt;
    GpuImage img;
    uint32_t image_id;
    int gpu_valid;
    uint32_t render_gen, clean_gen;
    int cpu_dirty;
    uint64_t lru;
} Target;

typedef struct {
    int used;
    uint32_t zba;
    GpuImage img;
} DepthTarget;

typedef struct {
    uint64_t key, hash, full_hash;
    int w, h;
    uint32_t addr, bytes;
    uint32_t levels;     /* levels the image holds, level 0 included */
    uint32_t mip_addr[MAX_MIP_LEVELS - 1u], mip_bytes[MAX_MIP_LEVELS - 1u];
    GpuImage img;
    uint32_t image_id;
    uint64_t lru;
    int content_valid;
} TexEntry;

typedef struct {
    DkMemBlock cmd_mem;
    DkFence fence;
    int fence_armed;
} Slot;

static GeState *s_ge;
static int s_ready;
static DkDevice s_device;
static DkQueue s_queue;
static DkSwapchain s_swapchain;
static DkCmdBuf s_cmd;
static DkImage s_swap_images[SWAP_IMAGES];
static DkImageView s_swap_views[SWAP_IMAGES];
static DkMemBlock s_swap_mem[SWAP_IMAGES];
static Slot s_slots[SLOT_COUNT];
static int s_slot;

static DkMemBlock s_code_mem, s_desc_mem, s_ubo_mem, s_vert_mem[SLOT_COUNT], s_staging_mem[SLOT_COUNT];
static DkMemBlock s_readback_mem, s_stamp_mem;
static DkShader s_vs, s_tnl_vs, s_fs, s_fast_fs, s_fast_at_fs, s_present_vs, s_present_fs;
static DkMemBlock s_tnl_vert_mem[SLOT_COUNT], s_tnl_idx_mem[SLOT_COUNT], s_tnl_param_mem[SLOT_COUNT];
static uint32_t s_code_used;
static DkImageDescriptor *s_image_desc;
static uint8_t s_image_id_used[MAX_IMAGE_IDS];
static int s_desc_dirty;

static GpuVert *s_vert_cpu;
static uint32_t s_nverts, s_draw_first;
static GeTnlVtx *s_tnl_vert_cpu;
static uint32_t *s_tnl_idx_cpu;
static GeTnlParams *s_tnl_param_cpu;
static uint32_t s_tnl_nverts, s_tnl_nidx, s_tnl_nparams, s_tnl_draw_first;
static int s_draw_tnl;    /* the gathered primitives are GPU T&L ones */
static uint8_t *s_staging_cpu;
static uint32_t s_staging_used;
static uint32_t *s_tex_scratch;

static Target s_targets[MAX_TARGETS];
static DepthTarget s_depths[MAX_DEPTHS];
static Target *s_cur;
static DepthTarget *s_cur_depth;
static TexEntry s_tex[MAX_TEXTURES];
static int s_tex_n;
static int16_t s_tex_map[TEX_MAP_SLOTS];    /* entry index + 1, 0 when empty */
static uint64_t s_tick;
static GpuImage s_white, s_snapshot, s_transfer;
static uint32_t s_scale = 1u;
static int s_lod_scale_bias;   /* 1/16 levels: a finer pixel can show a finer level */
static uint64_t s_gpu_window_ns;
static uint32_t s_gpu_window_frames, s_gpu_overloads;
static int s_docked = -1;
static uint32_t s_tex_levels;  /* levels of the entry the last tex_get returned */
static Target *s_snap_src;
static uint32_t s_snap_gen;

static DrawState s_state, s_bound;
static int s_state_valid, s_bound_valid;
static int s_state_persp, s_state_sprite;
static uint32_t s_state_serial;

static DkGeStats s_stats;

#define RENDER_W (FB_W * s_scale)
#define RENDER_H (FB_H * s_scale)

/* ---- small helpers ------------------------------------------------------------------------ */

static inline int is_vram(uint32_t addr) { return (addr & 0x0F000000u) == 0x04000000u; }
/* A finer pixel can show a finer mip level: log2(scale) levels, in 1/16 levels. */
static int lod_scale_bias(uint32_t scale) { return scale >= 4u ? 32 : scale == 3u ? 25 : scale == 2u ? 16 : 0; }
static inline uint32_t vram_off(uint32_t addr) { return addr & 0x001FFFFFu; }
static float snap16(float v) { return floorf(v * 16.0f + 0.375f) * (1.0f / 16.0f); }

static uint32_t image_id_alloc(void) {
    for (uint32_t i = IMAGE_FIRST_FREE; i < MAX_IMAGE_IDS; i++) {
        if (!s_image_id_used[i]) {
            s_image_id_used[i] = 1;
            return i;
        }
    }
    return 0;
}

static void image_id_describe(uint32_t id, const DkImageView *view) {
    dkImageDescriptorInitialize(&s_image_desc[id], view, false, false);
    s_desc_dirty = 1;
}

static int gpu_image_create_levels(GpuImage *out, uint32_t w, uint32_t h, uint32_t levels, DkImageFormat fmt,
                                   uint32_t flags) {
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, s_device);
    lm.flags = flags;
    lm.format = fmt;
    lm.dimensions[0] = w;
    lm.dimensions[1] = h;
    lm.mipLevels = levels;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &lm);
    const uint32_t align = dkImageLayoutGetAlignment(&layout);
    const uint64_t size = dkImageLayoutGetSize(&layout);
    const uint32_t block = (uint32_t)((size + align + DK_MEMBLOCK_ALIGNMENT - 1u) & ~(uint64_t)(DK_MEMBLOCK_ALIGNMENT - 1u));
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, s_device, block);
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    out->mem = dkMemBlockCreate(&mm);
    if (!out->mem) return 0;
    dkImageInitialize(&out->image, &layout, out->mem, 0);
    dkImageViewDefaults(&out->view, &out->image);
    return 1;
}

static int gpu_image_create(GpuImage *out, uint32_t w, uint32_t h, DkImageFormat fmt, uint32_t flags) {
    return gpu_image_create_levels(out, w, h, 1u, fmt, flags);
}

static void gpu_image_destroy(GpuImage *img) {
    if (img->mem) dkMemBlockDestroy(img->mem);
    img->mem = NULL;
}

static DkMemBlock cpu_block(uint32_t bytes, uint32_t extra_flags) {
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, s_device, (bytes + DK_MEMBLOCK_ALIGNMENT - 1u) & ~(DK_MEMBLOCK_ALIGNMENT - 1u));
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | extra_flags;
    return dkMemBlockCreate(&mm);
}

static void shader_load(DkShader *shader, const uint8_t *code, uint32_t size) {
    const uint32_t offset = s_code_used;
    memcpy((uint8_t *)dkMemBlockGetCpuAddr(s_code_mem) + offset, code, size);
    s_code_used = (offset + size + DK_SHADER_CODE_ALIGNMENT - 1u) & ~(DK_SHADER_CODE_ALIGNMENT - 1u);
    DkShaderMaker sm;
    dkShaderMakerDefaults(&sm, s_code_mem, offset);
    dkShaderInitialize(shader, &sm);
}

/* ---- command recording -------------------------------------------------------------------- */

static void global_state_bind(void);

/* Two GPU timestamps per slot, around its commands. A report is a value and a time. */
static DkGpuAddr stamp_addr(int slot, int end) {
    return dkMemBlockGetGpuAddr(s_stamp_mem) + (uint32_t)slot * STAMP_BYTES + (end ? 16u : 0u);
}

static void stamp_collect(int slot) {
    const volatile uint64_t *stamps =
        (const volatile uint64_t *)((const uint8_t *)dkMemBlockGetCpuAddr(s_stamp_mem) + (uint32_t)slot * STAMP_BYTES);
    if (stamps[3] > stamps[1]) {
        const uint64_t ns = dkTimestampToNs(stamps[3] - stamps[1]);
        s_stats.gpu_ns += ns;
        s_gpu_window_ns += ns;
    }
}

static void slot_begin(void) {
    Slot *slot = &s_slots[s_slot];
    if (slot->fence_armed) {
        const uint64_t wait_started = SDL_GetTicksNS();
        dkFenceWait(&slot->fence, -1);
        s_stats.fence_ns += SDL_GetTicksNS() - wait_started;
        slot->fence_armed = 0;
        stamp_collect(s_slot);
    }
    dkCmdBufClear(s_cmd);
    dkCmdBufAddMemory(s_cmd, slot->cmd_mem, 0, CMD_BYTES);
    dkCmdBufReportCounter(s_cmd, DkCounter_TimestampPipelineTop, stamp_addr(s_slot, 0));
    s_vert_cpu = dkMemBlockGetCpuAddr(s_vert_mem[s_slot]);
    s_staging_cpu = dkMemBlockGetCpuAddr(s_staging_mem[s_slot]);
    s_nverts = s_draw_first = 0;
    s_tnl_vert_cpu = dkMemBlockGetCpuAddr(s_tnl_vert_mem[s_slot]);
    s_tnl_idx_cpu = dkMemBlockGetCpuAddr(s_tnl_idx_mem[s_slot]);
    s_tnl_param_cpu = dkMemBlockGetCpuAddr(s_tnl_param_mem[s_slot]);
    s_tnl_nverts = s_tnl_nidx = s_tnl_nparams = s_tnl_draw_first = 0;
    s_staging_used = 0;
    s_bound_valid = 0;
    global_state_bind();
}

static void bind_render_targets(void) {
    if (!s_cur) return;
    const DkImageView *color = &s_cur->img.view;
    dkCmdBufBindRenderTargets(s_cmd, &color, 1, s_cur_depth ? &s_cur_depth->img.view : NULL);
    DkViewport viewport = {0.0f, 0.0f, (float)RENDER_W, (float)RENDER_H, 0.0f, 1.0f};
    dkCmdBufSetViewports(s_cmd, 0, &viewport, 1);
}

static void global_state_bind(void) {
    const DkShader *shaders[2] = {&s_vs, &s_fs};
    dkCmdBufBindShaders(s_cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
    dkCmdBufBindImageDescriptorSet(s_cmd, dkMemBlockGetGpuAddr(s_desc_mem), MAX_IMAGE_IDS);
    dkCmdBufBindSamplerDescriptorSet(s_cmd, dkMemBlockGetGpuAddr(s_desc_mem) + MAX_IMAGE_IDS * sizeof(DkImageDescriptor),
                                     SAMPLER_COUNT + 1);
    dkCmdBufBarrier(s_cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
    s_desc_dirty = 0;
    dkCmdBufBindIdxBuffer(s_cmd, DkIdxFormat_Uint32, dkMemBlockGetGpuAddr(s_tnl_idx_mem[s_slot]));
    dkCmdBufBindStorageBuffer(s_cmd, DkStage_Vertex, 0, dkMemBlockGetGpuAddr(s_tnl_param_mem[s_slot]),
                              TNL_PARAM_CAPACITY * sizeof(GeTnlParams));
    dkCmdBufBindUniformBuffer(s_cmd, DkStage_Fragment, 0, dkMemBlockGetGpuAddr(s_ubo_mem), UBO_BYTES);
    dkCmdBufBindTexture(s_cmd, DkStage_Fragment, 1, dkMakeTextureHandle(IMAGE_SNAPSHOT, 0));
    bind_render_targets();
}

static void slot_submit(void) {
    Slot *slot = &s_slots[s_slot];
    dkCmdBufReportCounter(s_cmd, DkCounter_Timestamp, stamp_addr(s_slot, 1));
    dkCmdBufSignalFence(s_cmd, &slot->fence, true);
    slot->fence_armed = 1;
    dkQueueSubmitCommands(s_queue, dkCmdBufFinishList(s_cmd));
    dkQueueFlush(s_queue);
    s_slot = (s_slot + 1) % SLOT_COUNT;
    slot_begin();
}

static void snapshot_refresh(Target *src) {
    if (s_snap_src == src && s_snap_gen == src->render_gen) return;
    DkImageRect rect = {0, 0, 0, RENDER_W, RENDER_H, 1};
    dkCmdBufBarrier(s_cmd, DkBarrier_Fragments, 0);
    dkCmdBufCopyImage(s_cmd, &src->img.view, &rect, &s_snapshot.view, &rect, 0);
    dkCmdBufBarrier(s_cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    s_snap_src = src;
    s_snap_gen = src->render_gen;
    s_stats.snapshots++;
}

static void shaders_bind(int tnl, uint32_t frag) {
    const DkShader *fragment = frag == FRAG_FAST ? &s_fast_fs : frag == FRAG_FAST_ALPHA_TEST ? &s_fast_at_fs : &s_fs;
    const DkShader *shaders[2] = {tnl ? &s_tnl_vs : &s_vs, fragment};
    dkCmdBufBindShaders(s_cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
}

static void vertex_stage_bind(int tnl) {
    static const DkVtxAttribState attribs[4] = {
        {0, 0, offsetof(GpuVert, x), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(GpuVert, u), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(GpuVert, fog), DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(GpuVert, rgba), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
    };
    static const DkVtxBufferState buffers[1] = {{sizeof(GpuVert), 0}};
    static const DkVtxAttribState tnl_attribs[7] = {
        {0, 0, offsetof(GeTnlVtx, x), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(GeTnlVtx, nx), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(GeTnlVtx, u), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(GeTnlVtx, rgba), DkVtxAttribSize_4x8, DkVtxAttribType_Uint, 0},
        {0, 0, offsetof(GeTnlVtx, w), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(GeTnlVtx, w) + 16u, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(GeTnlVtx, param), DkVtxAttribSize_1x32, DkVtxAttribType_Uint, 0},
    };
    static const DkVtxBufferState tnl_buffers[1] = {{sizeof(GeTnlVtx), 0}};
    if (tnl) {
        dkCmdBufBindVtxAttribState(s_cmd, tnl_attribs, 7);
        dkCmdBufBindVtxBufferState(s_cmd, tnl_buffers, 1);
        dkCmdBufBindVtxBuffer(s_cmd, 0, dkMemBlockGetGpuAddr(s_tnl_vert_mem[s_slot]),
                              TNL_VERT_CAPACITY * sizeof(GeTnlVtx));
    } else {
        dkCmdBufBindVtxAttribState(s_cmd, attribs, 4);
        dkCmdBufBindVtxBufferState(s_cmd, buffers, 1);
        dkCmdBufBindVtxBuffer(s_cmd, 0, dkMemBlockGetGpuAddr(s_vert_mem[s_slot]), VERT_CAPACITY * sizeof(GpuVert));
    }
}

static int draw_pending(void) {
    return s_nverts != s_draw_first || s_tnl_nidx != s_tnl_draw_first;
}

/* Record the primitives gathered under s_state. */
static void draw_flush(void) {
    const uint32_t count = s_draw_tnl ? s_tnl_nidx - s_tnl_draw_first : s_nverts - s_draw_first;
    if (!count || !s_state_valid || !s_cur) {
        s_draw_first = s_nverts;
        s_tnl_draw_first = s_tnl_nidx;
        return;
    }
    s_state.tnl = (uint8_t)s_draw_tnl;
    const DrawState *st = &s_state;
    if (st->reads_dst) snapshot_refresh(s_cur);
    if (s_desc_dirty) {
        dkCmdBufBarrier(s_cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
        s_desc_dirty = 0;
    }
    const DrawState *old = s_bound_valid ? &s_bound : NULL;
    if (!old || old->tnl != st->tnl || old->frag != st->frag) shaders_bind(st->tnl, st->frag);
    if (!old || old->tnl != st->tnl) vertex_stage_bind(st->tnl);
    if (!old || memcmp(&old->pc, &st->pc, sizeof(st->pc)) != 0)
        dkCmdBufPushConstants(s_cmd, dkMemBlockGetGpuAddr(s_ubo_mem), UBO_BYTES, 0, sizeof(st->pc), &st->pc);
    if (!old || old->image_id != st->image_id || old->sampler_id != st->sampler_id)
        dkCmdBufBindTexture(s_cmd, DkStage_Fragment, 0, dkMakeTextureHandle(st->image_id, st->sampler_id));
    if (!old || old->sx != st->sx || old->sy != st->sy || old->sw != st->sw || old->sh != st->sh) {
        DkScissor scissor = {st->sx * s_scale, st->sy * s_scale, st->sw * s_scale, st->sh * s_scale};
        dkCmdBufSetScissors(s_cmd, 0, &scissor, 1);
    }
    /* The CPU route culls in hook_tri. GPU T&L positions exist only on the GPU, and ge.c has
     * ordered each triangle so that a front face is clockwise on screen. */
    const int cull_now = st->tnl && st->cull;
    if (!old || (old->tnl && old->cull) != cull_now) {
        DkRasterizerState raster;
        dkRasterizerStateDefaults(&raster);
        raster.cullMode = cull_now ? DkFace_Back : DkFace_None;
        raster.frontFace = DkFrontFace_CW;
        dkCmdBufBindRasterizerState(s_cmd, &raster);
    }
    if (!old || old->blend_on != st->blend_on) {
        DkColorState color;
        dkColorStateDefaults(&color);
        dkColorStateSetBlendEnable(&color, 0, st->blend_on != 0);
        dkCmdBufBindColorState(s_cmd, &color);
    }
    if (!old || old->cmask != st->cmask) {
        DkColorWriteState write;
        dkColorWriteStateDefaults(&write);
        dkColorWriteStateSetMask(&write, 0, st->cmask);
        dkCmdBufBindColorWriteState(s_cmd, &write);
    }
    if (st->blend_on && (!old || old->srcf != st->srcf || old->dstf != st->dstf || old->eq != st->eq ||
                         old->azero != st->azero || !old->blend_on)) {
        DkBlendState blend;
        dkBlendStateDefaults(&blend);
        dkBlendStateSetOps(&blend, (DkBlendOp)st->eq, DkBlendOp_Add);
        const DkBlendFactor alpha = st->azero ? DkBlendFactor_Zero : DkBlendFactor_One;
        dkBlendStateSetFactors(&blend, (DkBlendFactor)st->srcf, (DkBlendFactor)st->dstf, alpha, alpha);
        dkCmdBufBindBlendState(s_cmd, 0, &blend);
    }
    if (st->blend_on && (!old || memcmp(old->bconst, st->bconst, sizeof(st->bconst)) != 0))
        dkCmdBufSetBlendConst(s_cmd, st->bconst[0], st->bconst[1], st->bconst[2], st->bconst[3]);
    if (!old || old->ztest != st->ztest || old->zwrite != st->zwrite || old->zfunc != st->zfunc) {
        DkDepthStencilState depth;
        dkDepthStencilStateDefaults(&depth);
        depth.depthTestEnable = st->ztest;
        depth.depthWriteEnable = st->zwrite;
        depth.depthCompareOp = (DkCompareOp)st->zfunc;
        dkCmdBufBindDepthStencilState(s_cmd, &depth);
    }
    if (st->tnl) dkCmdBufDrawIndexed(s_cmd, DkPrimitive_Triangles, count, 1, s_tnl_draw_first, 0, 0);
    else dkCmdBufDraw(s_cmd, DkPrimitive_Triangles, count, 1, s_draw_first, 0);
    s_bound = *st;
    s_bound_valid = 1;
    s_draw_first = s_nverts;
    s_tnl_draw_first = s_tnl_nidx;
    s_cur->render_gen++;
    s_stats.draws++;
    s_stats.frag_draws[st->frag]++;
}

/* ---- guest memory <-> images --------------------------------------------------------------- */

/* Copy the guest framebuffer at `fba` into a target, converted to RGBA8. */
static void target_upload(Target *t) {
    draw_flush();
    const uint32_t bytes = FB_W * FB_H * 4u;
    if (s_staging_used + bytes > STAGING_BYTES) slot_submit();
    uint32_t *out = (uint32_t *)(s_staging_cpu + s_staging_used);
    const uint32_t base = t->base;
    const uint32_t stride = t->stride ? t->stride : FB_W;
    for (uint32_t y = 0; y < FB_H; y++) {
        uint32_t *row = out + y * FB_W;
        if (t->fmt == 3) {
            const uint32_t *src = (const uint32_t *)SR_HOST(base + y * stride * 4u);
            memcpy(row, src, FB_W * 4u);
            continue;
        }
        const uint16_t *src = (const uint16_t *)SR_HOST(base + y * stride * 2u);
        for (uint32_t x = 0; x < FB_W; x++) {
            const uint32_t p = src[x];
            uint32_t r, g, b, a;
            if (t->fmt == 1) {
                r = (p & 0x1fu) << 3; g = ((p >> 5) & 0x1fu) << 3; b = ((p >> 10) & 0x1fu) << 3;
                a = (p & 0x8000u) ? 255u : 0u;
            } else if (t->fmt == 2) {
                r = (p & 0xfu) << 4; g = ((p >> 4) & 0xfu) << 4; b = ((p >> 8) & 0xfu) << 4;
                a = ((p >> 12) & 0xfu) << 4;
            } else {
                r = (p & 0x1fu) << 3; g = ((p >> 5) & 0x3fu) << 2; b = ((p >> 11) & 0x1fu) << 3;
                a = 255u;
            }
            row[x] = r | (g << 8) | (b << 16) | (a << 24);
        }
    }
    DkCopyBuf src = {dkMemBlockGetGpuAddr(s_staging_mem[s_slot]) + s_staging_used, FB_W * 4u, 0};
    DkImageRect rect = {0, 0, 0, FB_W, FB_H, 1};
    s_staging_used += bytes;
    if (s_scale == 1u) {
        dkCmdBufCopyBufferToImage(s_cmd, &src, &t->img.view, &rect, 0);
    } else {
        const DkImageRect scaled = {0, 0, 0, RENDER_W, RENDER_H, 1};
        dkCmdBufCopyBufferToImage(s_cmd, &src, &s_transfer.view, &rect, 0);
        dkCmdBufBarrier(s_cmd, DkBarrier_Full, DkInvalidateFlags_Image);
        dkCmdBufBlitImage(s_cmd, &s_transfer.view, &rect, &t->img.view, &scaled, DkBlitFlag_FilterLinear, 0);
    }
    dkCmdBufBarrier(s_cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    t->gpu_valid = 1;
    t->cpu_dirty = 0;
    t->render_gen++;
    t->clean_gen = t->render_gen;
    s_stats.target_uploads++;
}

/* Write a target's image back to guest memory, in the target's own format. Stalls for the GPU. */
static void target_readback(Target *t) {
    if (!t->gpu_valid || t->clean_gen == t->render_gen) return;
    draw_flush();
    DkImageRect rect = {0, 0, 0, FB_W, FB_H, 1};
    DkCopyBuf dst = {dkMemBlockGetGpuAddr(s_readback_mem), FB_W * 4u, 0};
    dkCmdBufBarrier(s_cmd, DkBarrier_Fragments, 0);
    if (s_scale == 1u) {
        dkCmdBufCopyImageToBuffer(s_cmd, &t->img.view, &rect, &dst, 0);
    } else {
        const DkImageRect scaled = {0, 0, 0, RENDER_W, RENDER_H, 1};
        dkCmdBufBlitImage(s_cmd, &t->img.view, &scaled, &s_transfer.view, &rect, DkBlitFlag_FilterNearest, 0);
        dkCmdBufBarrier(s_cmd, DkBarrier_Full, DkInvalidateFlags_Image);
        dkCmdBufCopyImageToBuffer(s_cmd, &s_transfer.view, &rect, &dst, 0);
    }
    slot_submit();
    dkQueueWaitIdle(s_queue);
    dkMemBlockFlushCpuCache(s_readback_mem, 0, READBACK_BYTES);
    const uint32_t *in = dkMemBlockGetCpuAddr(s_readback_mem);
    const uint32_t base = t->base;
    const uint32_t stride = t->stride ? t->stride : FB_W;
    for (uint32_t y = 0; y < FB_H; y++) {
        const uint32_t *row = in + y * FB_W;
        if (t->fmt == 3) {
            memcpy(SR_HOST(base + y * stride * 4u), row, stride * 4u < FB_W * 4u ? stride * 4u : FB_W * 4u);
            continue;
        }
        uint16_t *out = (uint16_t *)SR_HOST(base + y * stride * 2u);
        const uint32_t width = stride < FB_W ? stride : FB_W;
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t p = row[x];
            const uint32_t r = p & 0xffu, g = (p >> 8) & 0xffu, b = (p >> 16) & 0xffu, a = p >> 24;
            if (t->fmt == 1) out[x] = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (a >= 128u ? 0x8000u : 0u));
            else if (t->fmt == 2) out[x] = (uint16_t)((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12));
            else out[x] = (uint16_t)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
        }
    }
    t->clean_gen = t->render_gen;
    s_stats.target_readbacks++;
}

static uint32_t target_bytes(const Target *t) {
    return (t->stride ? t->stride : FB_W) * FB_H * (t->fmt == 3 ? 4u : 2u);
}

static int target_overlaps(const Target *t, uint32_t off0, uint32_t off1) {
    return t->used && off0 < t->fba + target_bytes(t) && t->fba < off1;
}

static Target *target_acquire(uint32_t fba, uint32_t stride, uint32_t fmt) {
    Target *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &s_targets[i];
        if (!t->used) {
            if (!free_slot) free_slot = t;
            continue;
        }
        if (t->fba == fba && t->fmt == fmt && t->stride == stride) {
            t->lru = ++s_tick;
            return t;
        }
        if (t != s_cur && (!oldest || t->lru < oldest->lru)) oldest = t;
    }
    Target *t = free_slot;
    if (!t) {
        if (!oldest) return NULL;
        target_readback(oldest);
        draw_flush();
        slot_submit();
        dkQueueWaitIdle(s_queue);
        gpu_image_destroy(&oldest->img);
        s_image_id_used[oldest->image_id] = 0;
        if (s_snap_src == oldest) s_snap_src = NULL;
        oldest->used = 0;
        t = oldest;
    }
    memset(t, 0, sizeof(*t));
    if (!gpu_image_create(&t->img, RENDER_W, RENDER_H, DkImageFormat_RGBA8_Unorm,
                          DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine))
        return NULL;
    t->image_id = image_id_alloc();
    if (!t->image_id) {
        gpu_image_destroy(&t->img);
        return NULL;
    }
    image_id_describe(t->image_id, &t->img.view);
    t->used = 1;
    t->fba = fba;
    t->base = 0x04000000u + fba;
    t->stride = stride;
    t->fmt = fmt;
    t->lru = ++s_tick;
    s_stats.targets_created++;
    return t;
}

static DepthTarget *depth_acquire(uint32_t zba) {
    for (int i = 0; i < MAX_DEPTHS; i++) {
        if (s_depths[i].used && s_depths[i].zba == zba) return &s_depths[i];
    }
    for (int i = 0; i < MAX_DEPTHS; i++) {
        DepthTarget *d = &s_depths[i];
        if (d->used) continue;
        if (!gpu_image_create(&d->img, RENDER_W, RENDER_H, DkImageFormat_Z16,
                              DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression))
            return NULL;
        d->used = 1;
        d->zba = zba;
        return d;
    }
    return &s_depths[0];
}

/* Make the GE's current framebuffer the render target. */
static int target_begin(void) {
    const uint32_t stride = s_ge->fbw ? s_ge->fbw : FB_W;
    if (!is_vram(s_ge->fbp) || stride > FB_W) {
        s_stats.unsupported_fb++;
        return 0;
    }
    const uint32_t fba = vram_off(s_ge->fbp), fmt = s_ge->fbfmt & 3u;
    const uint32_t zba = vram_off(s_ge->zbp);
    if (s_cur && s_cur->fba == fba && s_cur->fmt == fmt && s_cur->stride == stride && s_cur_depth &&
        s_cur_depth->zba == zba && !s_cur->cpu_dirty)
        return 1;
    draw_flush();
    Target *t = target_acquire(fba, stride, fmt);
    DepthTarget *d = depth_acquire(zba);
    if (!t || !d) return 0;
    if (t->cpu_dirty || !t->gpu_valid) target_upload(t);
    const int new_depth = s_cur_depth != d;
    dkCmdBufBarrier(s_cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
    s_cur = t;
    s_cur_depth = d;
    bind_render_targets();
    if (new_depth) s_bound_valid = 0;
    s_stats.target_switches++;
    return 1;
}

/* ---- textures ------------------------------------------------------------------------------ */

static uint64_t fnv64_words(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = data;
    for (size_t i = 0; i + 8u <= n; i += 8u) {
        uint64_t w;
        memcpy(&w, p + i, 8u);
        h ^= w;
        h *= 1099511628211ull;
    }
    return h;
}

static uint32_t tex_span_bytes(uint32_t stride, uint32_t h) {
    uint32_t bits;
    switch (s_ge->tex_fmt) {
        case 3: case 7: bits = 32; break;
        case 4: case 8: bits = 4; break;
        case 5: case 9: case 10: bits = 8; break;
        default: bits = 16; break;
    }
    return (uint32_t)(((uint64_t)stride * h * bits) / 8u);
}

static uint32_t log2_pow2(uint32_t v) {
    uint32_t l = 0;
    while (v > 1u) { v >>= 1; l++; }
    return l;
}

/* The levels one image can hold for the current texture. An image halves every level, while the
 * GE takes each level's size from its own TEXSIZE, so a list that does not halve keeps level 0. */
static uint32_t tex_chain_levels(void) {
    const GeState *g = s_ge;
    if (!ge_tex_mip_active(g)) return 1u;
    const uint32_t wl = log2_pow2((uint32_t)g->tex_w), hl = log2_pow2((uint32_t)g->tex_h);
    if (g->tex_max_level > (wl > hl ? wl : hl) || g->tex_max_level + 1u > MAX_MIP_LEVELS) {
        s_stats.approx_mip++;
        return 1u;
    }
    for (uint32_t level = 1; level <= g->tex_max_level; level++) {
        if (g->tex_mip_wlog2[level - 1u] != (wl > level ? wl - level : 0u) ||
            g->tex_mip_hlog2[level - 1u] != (hl > level ? hl - level : 0u)) {
            s_stats.approx_mip++;
            return 1u;
        }
    }
    return g->tex_max_level + 1u;
}

/* Where each level's source bytes are. */
static void tex_chain_spans(uint32_t levels, uint32_t *addrs, uint32_t *sizes) {
    const GeState *g = s_ge;
    const uint32_t stride = g->tex_bufw ? g->tex_bufw : (uint32_t)g->tex_w;
    addrs[0] = g->tex_addr;
    sizes[0] = tex_span_bytes(stride, (uint32_t)g->tex_h);
    for (uint32_t level = 1; level < levels; level++) {
        const uint32_t i = level - 1u;
        const uint32_t w = 1u << g->tex_mip_wlog2[i];
        addrs[level] = g->tex_mip_addr[i];
        sizes[level] = tex_span_bytes(g->tex_mip_bufw[i] ? g->tex_mip_bufw[i] : w, 1u << g->tex_mip_hlog2[i]);
    }
}

/* Sampled content hash, as the Vulkan texture cache does it: every texel would cost too much. */
static uint64_t tex_hash(uint32_t addr, uint32_t bytes) {
    if (!bytes || !sr_guest_span_readable_current(addr, bytes)) return 0;
    if (is_vram(addr) && (uint64_t)vram_off(addr) + bytes > 0x200000u) return 0;
    const uint8_t *p = (const uint8_t *)SR_HOST(addr);
    uint64_t size = bytes;
    uint64_t h = fnv64_words(1469598103934665603ull, &size, sizeof(size));
    uint64_t step = size / 64u;
    if (step < 16u) step = 16u;
    for (uint64_t off = 0; off + 16u <= size; off += step) h = fnv64_words(h, p + off, 16u);
    if (size >= 16u) h = fnv64_words(h, p + size - 16u, 16u);
    return h;
}

/* A hash of the whole chain, sampled or of every byte. 0 when a level cannot be read. */
static uint64_t tex_chain_hash(const uint32_t *addrs, const uint32_t *sizes, uint32_t levels, int full) {
    uint64_t h = 0;
    for (uint32_t level = 0; level < levels; level++) {
        const uint64_t part = full ? fnv64_words(1469598103934665603ull, SR_HOST(addrs[level]), sizes[level])
                                   : tex_hash(addrs[level], sizes[level]);
        if (!part) return 0;
        h = level ? (h ^ part) * 1099511628211ull : part;
    }
    return h;
}

static int tex_upload(TexEntry *e) {
    draw_flush();
    for (uint32_t level = 0; level < e->levels; level++) {
        uint32_t w = (uint32_t)e->w >> level, h = (uint32_t)e->h >> level;
        if (!w) w = 1u;
        if (!h) h = 1u;
        const uint32_t bytes = w * h * 4u;
        if (s_staging_used + bytes > STAGING_BYTES) slot_submit();
        if (level == 0u) ge_decode_tex_rgba(s_tex_scratch);
        else ge_decode_tex_level_rgba(level, s_tex_scratch);
        memcpy(s_staging_cpu + s_staging_used, s_tex_scratch, bytes);
        DkCopyBuf src = {dkMemBlockGetGpuAddr(s_staging_mem[s_slot]) + s_staging_used, w * 4u, 0};
        DkImageRect rect = {0, 0, 0, w, h, 1};
        DkImageView view = e->img.view;
        view.mipLevelOffset = (uint8_t)level;
        view.mipLevelCount = 1;
        s_staging_used += bytes;
        dkCmdBufCopyBufferToImage(s_cmd, &src, &view, &rect, 0);
    }
    dkCmdBufBarrier(s_cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    s_stats.tex_uploads++;
    return 1;
}

/* The cache is searched many times a frame, so entries are found by key and hash in an open-addressed
 * map. An entry without a hash can never be found, so it stays out of the map. */
static uint32_t tex_map_start(uint64_t key, uint64_t hash) {
    return (uint32_t)(((key ^ hash) * 0x9E3779B97F4A7C15ull) >> TEX_MAP_SHIFT);
}

static TexEntry *tex_find(uint64_t key, uint64_t hash) {
    for (uint32_t slot = tex_map_start(key, hash);; slot = (slot + 1u) & (TEX_MAP_SLOTS - 1u)) {
        const int stored = s_tex_map[slot];
        if (!stored) return NULL;
        TexEntry *e = &s_tex[stored - 1];
        if (e->key == key && e->hash == hash) return e;
    }
}

static void tex_map_insert(int index) {
    const TexEntry *e = &s_tex[index];
    uint32_t slot = tex_map_start(e->key, e->hash);
    while (s_tex_map[slot]) slot = (slot + 1u) & (TEX_MAP_SLOTS - 1u);
    s_tex_map[slot] = (int16_t)(index + 1);
}

/* Free the least recently used entries at once: freeing waits for the GPU. */
static void tex_evict_batch(void) {
    draw_flush();
    slot_submit();
    dkQueueWaitIdle(s_queue);
    for (int n = 0; n < TEX_EVICT_BATCH && s_tex_n > 0; n++) {
        int oldest = 0;
        for (int i = 1; i < s_tex_n; i++) {
            if (s_tex[i].lru < s_tex[oldest].lru) oldest = i;
        }
        gpu_image_destroy(&s_tex[oldest].img);
        s_image_id_used[s_tex[oldest].image_id] = 0;
        s_tex[oldest] = s_tex[--s_tex_n];
        s_stats.tex_evictions++;
    }
    memset(s_tex_map, 0, sizeof(s_tex_map));
    for (int i = 0; i < s_tex_n; i++) {
        if (s_tex[i].hash) tex_map_insert(i);
    }
    s_state_valid = 0;
}

/* The cache is keyed by content as well as by state: a game that streams textures through one
 * VRAM area brings the same few images back, and each keeps its own entry. A CPU write over the
 * source only marks an entry unverified, and a full hash decides whether the bytes really moved. */
static uint32_t tex_get(void) {
    const int w = s_ge->tex_w, h = s_ge->tex_h;
    s_tex_levels = 1u;
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return IMAGE_WHITE;
    const uint32_t levels = tex_chain_levels();
    uint64_t key = (uint64_t)s_ge->tex_addr | ((uint64_t)s_ge->tex_fmt << 32) | ((uint64_t)(uint32_t)w << 36) |
                   ((uint64_t)(uint32_t)h << 46) | ((uint64_t)s_ge->tex_swizzle << 56);
    key ^= (uint64_t)s_ge->tex_bufw << 16;
    if (s_ge->tex_fmt >= 4 && s_ge->tex_fmt <= 7) {
        const uint64_t clut = fnv64_words(1469598103934665603ull, s_ge->clutram, sizeof(s_ge->clutram));
        key ^= (clut ^ s_ge->clut_fmt) | 1u;
    }
    uint32_t addrs[MAX_MIP_LEVELS], sizes[MAX_MIP_LEVELS];
    tex_chain_spans(levels, addrs, sizes);
    if (levels > 1u) {
        uint64_t chain[2 * MAX_MIP_LEVELS];
        memset(chain, 0, sizeof(chain));
        for (uint32_t level = 0; level < levels; level++) {
            chain[2u * level] = addrs[level];
            chain[2u * level + 1u] = sizes[level];
        }
        key ^= fnv64_words(1469598103934665603ull, chain, sizeof(chain)) | 2u;
    }
    const uint64_t hash = tex_chain_hash(addrs, sizes, levels, 0);
    if (!hash) s_stats.tex_no_hash++;
    TexEntry *known = hash ? tex_find(key, hash) : NULL;
    if (known) {
        known->lru = ++s_tick;
        s_tex_levels = known->levels;
        if (known->content_valid) return known->image_id;
        const uint64_t full = tex_chain_hash(addrs, sizes, levels, 1);
        if (full == known->full_hash) {
            known->content_valid = 1;
            s_stats.tex_revalidated++;
            return known->image_id;
        }
        tex_upload(known);
        known->full_hash = full;
        known->content_valid = 1;
        return known->image_id;
    }
    if (s_tex_n >= MAX_TEXTURES) tex_evict_batch();
    TexEntry *e = &s_tex[s_tex_n];
    memset(e, 0, sizeof(*e));
    if (!gpu_image_create_levels(&e->img, (uint32_t)w, (uint32_t)h, levels, DkImageFormat_RGBA8_Unorm,
                                 DkImageFlags_Usage2DEngine))
        return IMAGE_WHITE;
    e->image_id = image_id_alloc();
    if (!e->image_id) {
        gpu_image_destroy(&e->img);
        return IMAGE_WHITE;
    }
    image_id_describe(e->image_id, &e->img.view);
    s_tex_n++;
    e->key = key;
    e->w = w;
    e->h = h;
    e->addr = addrs[0];
    e->bytes = sizes[0];
    e->levels = levels;
    for (uint32_t level = 1; level < levels; level++) {
        e->mip_addr[level - 1u] = addrs[level];
        e->mip_bytes[level - 1u] = sizes[level];
    }
    e->lru = ++s_tick;
    tex_upload(e);
    e->hash = hash;
    if (hash) tex_map_insert((int)(e - s_tex));
    e->full_hash = hash ? tex_chain_hash(addrs, sizes, levels, 1) : 0;
    e->content_valid = hash != 0;
    s_stats.tex_new++;
    if (levels > 1u) s_stats.tex_chains++;
    s_tex_levels = levels;
    return e->image_id;
}

/* ---- state --------------------------------------------------------------------------------- */

static DkBlendFactor map_factor(uint32_t f, int src_side, uint32_t fixed, int *need_const, int *premul) {
    switch (f & 0xfu) {
        case 0: return src_side ? DkBlendFactor_DstColor : DkBlendFactor_SrcColor;
        case 1: return src_side ? DkBlendFactor_InvDstColor : DkBlendFactor_InvSrcColor;
        case 2: return DkBlendFactor_SrcAlpha;
        case 3: return DkBlendFactor_InvSrcAlpha;
        case 4: return DkBlendFactor_DstAlpha;
        case 5: return DkBlendFactor_InvDstAlpha;
        case 6:
            if (src_side) { *premul = F_SA2X; return DkBlendFactor_One; }
            return DkBlendFactor_SrcAlpha;
        case 7:
            if (src_side) { *premul = F_SA2XI; return DkBlendFactor_One; }
            return DkBlendFactor_InvSrcAlpha;
        case 8: return DkBlendFactor_DstAlpha;
        case 9: return DkBlendFactor_InvDstAlpha;
        default:
            if ((fixed & 0xffffffu) == 0u) return DkBlendFactor_Zero;
            if ((fixed & 0xffffffu) == 0xffffffu) return DkBlendFactor_One;
            *need_const = 1;
            return DkBlendFactor_ConstColor;
    }
}

static int blend_dual_fix(uint32_t sfp, uint32_t dfp, uint32_t fixa, uint32_t fixb) {
    return sfp >= 10 && dfp >= 10 && ((fixa ^ fixb) & 0xffffffu) != 0;
}

static int blend_needs_shader(uint32_t sfp, uint32_t dfp, uint32_t eq, uint32_t fixa, uint32_t fixb) {
    return eq >= 5 || (sfp >= 6 && sfp <= 9) || (dfp >= 6 && dfp <= 9) || blend_dual_fix(sfp, dfp, fixa, fixb);
}

static int stencil_fixed_alpha(const GeState *g) {
    if ((g->fbfmt & 3u) != 3u || (g->stencil_test & 7u) != 1u) return SFIX_NONE;
    const int blended = g->blend_enable != 0;
    if (blended && blend_needs_shader(g->blend_mode & 0xfu, (g->blend_mode >> 4) & 0xfu, (g->blend_mode >> 8) & 7u,
                                      g->blend_fixa, g->blend_fixb))
        return SFIX_NONE;
    const uint32_t amask = (uint32_t)g->mask_alpha & 0xffu, zpass = (g->stencil_op >> 16) & 7u;
    if (zpass == 0u || amask == 0xffu) return SFIX_KEEP;
    if (amask != 0u) return SFIX_NONE;
    const int value = zpass == 1u ? 0 : zpass == 2u ? (int)((g->stencil_test >> 8) & 0xffu) : -1;
    if (value < 0 || (blended && value != 0)) return SFIX_NONE;
    return value;
}

static Target *target_containing(uint32_t off) {
    Target *best = NULL;
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &s_targets[i];
        if (t->used && t->gpu_valid && off >= t->fba && off < t->fba + target_bytes(t) && (!best || t->fba > best->fba))
            best = t;
    }
    return best;
}

static void color4(float *out, uint32_t rgb) {
    out[0] = (float)(rgb & 0xffu) / 255.0f;
    out[1] = (float)((rgb >> 8) & 0xffu) / 255.0f;
    out[2] = (float)((rgb >> 16) & 0xffu) / 255.0f;
}

/* Pick the fragment shader for a finished state and fill the fields shaders/ge_dk_fast.frag reads.
 * The reason a state needs the full shader goes to the log, to show what is worth adding. */
static uint32_t frag_choose(const GeState *g, DrawState *out, int flags, uint32_t lodcfg) {
    const uint32_t at_func = (uint32_t)out->pc.cfg[1] & 7u;
    const uint32_t at_ref = ((uint32_t)out->pc.cfg[1] >> 8) & 0xffu;
    const uint32_t at_mask = ((uint32_t)out->pc.cfg[1] >> 16) & 0xffu;
    const uint32_t tf = g->tex_func & 7u;
    int why = -1;
    if ((g->fbfmt & 3u) != 3u) why = DK_FULL_FORMAT;
    else if (flags & (F_SA2X | F_SA2XI | F_SHBLEND | F_STENCIL | F_SALPHA)) why = DK_FULL_BLEND;
    else if ((flags & F_PERSP) && (g->minz != 0u || g->maxz < 65535u)) why = DK_FULL_DEPTH_RANGE;
    else if ((flags & F_TEX) && tf != 0u && tf != 3u) why = DK_FULL_TEXFUNC;
    else if ((flags & F_LODFRAG) && !(lodcfg & (LOD_SLOPE | LOD_CONSTANT))) why = DK_FULL_LOD;
    else if (at_func != 1u && !((at_func == 6u || at_func == 7u) && at_mask == 0xffu)) why = DK_FULL_ALPHA_TEST;
    if (why >= 0) {
        s_stats.frag_full_why[why]++;
        return FRAG_FULL;
    }

    out->pc.fixa[0] = 1.0f / out->pc.texsize[0];
    out->pc.fixa[1] = 1.0f / out->pc.texsize[1];
    out->pc.fixa[3] = tf == 0u ? 1.0f : 0.0f;
    out->pc.fixb[0] = (g->tex_func & 0x100u) ? 1.0f : 0.0f;
    out->pc.fixb[1] = (g->tex_func & 0x10000u) ? 2.0f : 1.0f;
    if (flags & F_LODFRAG) {
        /* ge.c tex_detail16 in levels: bias, plus 1 + log2(slope) - log2(q) in slope mode. */
        const float bias = out->pc.fogcol[3] / 16.0f;
        const int slope = (lodcfg & LOD_SLOPE) != 0u;
        out->pc.texenv[3] = (float)((lodcfg >> LOD_MAX_SHIFT) & 7u);
        out->pc.fixb[2] = slope ? bias + 1.0f + log2f(fabsf(g->tex_lod_slope)) : bias;
        out->pc.fixb[3] = slope ? 1.0f : 0.0f;
    }

    /* A pixel's alpha, rounded to 8 bits, passes from this value up. */
    if (at_func == 1u || (at_func == 7u && at_ref == 0u)) return FRAG_FAST;
    out->pc.fixa[2] = ((float)at_ref + (at_func == 7u ? -0.5f : 0.5f)) / 255.0f;
    return FRAG_FAST_ALPHA_TEST;
}

/* Fill `out` from the GE state. Mirrors ge_gpu.c build_state. Returns 0 when nothing can draw. */
static int state_build(int persp, int sprite, DrawState *out) {
    const GeState *g = s_ge;
    memset(out, 0, sizeof(*out));
    const int clear = g->clear;
    int premul = 0;

    const int sx1 = g->scis_x1 > 0 ? g->scis_x1 : 0, sy1 = g->scis_y1 > 0 ? g->scis_y1 : 0;
    const int sx2 = g->scis_x2 < FB_W - 1 ? g->scis_x2 : FB_W - 1;
    const int sy2 = g->scis_y2 < FB_H - 1 ? g->scis_y2 : FB_H - 1;
    if (sx1 > sx2 || sy1 > sy2) return 0;
    out->sx = (uint32_t)sx1; out->sy = (uint32_t)sy1;
    out->sw = (uint32_t)(sx2 - sx1 + 1); out->sh = (uint32_t)(sy2 - sy1 + 1);

    const int sfix = !clear && g->stencil_enable ? stencil_fixed_alpha(g) : SFIX_NONE;
    const int stencil = !clear && g->stencil_enable && sfix == SFIX_NONE;

    if (clear) {
        out->cmask = ((g->clear_mode & 0x100u) ? (DkColorMask_R | DkColorMask_G | DkColorMask_B) : 0u) |
                     ((g->clear_mode & 0x200u) ? DkColorMask_A : 0u);
    } else {
        const uint32_t mr = g->mask_rgb & 0xffu, mg = (g->mask_rgb >> 8) & 0xffu, mb = (g->mask_rgb >> 16) & 0xffu;
        if ((mr && mr != 0xffu) || (mg && mg != 0xffu) || (mb && mb != 0xffu)) s_stats.approx_write_mask++;
        out->cmask = (mr >= 0x80u ? 0u : DkColorMask_R) | (mg >= 0x80u ? 0u : DkColorMask_G) |
                     (mb >= 0x80u ? 0u : DkColorMask_B);
        if (g->stencil_enable && sfix != SFIX_KEEP) out->cmask |= DkColorMask_A;
    }
    if (g->color_test_enable) s_stats.approx_color_test++;
    if (g->logic_op_enable) s_stats.approx_logic_op++;

    int shblend = 0;
    out->bconst[3] = 1.0f;
    if (!clear && g->blend_enable) {
        const uint32_t sfp = g->blend_mode & 0xfu, dfp = (g->blend_mode >> 4) & 0xfu, eq = (g->blend_mode >> 8) & 7u;
        /* Dither alone does not route to the shader: on this 8888 path the fixed-function blend
         * is far cheaper than a destination copy, and the visible difference is one 8-bit step. */
        shblend = stencil || (g->fbfmt & 3u) != 3u || blend_needs_shader(sfp, dfp, eq, g->blend_fixa, g->blend_fixb);
        if (shblend) {
            out->pc.bl[0] = (int32_t)(sfp | (dfp << 8) | (eq << 16));
            color4(out->pc.fixa, g->blend_fixa);
            color4(out->pc.fixb, g->blend_fixb);
        } else {
            int const_a = 0, const_b = 0;
            const DkBlendFactor sf = map_factor(sfp, 1, g->blend_fixa, &const_a, &premul);
            const DkBlendFactor df = map_factor(dfp, 0, g->blend_fixb, &const_b, &premul);
            if (eq == 3 || eq == 4) premul = 0;
            color4(out->bconst, const_a ? g->blend_fixa : g->blend_fixb);
            out->blend_on = 1;
            out->srcf = (uint8_t)sf;
            out->dstf = (uint8_t)df;
            switch (eq) {
                case 1: out->eq = DkBlendOp_Sub; break;
                case 2: out->eq = DkBlendOp_RevSub; break;
                case 3: out->eq = DkBlendOp_Min; break;
                case 4: out->eq = DkBlendOp_Max; break;
                default: out->eq = DkBlendOp_Add; break;
            }
        }
    }

    static const uint8_t zmap[8] = {
        DkCompareOp_Never, DkCompareOp_Always, DkCompareOp_Equal, DkCompareOp_NotEqual,
        DkCompareOp_Less, DkCompareOp_Lequal, DkCompareOp_Greater, DkCompareOp_Gequal,
    };
    if (clear) {
        out->ztest = out->zwrite = (g->clear_mode & 0x400u) ? 1 : 0;
        out->zfunc = DkCompareOp_Always;
    } else {
        out->ztest = g->ztest_enable ? 1 : 0;
        out->zwrite = (out->ztest && !g->zwrite_disable) ? 1 : 0;
        out->zfunc = out->ztest ? zmap[g->ztest & 7u] : DkCompareOp_Always;
    }
    out->cull = (!sprite && g->cull_enable && !clear) ? 1 : 0;

    const int textured = g->tex_enable && !clear && g->tex_addr;
    const uint32_t variant = ((g->tex_filter & 1u) ? 2u : 0u) | ((g->tex_filter & 0x100u) ? 1u : 0u);
    uint32_t lodcfg = 0u;
    int flags = premul;
    if (clear) flags |= F_CLEAR;
    if (persp && !(clear && sprite)) flags |= F_PERSP;
    if (persp && g->fog_enable && !clear) flags |= F_FOG;
    out->pc.texsize[0] = out->pc.texsize[1] = 1.0f;
    out->image_id = IMAGE_WHITE;
    if (textured) {
        flags |= F_TEX;
        if (g->tex_func & 0x100u) flags |= F_RGBA;
        if (g->tex_func & 0x10000u) flags |= F_DBL;
        if (variant == 0u) flags |= F_NEAREST;
        out->pc.texsize[0] = (float)g->tex_w;
        out->pc.texsize[1] = (float)g->tex_h;
        out->sampler_id = (variant << 2) | ((g->tex_wrap & 1u) << 1) | ((g->tex_wrap >> 8) & 1u);
        Target *src = is_vram(g->tex_addr) ? target_containing(vram_off(g->tex_addr)) : NULL;
        if (src && src->cpu_dirty) target_upload(src);
        if (src) {
            const uint32_t bpp = src->fmt == 3 ? 4u : 2u;
            const uint32_t toff = vram_off(g->tex_addr) - src->fba;
            if (g->tex_fmt == src->fmt && !g->tex_swizzle && (g->tex_bufw == src->stride || !g->tex_bufw) &&
                (toff % bpp) == 0u) {
                const uint32_t pix = toff / bpp;
                /* A target holds more pixels than guest memory: rounding the coordinate to a
                 * guest texel would turn a full-screen copy back into the PSP's resolution. */
                if (s_scale > 1u) flags &= ~F_NEAREST;
                if (src == s_cur) {
                    out->image_id = IMAGE_SNAPSHOT;
                    out->reads_dst = 1;
                } else {
                    draw_flush();
                    dkCmdBufBarrier(s_cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
                    out->image_id = src->image_id;
                }
                out->pc.texsize[0] = (float)FB_W;
                out->pc.texsize[1] = (float)FB_H;
                out->pc.texsize[2] = (float)(pix % src->stride);
                out->pc.texsize[3] = (float)(pix / src->stride);
            } else {
                target_readback(src);
                out->image_id = tex_get();
            }
        } else {
            const uint64_t tex_started = SDL_GetTicksNS();
            out->image_id = tex_get();
            s_stats.tex_ns += SDL_GetTicksNS() - tex_started;
            /* The level of detail is per fragment, in the PSP's own arithmetic. */
            if (s_tex_levels > 1u) {
                const uint32_t linear = (g->tex_filter >> 1) & 1u;
                const int constant = g->tex_level_mode == 1u || g->tex_level_mode == 3u;
                flags |= F_LODFRAG;
                lodcfg = (g->tex_level_mode == 2u ? LOD_SLOPE : 0u) | (linear ? LOD_LINEAR : 0u) |
                         ((s_tex_levels - 1u) << LOD_MAX_SHIFT) | (constant ? LOD_CONSTANT : 0u);
                out->sampler_id += (1u + linear) * SAMPLER_VARIANTS;
                out->pc.texenv[3] = g->tex_lod_slope;
                out->pc.fogcol[3] = (float)((int)(g->tex_lod_bias * 16.0f) - (constant ? 0 : s_lod_scale_bias));
            }
        }
    }
    if (g->dither_enable && !out->blend_on) {
        flags |= F_DITHER;
        for (int row = 0; row < 4; row++) {
            out->pc.dith[row] = (int32_t)((uint32_t)(uint8_t)g->dith[row][0] | ((uint32_t)(uint8_t)g->dith[row][1] << 8) |
                                          ((uint32_t)(uint8_t)g->dith[row][2] << 16) |
                                          ((uint32_t)(uint8_t)g->dith[row][3] << 24));
        }
    }
    flags |= (int)(g->fbfmt & 3u) << F_FMT_SHIFT;
    if (shblend) {
        flags |= F_SHBLEND;
        out->reads_dst = 1;
    }
    out->pc.bl[1] = (int32_t)s_scale;
    if (stencil) {
        flags |= F_STENCIL;
        out->reads_dst = 1;
        out->pc.bl[2] = (int32_t)(g->stencil_test & 0xffffffu);
        out->pc.bl[3] = (int32_t)((g->stencil_op & 0x070707u) | ((uint32_t)(g->mask_alpha & 0xff) << 24));
    } else if (sfix >= 0 && out->blend_on) {
        out->azero = 1;
    } else if (sfix >= 0) {
        flags |= F_SALPHA;
        out->pc.bl[2] = sfix;
    }
    out->pc.cfg[1] = (int32_t)(((!clear && g->atest_enable) ? (g->atest & 0xffffffu) : 1u) | (lodcfg << LOD_CFG_SHIFT));
    out->pc.cfg[2] = (int32_t)g->minz;
    out->pc.cfg[3] = (int32_t)g->maxz;
    color4(out->pc.texenv, g->tex_env);
    color4(out->pc.fogcol, g->fog_color);
    out->pc.cfg[0] = (int32_t)((g->tex_func & 7u) | ((uint32_t)flags << 8));
    out->frag = (uint8_t)frag_choose(g, out, flags, lodcfg);
    return 1;
}

/* Make s_state current for the next vertices. Returns 0 when the primitive cannot be drawn. */
static int state_prepare(int persp, int sprite) {
    if (!target_begin()) return 0;
    const uint32_t serial = ge_state_serial();
    if (s_state_valid && serial == s_state_serial && persp == s_state_persp && sprite == s_state_sprite) return 1;
    DrawState next;
    /* state_build can record copies and uploads, so the gathered vertices go first. */
    if (draw_pending()) draw_flush();
    const uint64_t started = SDL_GetTicksNS();
    const int built = state_build(persp, sprite, &next);
    s_stats.state_ns += SDL_GetTicksNS() - started;
    if (!built) {
        s_state_valid = 0;
        return 0;
    }
    s_state = next;
    s_state_valid = 1;
    s_state_serial = serial;
    s_state_persp = persp;
    s_state_sprite = sprite;
    return 1;
}

/* Gathered primitives of one kind only: a kind change records what came before it. */
static void draw_mode_set(int tnl) {
    if (s_draw_tnl == tnl) return;
    draw_flush();
    s_draw_tnl = tnl;
}

static void room_for(uint32_t verts) {
    draw_mode_set(0);
    if (s_nverts + verts <= VERT_CAPACITY) return;
    draw_flush();
    slot_submit();
}

static void put_vert(float x, float y, float z, float rw, float u, float v, float fog, int r, int g, int b, int a) {
    GpuVert *o = &s_vert_cpu[s_nverts++];
    o->x = snap16(x);
    o->y = snap16(y);
    o->z = z;
    o->rw = rw;
    o->u = u;
    o->v = v;
    o->fog = fog;
    const uint32_t cr = (uint32_t)(r < 0 ? 0 : r > 255 ? 255 : r);
    const uint32_t cg = (uint32_t)(g < 0 ? 0 : g > 255 ? 255 : g);
    const uint32_t cb = (uint32_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    const uint32_t ca = (uint32_t)(a < 0 ? 0 : a > 255 ? 255 : a);
    o->rgba = cr | (cg << 8) | (cb << 16) | (ca << 24);
}

/* ---- hooks: a hook returns 1 when it took the primitive --------------------------------------- */

static int hook_tri(const GeVtx *A, const GeVtx *B, const GeVtx *C, int persp) {
    if (!s_ready) return 0;
    if (!ge_vtx_finite(A) || !ge_vtx_finite(B) || !ge_vtx_finite(C)) return 1;
    /* The rule of ge.c raster_tri, on the CPU, so it does not rest on a host winding convention:
     * draw_prim has ordered the vertices so that a front face has a positive cross product. */
    if (s_ge->cull_enable && !s_ge->clear) {
        const double area = (double)(B->x - A->x) * (C->y - A->y) - (double)(C->x - A->x) * (B->y - A->y);
        if (area < 0.0) return 1;
    }
    if (!state_prepare(persp, 0)) return 1;
    room_for(3);
    const int flat = !s_ge->shade_gouraud || s_ge->clear;
    const GeVtx *ca = flat ? C : A;
    const GeVtx *cb = flat ? C : B;
    put_vert(A->x, A->y, A->z, A->rw, A->u, A->v, A->fog, ca->r, ca->g, ca->b, ca->a);
    put_vert(B->x, B->y, B->z, B->rw, B->u, B->v, B->fog, cb->r, cb->g, cb->b, cb->a);
    put_vert(C->x, C->y, C->z, C->rw, C->u, C->v, C->fog, C->r, C->g, C->b, C->a);
    sr_vram_mark_renderer_pending();
    return 1;
}

static int hook_sprite(const GeVtx *p0, const GeVtx *p1, int persp) {
    if (!s_ready) return 0;
    if (!ge_vtx_finite(p0) || !ge_vtx_finite(p1)) return 1;
    if (!state_prepare(persp, 1)) return 1;
    const float xa = floorf(fminf(p0->x, p1->x)), xb = floorf(fmaxf(p0->x, p1->x));
    const float ya = floorf(fminf(p0->y, p1->y)), yb = floorf(fmaxf(p0->y, p1->y));
    if (xb <= xa || yb <= ya) return 1;
    float u0 = p0->u, v0 = p0->v, u1 = p1->u, v1 = p1->v;
    if (persp) {
        const float rw0 = p0->rw != 0.0f ? p0->rw : 1.0f, rw1 = p1->rw != 0.0f ? p1->rw : 1.0f;
        u0 /= rw0; v0 /= rw0; u1 /= rw1; v1 /= rw1;
    }
    const float ua = (p0->x <= p1->x) ? u0 : u1, ub = (p0->x <= p1->x) ? u1 : u0;
    const float va = (p0->y <= p1->y) ? v0 : v1, vb = (p0->y <= p1->y) ? v1 : v0;
    const float z = p1->z, fog = p1->fog;
    const int r = p1->r, g = p1->g, b = p1->b, a = p1->a;
    room_for(6);
    put_vert(xa, ya, z, 1.0f, ua, va, fog, r, g, b, a);
    put_vert(xb, ya, z, 1.0f, ub, va, fog, r, g, b, a);
    put_vert(xa, yb, z, 1.0f, ua, vb, fog, r, g, b, a);
    put_vert(xb, ya, z, 1.0f, ub, va, fog, r, g, b, a);
    put_vert(xb, yb, z, 1.0f, ub, vb, fog, r, g, b, a);
    put_vert(xa, yb, z, 1.0f, ua, vb, fog, r, g, b, a);
    sr_vram_mark_renderer_pending();
    return 1;
}

static int hook_line(const GeVtx *A, const GeVtx *B, int persp) {
    if (!s_ready) return 0;
    if (!ge_vtx_finite(A) || !ge_vtx_finite(B)) return 1;
    if (!state_prepare(persp, 1)) return 1;
    float dx = B->x - A->x, dy = B->y - A->y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-6f) { dx = 1.0f; dy = 0.0f; len = 1.0f; }
    const float nx = -dy / len * 0.5f, ny = dx / len * 0.5f;
    const float ex = dx / len * 0.5f, ey = dy / len * 0.5f;
    const float ax = A->x + 0.5f, ay = A->y + 0.5f, bx = B->x + 0.5f, by = B->y + 0.5f;
    const GeVtx *cA = !s_ge->shade_gouraud ? B : A;
    room_for(6);
    put_vert(ax - ex - nx, ay - ey - ny, A->z, A->rw, A->u, A->v, A->fog, cA->r, cA->g, cA->b, cA->a);
    put_vert(ax - ex + nx, ay - ey + ny, A->z, A->rw, A->u, A->v, A->fog, cA->r, cA->g, cA->b, cA->a);
    put_vert(bx + ex - nx, by + ey - ny, B->z, B->rw, B->u, B->v, B->fog, B->r, B->g, B->b, B->a);
    put_vert(ax - ex + nx, ay - ey + ny, A->z, A->rw, A->u, A->v, A->fog, cA->r, cA->g, cA->b, cA->a);
    put_vert(bx + ex + nx, by + ey + ny, B->z, B->rw, B->u, B->v, B->fog, B->r, B->g, B->b, B->a);
    put_vert(bx + ex - nx, by + ey - ny, B->z, B->rw, B->u, B->v, B->fog, B->r, B->g, B->b, B->a);
    sr_vram_mark_renderer_pending();
    return 1;
}

static int hook_point(const GeVtx *A, int persp) {
    if (!s_ready) return 0;
    if (!ge_vtx_finite(A)) return 1;
    if (!state_prepare(persp, 1)) return 1;
    const float x0 = floorf(A->x), y0 = floorf(A->y);
    room_for(6);
    put_vert(x0, y0, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0 + 1.0f, y0, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0, y0 + 1.0f, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0 + 1.0f, y0, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0 + 1.0f, y0 + 1.0f, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0, y0 + 1.0f, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    sr_vram_mark_renderer_pending();
    return 1;
}

/* GPU transform and lighting: a whole triangle PRIM ge.c proved eligible. Its references go up
 * once and its triangles index them. Declining sends the PRIM down ge.c's own T&L. */
static int hook_tnl_prim(const GeTnlPrim *p) {
    if (!s_ready) return 0;
    const uint32_t nidx = p->ntri * 3u;
    if (!nidx || p->nrefs > TNL_VERT_CAPACITY || nidx > TNL_IDX_CAPACITY) return 0;
    if (!state_prepare(1, 0)) return 1;
    draw_mode_set(1);
    if (s_tnl_nverts + p->nrefs > TNL_VERT_CAPACITY || s_tnl_nidx + nidx > TNL_IDX_CAPACITY ||
        s_tnl_nparams >= TNL_PARAM_CAPACITY) {
        draw_flush();
        slot_submit();
    }
    const uint32_t param = s_tnl_nparams++, base = s_tnl_nverts;
    memcpy(&s_tnl_param_cpu[param], p->params, sizeof(GeTnlParams));
    GeTnlVtx *verts = s_tnl_vert_cpu + base;
    for (uint32_t i = 0; i < p->nrefs; i++) {
        memcpy(&verts[i], &p->refs[i], offsetof(GeTnlVtx, param));
        verts[i].param = param;
    }
    uint32_t *idx = s_tnl_idx_cpu + s_tnl_nidx;
    for (uint32_t i = 0; i < nidx; i++) idx[i] = p->tri[i] + base;
    s_tnl_nverts += p->nrefs;
    s_tnl_nidx += nidx;
    s_stats.tnl_prims++;
    s_stats.tnl_tris += p->ntri;
    sr_vram_mark_renderer_pending();
    return 1;
}

static void readback_span(uint32_t off0, uint32_t off1) {
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &s_targets[i];
        if (target_overlaps(t, off0, off1) && t->gpu_valid) target_readback(t);
    }
}

static void hook_flush(const char *reason) {
    if (!s_ready || !reason) return;
    if (strcmp(reason, "xfersrc") == 0) {
        const uint32_t base = (s_ge->xf_src & 0xfffff0u) | ((s_ge->xf_srcw & 0xff0000u) << 8);
        if (!is_vram(base)) return;
        const uint32_t stride = s_ge->xf_srcw & 0x7f8u;
        const uint32_t y = (s_ge->xf_spos >> 10) & 0x3ffu, h = ((s_ge->xf_size >> 10) & 0x3ffu) + 1u;
        const uint32_t off = vram_off(base);
        readback_span(off, off + (y + h) * (stride ? stride : FB_W) * 4u);
    } else if (strcmp(reason, "loadclut") == 0) {
        if (!is_vram(s_ge->clut_addr)) return;
        const uint32_t off = vram_off(s_ge->clut_addr);
        readback_span(off, off + 2048u);
    }
}

static int tex_entry_overlaps(const TexEntry *e, uint32_t phys, uint32_t bytes) {
    uint32_t start = SR_PHYS(e->addr);
    if (phys < start + e->bytes && start < phys + bytes) return 1;
    for (uint32_t i = 0; i + 1u < e->levels; i++) {
        start = SR_PHYS(e->mip_addr[i]);
        if (phys < start + e->mip_bytes[i] && start < phys + bytes) return 1;
    }
    return 0;
}

static void hook_vram_dirty(uint32_t addr, uint32_t bytes) {
    if (!s_ready || !bytes) return;
    const uint32_t phys = SR_PHYS(addr);
    for (int i = 0; i < s_tex_n; i++) {
        TexEntry *e = &s_tex[i];
        if (e->key && tex_entry_overlaps(e, phys, bytes)) {
            e->content_valid = 0;
            s_state_valid = 0;
        }
    }
    if (!is_vram(addr)) return;
    const uint32_t off = vram_off(addr);
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &s_targets[i];
        if (target_overlaps(t, off, off + bytes)) {
            t->cpu_dirty = 1;
            s_state_valid = 0;
        }
    }
}

static int hook_xfer(uint32_t startdata) {
    (void)startdata;
    return 0;
}

static int hook_capture_boundary(void) { return 1; }

static int hook_vram_read(uint32_t addr, uint32_t bytes) {
    if (!s_ready || !bytes || !is_vram(addr)) return 1;
    const uint32_t off = vram_off(addr);
    readback_span(off, off + bytes);
    return 1;
}

static int hook_vram_write_prepare(uint32_t addr, uint32_t bytes) {
    (void)addr;
    (void)bytes;
    return 1;
}

static int hook_vram_pending(void) {
    if (draw_pending()) return 1;
    for (int i = 0; i < MAX_TARGETS; i++) {
        const Target *t = &s_targets[i];
        if (t->used && t->gpu_valid && t->clean_gen != t->render_gen) return 1;
    }
    return 0;
}

static const GeGpuHooks k_hooks = {
    hook_tri, hook_sprite, hook_line, hook_point, hook_flush, hook_vram_dirty, hook_xfer,
    hook_capture_boundary, hook_vram_read, hook_vram_write_prepare, hook_vram_pending, hook_tnl_prim,
};

/* ---- present --------------------------------------------------------------------------------- */

/* The GPU is behind at this scale: go one step lower. Called with nothing recorded and no target
 * bound, right after a flip, so the images can go; the next frame's draws make new ones. */
static void render_scale_lower(void) {
    dkQueueWaitIdle(s_queue);
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &s_targets[i];
        if (!t->used) continue;
        gpu_image_destroy(&t->img);
        s_image_id_used[t->image_id] = 0;
        memset(t, 0, sizeof(*t));
    }
    for (int i = 0; i < MAX_DEPTHS; i++) {
        if (!s_depths[i].used) continue;
        gpu_image_destroy(&s_depths[i].img);
        memset(&s_depths[i], 0, sizeof(s_depths[i]));
    }
    gpu_image_destroy(&s_snapshot);
    s_scale--;
    s_lod_scale_bias = lod_scale_bias(s_scale);
    if (gpu_image_create(&s_snapshot, RENDER_W, RENDER_H, DkImageFormat_RGBA8_Unorm, DkImageFlags_Usage2DEngine))
        image_id_describe(IMAGE_SNAPSHOT, &s_snapshot.view);
    s_cur = NULL;
    s_cur_depth = NULL;
    s_snap_src = NULL;
    s_state_valid = 0;
    s_bound_valid = 0;
    fprintf(stderr, "ge_dk: GPU behind, render scale now %u\n", s_scale);
}

/* One more frame's GPU time is in. Reports whether the scale should come down. */
static int gpu_overloaded(void) {
    if (s_scale <= 1u || ++s_gpu_window_frames < GPU_WINDOW_FRAMES) return 0;
    s_gpu_overloads = s_gpu_window_ns / GPU_WINDOW_FRAMES > GPU_OVERLOAD_NS ? s_gpu_overloads + 1u : 0u;
    s_gpu_window_ns = 0;
    s_gpu_window_frames = 0;
    if (s_gpu_overloads < GPU_OVERLOAD_WINDOWS) return 0;
    s_gpu_overloads = 0;
    return 1;
}

int ge_dk_present(uint32_t fbaddr, int fmt, uint32_t stride) {
    if (!s_ready) return 0;
    draw_flush();
    Target *t;
    if (is_vram(fbaddr)) {
        t = target_acquire(vram_off(fbaddr), stride ? stride : FB_W, (uint32_t)fmt & 3u);
        if (!t) return 0;
    } else {
        /* A display buffer in main RAM (a CPU-composed screen): shown through its own target,
         * with an offset no VRAM address reaches, reloaded for every frame. */
        t = target_acquire(TARGET_FBA_MAIN_RAM, stride ? stride : FB_W, (uint32_t)fmt & 3u);
        if (!t) return 0;
        t->base = fbaddr;
        t->cpu_dirty = 1;
        s_stats.present_outside_vram++;
    }
    if (t->cpu_dirty || !t->gpu_valid) target_upload(t);
    dkCmdBufBarrier(s_cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);

    /* Docked or handheld decides how much of the screen image is shown. */
    const int docked = appletGetOperationMode() == AppletOperationMode_Console;
    const uint32_t out_w = docked ? SWAP_W : HANDHELD_W, out_h = docked ? SWAP_H : HANDHELD_H;
    if (docked != s_docked) {
        s_docked = docked;
        dkSwapchainSetCrop(s_swapchain, 0, 0, (int32_t)out_w, (int32_t)out_h);
        fprintf(stderr, "ge_dk: output %ux%u\n", out_w, out_h);
    }

    /* The frame's commands finish first: acquiring may block until the display frees an image. */
    Slot *slot = &s_slots[s_slot];
    const uint64_t acquire_started = SDL_GetTicksNS();
    const int image = dkQueueAcquireImage(s_queue, s_swapchain);
    s_stats.acquire_ns += SDL_GetTicksNS() - acquire_started;
    const DkImageView *color = &s_swap_views[image];
    dkCmdBufBindRenderTargets(s_cmd, &color, 1, NULL);
    DkViewport viewport = {0.0f, 0.0f, (float)out_w, (float)out_h, 0.0f, 1.0f};
    DkScissor scissor = {0, 0, out_w, out_h};
    dkCmdBufSetViewports(s_cmd, 0, &viewport, 1);
    dkCmdBufSetScissors(s_cmd, 0, &scissor, 1);
    const DkShader *shaders[2] = {&s_present_vs, &s_present_fs};
    dkCmdBufBindShaders(s_cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
    DkRasterizerState raster;
    dkRasterizerStateDefaults(&raster);
    raster.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(s_cmd, &raster);
    DkColorState colorst;
    dkColorStateDefaults(&colorst);
    dkCmdBufBindColorState(s_cmd, &colorst);
    DkColorWriteState write;
    dkColorWriteStateDefaults(&write);
    dkCmdBufBindColorWriteState(s_cmd, &write);
    DkDepthStencilState depth;
    dkDepthStencilStateDefaults(&depth);
    depth.depthTestEnable = 0;
    depth.depthWriteEnable = 0;
    dkCmdBufBindDepthStencilState(s_cmd, &depth);
    dkCmdBufBindVtxAttribState(s_cmd, NULL, 0);
    dkCmdBufBindVtxBufferState(s_cmd, NULL, 0);
    const float show[4] = {(float)SHOW_W / (float)FB_W, (float)SHOW_H / (float)FB_H, 0.0f, 0.0f};
    dkCmdBufPushConstants(s_cmd, dkMemBlockGetGpuAddr(s_ubo_mem), UBO_BYTES, 0, sizeof(show), show);
    dkCmdBufBindUniformBuffer(s_cmd, DkStage_Vertex, 0, dkMemBlockGetGpuAddr(s_ubo_mem), UBO_BYTES);
    dkCmdBufBindTexture(s_cmd, DkStage_Fragment, 0, dkMakeTextureHandle(t->image_id, SAMPLER_PRESENT));
    dkCmdBufDraw(s_cmd, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
    dkCmdBufBarrier(s_cmd, DkBarrier_Fragments, 0);

    dkCmdBufReportCounter(s_cmd, DkCounter_Timestamp, stamp_addr(s_slot, 1));
    dkCmdBufSignalFence(s_cmd, &slot->fence, true);
    slot->fence_armed = 1;
    dkQueueSubmitCommands(s_queue, dkCmdBufFinishList(s_cmd));
    dkQueuePresentImage(s_queue, s_swapchain, image);
    s_slot = (s_slot + 1) % SLOT_COUNT;
    if (gpu_overloaded()) render_scale_lower();
    slot_begin();
    s_bound_valid = 0;
    s_stats.presents++;
    return 1;
}

void ge_dk_stats_take(DkGeStats *out) {
    *out = s_stats;
    memset(&s_stats, 0, sizeof(s_stats));
}

/* ---- setup ------------------------------------------------------------------------------------ */

/* An empty scale<N>.txt beside the game picks the render scale. */
static uint32_t render_scale_choice(void) {
    if (access("scale1.txt", F_OK) == 0) return 1u;
    if (access("scale3.txt", F_OK) == 0) return 3u;
    if (access("scale4.txt", F_OK) == 0) return 4u;
    return DEFAULT_RENDER_SCALE;
}

int ge_dk_init(void *native_window) {
    s_ge = ge_state_ptr();
    s_scale = render_scale_choice();
    s_lod_scale_bias = lod_scale_bias(s_scale);
    s_tex_scratch = memalign(0x40, TEX_SCRATCH_TEXELS * 4u);
    if (!s_tex_scratch) return 0;

    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    s_device = dkDeviceCreate(&dm);
    if (!s_device) return 0;

    s_code_mem = cpu_block(CODE_BYTES, DkMemBlockFlags_Code);
    const uint32_t desc_bytes = MAX_IMAGE_IDS * sizeof(DkImageDescriptor) + (SAMPLER_COUNT + 1) * sizeof(DkSamplerDescriptor);
    s_desc_mem = cpu_block(desc_bytes, 0);
    s_ubo_mem = cpu_block(UBO_BYTES, 0);
    s_stamp_mem = cpu_block(SLOT_COUNT * STAMP_BYTES, 0);
    DkMemBlockMaker rm;
    dkMemBlockMakerDefaults(&rm, s_device, (READBACK_BYTES + DK_MEMBLOCK_ALIGNMENT - 1u) & ~(DK_MEMBLOCK_ALIGNMENT - 1u));
    rm.flags = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached;
    s_readback_mem = dkMemBlockCreate(&rm);
    for (int i = 0; i < SLOT_COUNT; i++) {
        s_slots[i].cmd_mem = cpu_block(CMD_BYTES, 0);
        s_vert_mem[i] = cpu_block(VERT_CAPACITY * sizeof(GpuVert), 0);
        s_staging_mem[i] = cpu_block(STAGING_BYTES, 0);
        s_tnl_vert_mem[i] = cpu_block(TNL_VERT_CAPACITY * sizeof(GeTnlVtx), 0);
        s_tnl_idx_mem[i] = cpu_block(TNL_IDX_CAPACITY * sizeof(uint32_t), 0);
        s_tnl_param_mem[i] = cpu_block(TNL_PARAM_CAPACITY * sizeof(GeTnlParams), 0);
        if (!s_slots[i].cmd_mem || !s_vert_mem[i] || !s_staging_mem[i] || !s_tnl_vert_mem[i] ||
            !s_tnl_idx_mem[i] || !s_tnl_param_mem[i])
            return 0;
    }
    if (!s_code_mem || !s_desc_mem || !s_ubo_mem || !s_readback_mem || !s_stamp_mem) return 0;
    memset(dkMemBlockGetCpuAddr(s_stamp_mem), 0, SLOT_COUNT * STAMP_BYTES);

    shader_load(&s_vs, ge_dk_vert_dksh, ge_dk_vert_dksh_size);
    shader_load(&s_tnl_vs, ge_dk_tnl_vert_dksh, ge_dk_tnl_vert_dksh_size);
    shader_load(&s_fs, ge_dk_frag_dksh, ge_dk_frag_dksh_size);
    shader_load(&s_fast_fs, ge_dk_fast_frag_dksh, ge_dk_fast_frag_dksh_size);
    shader_load(&s_fast_at_fs, ge_dk_fast_at_frag_dksh, ge_dk_fast_at_frag_dksh_size);
    shader_load(&s_present_vs, present_vert_dksh, present_vert_dksh_size);
    shader_load(&s_present_fs, present_frag_dksh, present_frag_dksh_size);

    /* Samplers: mip mode, PSP_FILTER variant, clamp u, clamp v; one more for the screen. */
    s_image_desc = dkMemBlockGetCpuAddr(s_desc_mem);
    DkSamplerDescriptor *samplers = (DkSamplerDescriptor *)(s_image_desc + MAX_IMAGE_IDS);
    for (uint32_t i = 0; i < SAMPLER_COUNT; i++) {
        static const DkMipFilter mips[3] = {DkMipFilter_None, DkMipFilter_Nearest, DkMipFilter_Linear};
        DkSampler sampler;
        dkSamplerDefaults(&sampler);
        const uint32_t variant = (i % SAMPLER_VARIANTS) >> 2;
        sampler.minFilter = (variant & 2u) ? DkFilter_Linear : DkFilter_Nearest;
        sampler.magFilter = (variant & 1u) ? DkFilter_Linear : DkFilter_Nearest;
        sampler.mipFilter = mips[i / SAMPLER_VARIANTS];
        sampler.wrapMode[0] = (i & 2u) ? DkWrapMode_ClampToEdge : DkWrapMode_Repeat;
        sampler.wrapMode[1] = (i & 1u) ? DkWrapMode_ClampToEdge : DkWrapMode_Repeat;
        dkSamplerDescriptorInitialize(&samplers[i], &sampler);
    }
    DkSampler screen;
    dkSamplerDefaults(&screen);
    screen.minFilter = DkFilter_Linear;
    screen.magFilter = DkFilter_Linear;
    screen.wrapMode[0] = DkWrapMode_ClampToEdge;
    screen.wrapMode[1] = DkWrapMode_ClampToEdge;
    dkSamplerDescriptorInitialize(&samplers[SAMPLER_PRESENT], &screen);

    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, s_device);
    s_cmd = dkCmdBufCreate(&cm);
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, s_device);
    qm.flags = DkQueueFlags_Graphics;
    s_queue = dkQueueCreate(&qm);
    if (!s_cmd || !s_queue) return 0;

    const DkImage *swap_ptrs[SWAP_IMAGES];
    for (int i = 0; i < SWAP_IMAGES; i++) {
        GpuImage img;
        if (!gpu_image_create(&img, SWAP_W, SWAP_H, DkImageFormat_RGBA8_Unorm,
                              DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression))
            return 0;
        s_swap_mem[i] = img.mem;
        s_swap_images[i] = img.image;
        dkImageViewDefaults(&s_swap_views[i], &s_swap_images[i]);
        swap_ptrs[i] = &s_swap_images[i];
    }
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, s_device, native_window, swap_ptrs, SWAP_IMAGES);
    s_swapchain = dkSwapchainCreate(&sm);
    if (!s_swapchain) return 0;

    if (!gpu_image_create(&s_white, 1, 1, DkImageFormat_RGBA8_Unorm, DkImageFlags_Usage2DEngine) ||
        !gpu_image_create(&s_snapshot, RENDER_W, RENDER_H, DkImageFormat_RGBA8_Unorm, DkImageFlags_Usage2DEngine) ||
        !gpu_image_create(&s_transfer, FB_W, FB_H, DkImageFormat_RGBA8_Unorm, DkImageFlags_Usage2DEngine))
        return 0;
    s_image_id_used[IMAGE_WHITE] = s_image_id_used[IMAGE_SNAPSHOT] = 1;
    image_id_describe(IMAGE_WHITE, &s_white.view);
    image_id_describe(IMAGE_SNAPSHOT, &s_snapshot.view);

    s_slot = 0;
    slot_begin();
    const uint32_t white = 0xffffffffu;
    memcpy(s_staging_cpu, &white, sizeof(white));
    DkCopyBuf src = {dkMemBlockGetGpuAddr(s_staging_mem[s_slot]), 4, 0};
    DkImageRect rect = {0, 0, 0, 1, 1, 1};
    dkCmdBufCopyBufferToImage(s_cmd, &src, &s_white.view, &rect, 0);
    s_staging_used = 16;
    dkCmdBufBarrier(s_cmd, DkBarrier_Full, DkInvalidateFlags_Image);

    ge_set_gpu_hooks(&k_hooks);
    /* An empty notnl.txt beside the game keeps T&L on the CPU, to compare the two. */
    if (access("notnl.txt", F_OK) != 0) {
        ge_pow_set_exact(1);
        ge_tnl_set_enabled(1);
    }
    s_ready = 1;
    fprintf(stderr, "ge_dk: deko3d renderer active (%ux%u targets, scale %u)\n", RENDER_W, RENDER_H, s_scale);
    return 1;
}

void ge_dk_shutdown(void) {
    if (!s_ready) return;
    s_ready = 0;
    ge_set_gpu_hooks(NULL);
    dkQueueWaitIdle(s_queue);
    dkSwapchainDestroy(s_swapchain);
}
