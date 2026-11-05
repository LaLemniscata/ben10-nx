/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The GE on its own CPU core, for the Nintendo Switch. On a PSP the GE is a separate processor:
 * a list runs while the game goes on. Here one worker thread owns ge.c's execution and the
 * renderer, and takes its work from a queue in submission order, so a frame costs the longer of
 * the game and the GE instead of their sum.
 *
 * Everything except ge_async_worker_* runs on the guest thread. While the worker is idle after
 * ge_async_join, the guest thread may use ge.c and the renderer directly.
 */
#ifndef NK_SWITCH_GE_ASYNC_H
#define NK_SWITCH_GE_ASYNC_H

#include <stdint.h>

/* Start the worker. Until then, and if this fails, every call below reports it inactive. */
int ge_async_start(void);
/* Finish the queued work and stop taking new work. */
void ge_async_stop(void);
int ge_async_active(void);
/* 1 on the worker thread. */
int ge_async_on_worker(void);
/* The worker's kernel handle, for the sampling profiler. */
uint32_t ge_async_worker_handle(void);

/* A list starts at `pc`: the worker forgets what it knew about the slot's previous list. */
void ge_async_list_begin(int slot, uint32_t pc);
/* Run a list on from where it stopped, up to `stall` (0: to its END). */
void ge_async_run(int slot, int resume, uint32_t stall);
/* The worker reached a list's END. Returns 0 when no such report is waiting. */
int ge_async_pop_result(int *slot, uint32_t *finish_id);
/* The address a stopped list continues from. Only meaningful after ge_async_join. */
uint32_t ge_async_list_pc(int slot);

/* Wait until the worker has done everything queued so far. */
void ge_async_join(void);
/* Queue a CPU write notice. Returns 0 when the caller must deliver it itself. */
int ge_async_post_dirty(uint32_t addr, uint32_t bytes);
/* Queue a display flip. Returns 0 when the frame was dropped because the GE is two flips behind. */
int ge_async_present(uint32_t fbaddr, int fmt, uint32_t stride);
/* When the scheduler should next look for a finished list: 0 now, UINT64_MAX never. */
uint64_t ge_async_next_deadline_us(uint64_t now_us);

/* Time the worker has spent idle, for the frame meter. */
uint64_t ge_async_idle_ns(void);
/* Times the guest thread had to wait for the worker, and for how long. */
uint32_t ge_async_take_join_stats(uint64_t *wait_ns);

/* The worker's view of a flip, implemented by the presenter. */
void ge_async_worker_present(uint32_t fbaddr, int fmt, uint32_t stride);

#endif
