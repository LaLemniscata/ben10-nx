/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * A sampling profiler for the Nintendo Switch, on when an empty profile.txt sits beside the game.
 * A thread on the audio core pauses the game's thread and the GE worker for a moment, reads where
 * each one is, and appends that to profile.bin. Each sample is two little-endian words: the program
 * counter and the link register, as offsets from the start of the program, with the top bit of the
 * first word set for the GE worker. A program's symbol table turns them into function names.
 */
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <switch.h>

#include "ge_async.h"
#include "prof_switch.h"

#define SAMPLE_PERIOD_NS 2000000
#define BUFFER_WORDS (1u << 14)
#define THREAD_STACK_BYTES 0x4000
#define THREAD_PRIORITY 0x2a
#define THREAD_CORE 1
#define WORKER_BIT 0x80000000u

extern char __start__;

static Thread s_thread;
static int s_running;
static volatile int s_stop;
static FILE *s_file;
static uint32_t s_words[BUFFER_WORDS];
static uint32_t s_count;

/* Nothing here may take a lock the paused thread could hold: no allocation and no output. */
static void sample(Handle thread, uint32_t tag) {
    if (R_FAILED(svcSetThreadActivity(thread, ThreadActivity_Paused))) return;
    ThreadContext context;
    const Result rc = svcGetThreadContext3(&context, thread);
    svcSetThreadActivity(thread, ThreadActivity_Runnable);
    if (R_FAILED(rc)) return;
    const uintptr_t base = (uintptr_t)&__start__;
    s_words[s_count++] = (uint32_t)((context.pc.x - base) & ~(uint64_t)WORKER_BIT) | tag;
    s_words[s_count++] = (uint32_t)(context.lr - base);
}

static void buffer_write(void) {
    if (!s_count) return;
    fwrite(s_words, sizeof(s_words[0]), s_count, s_file);
    fflush(s_file);
    s_count = 0;
}

static void sampler_main(void *arg) {
    (void)arg;
    const Handle game = envGetMainThreadHandle();
    while (!s_stop) {
        svcSleepThread(SAMPLE_PERIOD_NS);
        sample(game, 0u);
        if (ge_async_active()) sample(ge_async_worker_handle(), WORKER_BIT);
        if (s_count + 4u > BUFFER_WORDS) buffer_write();
    }
    buffer_write();
}

void prof_switch_start(void) {
    if (s_running || access("profile.txt", F_OK) != 0) return;
    s_file = fopen("profile.bin", "wb");
    if (!s_file) return;
    if (R_FAILED(threadCreate(&s_thread, sampler_main, NULL, NULL, THREAD_STACK_BYTES, THREAD_PRIORITY, THREAD_CORE)) ||
        R_FAILED(threadStart(&s_thread))) {
        fclose(s_file);
        return;
    }
    s_running = 1;
    fprintf(stderr, "prof_switch: sampling every %d us\n", SAMPLE_PERIOD_NS / 1000);
}

void prof_switch_stop(void) {
    if (!s_running) return;
    s_stop = 1;
    threadWaitForExit(&s_thread);
    threadClose(&s_thread);
    fclose(s_file);
    s_running = 0;
}
