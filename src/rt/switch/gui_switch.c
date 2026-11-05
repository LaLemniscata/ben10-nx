/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The gui_* presenter front-end for the Nintendo Switch: a CPU framebuffer the display hardware
 * scales to the screen, the pad, and a frame meter. With an empty debug.txt beside the game, the
 * meter goes to the log every few seconds. Its busy time is everything the process did on its way
 * to a frame except sleeping.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <switch.h>

#include <SDL3/nk_switch_sdl.h>

#include "ge_async.h"
#include "ge_dk.h"
#include "ge_shared.h"
#include "gpu_sdl3vk/ge_gpu.h"
#include "prof_switch.h"
#include "recomp.h"

#define PSP_W 480
#define PSP_H 272
#define METER_PERIOD_NS 5000000000ull

/* PSP button bits (sceCtrl). */
#define PSP_SELECT 0x0001u
#define PSP_START 0x0008u
#define PSP_UP 0x0010u
#define PSP_RIGHT 0x0020u
#define PSP_DOWN 0x0040u
#define PSP_LEFT 0x0080u
#define PSP_L 0x0100u
#define PSP_R 0x0200u
#define PSP_TRIANGLE 0x1000u
#define PSP_CIRCLE 0x2000u
#define PSP_CROSS 0x4000u
#define PSP_SQUARE 0x8000u

static Framebuffer s_fb;
static int s_gpu;
static int s_boost;
static int s_meter_on;
static AppletHookCookie s_applet_hook;
static PadState s_pad;
static int s_on;
static int s_quit;
static uint32_t s_buttons;
static uint32_t s_pulses;
static uint8_t s_lx = 128;
static uint8_t s_ly = 128;

static uint64_t s_meter_start_ns;
static uint64_t s_meter_start_idle_ns;
static uint64_t s_meter_present_ns;
static uint32_t s_meter_frames;
uint64_t nk_switch_ge_ns;    /* time in GE list execution, added by hle_ge.c */

/* The console's own performance mode: the CPU at 1785 MHz and the GPU at its minimum. Asked for
 * again after the system's menus and docking, which can change the clocks. */
static void boost_request(void) {
    const Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    if (R_SUCCEEDED(rc)) s_boost = 1;
    else fprintf(stderr, "gui_switch: CPU boost unavailable (0x%x)\n", (unsigned)rc);
}

static void on_applet_event(AppletHookType hook, void *param) {
    (void)param;
    if (!s_boost) return;
    if (hook == AppletHookType_OnFocusState || hook == AppletHookType_OnOperationMode ||
        hook == AppletHookType_OnPerformanceMode)
        boost_request();
}

void gui_init(const char *title) {
    (void)title;
    s_meter_on = access("debug.txt", F_OK) == 0;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&s_pad);
    NWindow *window = nwindowGetDefault();
    s_meter_start_ns = SDL_GetTicksNS();
    s_meter_start_idle_ns = nk_switch_idle_ns();
    /* An empty software.txt beside the game selects the CPU renderer, to compare the two. */
    if (access("software.txt", F_OK) != 0) {
        if (ge_dk_init(window)) {
            s_gpu = 1;
            s_on = 1;
            /* An empty nogethread.txt beside the game runs the GE on the game's core. */
            if (access("nogethread.txt", F_OK) != 0) ge_async_start();
            /* An empty noboost.txt beside the game leaves the normal clocks. */
            if (access("noboost.txt", F_OK) != 0) {
                boost_request();
                if (s_boost) appletHook(&s_applet_hook, on_applet_event, NULL);
            }
            prof_switch_start();
            return;
        }
        fprintf(stderr, "gui_switch: deko3d failed to start; using the CPU renderer\n");
    }
    if (R_FAILED(framebufferCreate(&s_fb, window, PSP_W, PSP_H, PIXEL_FORMAT_RGBA_8888, 2))) {
        fprintf(stderr, "gui_switch: framebufferCreate failed\n");
        return;
    }
    framebufferMakeLinear(&s_fb);
    /* Show exactly the PSP picture: the allocated buffer can be wider than the crop. */
    nwindowSetCrop(window, 0, 0, PSP_W, PSP_H);
    s_on = 1;
    fprintf(stderr, "gui_switch: CPU framebuffer %dx%d\n", PSP_W, PSP_H);
}

