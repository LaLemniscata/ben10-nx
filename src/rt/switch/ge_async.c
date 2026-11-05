/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdatomic.h>
#include <stdio.h>

#include <switch.h>

#include <SDL3/nk_switch_sdl.h>

#include "ge_async.h"

extern uint32_t g_ge_stall_addr;
uint32_t ge_run_list(uint32_t addr, int resume);
uint32_t ge_last_finish_id(void);
void sr_gpu_vram_dirty_now(uint32_t addr, uint32_t bytes);

#define JOB_CAPACITY (1u << 14)
#define RESULT_CAPACITY 256u
#define SLOT_COUNT 64
#define WORKER_STACK_BYTES 0x200000
#define WORKER_PRIORITY 0x2c
#define WORKER_CORE 2
#define WORKER_SPIN_NS 50000u
#define FULL_QUEUE_WAIT_NS 100000
#define JOIN_WAIT_NS 20000
#define POLL_INTERVAL_US 500u
#define MAX_FLIPS_BEHIND 2u

typedef enum { JOB_LIST_BEGIN, JOB_RUN, JOB_DIRTY, JOB_PRESENT } JobType;

typedef struct {
    uint32_t type, a, b, c;
} Job;

typedef struct {
    int slot;
    uint32_t finish_id;
} ListResult;

static Job s_jobs[JOB_CAPACITY];
static _Atomic uint32_t s_job_head, s_job_tail;   /* consumed by the worker / published by the guest */
static ListResult s_results[RESULT_CAPACITY];
static _Atomic uint32_t s_result_head, s_result_tail;
static _Atomic uint32_t s_runs_pending, s_flips_pending;

static Thread s_worker;
static Handle s_worker_handle;
static int s_active;
static Mutex s_sleep_lock;
static CondVar s_sleep_cond;
static _Atomic int s_worker_sleeping;
static _Atomic uint64_t s_idle_ns;

/* Worker-owned: where each list has got to. */
static uint32_t s_list_pc[SLOT_COUNT];
static uint8_t s_list_ended[SLOT_COUNT];

/* Guest-owned: a run of adjacent CPU writes becomes one notice, which keeps a word-by-word copy
 * into VRAM from filling the queue. */
static uint32_t s_dirty_start, s_dirty_end;
static uint64_t s_join_wait_ns;
static uint32_t s_join_count;

int ge_async_active(void) { return s_active; }
int ge_async_on_worker(void) { return s_active && threadGetCurHandle() == s_worker_handle; }
uint32_t ge_async_worker_handle(void) { return s_worker_handle; }

static void worker_run(const Job *job) {
    const int slot = (int)job->a;
    if (!s_list_ended[slot]) {
        g_ge_stall_addr = job->c;
        const uint64_t started = SDL_GetTicksNS();
        const uint32_t next = ge_run_list(s_list_pc[slot], (int)job->b);
        extern uint64_t nk_switch_ge_ns;
        nk_switch_ge_ns += SDL_GetTicksNS() - started;
        g_ge_stall_addr = 0;
        s_list_pc[slot] = next;
        if (next == 0) {
            s_list_ended[slot] = 1;
            const uint32_t tail = atomic_load_explicit(&s_result_tail, memory_order_relaxed);
            /* At most one report per slot can be waiting for the guest. */
            s_results[tail % RESULT_CAPACITY] = (ListResult){slot, ge_last_finish_id()};
            atomic_store_explicit(&s_result_tail, tail + 1u, memory_order_release);
        }
    }
    atomic_fetch_sub_explicit(&s_runs_pending, 1u, memory_order_release);
}

static void worker_do(const Job *job) {
    switch (job->type) {
        case JOB_LIST_BEGIN:
            s_list_pc[job->a] = job->b;
            s_list_ended[job->a] = 0;
            break;
        case JOB_RUN:
            worker_run(job);
            break;
        case JOB_DIRTY:
            sr_gpu_vram_dirty_now(job->a, job->b);
            break;
        case JOB_PRESENT:
            ge_async_worker_present(job->a, (int)job->b, job->c);
            atomic_fetch_sub_explicit(&s_flips_pending, 1u, memory_order_release);
            break;
    }
}

static void worker_wait_for_job(uint32_t head) {
    const uint64_t started = SDL_GetTicksNS();
    /* A short spin: a new job usually follows at once, and a wake-up costs a system call. */
    while (atomic_load_explicit(&s_job_tail, memory_order_acquire) == head) {
        if (SDL_GetTicksNS() - started < WORKER_SPIN_NS) continue;
        mutexLock(&s_sleep_lock);
        atomic_store_explicit(&s_worker_sleeping, 1, memory_order_seq_cst);
        atomic_thread_fence(memory_order_seq_cst);
        if (atomic_load_explicit(&s_job_tail, memory_order_seq_cst) == head)
            condvarWait(&s_sleep_cond, &s_sleep_lock);
        atomic_store_explicit(&s_worker_sleeping, 0, memory_order_seq_cst);
        mutexUnlock(&s_sleep_lock);
    }
    atomic_fetch_add_explicit(&s_idle_ns, SDL_GetTicksNS() - started, memory_order_relaxed);
}

static void worker_main(void *arg) {
    (void)arg;
    for (;;) {
        const uint32_t head = atomic_load_explicit(&s_job_head, memory_order_relaxed);
        if (atomic_load_explicit(&s_job_tail, memory_order_acquire) == head) worker_wait_for_job(head);
        const Job job = s_jobs[head % JOB_CAPACITY];
        worker_do(&job);
        /* Only now: a join must not return while the job is still running. */
        atomic_store_explicit(&s_job_head, head + 1u, memory_order_release);
    }
}

