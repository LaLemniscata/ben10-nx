/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The small part of SDL3 the portable runtime calls, for the Nintendo Switch host. devkitPro has
 * no SDL3, and the runtime uses it here only for time, sleeping, locks and one timer thread. The
 * types stay opaque so runtime files never see libnx's global names.
 */
#ifndef NK_SWITCH_SDL_H
#define NK_SWITCH_SDL_H

#include <stdbool.h>
#include <stdint.h>

#define SDLCALL

typedef struct SDL_Mutex SDL_Mutex;
typedef struct SDL_Condition SDL_Condition;
typedef struct SDL_Thread SDL_Thread;
typedef int (*SDL_ThreadFunction)(void *data);

SDL_Mutex *SDL_CreateMutex(void);
void SDL_DestroyMutex(SDL_Mutex *mutex);
void SDL_LockMutex(SDL_Mutex *mutex);
void SDL_UnlockMutex(SDL_Mutex *mutex);

SDL_Condition *SDL_CreateCondition(void);
void SDL_DestroyCondition(SDL_Condition *cond);
void SDL_SignalCondition(SDL_Condition *cond);
void SDL_BroadcastCondition(SDL_Condition *cond);
bool SDL_WaitCondition(SDL_Condition *cond, SDL_Mutex *mutex);
bool SDL_WaitConditionTimeout(SDL_Condition *cond, SDL_Mutex *mutex, int32_t timeout_ms);

SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data);
void SDL_DetachThread(SDL_Thread *thread);
void SDL_WaitThread(SDL_Thread *thread, int *status);

uint64_t SDL_GetTicksNS(void);
uint64_t SDL_GetTicks(void);
void SDL_DelayNS(uint64_t ns);
void SDL_DelayPrecise(uint64_t ns);
void SDL_Delay(uint32_t ms);

const char *SDL_GetCurrentVideoDriver(void);
const char *SDL_GetError(void);

/* Nanoseconds this process spent in SDL_Delay*: the frame meter subtracts it to get busy time. */
uint64_t nk_switch_idle_ns(void);

#endif
