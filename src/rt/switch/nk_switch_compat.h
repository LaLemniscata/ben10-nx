/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Force-included in every C file of the Switch build: POSIX names newlib on this host lacks.
 * Only one process ever uses the SD folder, so an advisory file lock always succeeds.
 */
#ifndef NK_SWITCH_COMPAT_H
#define NK_SWITCH_COMPAT_H

/* The program is linked statically, so nothing can replace a symbol at load time. Saying so spares
 * every access to a global variable a load through the global offset table. */
#pragma GCC visibility push(hidden)

#ifndef LOCK_SH
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
static inline int flock(int fd, int operation) {
    (void)fd;
    (void)operation;
    return 0;
}
#endif

#endif
