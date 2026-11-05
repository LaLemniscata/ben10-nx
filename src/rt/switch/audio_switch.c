/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The sr_audio_* host backend for the Nintendo Switch. Each PSP channel owns a queue of 44.1 kHz
 * stereo frames. A thread on its own core mixes them, resamples the mix to the 48 kHz output with
 * a four-point curve, and submits blocks to audout.
 */
#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

#define AUDIO_CHANNELS 9
#define QUEUE_FRAMES (1u << 17)
#define BLOCK_FRAMES 1024u
#define BLOCK_BYTES (BLOCK_FRAMES * 4u)
#define BLOCK_BUFFER_BYTES ((BLOCK_BYTES + 0xfffu) & ~0xfffu)
#define BLOCK_COUNT 3
#define INPUT_RATE 44100u
#define OUTPUT_RATE 48000u
#define MIX_FRAMES (BLOCK_FRAMES * INPUT_RATE / OUTPUT_RATE + 2u)
#define HISTORY_FRAMES 4
#define AUDIO_THREAD_STACK_BYTES 0x8000
#define AUDIO_THREAD_PRIORITY 0x2b
#define AUDIO_THREAD_CORE 1

typedef struct {
    int16_t *frames;
    uint32_t read;
    uint32_t count;
} ChannelQueue;

static ChannelQueue s_queues[AUDIO_CHANNELS];
static Mutex s_lock;
static Thread s_thread;
static AudioOutBuffer s_blocks[BLOCK_COUNT];
static void *s_block_memory[BLOCK_COUNT];
static int32_t s_mix[MIX_FRAMES * 2u];
static float s_history[HISTORY_FRAMES][2];    /* the output falls between entries 1 and 2 */
static uint32_t s_phase;                      /* position between input frames, in OUTPUT_RATE units */
static int s_active;
static int s_paused;
static volatile int s_stop;

static int16_t clamp16(int32_t value) {
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

/* Take `frames` input frames from every channel into s_mix. A channel with too few leaves silence
 * after what it had. */
static void mix_input_frames(uint32_t frames) {
    memset(s_mix, 0, frames * 2u * sizeof(s_mix[0]));
    mutexLock(&s_lock);
    for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
        ChannelQueue *q = &s_queues[ch];
        const uint32_t take = frames < q->count ? frames : q->count;
        for (uint32_t i = 0; i < take; i++) {
            const int16_t *frame = &q->frames[q->read * 2u];
            s_mix[i * 2u] += frame[0];
            s_mix[i * 2u + 1u] += frame[1];
            q->read = (q->read + 1u) & (QUEUE_FRAMES - 1u);
        }
        q->count -= take;
    }
    mutexUnlock(&s_lock);
}

/* The Catmull-Rom curve through b and c. */
static float curve(float a, float b, float c, float d, float t) {
    const float c1 = 0.5f * (c - a);
    const float c2 = a - 2.5f * b + 2.0f * c - 0.5f * d;
    const float c3 = 0.5f * (d - a) + 1.5f * (b - c);
    return ((c3 * t + c2) * t + c1) * t + b;
}

static void fill_block(int16_t *out) {
    if (s_paused) {
        memset(out, 0, BLOCK_BYTES);
        return;
    }
    mix_input_frames((uint32_t)(((uint64_t)s_phase + (uint64_t)BLOCK_FRAMES * INPUT_RATE) / OUTPUT_RATE));
    uint32_t next = 0;
    for (uint32_t i = 0; i < BLOCK_FRAMES; i++) {
        const float t = (float)s_phase / (float)OUTPUT_RATE;
        for (int side = 0; side < 2; side++) {
            const float value = curve(s_history[0][side], s_history[1][side], s_history[2][side], s_history[3][side], t);
            out[i * 2u + (uint32_t)side] = clamp16((int32_t)lrintf(value));
        }
        s_phase += INPUT_RATE;
        if (s_phase >= OUTPUT_RATE) {
            s_phase -= OUTPUT_RATE;
            memmove(s_history[0], s_history[1], sizeof(s_history[0]) * (HISTORY_FRAMES - 1));
            s_history[HISTORY_FRAMES - 1][0] = (float)s_mix[next * 2u];
            s_history[HISTORY_FRAMES - 1][1] = (float)s_mix[next * 2u + 1u];
            next++;
        }
    }
}

