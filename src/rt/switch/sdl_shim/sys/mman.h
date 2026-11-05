/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * What sr_coro.c needs from mmap, on a host without it: coroutine stacks from the heap. The
 * guard page has no hardware protection here, because homebrew cannot change its heap's mapping
 * through this interface, so a stack overflow is not trapped.
 */
#ifndef NK_SWITCH_SYS_MMAN_H
#define NK_SWITCH_SYS_MMAN_H

#include <malloc.h>
#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_STACK 0
#define MAP_NORESERVE 0
#define MAP_FAILED ((void *)-1)

static inline void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr; (void)prot; (void)flags; (void)fd; (void)offset;
    void *block = memalign(0x1000, length);
    return block ? block : MAP_FAILED;
}
static inline int munmap(void *addr, size_t length) {
    (void)length;
    free(addr);
    return 0;
}
static inline int mprotect(void *addr, size_t length, int prot) {
    (void)addr; (void)length; (void)prot;
    return 0;
}

#endif
