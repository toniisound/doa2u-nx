/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Weak fallbacks for POSIX calls the runtime links against but devkitA64's
   newlib/libsysbase may not provide. A strong definition in the toolchain
   libraries wins whenever it exists. */
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <switch.h>

__attribute__((weak)) long sysconf(int name)
{
    if (name == _SC_PAGESIZE) return 0x1000;
    errno = EINVAL;
    return -1;
}

/* File times are cosmetic for the guest; the SD card keeps its own. */
__attribute__((weak)) int futimens(int fd, const struct timespec times[2])
{
    (void)fd;
    (void)times;
    return 0;
}

/* C11 timespec_get: newlib declares it, devkitA64 does not implement it. */
__attribute__((weak)) int timespec_get(struct timespec *ts, int base)
{
    if (ts == NULL || base != TIME_UTC) return 0;
    if (clock_gettime(CLOCK_REALTIME, ts) != 0) return 0;
    return base;
}

/* One process owns the SD card's save journal; the advisory lock always holds. */
__attribute__((weak)) int flock(int fd, int operation)
{
    (void)operation;
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    return 0;
}

__attribute__((weak)) int fchmod(int fd, mode_t mode)
{
    (void)fd;
    (void)mode;
    return 0;
}

/* pread: newlib declares it, devkitA64 does not implement it. The runtime
   reads disc files from a worker thread with it while the game keeps using
   the same handle, so it must not move the file offset: on libnx's SD card
   device the handle's file struct begins with its FsFile, which reads at an
   absolute offset. */
__attribute__((weak)) ssize_t pread(int fd, void *buffer, size_t count, off_t offset)
{
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    __handle *handle = __get_handle(fd);
    const devoptab_t *device = handle != NULL ? devoptab_list[handle->device] : NULL;
    if (handle == NULL || handle->fileStruct == NULL || device == NULL || device->name == NULL ||
        strcmp(device->name, "sdmc") != 0) {
        errno = EBADF;
        return -1;
    }
    FsFile *file = (FsFile *)handle->fileStruct;
    u64 read = 0;
    const Result rc = fsFileRead(file, (s64)offset, buffer, count, FsReadOption_None, &read);
    if (R_FAILED(rc)) {
        errno = EIO;
        return -1;
    }
    return (ssize_t)read;
}
