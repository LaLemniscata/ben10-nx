/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * POSIX calls the runtime's contained file layer uses and newlib lacks on the Switch. The *at()
 * family works from a table of directory descriptors, each holding that directory's path. The
 * SD card has no symbolic links, so resolving by path gives the same answer as resolving by
 * descriptor. The SD layer cannot open a directory at all, so the calls that take or hand out a
 * descriptor are linked with --wrap and see the table.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <switch.h>

#define DIR_FD_FIRST 100000
#define DIR_FD_COUNT 64
#define DIR_STREAM_COUNT 16
/* What a Memory Stick uses, and the PSP-visible size of a cluster. */
#define SD_REPORTED_BLOCK_BYTES 0x8000u

static char s_dir_paths[DIR_FD_COUNT][PATH_MAX];
static unsigned char s_dir_used[DIR_FD_COUNT];
static struct { DIR *dir; int fd; } s_streams[DIR_STREAM_COUNT];

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);
int __real_dup(int fd);
int __real_fstat(int fd, struct stat *st);
int __real_fsync(int fd);
int __real_closedir(DIR *dir);

static const char *dir_fd_path(int fd) {
    const int i = fd - DIR_FD_FIRST;
    if (i < 0 || i >= DIR_FD_COUNT || !s_dir_used[i]) return NULL;
    return s_dir_paths[i];
}

static int dir_fd_open(const char *path) {
    for (int i = 0; i < DIR_FD_COUNT; i++) {
        if (s_dir_used[i]) continue;
        if (strlen(path) >= sizeof(s_dir_paths[i])) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(s_dir_paths[i], path);
        s_dir_used[i] = 1;
        return DIR_FD_FIRST + i;
    }
    errno = EMFILE;
    return -1;
}

/* The path a descriptor-relative name stands for. Returns 0 and sets errno on failure. */
static int at_path(int fd, const char *name, char *out, size_t cap) {
    int n;
    if (fd == AT_FDCWD || name[0] == '/' || strchr(name, ':')) {
        n = snprintf(out, cap, "%s", name);
    } else {
        const char *base = dir_fd_path(fd);
        if (!base) {
            errno = EBADF;
            return 0;
        }
        n = name[0] ? snprintf(out, cap, "%s/%s", base, name) : snprintf(out, cap, "%s", base);
    }
    if (n < 0 || (size_t)n >= cap) {
        errno = ENAMETOOLONG;
        return 0;
    }
    return 1;
}

static int open_path(const char *path, int flags, int mode) {
    struct stat st;
    const int exists = stat(path, &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) {
        if (flags & (O_WRONLY | O_RDWR)) {
            errno = EISDIR;
            return -1;
        }
        return dir_fd_open(path);
    }
    if (flags & O_DIRECTORY) {
        errno = exists ? ENOTDIR : ENOENT;
        return -1;
    }
    if (exists && (flags & O_CREAT) && (flags & O_EXCL)) {
        errno = EEXIST;
        return -1;
    }
    return __real_open(path, flags & ~(O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC), mode);
}

int __wrap_open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    return open_path(path, flags, mode);
}

int openat(int fd, const char *name, int flags, ...) {
    char path[PATH_MAX];
    if (!at_path(fd, name, path, sizeof(path))) return -1;
    int mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    return open_path(path, flags, mode);
}

int __wrap_dup(int fd) {
    const char *path = dir_fd_path(fd);
    return path ? dir_fd_open(path) : __real_dup(fd);
}

int fstatat(int fd, const char *name, struct stat *st, int flags) {
    (void)flags;
    char path[PATH_MAX];
    if (!at_path(fd, name, path, sizeof(path))) return -1;
    return stat(path, st);
}

int mkdirat(int fd, const char *name, mode_t mode) {
    char path[PATH_MAX];
    if (!at_path(fd, name, path, sizeof(path))) return -1;
    return mkdir(path, mode);
}

int unlinkat(int fd, const char *name, int flags) {
    char path[PATH_MAX];
    if (!at_path(fd, name, path, sizeof(path))) return -1;
    return (flags & AT_REMOVEDIR) ? rmdir(path) : unlink(path);
}

/* The SD layer refuses to rename onto an existing name, where POSIX replaces it. The old file
 * stays aside until the new one has its name, so a failed rename loses nothing. */