void gui_shutdown(void) {
    if (!s_on) return;
    s_on = 0;
    prof_switch_stop();
    ge_async_stop();
    if (s_boost) {
        appletUnhook(&s_applet_hook);
        appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    }
    if (s_gpu) ge_dk_shutdown();
    else framebufferClose(&s_fb);
}

int gui_on(void) { return s_on; }
int gui_interactive(void) { return s_on; }
uint32_t gui_buttons(void) { return s_buttons | s_pulses; }
void gui_analog(uint8_t *lx, uint8_t *ly) {
    if (lx) *lx = s_lx;
    if (ly) *ly = s_ly;
}
int gui_pad_present(void) { return 1; }
void gui_consume_button_pulses(void) { s_pulses = 0; }
uint32_t gui_pad_pulses_take(void) {
    uint32_t pulses = s_pulses;
    s_pulses = 0;
    return pulses;
}
void gui_ace_vblank(uint32_t vcount) { (void)vcount; }
int gui_quit_requested(void) { return s_quit; }
int gui_stop_requested(void) { return s_quit; }

int gui_headless_acceptance_requested(void) { return 0; }
int gui_headless_acceptance_ready(void) { return 0; }
int gui_headless_acceptance_complete(void) { return 0; }
uint32_t gui_headless_acceptance_presented_frames(void) { return 0; }

int gui_pause_presenter(void) { return 0; }
int gui_pause_busy_reason(void) { return 0; }
void gui_pause_enter(void) {}
SrPauseFrame gui_pause_frame(void) { return SR_PAUSE_FRAME_RESUME; }
void gui_pause_leave(void) {}

/* No GPU target exists on this presenter, so guest memory is authoritative. */
int gegpu_sync_guest_fb(const GeGpuFbDescriptor *desc) {
    (void)desc;
    return GEGPU_SYNC_NO_TARGET;
}

/* A Switch stick moves in a circle, so a diagonal reports about 0.7 on each axis and a worn stick
 * may not reach the edge at all. A PSP nub reaches much further into the corners, and a game can
 * read one axis to tell walking from running. So the travel is rescaled between a small centre
 * dead zone and an outer one, and the circle is stretched onto the square. */
#define STICK_DEAD_ZONE 0.08f
#define STICK_FULL_TRAVEL 0.90f

static uint8_t stick_byte(float axis) {
    const float scaled = 128.0f + axis * 127.5f;
    if (scaled <= 0.0f) return 0;
    if (scaled >= 255.0f) return 255;
    return (uint8_t)scaled;
}

static void stick_to_psp(const HidAnalogStickState *stick, uint8_t *lx, uint8_t *ly) {
    float x = (float)stick->x / 32767.0f, y = -(float)stick->y / 32767.0f;
    const float length = sqrtf(x * x + y * y);
    if (length <= STICK_DEAD_ZONE) {
        *lx = *ly = 128;
        return;
    }
    float travel = (length - STICK_DEAD_ZONE) / (STICK_FULL_TRAVEL - STICK_DEAD_ZONE);
    if (travel > 1.0f) travel = 1.0f;
    const float ax = fabsf(x), ay = fabsf(y);
    const float longest = ax > ay ? ax : ay;
    *lx = stick_byte(x / longest * travel);
    *ly = stick_byte(y / longest * travel);
}

void gui_pump(void) {
    if (!appletMainLoop()) s_quit = 1;
    padUpdate(&s_pad);
    const uint64_t held = padGetButtons(&s_pad);
    const uint64_t down = padGetButtonsDown(&s_pad);
    /* Face buttons by position, as on the PSP. */
    static const struct { uint64_t pad; uint32_t psp; } map[] = {
        {HidNpadButton_A, PSP_CIRCLE}, {HidNpadButton_B, PSP_CROSS},
        {HidNpadButton_X, PSP_TRIANGLE}, {HidNpadButton_Y, PSP_SQUARE},
        {HidNpadButton_Up, PSP_UP}, {HidNpadButton_Down, PSP_DOWN},
        {HidNpadButton_Left, PSP_LEFT}, {HidNpadButton_Right, PSP_RIGHT},
        {HidNpadButton_L, PSP_L}, {HidNpadButton_ZL, PSP_L},
        {HidNpadButton_R, PSP_R}, {HidNpadButton_ZR, PSP_R},
        {HidNpadButton_Plus, PSP_START}, {HidNpadButton_Minus, PSP_SELECT},
    };
    uint32_t buttons = 0;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (held & map[i].pad) buttons |= map[i].psp;
        if (down & map[i].pad) s_pulses |= map[i].psp;
    }
    s_buttons = buttons;
    const HidAnalogStickState stick = padGetStickPos(&s_pad, 0);
    stick_to_psp(&stick, &s_lx, &s_ly);
    if ((held & HidNpadButton_Plus) && (held & HidNpadButton_Minus)) s_quit = 1;
}

