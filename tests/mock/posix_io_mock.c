#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>

#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static bool mock_enospc_enabled(void) {
    const char* value = getenv("AINIUX_MOCK_ENOSPC");
    return value != NULL && strcmp(value, "1") == 0;
}

static bool mock_eacces_enabled(void) {
    const char* value = getenv("AINIUX_MOCK_EACCES");
    return value != NULL && strcmp(value, "1") == 0;
}

static bool path_matches(const char* path, const char* marker) {
    return path != NULL && strstr(path, marker) != NULL;
}

static bool path_is_mock_enospc(const char* path) {
    return path_matches(path, "mock-enospc");
}

static bool path_is_mock_eacces(const char* path) {
    return path_matches(path, "mock-eacces");
}

static bool write_flags(int flags) {
    const int accmode = flags & O_ACCMODE;
    return accmode == O_WRONLY || accmode == O_RDWR;
}

static int (*real_open)(const char*, int, ...) = NULL;
static int (*real_openat)(int, const char*, int, ...) = NULL;
static ssize_t (*real_write)(int, const void*, size_t) = NULL;

static void init_symbols(void) {
    if (real_open == NULL) {
        real_open = dlsym(RTLD_NEXT, "open");
    }
    if (real_openat == NULL) {
        real_openat = dlsym(RTLD_NEXT, "openat");
    }
    if (real_write == NULL) {
        real_write = dlsym(RTLD_NEXT, "write");
    }
}

static int mock_open_result(const char* path, int flags, mode_t mode, bool has_mode) {
    init_symbols();
    if (real_open == NULL) {
        errno = ENOSYS;
        return -1;
    }

    if (mock_enospc_enabled() && path_is_mock_enospc(path) && write_flags(flags)) {
        errno = ENOSPC;
        return -1;
    }
    if (mock_eacces_enabled() && path_is_mock_eacces(path) && write_flags(flags)) {
        errno = EACCES;
        return -1;
    }

    if (has_mode) {
        return real_open(path, flags, mode);
    }
    return real_open(path, flags);
}

int open(const char* pathname, int flags, ...) {
    mode_t mode = 0;
    bool has_mode = false;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
        has_mode = true;
    }
    return mock_open_result(pathname, flags, mode, has_mode);
}

static int mock_openat_result(int dirfd, const char* pathname, int flags, mode_t mode, bool has_mode) {
    init_symbols();
    if (real_openat == NULL) {
        errno = ENOSYS;
        return -1;
    }

    char absolute[8192];
    if (pathname != NULL && pathname[0] == '/') {
        snprintf(absolute, sizeof(absolute), "%s", pathname);
    } else if (dirfd == AT_FDCWD) {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            snprintf(absolute, sizeof(absolute), "%s", pathname == NULL ? "" : pathname);
        } else {
            snprintf(absolute, sizeof(absolute), "%s/%s", cwd,
                     pathname == NULL ? "" : pathname);
        }
    } else {
        char proc_path[64];
        snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", dirfd);
        char dir_buffer[4096];
        const ssize_t dir_len = readlink(proc_path, dir_buffer, sizeof(dir_buffer) - 1);
        if (dir_len > 0) {
            dir_buffer[dir_len] = '\0';
            snprintf(absolute, sizeof(absolute), "%s/%s", dir_buffer,
                     pathname == NULL ? "" : pathname);
        } else {
            snprintf(absolute, sizeof(absolute), "%s", pathname == NULL ? "" : pathname);
        }
    }

    if (dirfd == AT_FDCWD) {
        if (mock_enospc_enabled() && path_is_mock_enospc(pathname) && write_flags(flags)) {
            errno = ENOSPC;
            return -1;
        }
        if (mock_eacces_enabled() && path_is_mock_eacces(pathname) && write_flags(flags)) {
            errno = EACCES;
            return -1;
        }
    }

    if (mock_enospc_enabled() && path_is_mock_enospc(absolute) && write_flags(flags)) {
        errno = ENOSPC;
        return -1;
    }
    if (mock_eacces_enabled() && path_is_mock_eacces(absolute) && write_flags(flags)) {
        errno = EACCES;
        return -1;
    }

    if (has_mode) {
        return real_openat(dirfd, pathname, flags, mode);
    }
    return real_openat(dirfd, pathname, flags);
}

int openat(int dirfd, const char* pathname, int flags, ...) {
    mode_t mode = 0;
    bool has_mode = false;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
        has_mode = true;
    }
    return mock_openat_result(dirfd, pathname, flags, mode, has_mode);
}

int __openat_2(int dirfd, const char* pathname, int flags) {
    return mock_openat_result(dirfd, pathname, flags, 0, false);
}

int __open_2(const char* pathname, int flags) {
    return mock_open_result(pathname, flags, 0, false);
}

ssize_t write(int fd, const void* buf, size_t count) {
    init_symbols();
    if (real_write == NULL) {
        errno = ENOSYS;
        return -1;
    }

    if (mock_enospc_enabled()) {
        char proc_path[64];
        snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
        char link_target[8192];
        const ssize_t link_len = readlink(proc_path, link_target, sizeof(link_target) - 1);
        if (link_len > 0) {
            link_target[link_len] = '\0';
            if (path_is_mock_enospc(link_target)) {
                errno = ENOSPC;
                return -1;
            }
        }
    }

    return real_write(fd, buf, count);
}