static void audio_thread(void *arg) {
    (void)arg;
    while (!s_stop) {
        AudioOutBuffer *released = NULL;
        u32 released_count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &released_count, 100000000ull))) continue;
        while (released) {
            AudioOutBuffer *next = released->next;
            fill_block((int16_t *)released->buffer);
            armDCacheFlush(released->buffer, BLOCK_BYTES);
            released->next = NULL;
            audoutAppendAudioOutBuffer(released);
            released = next;
        }
    }
}

int sr_audio_init(void) {
    if (s_active) return 0;
    mutexInit(&s_lock);
    for (int ch = 0; ch < AUDIO_CHANNELS; ch++) {
        s_queues[ch].frames = malloc(QUEUE_FRAMES * 4u);
        if (!s_queues[ch].frames) return -1;
    }
    if (R_FAILED(audoutInitialize()) || R_FAILED(audoutStartAudioOut())) {
        fprintf(stderr, "audio_switch: audout unavailable\n");
        return -1;
    }
    for (int i = 0; i < BLOCK_COUNT; i++) {
        s_block_memory[i] = memalign(0x1000, BLOCK_BUFFER_BYTES);
        if (!s_block_memory[i]) return -1;
        memset(s_block_memory[i], 0, BLOCK_BUFFER_BYTES);
        armDCacheFlush(s_block_memory[i], BLOCK_BUFFER_BYTES);
        s_blocks[i].next = NULL;
        s_blocks[i].buffer = s_block_memory[i];
        s_blocks[i].buffer_size = BLOCK_BUFFER_BYTES;
        s_blocks[i].data_size = BLOCK_BYTES;
        s_blocks[i].data_offset = 0;
        audoutAppendAudioOutBuffer(&s_blocks[i]);
    }
    if (R_FAILED(threadCreate(&s_thread, audio_thread, NULL, NULL, AUDIO_THREAD_STACK_BYTES,
                              AUDIO_THREAD_PRIORITY, AUDIO_THREAD_CORE)) ||
        R_FAILED(threadStart(&s_thread))) {
        fprintf(stderr, "audio_switch: thread start failed\n");
        return -1;
    }
    s_active = 1;
    fprintf(stderr, "audio_switch: audout 48000Hz stereo\n");
    return 0;
}

void sr_audio_push(int ch, const int16_t *lr, int nframes, int volL, int volR) {
    if (ch < 0 || ch >= AUDIO_CHANNELS || !lr || nframes <= 0) return;
    if (!s_active && sr_audio_init() != 0) return;
    ChannelQueue *q = &s_queues[ch];
    mutexLock(&s_lock);
    uint32_t write = (q->read + q->count) & (QUEUE_FRAMES - 1u);
    for (int i = 0; i < nframes && q->count < QUEUE_FRAMES; i++) {
        q->frames[write * 2u] = clamp16(((int32_t)lr[i * 2] * volL) >> 15);
        q->frames[write * 2u + 1u] = clamp16(((int32_t)lr[i * 2 + 1] * volR) >> 15);
        write = (write + 1u) & (QUEUE_FRAMES - 1u);
        q->count++;
    }
    mutexUnlock(&s_lock);
}

int sr_audio_queued(int ch) {
    if (ch < 0 || ch >= AUDIO_CHANNELS) return -1;
    if (!s_active && sr_audio_init() != 0) return -1;
    mutexLock(&s_lock);
    const int frames = (int)s_queues[ch].count;
    mutexUnlock(&s_lock);
    return frames;
}

void sr_audio_pause_enter(void) { s_paused = 1; }
void sr_audio_pause_leave(void) { s_paused = 0; }
int sr_audio_is_active(void) { return s_active; }
int sr_audio_pause_script_ready(void) { return 0; }
void sr_audio_dump_stats(void) {}
void sr_audio_dump_finish(void) {}