/* Copy the guest framebuffer. Storage is read directly: the display span was validated by the
 * caller, and sr_r32 would treat the GE status port inside VRAM as a device register. */
static void present_copy(uint32_t fbaddr, int fmt, uint32_t stride) {
    uint32_t out_stride = 0;
    uint32_t *out = framebufferBegin(&s_fb, &out_stride);
    const uint32_t out_words = out_stride / 4u;
    for (uint32_t y = 0; y < PSP_H; y++) {
        uint32_t *row = out + y * out_words;
        if (fmt == 3) {
            const uint32_t *src = (const uint32_t *)SR_HOST(fbaddr + y * stride * 4u);
            for (uint32_t x = 0; x < PSP_W; x++) row[x] = src[x] | 0xff000000u;
            continue;
        }
        const uint16_t *src = (const uint16_t *)SR_HOST(fbaddr + y * stride * 2u);
        for (uint32_t x = 0; x < PSP_W; x++) {
            const uint32_t p = src[x];
            uint32_t r, g, b;
            if (fmt == 1) {
                r = (p & 0x1fu) << 3; g = ((p >> 5) & 0x1fu) << 3; b = ((p >> 10) & 0x1fu) << 3;
            } else if (fmt == 2) {
                r = (p & 0xfu) << 4; g = ((p >> 4) & 0xfu) << 4; b = ((p >> 8) & 0xfu) << 4;
            } else {
                r = (p & 0x1fu) << 3; g = ((p >> 5) & 0x3fu) << 2; b = ((p >> 11) & 0x1fu) << 3;
            }
            row[x] = r | (g << 8) | (b << 16) | 0xff000000u;
        }
    }
    framebufferEnd(&s_fb);
}

