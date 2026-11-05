/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The SDL3 subset in sdl_shim/SDL3/nk_switch_sdl.h, implemented over libnx. */
#include <stdatomic.h>
#include <stdlib.h>

#include <switch.h>

#include <SDL3/nk_switch_sdl.h>

#define SHIM_THREAD_STACK_BYTES 0x40000
/* The scheduler's timer. Core 0 runs the game and core 2 the GE, at a priority that lets neither
 * take turns with a thread of the same priority, so this shares core 1 with audio, both mostly
 * asleep. */
#define SHIM_THREAD_PRIORITY 0x2b
#define SHIM_THREAD_CORE 1

struct SDL_Mutex { Mutex m; };
struct SDL_Condition { CondVar c; };
struct SDL_Thread {
    Thread t;
    SDL_ThreadFunction fn;
    void *data;
    int status;
};

static _Atomic uint64_t s_idle_ns;

SDL_Mutex *SDL_CreateMutex(void) {
    SDL_Mutex *mutex = calloc(1, sizeof(*mutex));
    if (mutex) mutexInit(&mutex->m);
    return mutex;
}
void SDL_DestroyMutex(SDL_Mutex *mutex) { free(mutex); }
void SDL_LockMutex(SDL_Mutex *mutex) { mutexLock(&mutex->m); }
void SDL_UnlockMutex(SDL_Mutex *mutex) { mutexUnlock(&mutex->m); }

SDL_Condition *SDL_CreateCondition(void) {
    SDL_Condition *cond = calloc(1, sizeof(*cond));
    if (cond) condvarInit(&cond->c);
    return cond;
}
void SDL_DestroyCondition(SDL_Condition *cond) { free(cond); }
void SDL_SignalCondition(SDL_Condition *cond) { condvarWakeOne(&cond->c); }
void SDL_BroadcastCondition(SDL_Condition *cond) { condvarWakeAll(&cond->c); }
bool SDL_WaitCondition(SDL_Condition *cond, SDL_Mutex *mutex) {
    return R_SUCCEEDED(condvarWait(&cond->c, &mutex->m));
}
bool SDL_WaitConditionTimeout(SDL_Condition *cond, SDL_Mutex *mutex, int32_t timeout_ms) {
    if (timeout_ms < 0) return SDL_WaitCondition(cond, mutex);
    return R_SUCCEEDED(condvarWaitTimeout(&cond->c, &mutex->m, (uint64_t)timeout_ms * 1000000ull));
}

static void shim_thread_entry(void *arg) {
    SDL_Thread *thread = arg;
    thread->status = thread->fn(thread->data);
}

SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data) {
    (void)name;
    SDL_Thread *thread = calloc(1, sizeof(*thread));
    if (!thread) return NULL;
    thread->fn = fn;
    thread->data = data;
    if (R_FAILED(threadCreate(&thread->t, shim_thread_entry, thread, NULL, SHIM_THREAD_STACK_BYTES,
                              SHIM_THREAD_PRIORITY, SHIM_THREAD_CORE)) ||
        R_FAILED(threadStart(&thread->t))) {
        free(thread);
        return NULL;
    }
    return thread;
}
/* A detached thread lives until the process ends, so its record is kept. */
void SDL_DetachThread(SDL_Thread *thread) { (void)thread; }
void SDL_WaitThread(SDL_Thread *thread, int *status) {
    if (!thread) return;
    threadWaitForExit(&thread->t);
    threadClose(&thread->t);
    if (status) *status = thread->status;
    free(thread);
}

uint64_t SDL_GetTicksNS(void) { return armTicksToNs(armGetSystemTick()); }
uint64_t SDL_GetTicks(void) { return SDL_GetTicksNS() / 1000000ull; }

void SDL_DelayNS(uint64_t ns) {
    uint64_t start = SDL_GetTicksNS();
    svcSleepThread((int64_t)ns);
    /* Only the game's own thread: the frame meter subtracts this from wall-clock time. */
    if (threadGetCurHandle() == envGetMainThreadHandle())
        atomic_fetch_add(&s_idle_ns, SDL_GetTicksNS() - start);
}
void SDL_DelayPrecise(uint64_t ns) { SDL_DelayNS(ns); }
void SDL_Delay(uint32_t ms) { SDL_DelayNS((uint64_t)ms * 1000000ull); }

const char *SDL_GetCurrentVideoDriver(void) { return "switch"; }
const char *SDL_GetError(void) { return "libnx"; }

uint64_t nk_switch_idle_ns(void) { return atomic_load(&s_idle_ns); }

/* The running thread's stack bounds, for sr_coro's main-stack check. */
int nk_switch_thread_stack(void **stack_addr, size_t *stack_size) {
    Thread *self = threadGetSelf();
    if (!self || !self->stack_mirror || !self->stack_sz) return 0;
    *stack_addr = self->stack_mirror;
    *stack_size = self->stack_sz;
    return 1;
}