int renameat(int old_fd, const char *old_name, int new_fd, const char *new_name) {
    char old_path[PATH_MAX];
    char new_path[PATH_MAX];
    char aside[PATH_MAX];
    if (!at_path(old_fd, old_name, old_path, sizeof(old_path)) ||
        !at_path(new_fd, new_name, new_path, sizeof(new_path))) {
        return -1;
    }
    if (strcmp(old_path, new_path) == 0) return 0;
    struct stat st;
    if (stat(new_path, &st) != 0 || !S_ISREG(st.st_mode)) return rename(old_path, new_path);
    if (snprintf(aside, sizeof(aside), "%s.replaced", new_path) >= (int)sizeof(aside)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    unlink(aside);
    if (rename(new_path, aside) != 0) return -1;
    if (rename(old_path, new_path) != 0) {
        const int err = errno;
        rename(aside, new_path);
        errno = err;
        return -1;
    }
    unlink(aside);
    return 0;
}

/* The directory stream owns its descriptor, as fdopendir specifies: closedir releases it. */
DIR *fdopendir(int fd) {
    const char *path = dir_fd_path(fd);
    if (!path) {
        errno = EBADF;
        return NULL;
    }
    DIR *dir = opendir(path);
    if (!dir) return NULL;
    for (int i = 0; i < DIR_STREAM_COUNT; i++) {
        if (s_streams[i].dir) continue;
        s_streams[i].dir = dir;
        s_streams[i].fd = fd;
        return dir;
    }
    __real_closedir(dir);
    errno = EMFILE;
    return NULL;
}

int dirfd(DIR *dir) {
    for (int i = 0; i < DIR_STREAM_COUNT; i++) {
        if (s_streams[i].dir == dir) return s_streams[i].fd;
    }
    errno = EINVAL;
    return -1;
}

int __wrap_close(int fd) {
    const int i = fd - DIR_FD_FIRST;
    if (i >= 0 && i < DIR_FD_COUNT) {
        if (!s_dir_used[i]) {
            errno = EBADF;
            return -1;
        }
        s_dir_used[i] = 0;
        return 0;
    }
    return __real_close(fd);
}

int __wrap_closedir(DIR *dir) {
    for (int i = 0; i < DIR_STREAM_COUNT; i++) {
        if (s_streams[i].dir != dir) continue;
        s_streams[i].dir = NULL;
        __wrap_close(s_streams[i].fd);
        break;
    }
    return __real_closedir(dir);
}

/* Free space belongs to the whole card, and the SD layer answers for its root. */
int __wrap_statvfs(const char *path, struct statvfs *out) {
    (void)path;
    FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
    s64 free_bytes = 0;
    s64 total_bytes = 0;
    if (!fs || R_FAILED(fsFsGetFreeSpace(fs, "/", &free_bytes)) ||
        R_FAILED(fsFsGetTotalSpace(fs, "/", &total_bytes))) {
        errno = EIO;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->f_bsize = SD_REPORTED_BLOCK_BYTES;
    out->f_frsize = SD_REPORTED_BLOCK_BYTES;
    out->f_blocks = (fsblkcnt_t)((u64)total_bytes / SD_REPORTED_BLOCK_BYTES);
    out->f_bfree = (fsblkcnt_t)((u64)free_bytes / SD_REPORTED_BLOCK_BYTES);
    out->f_bavail = out->f_bfree;
    return 0;
}

int __wrap_fstat(int fd, struct stat *st) {
    const char *path = dir_fd_path(fd);
    return path ? stat(path, st) : __real_fstat(fd, st);
}

int __wrap_fsync(int fd) {
    return dir_fd_path(fd) ? 0 : __real_fsync(fd);
}

/* Timestamps on saved files are cosmetic, and the SD layer does not set them by descriptor. */
int futimens(int fd, const struct timespec times[2]) {
    (void)fd;
    (void)times;
    return 0;
}

int posix_memalign(void **out, size_t alignment, size_t size) {
    void *block = memalign(alignment, size);
    if (!block) return ENOMEM;
    *out = block;
    return 0;
}

long sysconf(int name) {
    if (name == _SC_PAGESIZE) return 0x1000;
    errno = EINVAL;
    return -1;
}

/* A title never starts another process on this host. */
int execv(const char *path, char *const argv[]) {
    (void)path;
    (void)argv;
    errno = ENOSYS;
    return -1;
}
int setpgid(pid_t pid, pid_t pgid) {
    (void)pid;
    (void)pgid;
    errno = ENOSYS;
    return -1;
}
pid_t waitpid(pid_t pid, int *status, int options) {
    (void)pid;
    (void)status;
    (void)options;
    errno = ENOSYS;
    return -1;
}