static void meter_frame(void) {
    if (!s_meter_on) return;
    s_meter_frames++;
    const uint64_t now = SDL_GetTicksNS();
    const uint64_t elapsed = now - s_meter_start_ns;
    if (elapsed < METER_PERIOD_NS) return;
    const uint64_t idle = nk_switch_idle_ns();
    const uint64_t slept = idle - s_meter_start_idle_ns;
    const uint64_t busy = elapsed > slept ? elapsed - slept : 0;
    const double frames = (double)s_meter_frames;
    /* With the worker, the guest thread's time holds neither the GE nor the presenter. */
    const uint64_t other = ge_async_active() ? busy
                           : busy > nk_switch_ge_ns + s_meter_present_ns
                               ? busy - nk_switch_ge_ns - s_meter_present_ns : 0;
    fprintf(stderr, "FRAME_METER fps=%.1f busy_ms=%.1f ge_ms=%.1f present_ms=%.1f logic_ms=%.1f sleep_ms=%.1f\n",
            frames * 1e9 / (double)elapsed, (double)busy / frames / 1e6,
            (double)nk_switch_ge_ns / frames / 1e6,
            (double)s_meter_present_ns / frames / 1e6, (double)other / frames / 1e6,
            (double)slept / frames / 1e6);
    if (ge_async_active()) {
        static uint64_t worker_idle_before;
        const uint64_t worker_idle = ge_async_idle_ns();
        uint64_t join_ns = 0;
        const uint32_t joins = ge_async_take_join_stats(&join_ns);
        fprintf(stderr, "GE_ASYNC worker_idle_ms=%.1f joins=%.1f join_ms=%.1f\n",
                (double)(worker_idle - worker_idle_before) / frames / 1e6, (double)joins / frames,
                (double)join_ns / frames / 1e6);
        worker_idle_before = worker_idle;
    }
    if (s_gpu) {
        DkGeStats st;
        ge_dk_stats_take(&st);
        fprintf(stderr,
                "GE_STATS draws=%u switches=%u tex_up=%u tex_new=%u tex_reval=%u tex_nohash=%u tex_evict=%u snap=%u fb_up=%u fb_rb=%u fb_new=%u "
                "ram_show=%u bad_fb=%u tex_chains=%u approx mask=%u ctest=%u logic=%u mip=%u\n",
                st.draws, st.target_switches, st.tex_uploads, st.tex_new, st.tex_revalidated, st.tex_no_hash, st.tex_evictions, st.snapshots, st.target_uploads,
                st.target_readbacks, st.targets_created, st.present_outside_vram, st.unsupported_fb, st.tex_chains,
                st.approx_write_mask, st.approx_color_test, st.approx_logic_op, st.approx_mip);
        fprintf(stderr, "GE_FRAG full=%u fast=%u at=%u why fmt=%u blend=%u range=%u tf=%u lod=%u atest=%u\n",
                st.frag_draws[0], st.frag_draws[1], st.frag_draws[2], st.frag_full_why[DK_FULL_FORMAT],
                st.frag_full_why[DK_FULL_BLEND], st.frag_full_why[DK_FULL_DEPTH_RANGE],
                st.frag_full_why[DK_FULL_TEXFUNC], st.frag_full_why[DK_FULL_LOD],
                st.frag_full_why[DK_FULL_ALPHA_TEST]);
        static GeCpuProfileStats ge_prof;
        ge_cpu_profile_get(&ge_prof);
        fprintf(stderr,
                "GE_PROF cmd_ms=%.1f prim_ms=%.1f hook_ms=%.1f xfer_ms=%.1f clut_ms=%.1f flush_ms=%.1f "
                "state_ms=%.1f tex_ms=%.1f cmds=%.0f prims=%.0f verts=%.0f hooks=%.0f\n",
                (double)ge_prof.phase[GE_CPU_COMMAND_DISPATCH].ns / frames / 1e6,
                (double)ge_prof.phase[GE_CPU_PRIMITIVE].ns / frames / 1e6,
                (double)ge_prof.phase[GE_CPU_GPU_HOOK].ns / frames / 1e6,
                (double)ge_prof.phase[GE_CPU_BLOCK_TRANSFER].ns / frames / 1e6,
                (double)ge_prof.phase[GE_CPU_CLUT_LOAD].ns / frames / 1e6,
                (double)ge_prof.phase[GE_CPU_FLUSH].ns / frames / 1e6,
                (double)st.state_ns / frames / 1e6, (double)st.tex_ns / frames / 1e6,
                (double)ge_prof.commands / frames, (double)ge_prof.primitive_commands / frames,
                (double)ge_prof.primitive_vertices / frames,
                (double)ge_prof.phase[GE_CPU_GPU_HOOK].calls / frames);
        ge_cpu_profile_reset();
        unsigned long long tnl_taken = 0, tnl_cpu = 0;
        ge_tnl_counts(&tnl_taken, &tnl_cpu, 1);
        fprintf(stderr, "GE_TNL taken=%llu cpu=%llu tris=%u\n", tnl_taken, tnl_cpu, st.tnl_tris);
        fprintf(stderr, "GPU_WAIT gpu_ms=%.1f fence_ms=%.1f acquire_ms=%.1f\n", (double)st.gpu_ns / frames / 1e6,
                (double)st.fence_ns / frames / 1e6, (double)st.acquire_ns / frames / 1e6);
    }
    fflush(stderr);
    s_meter_start_ns = now;
    s_meter_start_idle_ns = idle;
    s_meter_present_ns = 0;
    nk_switch_ge_ns = 0;
    s_meter_frames = 0;
}

/* The flip reaches the worker after every list queued before it. */
void ge_async_worker_present(uint32_t fbaddr, int fmt, uint32_t stride) {
    const uint64_t start = SDL_GetTicksNS();
    ge_dk_present(fbaddr, fmt, stride);
    s_meter_present_ns += SDL_GetTicksNS() - start;
    meter_frame();
}

int gui_present(uint32_t fbaddr, int fmt, uint32_t stride) {
    gui_pump();
    if (!s_on) return 0;
    if (ge_async_active()) return ge_async_present(fbaddr, fmt, stride);
    const uint64_t start = SDL_GetTicksNS();
    if (s_gpu) ge_dk_present(fbaddr, fmt, stride);
    else present_copy(fbaddr, fmt, stride);
    s_meter_present_ns += SDL_GetTicksNS() - start;
    meter_frame();
    return 1;
}