static void job_push_raw(const Job *job) {
    const uint32_t tail = atomic_load_explicit(&s_job_tail, memory_order_relaxed);
    while (tail - atomic_load_explicit(&s_job_head, memory_order_acquire) >= JOB_CAPACITY)
        svcSleepThread(FULL_QUEUE_WAIT_NS);
    s_jobs[tail % JOB_CAPACITY] = *job;
    atomic_store_explicit(&s_job_tail, tail + 1u, memory_order_seq_cst);
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&s_worker_sleeping, memory_order_seq_cst)) {
        mutexLock(&s_sleep_lock);
        condvarWakeOne(&s_sleep_cond);
        mutexUnlock(&s_sleep_lock);
    }
}

static void dirty_flush(void) {
    if (s_dirty_end == s_dirty_start) return;
    const Job job = {JOB_DIRTY, s_dirty_start, s_dirty_end - s_dirty_start, 0};
    s_dirty_start = s_dirty_end = 0;
    job_push_raw(&job);
}

static void job_push(const Job *job) {
    dirty_flush();
    job_push_raw(job);
}

int ge_async_start(void) {
    if (s_active) return 1;
    mutexInit(&s_sleep_lock);
    condvarInit(&s_sleep_cond);
    if (R_FAILED(threadCreate(&s_worker, worker_main, NULL, NULL, WORKER_STACK_BYTES, WORKER_PRIORITY, WORKER_CORE)))
        return 0;
    s_worker_handle = s_worker.handle;
    if (R_FAILED(threadStart(&s_worker))) {
        threadClose(&s_worker);
        return 0;
    }
    s_active = 1;
    fprintf(stderr, "ge_async: GE worker on core %d\n", WORKER_CORE);
    return 1;
}

void ge_async_stop(void) {
    if (!s_active) return;
    ge_async_join();
    s_active = 0;
}

void ge_async_list_begin(int slot, uint32_t pc) {
    const Job job = {JOB_LIST_BEGIN, (uint32_t)slot, pc, 0};
    job_push(&job);
}

void ge_async_run(int slot, int resume, uint32_t stall) {
    atomic_fetch_add_explicit(&s_runs_pending, 1u, memory_order_relaxed);
    const Job job = {JOB_RUN, (uint32_t)slot, (uint32_t)resume, stall};
    job_push(&job);
}

int ge_async_pop_result(int *slot, uint32_t *finish_id) {
    const uint32_t head = atomic_load_explicit(&s_result_head, memory_order_relaxed);
    if (atomic_load_explicit(&s_result_tail, memory_order_acquire) == head) return 0;
    *slot = s_results[head % RESULT_CAPACITY].slot;
    *finish_id = s_results[head % RESULT_CAPACITY].finish_id;
    atomic_store_explicit(&s_result_head, head + 1u, memory_order_release);
    return 1;
}

uint32_t ge_async_list_pc(int slot) { return s_list_pc[slot]; }

void ge_async_join(void) {
    if (!s_active || ge_async_on_worker()) return;
    dirty_flush();
    const uint32_t tail = atomic_load_explicit(&s_job_tail, memory_order_relaxed);
    if (atomic_load_explicit(&s_job_head, memory_order_acquire) == tail) return;
    const uint64_t started = SDL_GetTicksNS();
    while (atomic_load_explicit(&s_job_head, memory_order_acquire) != tail) svcSleepThread(JOIN_WAIT_NS);
    s_join_wait_ns += SDL_GetTicksNS() - started;
    s_join_count++;
}

int ge_async_post_dirty(uint32_t addr, uint32_t bytes) {
    if (!s_active || ge_async_on_worker()) return 0;
    if (s_dirty_end != s_dirty_start && addr == s_dirty_end && bytes < 0x40000000u) {
        s_dirty_end += bytes;
        return 1;
    }
    dirty_flush();
    s_dirty_start = addr;
    s_dirty_end = addr + bytes;
    return 1;
}

int ge_async_present(uint32_t fbaddr, int fmt, uint32_t stride) {
    if (atomic_load_explicit(&s_flips_pending, memory_order_acquire) >= MAX_FLIPS_BEHIND) return 0;
    atomic_fetch_add_explicit(&s_flips_pending, 1u, memory_order_relaxed);
    const Job job = {JOB_PRESENT, fbaddr, (uint32_t)fmt, stride};
    job_push(&job);
    return 1;
}

uint64_t ge_async_next_deadline_us(uint64_t now_us) {
    if (atomic_load_explicit(&s_result_tail, memory_order_acquire) !=
        atomic_load_explicit(&s_result_head, memory_order_relaxed))
        return 0;
    /* A running list will report, and the worker cannot wake the scheduler itself. */
    if (atomic_load_explicit(&s_runs_pending, memory_order_acquire)) return now_us + POLL_INTERVAL_US;
    return UINT64_MAX;
}

uint64_t ge_async_idle_ns(void) { return atomic_load_explicit(&s_idle_ns, memory_order_relaxed); }

uint32_t ge_async_take_join_stats(uint64_t *wait_ns) {
    const uint32_t count = s_join_count;
    *wait_ns = s_join_wait_ns;
    s_join_count = 0;
    s_join_wait_ns = 0;
    return count;
}
